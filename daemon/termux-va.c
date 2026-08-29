/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * termux-va - Android MediaCodec hardware decode proxy daemon for Termux
 * Copyright (C) 2026 lfdevs
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3 of the License.
 *
 * ******************************************************************************
 * MODIFICATION NOTICE (GPL-3.0 section 5)
 *
 * This file is a MODIFIED version of src/decode-daemon.c from the
 * droidspaces-media-decode project (Apache License, Version 2.0,
 * https://github.com/... droidspaces-media-decode).  It was ported to the
 * termux-va project and relicensed under the GNU General Public License
 * version 3.  Summary of the modifications relative to the original file:
 *
 *   1. Renamed decode-daemon -> termux-va.
 *   2. The TCP transport was REMOVED entirely: the daemon listens on a
 *      path-based Unix socket only.  Dropped: DEFAULT_PORT, the AF_INET
 *      listener, SO_REUSEADDR, TCP_NODELAY and the positional port
 *      argument.  The wire-level transport identifier 0 was renamed
 *      XFER_TCP -> XFER_INLINE (value unchanged, wire format unchanged).
 *   3. Default endpoint resolution: TERMUX_VA_SOCKET (full path) >
 *      TERMUX_VA_SOCKET_DIR (directory) > $TMPDIR/termux-va/termux-va.sock,
 *      with a termux-x11-style TMPDIR fallback chain.
 *   4. Parent directories of the socket are created recursively.
 *   5. Protocol constants were extracted into common/tva_protocol.h
 *      (values unchanged; that header is mirrored into the Mesa bridge).
 *   6. Socket file name decode.sock -> termux-va.sock.
 *   7. Comments and log messages were translated to English.
 *
 * Everything else - the DMD v3 wire protocol, the MediaCodec session
 * lifecycle, the memfd/SCM_RIGHTS shared-memory handoff, the drain
 * semantics and the concurrency model - is kept faithful to the original.
 * ******************************************************************************
 *
 * Listens on a path-based Unix socket, receives H.264/HEVC/VP8/VP9 units,
 * decodes them with MediaCodec in hardware, and returns NV12 frames.
 * Consumers live in Linux containers sharing Termux's tmp directory
 * (proot --shared-tmp mounts it as the container /tmp).
 *
 * Wire protocol (see common/tva_protocol.h and doc/protocol.md):
 *   client -> daemon:  [4B unit length, big-endian][unit data]
 *                      (H.264/HEVC: one Annex B NALU; VP8/VP9: full frame)
 *   daemon -> client:  [4B w][4B h][4B size][4B unit-index] + NV12 data
 *                      (inline), or a 24-byte SHM control message
 *
 * Concurrency structure (one session per client, two threads per session):
 *
 *   accept thread --+-> session 1 --+-- input thread : recv unit -> queueInputBuffer
 *                   |               \-- output thread: dequeueOutputBuffer -> send frame
 *                   \-> session 2 --+-- ...
 *
 * Why send and receive must be separate threads: an early version ran
 * "recv -> feed decoder -> take frame -> send 3.1MB" serially in one
 * thread; while sending, nothing was received and no frame was dequeued,
 * so the hardware decoder idled.  That structure saturated 85.6% of one
 * core (73% sys) and became the bottleneck of the whole pipeline.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>  /* htonl/ntohl: byte order of the wire format only */
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/file.h>   /* flock: single-instance check, see main() */
#include <fcntl.h>      /* open/O_*: do not rely on <sys/file.h> to pull
                         * these in - bionic/glibc pass but musl fails to
                         * compile without it (verified upstream 2026-08-25) */
#include <sys/mman.h>
#include <sys/syscall.h>
#include <stddef.h>
#include <linux/memfd.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include "tva_protocol.h"

#define INPUT_TIMEOUT_US   5000000
#define OUTPUT_TIMEOUT_US  20000
#define SEND_CHUNK         262144

/* MediaCodec buffer flags (not every NDK version exports these constants) */
#define FLAG_CODEC_CONFIG  2
#define FLAG_END_OF_STREAM 4

/* ------------------------------------------------------------------ log */
/*
 * 0=quiet (errors only)  1=info (connections/session stats, default)
 * 2=debug (per-frame)
 *
 * Per-frame logging must stay at level 2 and off by default: an early
 * version did fprintf + fflush per frame, synchronously hitting the disk,
 * which contributed measurably to the sys-time share.  At the default
 * level those calls are skipped entirely.
 */
static int log_level = 1;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void dlog(int lvl, const char *fmt, ...)
{
    if (lvl > log_level) return;
    pthread_mutex_lock(&log_lock);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    /* flush info-and-above immediately; debug level stays buffered */
    if (lvl <= 1) fflush(stderr);
    pthread_mutex_unlock(&log_lock);
}

/* ----------------------------------------------------------- exit control */
static volatile sig_atomic_t running = 1;

/* self-pipe: the running flag alone cannot wake a main loop blocked in
 * accept(); on a signal we write one byte to the pipe so the select()
 * before accept() returns immediately and SIGTERM is handled promptly
 * (otherwise the only way out is SIGKILL). */
static int wakefd[2] = { -1, -1 };

static void on_signal(int s)
{
    (void)s;
    running = 0;
    if (wakefd[1] >= 0) {
        ssize_t r = write(wakefd[1], "x", 1);
        (void)r;   /* errors cannot be handled inside a signal handler */
    }
}

/* -------------------------------------------------------- endpoint probe */
/*
 * (st_dev, st_ino) of the listening endpoint: after bind+listen succeeds we
 * stat() the final socket path once, and every handshake response reports
 * it verbatim.  The client stats the same path on its side and compares --
 * a mismatch means the client resolved a stale socket (classic cause: a
 * platform bind-mounting the single socket file instead of the directory
 * while the daemon restarted and changed the inode).  Historically both
 * sides could even stat the SAME orphan inode, which made this extremely
 * hard to diagnose by hand; having the daemon report the truth and the
 * client verify turns it into an automatic error.
 * Abstract-namespace endpoints have no path; they report 0 and the client
 * skips the check.
 */
static uint64_t g_ep_dev = 0;
static uint64_t g_ep_ino = 0;

/* Called after bind/listen succeeds: capture the real endpoint identity
 * and apply the TEST-ONLY overrides. */
static void endpoint_probe(const char *sock_path)
{
    struct stat st;
    if (sock_path && stat(sock_path, &st) == 0 && S_ISSOCK(st.st_mode)) {
        g_ep_dev = (uint64_t)st.st_dev;
        g_ep_ino = (uint64_t)st.st_ino;
    }
    /* ---- TEST-ONLY: for local verification of the client's mismatch
     * path only; never set in deployment. ---- */
    const char *fake = getenv("DMD_TEST_FAKE_INO");
    if (fake && *fake) {
        unsigned long long d = 0, i = 0;
        if (sscanf(fake, "%llu:%llu", &d, &i) == 2) {
            g_ep_dev = (uint64_t)d;
            g_ep_ino = (uint64_t)i;
            fprintf(stderr, "[TEST-ONLY] endpoint override: dev=%llu ino=%llu\n",
                    d, i);
        } else {
            fprintf(stderr, "[TEST-ONLY] DMD_TEST_FAKE_INO format is \"dev:ino\", ignored\n");
        }
    }
}

/*
 * Shared-memory pool: SHM_SLOTS slots used in rotation; the client returns
 * a slot by resetting its state word.
 *
 * Slot size is computed from the negotiated resolution (4K NV12 needs
 * 12441600 bytes; a hardcoded 8MB is not enough) with alignment headroom:
 * width aligned to 128, height to 32, then x1.5.  The whole pool is
 * rebuilt if the resolution grows mid-stream.
 *
 * SHM_SLOTS must stay >= the bridge-side pipeline depth (DMD_PIPELINE_DEPTH,
 * 6 in src/gallium/frontends/va/tva_bridge.c of the Mesa tree): the bridge
 * keeps issuing decode requests while pending < 6 without collecting
 * frames, so a 4-slot pool hits "all busy" at frame 5.  These two constants
 * live in different repositories; changing either requires checking the
 * other.  8 slots is ~95MB at 4K (8 x 12533760) and buys no-frame-loss
 * under slow consumers.
 */
/* TVA: shm_slot_bytes() kept identical to upstream (alignment 128x32, 1.5
 * bytes per pixel, 64KB floor), computed against the adaptive-playback
 * ceiling rather than the negotiated resolution - see the comment inside. */
static size_t shm_slot_bytes(int w, int h)
{
    /* Compute against the ceiling declared for adaptive playback, not the
     * handshake resolution.
     *
     * The decoder is configured with MAX_WIDTH/MAX_HEIGHT = max(w,1920) x
     * max(h,1088) and promises not to exceed that.  If slots were sized for
     * the current resolution only, a mid-stream resolution increase
     * (480p->720p) would overflow the slot and force the SHM session to
     * terminate - the same stream decodes fully over inline delivery but
     * truncates over SHM.  That was measured upstream as a real regression.
     *
     * The cost is that a 720p stream also reserves 1080p-sized slots
     * (4 x 3133440 ~= 12 MB) - trading memory for correctness, which beats
     * silently dropping the second half of the stream. */
    int mw = w > 1920 ? w : 1920;
    int mh = h > 1088 ? h : 1088;
    size_t aw = ((size_t)mw + 127) & ~(size_t)127;
    size_t ah = ((size_t)mh + 31)  & ~(size_t)31;
    size_t sz = aw * ah * 3 / 2;
    return sz < 64 * 1024 ? 64 * 1024 : sz;
}

/* MIME of each supported hardware decoder (see upstream
 * doc/verified-platform-facts.md for the capability matrix).
 *
 * This is a static id->MIME mapping only; it does not mean the device has
 * the corresponding hardware.  AV1 needs an Iris-class decoder (Snapdragon
 * 8 Elite tier); SM8150 has no such unit.  Actual availability is decided
 * by MediaCodec at configure time - failing to obtain a decoder fails the
 * handshake, which is the intended behavior; no per-device branching here. */
static const char *codec_mime(int id)
{
    switch (id) {
    case CODEC_H264: return "video/avc";
    case CODEC_HEVC: return "video/hevc";
    case CODEC_VP9:  return "video/x-vnd.on2.vp9";
    case CODEC_VP8:  return "video/x-vnd.on2.vp8";
    case CODEC_AV1:  return "video/av01";
    default:         return NULL;
    }
}

/* ------------------------------------------------------------ utilities */
/*
 * H.264 NAL unit type.  The start code may be 3 bytes (00 00 01) or
 * 4 bytes (00 00 00 01); the nal_unit_header must be read AFTER skipping
 * the start code, otherwise with a 4-byte start code buf[3]==0x01 and
 * SPS(7)/PPS(8) are misread as type 1, so the CSD is never recognized.
 * Returns -1 when the type cannot be determined.
 */
static int nalu_type(const uint8_t *b, size_t len)
{
    size_t off = 0;
    if (len >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1) off = 4;
    else if (len >= 3 && b[0] == 0 && b[1] == 0 && b[2] == 1) off = 3;
    if (off == 0 || off >= len) return -1;
    return b[off] & 0x1f;
}

/*
 * Whether this unit is a parameter set that must accumulate into the CSD.
 *
 * H.264: nal_unit_type = low 5 bits of the header byte, SPS=7 PPS=8
 * HEVC:  nal_unit_type = bits 1-6 of the first header byte, VPS=32 SPS=33 PPS=34
 * VP8/VP9: no Annex B parameter-set concept; whole frames are fed as-is
 */
static int is_param_set(int codec_id, const uint8_t *b, size_t len)
{
    if (codec_id == CODEC_H264) {
        int t = nalu_type(b, len);
        return (t == 7 || t == 8);
    }
    if (codec_id == CODEC_HEVC) {
        size_t off = 0;
        if (len >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1) off = 4;
        else if (len >= 3 && b[0] == 0 && b[1] == 0 && b[2] == 1) off = 3;
        if (off == 0 || off >= len) return 0;
        int t = (b[off] >> 1) & 0x3f;
        return (t == 32 || t == 33 || t == 34);
    }
    /* VP8 / VP9 / AV1 always return 0: they do not use Annex-B start codes
     * and have no independent parameter-set NALUs.  AV1's sequence header
     * is an OBU_SEQUENCE_HEADER inside the temporal unit that the consumer
     * forwards as a whole; the daemon needs no special handling. */
    return 0;
}

static int recv_all(int fd, void *b, size_t l)
{
    char *p = b;
    while (l > 0) {
        ssize_t n = read(fd, p, l);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; l -= n;
    }
    return 0;
}

/*
 * Returns 0 on success, SEND_PEER_GONE when the peer closed normally,
 * -1 on a real error.
 *
 * "Peer closed" is a separate bucket because a consumer closing after it
 * has enough frames is a normal shutdown (ffmpeg -f null, player
 * seek/stop), while the daemon usually still has a few frames in flight;
 * writing to the closed fd yields EPIPE/ECONNRESET.  Upstream measured
 * 78.6% of 412 real sessions ending this way; logging them as "send
 * failed" would bury real faults under noise.  Byte-level accounting
 * confirmed no frames are lost on this path: 933120000 bytes on disk /
 * (1920x1080x1.5) = exactly 300.000 frames.
 */
#define SEND_PEER_GONE (-2)

static int send_all(int fd, const void *b, size_t l)
{
    const char *p = b;
    while (l > 0) {
        ssize_t n = write(fd, p, l);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            /* the socket is blocking with no SO_SNDTIMEO, so a full buffer
             * blocks write() instead of returning an error; a returned
             * error means the peer is in trouble */
            if (errno == EPIPE || errno == ECONNRESET) {
                dlog(2, "send_all: peer closed (%s)", strerror(errno));
                return SEND_PEER_GONE;
            }
            dlog(1, "send_all: write failed: %s", strerror(errno));
            return -1;
        }
        if (n == 0) {
            dlog(2, "send_all: write returned 0 (peer closed)");
            return SEND_PEER_GONE;
        }
        p += n; l -= n;
    }
    return 0;
}

/* ---------------------------------------------------------------- session */
typedef struct {
    int              fd;
    AMediaCodec     *codec;
    volatile sig_atomic_t stop;        /* set on any-direction error; both threads exit */
    volatile sig_atomic_t input_done;  /* input thread finished (EOS attempted) */
    int              w, h;             /* written by the output thread only */
    long             units_in;
    long             frames_out;
    /* Pipeline tail frames that could not be delivered after the client
     * closed early.  Not frame loss: the client no longer needs them
     * (upstream verified per-frame md5 against the software reference,
     * 10/10 identical).  Counted separately so operations can tell a
     * normal shutdown from a transport fault at a glance. */
    long             frames_dropped_at_exit;
    int              peer_gone;        /* client closed normally, not a fault */
    int              id;

    /* handshake result */
    int              codec_id;         /* CodecId, defaults to CODEC_H264 */
    const char      *mime;
    int              negotiated;       /* 1 = client completed the handshake */

    /* output format details (written by the output thread only) */
    int              stride;
    int              slice_height;
    int              crop_l, crop_t, crop_r, crop_b;
    int              fmt_sent;         /* descriptor for the current format sent */
    int              fmt_changes;      /* format change count (including first) */

    /* shared-memory transport */
    XferMode         xfer;
    int              shm_fd;           /* memfd, -1 = not established */
    int              shm_listen;       /* abstract socket listen fd, -1 = none */
    char             shm_name[64];     /* abstract socket name, chosen by the daemon */
    uint8_t         *shm_base;         /* mmap base */
    size_t           shm_slot;         /* bytes per slot */
    size_t           shm_total;        /* pool size including control area */
    int              shm_next;         /* next slot to write */

    /* Reversible drain (in-band request of length 0)
     *
     * drain_req is incremented by the input thread, drain_done by the
     * output thread after finishing the EOS handling + AMediaCodec_flush.
     * The input thread waits until they are equal before feeding more
     * data, otherwise new data would be flushed away too (lost frames). */
    volatile sig_atomic_t drain_req;
    volatile sig_atomic_t drain_done;

    /* Input unit index used as the PTS tag (starting at 1).  Written by the
     * input thread only.  Gives every output frame a traceable "which
     * submission" identity; the bridge pairs surfaces by this index and
     * never needs to know the decoder's output order. */
    uint64_t         vcl_in;
    /* CSD copy: flush clears the parameter sets inside the decoder, so
     * after a drain they must be re-sent verbatim.  SPS+PPS is tiny;
     * 256 bytes is enough (1080p measured at 31+9). */
    uint8_t          csd_keep[256];
    size_t           csd_keep_len;
} Session;

/*
 * Shared-memory pool layout:
 *
 *   [control area 4096 bytes][slot 0][slot 1] ... [slot SHM_SLOTS-1]
 *
 * Each slot has a 32-bit state word in the control area; the daemon sets
 * it to 1 after writing (occupied), the client resets it to 0 when done
 * (free).  Accessed with __atomic builtins, no lock needed.  The return
 * path therefore never occupies the socket or interleaves with the unit
 * uplink.
 */

static inline volatile uint32_t *shm_slot_state(Session *s, int idx)
{
    return (volatile uint32_t *)(s->shm_base + (size_t)idx * sizeof(uint32_t));
}

static inline uint8_t *shm_slot_data(Session *s, int idx)
{
    return s->shm_base + SHM_CTRL_BYTES + (size_t)idx * s->shm_slot;
}

/* Pool setup/teardown are defined later; the handshake path needs them. */
static int  shm_prepare(Session *s, int w, int h);
static int  shm_handoff(Session *s);
static void shm_teardown(Session *s);

/*
 * Handshake.  Reads the first 4 bytes:
 *   equal to the magic -> read the remaining 20 bytes, configure the
 *                        session accordingly, send the handshake response
 *   anything else      -> protocol violation, reject (bare 12-byte status=4)
 * Returns 0 to continue, -1 to drop the connection.
 * Response format and version negotiation: see the comments inside the
 * function and at HELLO_VERSION in common/tva_protocol.h.
 */
static int do_handshake(Session *s)
{
    uint32_t first;
    if (recv_all(s->fd, &first, 4) < 0) return -1;   /* connected then gone */
    first = ntohl(first);

    if (first != HELLO_MAGIC) {
        /* The handshake is REQUIRED: daemon and consumers ship together, and
         * the no-handshake weak path is not kept.  Without a handshake there
         * is no MIME or initial size, and no stride/crop can be delivered. */
        dlog(1, "[%d] missing handshake (first word 0x%08x), rejecting", s->id, first);
        /* Same shape as every other rejection (12 bytes): the client reads
         * [status][mode][name_len] uniformly; replying only 4 bytes would
         * leave it blocked on the remaining 8. */
        uint32_t reply[3] = { htonl(4), htonl(XFER_INLINE), htonl(0) };
        send_all(s->fd, reply, sizeof(reply));
        return -1;
    }

    uint32_t rest[5];
    if (recv_all(s->fd, rest, sizeof(rest)) < 0) {
        dlog(1, "[%d] incomplete handshake frame", s->id);
        return -1;
    }
    uint32_t ver   = ntohl(rest[0]);
    uint32_t cid   = ntohl(rest[1]);
    uint32_t w     = ntohl(rest[2]);
    uint32_t h     = ntohl(rest[3]);
    uint32_t xfer  = ntohl(rest[4]);   /* requested transport, may be downgraded */

    uint32_t status = 0;
    const char *mime = (cid < CODEC_MAX) ? codec_mime((int)cid) : NULL;

    /* Version negotiation: the daemon accepts {2..HELLO_VERSION}.  Strict
     * equality is NOT required - that would force daemon and consumers to
     * upgrade in lockstep, and any one-sided update would break everyone.
     * A client at version>=3 gets the endpoint extension in the response;
     * a v2 client receives exactly the old 12 bytes. */
    if (ver < TVA_VERSION_MIN || ver > HELLO_VERSION) {
        dlog(1, "[%d] unsupported handshake version: %u (supported %d..%d)",
             s->id, ver, TVA_VERSION_MIN, HELLO_VERSION);
        status = 1;
    } else if (!mime) {
        dlog(1, "[%d] unknown codec id: %u", s->id, cid);
        status = 2;
    } else if (w < 96 || h < 96 || w > 8192 || h > 4320) {
        /* hardware range 96x96 .. 8192x4320 (upstream platform facts) */
        dlog(1, "[%d] resolution out of hardware range: %ux%u", s->id, w, h);
        status = 3;
    }

    if (status != 0) {
        uint32_t reply[3] = { htonl(status), htonl(XFER_INLINE), htonl(0) };
        send_all(s->fd, reply, sizeof(reply));
        return -1;
    }

    s->codec_id   = (int)cid;
    s->mime       = mime;
    s->w          = (int)w;
    s->h          = (int)h;
    s->negotiated = 1;

    /* The SHM pool must exist before the response is sent: the response
     * carries the abstract socket name.  The daemon picks the name - the
     * client cannot know it is connection #N, and guessing would collide. */
    XferMode want = XFER_INLINE;
    if (xfer == XFER_SHM) {
        if (shm_prepare(s, (int)w, (int)h) == 0) {
            want = XFER_SHM;
        } else {
            dlog(1, "[%d] SHM pool preparation failed, falling back to inline", s->id);
        }
    }

    /*
     * Response: [4B status][4B actual mode][4B name length][name...]
     * Inline mode has name length 0 and no following bytes.
     *
     * v3 extension (client requested version>=3): bit31 of the namelen word
     * is set as a marker (a real namelen is far below 2^31), followed by
     * 16 bytes before the name:
     *   [u32 dev_hi][u32 dev_lo][u32 ino_hi][u32 ino_lo]   (each big-endian)
     * i.e. g_ep_dev/g_ep_ino split into high/low u32s.
     * A v2 client's response is byte-identical to the old daemon; error
     * responses keep the bare 12-byte shape, and the client reads status
     * before deciding whether to parse the extension.
     *
     * TEST-ONLY: DMD_TEST_REPLY_LEGACY=1 forces a v2-shaped reply, to
     * exercise the client's downgrade path against an old daemon.
     */
    size_t nlen = (want == XFER_SHM) ? strlen(s->shm_name) : 0;
    int use_ext = (ver >= 3) && getenv("DMD_TEST_REPLY_LEGACY") == NULL;
    uint32_t nlen_wire = (uint32_t)nlen;
    if (use_ext)
        nlen_wire |= 0x80000000u;
    uint32_t reply[3] = { htonl(0), htonl((uint32_t)want), htonl(nlen_wire) };
    if (send_all(s->fd, reply, sizeof(reply)) < 0) goto reply_fail;
    if (use_ext) {
        uint32_t ext[4] = {
            htonl((uint32_t)(g_ep_dev >> 32)), htonl((uint32_t)(g_ep_dev & 0xffffffffu)),
            htonl((uint32_t)(g_ep_ino >> 32)), htonl((uint32_t)(g_ep_ino & 0xffffffffu)),
        };
        if (send_all(s->fd, ext, sizeof(ext)) < 0) goto reply_fail;
    }
    if (nlen > 0 && send_all(s->fd, s->shm_name, nlen) < 0) goto reply_fail;

    if (want == XFER_SHM) {
        /* The client has the name; now wait for it to pick up the memfd */
        if (shm_handoff(s) == 0) {
            s->xfer = XFER_SHM;
        } else {
            /* A failed handoff can only downgrade.  The client times out
             * into inline delivery on its side as well. */
            shm_teardown(s);
            s->xfer = XFER_INLINE;
            dlog(1, "[%d] SHM handoff failed, falling back to inline", s->id);
        }
    } else {
        s->xfer = XFER_INLINE;
    }

    /* Frame return has exactly two modes: SHM (memfd zero-copy) or inline
     * on the control connection.
     *
     * TVA: upstream deliberately did NOT print "TCP" here even when it
     * meant inline - the field describes "frames do not go through SHM but
     * are inlined on the control connection", regardless of the control
     * transport.  That ambiguity once cost upstream a misdiagnosis.  With
     * TCP removed the naming is unambiguous: SHM or inline.  The control
     * transport is always the Unix socket printed by "listening on ...". */
    dlog(1, "[%d] handshake ok: %s %ux%u frame-delivery=%s",
         s->id, mime, w, h, s->xfer == XFER_SHM ? "SHM" : "inline");
    return 0;

reply_fail:
    shm_teardown(s);
    return -1;
}

/*
 * Input thread: reads units from the socket and feeds the decoder.
 * SPS/PPS accumulate into codec-specific data submitted with
 * FLAG_CODEC_CONFIG; they produce no frames.
 */
static void *input_thread(void *arg)
{
    Session *s = arg;
    uint8_t *buf = malloc(MAX_FRAME);
    uint8_t *csd = malloc(MAX_FRAME);
    size_t csd_len = 0;

    if (!buf || !csd) {
        dlog(0, "[%d] input buffer allocation failed", s->id);
        s->stop = 1;
        goto done;
    }

    while (running && !s->stop) {
        uint32_t sz;
        if (recv_all(s->fd, &sz, 4) < 0) break;      /* client closed, normal end */
        sz = ntohl(sz);

        /* Length 0 = drain request (reversible), not a data unit.
         *
         * Why it exists: the consumer keeps only ~3 frames in flight while
         * the decoder needs a 4th input unit before emitting its first
         * frame when B-frames are present - a mutual wait.  The consumer
         * used to force frames out with shutdown(SHUT_WR), but that is
         * IRREVERSIBLE: the session is dead and must be rebuilt (measured
         * at 155 ms per frame, playback 4.7x slower).
         *
         * EOS + AMediaCodec_flush is reversible in the sense that the
         * session object stays usable: EOS makes the decoder emit whatever
         * it holds, then flush resets it and the CSD is re-sent, and the
         * session continues without a new connection.
         *
         * NOTE: "reversible" refers to the SESSION OBJECT only, not to
         * lossless video.  flush DESTROYS the reference-frame chain; P/B
         * frames decoded after it stay black until the next IDR (upstream
         * measured 135/708 all-black frames in a browser before tightening
         * the drain trigger).  Drain is a picture-quality-costly operation
         * and the bridge triggers it almost never (wait_is_futile logic).
         *
         * sz==0 carries it: it used to be an illegal length that old
         * clients never send, so no protocol version negotiation is needed. */
        if (sz == 0) {
            s->drain_req++;
            dlog(2, "[%d] drain request #%u", s->id, s->drain_req);

            ssize_t di = AMediaCodec_dequeueInputBuffer(s->codec, INPUT_TIMEOUT_US);
            if (di < 0) {
                dlog(1, "[%d] drain: dequeueInputBuffer failed: %zd", s->id, di);
                continue;
            }
            /* Queue EOS: the decoder emits everything queued and held back. */
            AMediaCodec_queueInputBuffer(s->codec, di, 0, 0, 0, FLAG_END_OF_STREAM);

            /* Wait for the output thread to see the EOS and finish the
             * flush before feeding the next unit.  This wait is mandatory:
             * flush discards everything inside the decoder, so data queued
             * meanwhile would be lost with it (lost frames).
             *
             * The wait MUST be bounded.  The output thread may have exited
             * early on a decode error, in which case drain_done never
             * catches up and an unbounded wait leaks the session forever.
             * Upstream saw all 8 sessions leak this way, the daemon spin at
             * 200% CPU and refuse new connections ("session limit reached")
             * while the browser simply decoded nothing - easy to misread as
             * an unrelated failure. */
            int waited_us = 0;
            const int drain_wait_max_us = 2000000;   /* 2s: far above normal drain time */
            while (running && !s->stop &&
                   s->drain_done < s->drain_req &&
                   waited_us < drain_wait_max_us) {
                usleep(1000);
                waited_us += 1000;
            }
            if (s->drain_done < s->drain_req) {
                dlog(1, "[%d] drain wait timed out (%d ms), abandoning session",
                     s->id, waited_us / 1000);
                s->stop = 1;
                break;
            }

            /* flush cleared the CSD; it must be re-sent or every following
             * VCL unit lacks its parameter sets.  Uses the accumulated copy
             * saved when the CSD was first submitted. */
            if (s->csd_keep_len > 0) {
                ssize_t ci = AMediaCodec_dequeueInputBuffer(s->codec, INPUT_TIMEOUT_US);
                if (ci >= 0) {
                    size_t cap;
                    uint8_t *ib = AMediaCodec_getInputBuffer(s->codec, ci, &cap);
                    if (ib && cap >= s->csd_keep_len) {
                        memcpy(ib, s->csd_keep, s->csd_keep_len);
                        AMediaCodec_queueInputBuffer(s->codec, ci, 0,
                                                     s->csd_keep_len, 0,
                                                     FLAG_CODEC_CONFIG);
                        dlog(2, "[%d] CSD re-sent after drain (%zu bytes)",
                             s->id, s->csd_keep_len);
                    } else {
                        AMediaCodec_queueInputBuffer(s->codec, ci, 0, 0, 0, 0);
                    }
                }
            }
            continue;
        }

        if (sz > MAX_FRAME) {
            dlog(1, "[%d] illegal unit length: %u", s->id, sz);
            break;
        }
        if (recv_all(s->fd, buf, sz) < 0) {
            dlog(1, "[%d] reading unit data interrupted", s->id);
            break;
        }
        s->units_in++;

        if (is_param_set(s->codec_id, buf, sz)) {      /* parameter set -> CSD */
            if (csd_len + sz <= MAX_FRAME) {
                memcpy(csd + csd_len, buf, sz);
                csd_len += sz;
            } else {
                dlog(1, "[%d] CSD accumulation over limit, dropped", s->id);
            }
            continue;
        }

        if (csd_len > 0) {
            ssize_t ci = AMediaCodec_dequeueInputBuffer(s->codec, INPUT_TIMEOUT_US);
            if (ci >= 0) {
                size_t cap;
                uint8_t *ib = AMediaCodec_getInputBuffer(s->codec, ci, &cap);
                if (ib && cap >= csd_len) {
                    memcpy(ib, csd, csd_len);
                    AMediaCodec_queueInputBuffer(s->codec, ci, 0, csd_len, 0,
                                                 FLAG_CODEC_CONFIG);
                    dlog(2, "[%d] CSD submitted (%zu bytes)", s->id, csd_len);
                    /* Keep a copy: the flush after a drain clears the CSD
                     * inside the decoder and it must be re-sent verbatim,
                     * or later VCL units have no parameter sets. */
                    if (csd_len <= sizeof(s->csd_keep)) {
                        memcpy(s->csd_keep, csd, csd_len);
                        s->csd_keep_len = csd_len;
                    }
                } else {
                    dlog(1, "[%d] CSD exceeds input buffer capacity", s->id);
                    AMediaCodec_queueInputBuffer(s->codec, ci, 0, 0, 0, 0);
                }
            } else {
                dlog(1, "[%d] dequeueInputBuffer(CSD) failed: %zd", s->id, ci);
            }
            csd_len = 0;
        }

        /* Dequeue an input buffer: failure here MUST be retried, never
         * silently dropped - dropping any VCL unit destroys the reference
         * chain and the picture is corrupted from that point on.
         *
         * When does it happen?  The input buffers are full, which only
         * occurs with aggressive feeders: ffmpeg and Firefox keep few
         * frames in flight and never trigger it; Chrome bursts hundreds of
         * decode requests and upstream measured 259 queued units exhausting
         * the input buffers, after which "dequeue failed: -1" ended the
         * session and Chrome lost hardware decoding entirely.
         *
         * Input-buffer exhaustion is BACKPRESSURE, not an error: the output
         * thread returns frames and the buffers are recycled.  Wait it out;
         * only a real error code gives up. */
        ssize_t bi;
        int bi_tries = 0;
        for (;;) {
            bi = AMediaCodec_dequeueInputBuffer(s->codec, INPUT_TIMEOUT_US);
            if (bi >= 0)
                break;
            if (bi != AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                dlog(1, "[%d] dequeueInputBuffer error: %zd", s->id, bi);
                break;
            }
            if (s->stop)
                break;
            bi_tries++;
            dlog(1, "[%d] input buffers full, retry #%d (wait %d ms)",
                 s->id, bi_tries, INPUT_TIMEOUT_US / 1000);
            if (bi_tries >= 12) {   /* 12 x 5s = 60s, enough for any sane backpressure */
                dlog(1, "[%d] input buffers persistently unavailable, giving up", s->id);
                break;
            }
        }
        if (bi < 0)
            goto done;   /* no buffer available: end the session, do not drop frames */
        size_t cap;
        uint8_t *ib = AMediaCodec_getInputBuffer(s->codec, bi, &cap);
        if (!ib || cap < sz) {
            dlog(1, "[%d] unit exceeds input buffer capacity (%u > %zu)", s->id, sz, cap);
            AMediaCodec_queueInputBuffer(s->codec, bi, 0, 0, 0, 0);
            continue;
        }
        memcpy(ib, buf, sz);
        /* PTS = index of this data unit (starting at 1, 1us steps).
         *
         * Not for timing: it gives every input unit a RETURNABLE identity.
         * MediaCodec carries presentationTimeUs verbatim onto the matching
         * output frame (verified upstream: values and strides match, never
         * rewritten).  The output thread puts it into the frame header so
         * the bridge knows exactly which submission a frame belongs to and
         * can pair it with a surface by submission index - completely
         * independent of the decoder's output order.
         *
         * That removes the need for the consumer to guess or negotiate the
         * output order: display order or decode order, the pairing is
         * correct either way.  Relying on output order was fragile: any
         * mismatch between the two sides produced silently misordered video
         * (105/150 frames out of order in an upstream measurement).
         *
         * Starting at 1: 0 is the "no PTS info" sentinel. */
        AMediaCodec_queueInputBuffer(s->codec, bi, 0, sz,
                                     (int64_t)s->vcl_in * PTS_UNIT_SCALE, 0);
        s->vcl_in++;
    }

    /* Submit end-of-stream so the output thread can deterministically
     * collect the remaining frames.  The correct way with buffered input is
     * an empty buffer flagged EOS, not AMediaCodec_signalEndOfInputStream
     * (that is for Surface input). */
    if (!s->stop) {
        ssize_t bi = AMediaCodec_dequeueInputBuffer(s->codec, INPUT_TIMEOUT_US);
        if (bi >= 0) {
            AMediaCodec_queueInputBuffer(s->codec, bi, 0, 0, 0, FLAG_END_OF_STREAM);
            dlog(2, "[%d] EOS submitted", s->id);
        } else {
            dlog(1, "[%d] could not submit EOS: %zd", s->id, bi);
        }
    }

done:
    free(buf);
    free(csd);
    s->input_done = 1;
    return NULL;
}

/*
 * Create the shared-memory pool and hand the memfd to the client through an
 * abstract socket.
 *
 * The name is derived from the session (dmd-shm-<pid>-<id>-<rand>); the
 * client listens on it right after the handshake response.  An abstract
 * socket is used instead of a path-based one because the container and
 * Termux share the network namespace (abstract sockets belong to the net
 * namespace and are visible on both sides), while mount namespaces are
 * separate - a path-based Unix socket created by the daemon would not exist
 * under the container's mount view.
 *
 * Returns 0 on success; on failure the caller degrades to inline delivery.
 */
static int shm_prepare(Session *s, int w, int h)
{
    s->shm_slot  = shm_slot_bytes(w, h);
    s->shm_total = SHM_CTRL_BYTES + s->shm_slot * SHM_SLOTS;

    s->shm_fd = (int)syscall(SYS_memfd_create, "dmd-frames", MFD_CLOEXEC);
    if (s->shm_fd < 0) {
        dlog(1, "[%d] memfd_create failed: %s", s->id, strerror(errno));
        return -1;
    }
    if (ftruncate(s->shm_fd, (off_t)s->shm_total) < 0) {
        dlog(1, "[%d] ftruncate failed: %s", s->id, strerror(errno));
        goto fail;
    }
    s->shm_base = mmap(NULL, s->shm_total, PROT_READ | PROT_WRITE,
                       MAP_SHARED, s->shm_fd, 0);
    if (s->shm_base == MAP_FAILED) {
        dlog(1, "[%d] mmap failed: %s", s->id, strerror(errno));
        s->shm_base = NULL;
        goto fail;
    }
    memset(s->shm_base, 0, SHM_CTRL_BYTES);   /* mark all slots free */

    /* The daemon picks the name and listens: the client cannot know which
     * connection number it is, and guessing would collide or miss.
     *
     * pid + session id is already unique, but predictable - any process in
     * the same net namespace could bind the name first, making the daemon's
     * bind fail and degrading the session (no fd leak, but a cheap
     * downgrade attack).  A 32-bit random suffix removes predictability. */
    snprintf(s->shm_name, sizeof(s->shm_name), "dmd-shm-%d-%d-%08x",
             (int)getpid(), s->id, (unsigned)arc4random());

    s->shm_listen = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s->shm_listen < 0) {
        dlog(1, "[%d] abstract socket creation failed: %s", s->id, strerror(errno));
        goto fail;
    }
    struct sockaddr_un ua;
    memset(&ua, 0, sizeof(ua));
    ua.sun_family = AF_UNIX;
    ua.sun_path[0] = 0;                        /* abstract namespace */
    strncpy(ua.sun_path + 1, s->shm_name, sizeof(ua.sun_path) - 2);
    socklen_t ulen = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
                                 + 1 + strlen(s->shm_name));
    if (bind(s->shm_listen, (struct sockaddr *)&ua, ulen) < 0 ||
        listen(s->shm_listen, 1) < 0) {
        dlog(1, "[%d] abstract socket bind/listen failed: %s", s->id, strerror(errno));
        close(s->shm_listen);
        s->shm_listen = -1;
        goto fail;
    }
    return 0;

fail:
    if (s->shm_base) { munmap(s->shm_base, s->shm_total); s->shm_base = NULL; }
    if (s->shm_fd >= 0) { close(s->shm_fd); s->shm_fd = -1; }
    return -1;
}

/*
 * Wait for the client to connect and hand over the memfd.  Must be called
 * after the handshake response - the client learns the name from it.
 */
static int shm_handoff(Session *s)
{
    /* Bounded wait: the client may have crashed or may not understand SHM;
     * never block the session indefinitely */
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(s->shm_listen, &rs);
    struct timeval tv = { 3, 0 };
    int r = select(s->shm_listen + 1, &rs, NULL, NULL, &tv);
    if (r <= 0) {
        dlog(1, "[%d] timed out waiting for the client to pick up SHM", s->id);
        return -1;
    }
    int cs = accept(s->shm_listen, NULL, NULL);
    if (cs < 0) {
        dlog(1, "[%d] accept failed: %s", s->id, strerror(errno));
        return -1;
    }

    /* SCM_RIGHTS carries the fd; the slot parameters accompany it so the
     * client can mmap correctly */
    uint32_t meta[3] = {
        htonl((uint32_t)SHM_SLOTS),
        htonl((uint32_t)s->shm_slot),
        htonl((uint32_t)s->shm_total)
    };
    struct iovec io = { meta, sizeof(meta) };
    char cbuf[CMSG_SPACE(sizeof(int))];
    memset(cbuf, 0, sizeof(cbuf));
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &io; mh.msg_iovlen = 1;
    mh.msg_control = cbuf; mh.msg_controllen = sizeof(cbuf);
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type  = SCM_RIGHTS;
    cm->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &s->shm_fd, sizeof(int));

    if (sendmsg(cs, &mh, 0) < 0) {
        dlog(1, "[%d] memfd transfer failed: %s", s->id, strerror(errno));
        close(cs);
        return -1;
    }
    close(cs);

    /* handoff complete; the listener is no longer needed */
    close(s->shm_listen);
    s->shm_listen = -1;

    dlog(1, "[%d] SHM handed over: %d slots x %zu bytes (total %zu)",
         s->id, SHM_SLOTS, s->shm_slot, s->shm_total);
    return 0;
}

static void shm_teardown(Session *s)
{
    if (s->shm_listen >= 0) { close(s->shm_listen); s->shm_listen = -1; }
    if (s->shm_base) { munmap(s->shm_base, s->shm_total); s->shm_base = NULL; }
    if (s->shm_fd >= 0) { close(s->shm_fd); s->shm_fd = -1; }
}

/*
 * Deliver one frame through shared memory: copy into a free slot and write
 * only a 24-byte control message on the socket.
 *
 *   [4B w][4B h][4B 0xFFFFFFFE][4B slot][4B length][4B unit index]
 *
 * SIX words (24 bytes), not five.  The 6th field is sent unconditionally;
 * CAP_FRAME_PTS only ANNOUNCES the field's existence in the format block,
 * it is not a send switch.  Reading 20 bytes shifts every subsequent frame
 * parse by 4 bytes and derails the whole stream - the upstream client/
 * reference implementation had exactly that bug, and this comment was the
 * source of the confusion.
 *
 * Compared with inline delivery this saves the two kernel copies (into the
 * socket buffer on send, out of it on recv).  One CPU copy remains:
 * MediaCodec output buffer -> shared memory (removing it needs dmabuf
 * zero-copy; the decoder output is gralloc-owned).
 *
 * Returns 0 on success, SEND_PEER_GONE when the client left normally,
 * -1 on a real error.
 */
static int send_frame_shm(Session *s, const uint8_t *data, size_t len,
                          uint32_t pts)
{
    if (len > s->shm_slot) {
        dlog(1, "[%d] frame of %zu bytes exceeds slot of %zu, pool rebuild needed",
             s->id, len, s->shm_slot);
        return -1;
    }

    /* Find a free slot.  Rotation starts at shm_next so all slots get fair
     * use; the client resets the state word when done.
     *
     * The wait bound must stay well above the bridge-side per-call frame
     * timeout (DMD_FRAME_TIMEOUT_MS, 5000ms in tva_bridge.c): the client may
     * legitimately block 5 seconds on each fetch, while an early version
     * here gave up after 1 second and killed the whole session -
     * 1000 < 5000 is a bug by construction.  Upstream measured 4K + slow
     * consumer (ffmpeg to disk) triggering it 10/10 times, dropping 56-83%
     * of frames with the client reporting "Conversion failed!".  This was
     * the only real frame-loss path in the module.
     *
     * A transiently exhausted pool is NORMAL BACKPRESSURE, not a fault: as
     * long as the client is alive we keep waiting and let the client's own
     * timeout decide when to give up.  Same stance as Wayland
     * wl_buffer.release, GstBufferPool blocking, V4L2 buffer pools - none of
     * them treat pool exhaustion as fatal. */
    int slot = -1;
    for (int spin = 0; spin < SHM_SLOT_WAIT_MS && slot < 0; spin++) {
        for (int k = 0; k < SHM_SLOTS; k++) {
            int idx = (s->shm_next + k) % SHM_SLOTS;
            uint32_t st = __atomic_load_n(shm_slot_state(s, idx), __ATOMIC_ACQUIRE);
            if (st == 0) { slot = idx; break; }
        }
        if (slot < 0) {
            /* client gone or daemon exiting: treat as "peer left", not a fault */
            if (!running || s->stop) return SEND_PEER_GONE;
            /* leave a trace during long waits to diagnose slow consumers
             * (one line every 2 seconds) */
            if (spin > 0 && spin % 2000 == 0)
                dlog(2, "[%d] all slots busy, waited %d ms (slow consumer, normal backpressure)",
                     s->id, spin);
            usleep(1000);
        }
    }
    if (slot < 0) {
        /* full bound elapsed with no slot - the client is genuinely stuck */
        dlog(1, "[%d] no free slot after %d ms (client stuck), ending session",
             s->id, SHM_SLOT_WAIT_MS);
        return -1;
    }

    memcpy(shm_slot_data(s, slot), data, len);
    /* release ordering: make the data visible before the state word */
    __atomic_store_n(shm_slot_state(s, slot), 1u, __ATOMIC_RELEASE);
    s->shm_next = (slot + 1) % SHM_SLOTS;

    /* 6th word = input unit index, same meaning as the 4th field of the
     * inline frame header, so both delivery modes expose identical pairing
     * information to the bridge. */
    uint32_t msg[6] = {
        htonl((uint32_t)s->w), htonl((uint32_t)s->h),
        htonl(SHMFRAME_SENTINEL),
        htonl((uint32_t)slot), htonl((uint32_t)len),
        htonl(pts)
    };
    int rc = send_all(s->fd, msg, sizeof(msg));
    if (rc < 0) {
        /* notify failed: release the slot immediately or the pool leaks */
        __atomic_store_n(shm_slot_state(s, slot), 0u, __ATOMIC_RELEASE);
        return rc;
    }
    return 0;
}

/*
 * Send the format descriptor block (sentinel header + 32 bytes of body).
 * Only sent to handshaked clients; old unhandshaked clients would not
 * understand it and must not receive it.
 * Returns 0 on success, -1 on failure (caller ends the session).
 */
static int send_format_desc(Session *s)
{
    /* Fallback for missing values: use what we know if FORMAT_CHANGED has
     * not arrived yet */
    if (s->stride <= 0)       s->stride = s->w;
    if (s->slice_height <= 0) s->slice_height = s->h;
    if (s->crop_r <= 0)       s->crop_r = s->w - 1;
    if (s->crop_b <= 0)       s->crop_b = s->h - 1;

    /* Word 2 used to be a reserved 0 and is now the capability flags:
     * CAP_FRAME_PTS announces that every frame header carries the 4th
     * field (PTS = input unit index).  Old daemons keep it 0 and clients
     * parse 3-word headers accordingly - backwards compatible. */
    uint32_t hdr[3] = { htonl(0), htonl(CAP_FRAME_PTS),
                        htonl(FMTDESC_SENTINEL) };
    uint32_t body[FMTDESC_WORDS] = {
        htonl((uint32_t)s->w),      htonl((uint32_t)s->h),
        htonl((uint32_t)s->stride), htonl((uint32_t)s->slice_height),
        htonl((uint32_t)s->crop_l), htonl((uint32_t)s->crop_t),
        htonl((uint32_t)s->crop_r), htonl((uint32_t)s->crop_b)
    };
    if (send_all(s->fd, hdr, sizeof(hdr)) < 0 ||
        send_all(s->fd, body, sizeof(body)) < 0) {
        dlog(1, "[%d] failed to send format descriptor", s->id);
        s->stop = 1;
        return -1;
    }
    s->fmt_sent = 1;
    return 0;
}

/*
 * Output thread: dequeues decoded frames and writes them back to the
 * socket.  Fully decoupled from the input thread - sending a large frame
 * never blocks unit reception or decoder feeding.
 */
static void *output_thread(void *arg)
{
    Session *s = arg;
    int idle_after_eof = 0;

    while (running && !s->stop) {
        AMediaCodecBufferInfo info;
        ssize_t oi = AMediaCodec_dequeueOutputBuffer(s->codec, &info, OUTPUT_TIMEOUT_US);

        if (oi >= 0) {
            idle_after_eof = 0;
            int eos = (info.flags & FLAG_END_OF_STREAM) != 0;

            /* Handshaked clients must receive the format descriptor before
             * any frame.  Normally FORMAT_CHANGED arrives first, but do not
             * depend on it: if the decoder yields a frame directly, send
             * the descriptor from known values to keep the client aligned. */
            if (!s->fmt_sent && info.size > 0) {
                if (send_format_desc(s) == 0)
                    dlog(2, "[%d] format descriptor sent late (FORMAT_CHANGED had not arrived)", s->id);
            }

            if (info.size > 0 && !s->stop) {
                size_t osz;
                uint8_t *ob = AMediaCodec_getOutputBuffer(s->codec, oi, &osz);
                if (ob) {
                    int rc;
                    if (s->xfer == XFER_SHM) {
                        rc = send_frame_shm(s, ob + info.offset,
                                            (size_t)info.size,
                                            (uint32_t)(info.presentationTimeUs / PTS_UNIT_SCALE));
                    } else {
                        /* 4th field = input unit index of this frame.
                         * MediaCodec carries the presentationTimeUs given
                         * at queueInputBuffer verbatim onto the output
                         * frame, so the bridge knows exactly which
                         * submission produced it. */
                        uint32_t hdr[4] = {
                            htonl((uint32_t)s->w),
                            htonl((uint32_t)s->h),
                            htonl((uint32_t)info.size),
                            htonl((uint32_t)(info.presentationTimeUs / PTS_UNIT_SCALE))
                        };
                        rc = send_all(s->fd, hdr, sizeof(hdr));
                        size_t off = (size_t)info.offset;
                        size_t rem = (size_t)info.size;
                        while (rc == 0 && rem > 0) {
                            size_t ch = rem > SEND_CHUNK ? SEND_CHUNK : rem;
                            rc = send_all(s->fd, ob + off, ch);
                            if (rc != 0) break;
                            off += ch; rem -= ch;
                        }
                    }
                    if (rc == SEND_PEER_GONE) {
                        /* The client took enough frames and closed normally;
                         * the remainder are pipeline tail frames (14 in the
                         * upstream measurement).  A normal shutdown, not a
                         * fault: per-frame md5 matched the software
                         * reference 10/10 with no loss.  Counted separately,
                         * logged at debug level. */
                        s->frames_dropped_at_exit++;
                        s->peer_gone = 1;
                        s->stop = 1;
                    } else if (rc < 0) {
                        dlog(1, "[%d] frame send failed (transport error), ending session", s->id);
                        s->stop = 1;
                    } else {
                        s->frames_out++;
                        dlog(2, "[%d] frame %dx%d %d bytes", s->id, s->w, s->h, info.size);
                    }
                }
            }
            AMediaCodec_releaseOutputBuffer(s->codec, oi, 0);
            if (eos) {
                /* EOS has two sources with very different handling:
                 *   1) drain request (drain_req > drain_done) - the session
                 *      stays alive; flush resets the decoder and the input
                 *      thread re-sends the CSD
                 *   2) the client really closed its write end - end of
                 *      stream, exit the thread
                 * The discriminator is whether a drain request is pending. */
                if (s->drain_req > s->drain_done) {
                    media_status_t fs = AMediaCodec_flush(s->codec);
                    if (fs != AMEDIA_OK) {
                        dlog(1, "[%d] flush after drain failed: %d", s->id, fs);
                        s->stop = 1;
                        break;
                    }
                    /* flush discards undelivered output; the format
                     * descriptor must be re-sent too or the client keeps
                     * parsing with stale stride/crop and every subsequent
                     * frame is misaligned. */
                    s->fmt_sent = 0;
                    s->drain_done++;
                    dlog(2, "[%d] drain #%u complete, session continues", s->id, s->drain_done);
                    continue;
                }
                dlog(2, "[%d] EOS received, output finished", s->id);
                break;
            }
        } else if (oi == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *of = AMediaCodec_getOutputFormat(s->codec);
            if (of) {
                int w = s->w, h = s->h;
                AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_WIDTH, &w);
                AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_HEIGHT, &h);
                s->w = w; s->h = h;

                /* stride / slice_height: the decoder output buffer's real
                 * line pitch and plane height.  Qualcomm Venus aligns width
                 * to 128 and height to 32; without these values the UV
                 * plane offset cannot be located correctly. */
                int stride = 0, slice = 0;
                if (!AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_STRIDE, &stride))
                    stride = w;
                if (!AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &slice))
                    slice = h;
                s->stride = stride;
                s->slice_height = slice;

                /* Display crop rect: the actually visible region.  When
                 * 1080p decodes into a 1920x1088 buffer, crop_bottom is
                 * 1079 and the extra 8 rows are alignment padding. */
                int cl = 0, ct = 0, cr = w - 1, cb = h - 1;
                AMediaFormat_getRect(of, AMEDIAFORMAT_KEY_DISPLAY_CROP,
                                     &cl, &ct, &cr, &cb);
                s->crop_l = cl; s->crop_t = ct;
                s->crop_r = cr; s->crop_b = cb;
                AMediaFormat_delete(of);

                s->fmt_changes++;
                dlog(1, "[%d] output format %dx%d stride=%d slice=%d crop=(%d,%d)-(%d,%d)%s",
                     s->id, w, h, stride, slice, cl, ct, cr, cb,
                     s->fmt_changes > 1 ? " (mid-stream change)" : "");

                /* Mark for (re)sending: on a mid-stream resolution change
                 * the client must learn the new stride/crop or it keeps
                 * parsing with the old geometry and every frame is offset. */
                s->fmt_sent = 0;
                send_format_desc(s);
            }
        } else if (oi == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            /* nothing to do: with the NDK the buffer pointer is re-fetched
             * via getOutputBuffer every time */
        } else if (oi == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            /* input finished but EOS never comes out: it could not be
             * submitted; avoid spinning forever */
            if (s->input_done && ++idle_after_eof > 100) {
                dlog(1, "[%d] no output long after input finished, giving up", s->id);
                break;
            }
        } else {
            dlog(1, "[%d] dequeueOutputBuffer unexpected: %zd", s->id, oi);
            break;
        }
    }
    return NULL;
}

/* ------------------------------------------------- concurrent client count */
static int client_count = 0;
static pthread_mutex_t count_lock = PTHREAD_MUTEX_INITIALIZER;

static void client_release(void)
{
    pthread_mutex_lock(&count_lock);
    client_count--;
    pthread_mutex_unlock(&count_lock);
}

/*
 * Session thread: creates the decoder for one client and runs the
 * input/output threads until the session ends.
 */
static void *session_thread(void *arg)
{
    Session *s = arg;

    /* The handshake must complete before the decoder is configured: it
     * decides the MIME and the initial resolution */
    if (do_handshake(s) < 0) goto out_fd;

    AMediaFormat *fmt = AMediaFormat_new();
    if (!fmt) { dlog(0, "[%d] AMediaFormat_new failed", s->id); goto out_fd; }
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, s->mime);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, s->w);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, s->h);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, MAX_FRAME);

    /* adaptive-playback: declare the largest size that may appear later so
     * the decoder pre-allocates its output pool accordingly and a
     * mid-stream resolution change does not require a reconfigure.
     * max(handshake size, 1080p) - larger wastes memory, smaller forces
     * reconfiguration after all. */
    int max_w = s->w > 1920 ? s->w : 1920;
    int max_h = s->h > 1088 ? s->h : 1088;
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_WIDTH,  max_w);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_HEIGHT, max_h);

    /* Low latency: make the decoder emit frames as soon as possible instead
     * of holding a few back to fill its pipeline.
     *
     * Why it matters: MediaCodec steadies out 2-3 input units behind, while
     * a browser's ffmpeg keeps only 3 frames in flight (H.264 has_b_frames=2
     * decides) - it submits the 3rd frame and then blocks in
     * vaSyncSurface waiting for the 1st.  A one-frame mutual wait ->
     * consumer-side flush fallback (irreversible shutdown(SHUT_WR)) ->
     * session destroyed -> browser falls back to software decoding
     * permanently.  Upstream measured Firefox 140 getting exactly one
     * hardware frame before dropping to software.  Command-line ffmpeg is
     * unaffected because it feeds far ahead of consumption.
     *
     * Why a literal string instead of AMEDIAFORMAT_KEY_LOW_LATENCY: the
     * symbol is __INTRODUCED_IN(30) (NdkMediaFormat.h) while this daemon
     * builds against API 29.  The key is just a string constant; writing
     * the literal avoids raising the build API and avoids weak-symbol
     * null checks.  On devices below API 30 MediaCodec ignores unknown
     * keys - the pre-existing behavior, never a failure. */
    AMediaFormat_setInt32(fmt, "low-latency", 1);

    /* Make the decoder output in DECODE order instead of buffering for
     * display order.
     *
     * This is the fix for the black/flickering frames.  In display order
     * with B-frames the first frame only comes out after the 4th input
     * unit, but a browser keeps 3 frames in flight - one-frame mutual wait
     * again.  Forcing EOS used to be the workaround, but EOS/flush/rebuild
     * destroy the reference chain and ~90% of frames came out black
     * (luma 16 in 54/60 frames).
     *
     * With this key the lag drops from 4 to 1 (upstream measured key by
     * key: low-latency, max-output-reorder-frames, output-delay,
     * vendor.qti-ext-dec-low-latency all had no effect - only this one),
     * the mutual wait disappears and drains/rebuilds are no longer needed.
     *
     * Historical note (resolved): the consumer once needed a compile-time
     * constant declaring the decoder's output order and had to agree
     * exactly, otherwise frames paired wrongly with no error (105/150
     * misordered).  That is gone - every frame now carries its own input
     * unit index (CAP_FRAME_PTS) and the bridge pairs by index, fully
     * decoupled from the output order.
     *
     * Literal string: this is a Qualcomm vendor extension, absent from NDK
     * headers.  Non-Qualcomm platforms ignore the unknown key - the
     * pre-existing behavior, never a failure. */
    AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-picture-order.enable", 1);

    s->codec = AMediaCodec_createDecoderByType(s->mime);
    if (!s->codec) { dlog(0, "[%d] no available decoder: %s", s->id, s->mime); goto out_fmt; }

    media_status_t st = AMediaCodec_configure(s->codec, fmt, NULL, NULL, 0);
    if (st != AMEDIA_OK) { dlog(0, "[%d] configure failed: %d", s->id, st); goto out_started; }

    st = AMediaCodec_start(s->codec);
    if (st != AMEDIA_OK) { dlog(0, "[%d] start failed: %d", s->id, st); goto out_started; }

    pthread_t tin, tout;
    int have_in = (pthread_create(&tin, NULL, input_thread, s) == 0);
    if (!have_in) { dlog(0, "[%d] cannot create input thread", s->id); s->stop = 1; }
    int have_out = (pthread_create(&tout, NULL, output_thread, s) == 0);
    if (!have_out) { dlog(0, "[%d] cannot create output thread", s->id); s->stop = 1; }

    if (have_in)  pthread_join(tin, NULL);
    if (have_out) pthread_join(tout, NULL);

    /* Log the shutdown reason: upstream measured 78.6% of 412 real sessions
     * ending as a normal client close, which used to share the "send frame
     * failed" log line with real faults, making almost every session look
     * broken. */
    if (s->peer_gone)
        dlog(1, "[%d] session end (client closed normally): %ld units in, %ld frames out, "
                "%ld tail frames undelivered",
             s->id, s->units_in, s->frames_out, s->frames_dropped_at_exit);
    else
        dlog(1, "[%d] session end: %ld units in, %ld frames out",
             s->id, s->units_in, s->frames_out);

out_started:
    /* configure / start failures must STILL stop the codec before deleting.
     *
     * This used to jump straight to delete on failure and the whole daemon
     * died with SIGABRT:
     *   FORTIFY: pthread_mutex_lock called on a destroyed mutex
     *   Fatal signal 6 (SIGABRT) in tid NNNN (CodecLooper)
     *
     * As soon as AMediaCodec_createDecoderByType succeeds, Codec2 has
     * already started its CodecLooper callback thread.  A configure failure
     * does not stop it, and deleting immediately destroys a mutex that
     * thread is using - the crash is inside Codec2's own thread, so no
     * pthread_mutex_destroy in this file is involved.
     *
     * The blast radius is far beyond one failed session: the watchdog
     * restarts five times into cooldown and hardware decoding is dead for
     * the whole device, including codecs that worked fine.  The trigger is
     * any configure failure (exposed on SM8750 when AV1 was added:
     * QC2VppFilterCaps does not know c2.qti.av1.decoder).
     *
     * stop() on a codec that never started is safe: the NDK allows it in
     * the Configured state and returns an error code without crashing; the
     * return value is deliberately ignored - the only goal is letting
     * Codec2 reap its callback thread. */
    AMediaCodec_stop(s->codec);
    AMediaCodec_delete(s->codec);
out_fmt:
    AMediaFormat_delete(fmt);
out_fd:
    shm_teardown(s);
    close(s->fd);
    free(s);
    client_release();
    return NULL;
}

/* -------------------------------------------------- endpoint resolution */
/*
 * TVA: default endpoint resolution (upstream had --sock only, defaulting to
 * a TCP listener that termux-va removed).
 *
 * Order:
 *   1. --sock command line argument (handled in main)
 *   2. TERMUX_VA_SOCKET      - full socket file path
 *   3. TERMUX_VA_SOCKET_DIR  - directory; TVA_SOCK_NAME is appended
 *   4. $TMPDIR/termux-va/termux-va.sock
 *
 * TMPDIR fallback chain mirrors termux-x11 (cmdentrypoint.cpp): an adb
 * shell injects TMPDIR=/data/local/tmp which is NOT what we want, so that
 * value is treated as unset; then /tmp is used if it exists (container /
 * shared-tmp view), and finally the Termux prefix tmp.
 */
#define TVA_DEFAULT_SOCK_DIR "/data/data/com.termux/files/usr/tmp"

static const char *resolve_socket_path(char *buf, size_t bufsz)
{
    const char *env_sock = getenv("TERMUX_VA_SOCKET");
    if (env_sock && *env_sock)
        return env_sock;

    const char *env_dir = getenv("TERMUX_VA_SOCKET_DIR");
    if (env_dir && *env_dir) {
        snprintf(buf, bufsz, "%s/%s", env_dir, TVA_SOCK_NAME);
        return buf;
    }

    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp || strcmp(tmp, "/data/local/tmp") == 0) {
        struct stat st;
        if (stat("/tmp", &st) == 0 && S_ISDIR(st.st_mode))
            tmp = "/tmp";
        else
            tmp = TVA_DEFAULT_SOCK_DIR;
    }
    snprintf(buf, bufsz, "%s/%s/%s", tmp, TVA_SOCKET_DIR_NAME, TVA_SOCK_NAME);
    return buf;
}

/*
 * TVA: create every missing parent directory of `path` (0755).  Upstream
 * created at most one level; the termux-va default endpoint lives one level
 * below $TMPDIR and env overrides may point deeper, so parents are created
 * recursively.  The socket file itself is created by bind(), never here.
 * Returns 0 on success, -1 on failure.
 */
static int ensure_parent_dirs(const char *path)
{
    char buf[512];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(buf)) return -1;
    memcpy(buf, path, len + 1);

    /* strip trailing slashes so "a/b/" does not end with an empty component */
    while (len > 1 && buf[len - 1] == '/') buf[--len] = '\0';

    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) < 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ main */
static void usage(const char *prog)
{
    printf(
        "Usage: %s [options]\n"
        "  --sock <path|dir>   Override the Unix socket location.\n"
        "                      A directory receives the fixed file name %s\n"
        "                      (created if missing); a path ending in .sock is\n"
        "                      used as a single socket file (its inode changes on\n"
        "                      every restart - prefer a directory).\n"
        "  -v                  Per-frame debug logging\n"
        "  -q                  Errors only\n"
        "  -h                  This help\n"
        "\n"
        "Endpoint resolution order:\n"
        "  1. --sock argument\n"
        "  2. TERMUX_VA_SOCKET      (full socket file path)\n"
        "  3. TERMUX_VA_SOCKET_DIR  (directory, %s is appended)\n"
        "  4. $TMPDIR/%s/%s  (TMPDIR falls back to /tmp, then\n"
        "     %s)\n"
        "\n"
        "termux-va listens on a path-based Unix socket ONLY - the upstream TCP\n"
        "fallback listener was removed.  Containers reach the socket through the\n"
        "shared tmp directory: with proot-distro --shared-tmp the same directory\n"
        "appears as /tmp/%s inside the container.\n"
        "\n"
        "MediaCodec access requires a SELinux domain that may talk to the media\n"
        "services.  Termux processes run as a normal app (untrusted_app), which\n"
        "is the standard path used by every video app - no root needed.\n",
        prog, TVA_SOCK_NAME, TVA_SOCK_NAME,
        TVA_SOCKET_DIR_NAME, TVA_SOCK_NAME, TVA_DEFAULT_SOCK_DIR,
        TVA_SOCKET_DIR_NAME);
}

int main(int argc, char **argv)
{
    const char *sock_arg = NULL;   /* non-NULL when --sock was given */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0)      log_level = 2;
        else if (strcmp(argv[i], "-q") == 0) log_level = 0;
        else if (strcmp(argv[i], "--sock") == 0 && i + 1 < argc) {
            sock_arg = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            /* TVA: upstream accepted a positional TCP port here; the TCP
             * transport no longer exists. */
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (pipe(wakefd) < 0) { perror("pipe"); return 1; }

    /* Resolve the endpoint (see resolve_socket_path for the order). */
    char resolved[512];
    const char *sock_path = sock_arg ? sock_arg
                                     : resolve_socket_path(resolved, sizeof(resolved));
    if (!sock_path || !*sock_path) {
        fprintf(stderr, "unable to determine the socket path: set --sock, "
                        "TERMUX_VA_SOCKET, TERMUX_VA_SOCKET_DIR or TMPDIR\n");
        return 1;
    }

    int srv;

    /* A path pointing at a DIRECTORY receives the fixed socket file name
     * inside it.
     *
     * This exists because of bind-mount inode semantics: bind() can only
     * create a NEW inode, so every daemon restart changes the socket's
     * inode while a bind mount pins an inode.  Mounting a single socket
     * file means every restart breaks the container side with
     * ECONNREFUSED until it is remounted.
     *
     * Mounting the DIRECTORY has no such problem: the directory inode is
     * stable and replacing the socket inside it does not affect the mount.
     * The consumer only ever needs:
     *   Termux $TMPDIR/termux-va/  ==  container /tmp/termux-va/
     * and the daemon may restart as often as it likes. */
    char sock_in_dir[512];
    char sock_trimmed[512];
    {
        /* trim trailing slashes first, otherwise the joined path becomes
         * dir//termux-va.sock - functional, but the doubled slash makes
         * log lines fail to match what docs and scripts expect. */
        size_t sl = strlen(sock_path);
        while (sl > 1 && sock_path[sl - 1] == '/') sl--;
        if (sl >= sizeof(sock_trimmed)) {
            fprintf(stderr, "socket path too long\n");
            return 1;
        }
        memcpy(sock_trimmed, sock_path, sl);
        sock_trimmed[sl] = '\0';
        sock_path = sock_trimmed;

        /* stat exactly once and remember the result - do not re-stat or
         * read errno in the branches: errno is undefined after a successful
         * stat, so branching on it is fragile. */
        struct stat dst;
        int st_ok = (stat(sock_path, &dst) == 0);

        if (st_ok && S_ISDIR(dst.st_mode)) {
            snprintf(sock_in_dir, sizeof(sock_in_dir), "%s/%s",
                     sock_path, TVA_SOCK_NAME);
            dlog(1, "--sock points at a directory, listening on %s", sock_in_dir);
            sock_path = sock_in_dir;
        } else if (!st_ok) {
            /* The path does not exist.  Blindly binding it as a socket file
             * would SILENTLY create a single-file socket, which breaks on
             * every restart when something bind-mounts the file (inode
             * changes).  So: a path that looks like a directory (no ".sock"
             * suffix) is created as a directory; a .sock path is kept in
             * single-file mode with a warning. */
            int wants_dir = (strstr(sock_path, ".sock") == NULL);
            if (wants_dir) {
                if (mkdir(sock_path, 0755) < 0 && errno != EEXIST) {
                    fprintf(stderr,
                            "cannot create directory %s: %s\n"
                            "(if a plain socket file was intended, make the\n"
                            " path end with .sock)\n",
                            sock_path, strerror(errno));
                    return 1;
                }
                snprintf(sock_in_dir, sizeof(sock_in_dir), "%s/%s",
                         sock_path, TVA_SOCK_NAME);
                dlog(1, "socket directory did not exist, created; listening on %s",
                     sock_in_dir);
                sock_path = sock_in_dir;
            } else {
                /* TVA: create missing parent directories so an env-provided
                 * .sock path in a fresh directory works on first start. */
                if (ensure_parent_dirs(sock_path) < 0) {
                    fprintf(stderr, "cannot create parent directories of %s: %s\n",
                            sock_path, strerror(errno));
                    return 1;
                }
                dlog(1, "WARNING: %s is a single socket file - its inode changes on"
                        " every restart; if anything bind-mounts the file, remount"
                        " after each restart.  Prefer passing a directory.",
                     sock_path);
            }
        }
    }

    /* Unix socket listener: path-based, not part of any net namespace,
     * shared with containers through the shared tmp directory. */
    srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    struct sockaddr_un ua;
    memset(&ua, 0, sizeof(ua));
    ua.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(ua.sun_path)) {
        fprintf(stderr, "socket path too long (limit %zu): %s\n",
                sizeof(ua.sun_path) - 1, sock_path);
        close(srv); return 1;
    }
    strncpy(ua.sun_path, sock_path, sizeof(ua.sun_path) - 1);

    /* Handling of a leftover socket file - read the deployment-order note.
     *
     * bind() can only create a NEW inode and cannot bind onto an existing
     * file.  A bind mount pins an inode, not a path: once the socket is
     * unlinked and re-created, the container's mount point refers to an
     * orphan inode and connect() returns ECONNREFUSED - while both sides
     * may even stat the SAME orphan inode, which looks identical and is
     * miserable to debug.
     *
     * Correct deployment order:
     *   1. the daemon starts first and creates the socket file
     *   2. the platform bind-mounts the DIRECTORY into the container
     *   3. after a daemon restart, only the socket file inside changes
     *      (directory mounts do not care)
     *
     * A leftover is only removed once no live listener is confirmed. */
    /* Single-instance check uses flock, NOT a connect() probe.
     *
     * An early version probed with connect("if it connects, someone is
     * serving").  Two problems:
     *   1. a blocking socket with an unbounded connect - a full backlog on
     *      the old instance hangs the new process at startup; worse, if the
     *      kernel returned EAGAIN it would be misread as "no listener" and
     *      the LIVE instance's socket file would be unlinked.
     *   2. side effects on the live instance: every rejected start made it
     *      run accept + thread create + teardown and briefly consume a
     *      concurrency slot.
     *
     * flock has none of these: it never touches the other process, needs no
     * timeout, and the kernel releases the lock no matter how the holder
     * dies (including SIGKILL), so no stale "occupied" state survives.  The
     * lock file lives next to the socket with its own name and the handle
     * is deliberately NEVER closed - holding it is what holds the lock for
     * the process lifetime. */
    char lockpath[560];
    snprintf(lockpath, sizeof(lockpath), "%s.lock", sock_path);
    int lockfd = open(lockpath, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lockfd < 0) {
        fprintf(stderr, "cannot open lock file %s: %s\n",
                lockpath, strerror(errno));
        close(srv);
        return 1;
    }
    if (flock(lockfd, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK)
            fprintf(stderr, "another instance is already serving %s, refusing to start\n", sock_path);
        else
            fprintf(stderr, "flock %s failed: %s\n",
                    lockpath, strerror(errno));
        close(lockfd);
        close(srv);
        return 1;
    }
    /* Lock acquired: no live instance.  lockfd stays open on purpose. */

    struct stat st;
    if (stat(sock_path, &st) == 0 && S_ISSOCK(st.st_mode)) {
        /* holding the lock and seeing a socket file = leftover from a dead
         * instance, safe to remove.
         * NOTE: this changes the inode; existing single-file bind mounts
         * must be remounted (directory mounts are unaffected). */
        fprintf(stderr, "removing stale socket file %s"
                        " (inode will change; single-file bind mounts need a remount)\n",
                sock_path);
        unlink(sock_path);
    }

    if (bind(srv, (struct sockaddr *)&ua, sizeof(ua)) < 0) {
        fprintf(stderr, "bind %s failed: %s\n", sock_path, strerror(errno));
        if (errno == EACCES)
            fprintf(stderr, "  hint: check the SELinux context of the parent directory; "
                            "Termux app data directories are writable by the app itself\n");
        close(srv); return 1;
    }
    /* 0666 is deliberately permissive: PRoot containers share the Termux uid
     * and need nothing special, but chroot/LXC consumers may run as root and
     * must be able to connect.  A real deployment can tighten this to a
     * specific gid (mirroring how anland-termux lets its container scripts
     * chmod the directory/socket). */
    if (chmod(sock_path, 0666) < 0)
        dlog(1, "chmod %s warning: %s", sock_path, strerror(errno));

    if (listen(srv, MAX_CLIENTS) < 0) {
        fprintf(stderr, "listen failed: %s\n", strerror(errno));
        close(srv); unlink(sock_path); return 1;
    }
    /* Startup-success marker for external scripts; keep the format stable. */
    fprintf(stderr, "listening on %s\n", sock_path);
    /* Capture the endpoint identity for the handshake; TEST-ONLY hooks
     * apply here too.  This is the "server tells the truth" half of the
     * inode verification mechanism - the client reconciles against it. */
    endpoint_probe(sock_path);
    fprintf(stderr, "listening endpoint: dev=%llu ino=%llu\n",
            (unsigned long long)g_ep_dev, (unsigned long long)g_ep_ino);
    fflush(stderr);

    int next_id = 1;
    while (running) {
        /* select() over the listener and the self-pipe so SIGTERM wakes the
         * loop promptly */
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(srv, &rs);
        FD_SET(wakefd[0], &rs);
        int mx = srv > wakefd[0] ? srv : wakefd[0];

        int sel = select(mx + 1, &rs, NULL, NULL, NULL);
        if (sel < 0) {
            if (errno == EINTR) continue;
            perror("select"); break;
        }
        if (FD_ISSET(wakefd[0], &rs)) break;          /* exit signal */
        if (!FD_ISSET(srv, &rs)) continue;

        int cli = accept(srv, NULL, NULL);
        if (cli < 0) {
            /* almost all accept() errors affect only this one connection and
             * must not take the daemon down.  Linux surfaces pending network
             * errors on the new connection through accept and the man page
             * asks to retry them like EAGAIN.  Only real process-level
             * failures (fd exhaustion) justify exiting. */
            switch (errno) {
            case EINTR:
            case ECONNABORTED:
            case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
            case EWOULDBLOCK:
#endif
            case EMSGSIZE:
            case EPROTO:
            case ENOPROTOOPT:
            case EHOSTDOWN:
            case EHOSTUNREACH:
            case ENETDOWN:
            case ENETUNREACH:
            case ENONET:
            case EOPNOTSUPP:
            case ETIMEDOUT:
                dlog(1, "accept skipped one connection: %s", strerror(errno));
                continue;
            default:
                /* EMFILE/ENFILE/ENOBUFS/ENOMEM may be transient too, but
                 * retrying in a loop becomes a busy-wait; restarting from
                 * outside is cleaner. */
                perror("accept");
                goto accept_loop_done;
            }
        }

        pthread_mutex_lock(&count_lock);
        int accepted = (client_count < MAX_CLIENTS);
        if (accepted) client_count++;
        pthread_mutex_unlock(&count_lock);

        if (!accepted) {
            dlog(1, "concurrent session limit %d reached, rejecting new connection", MAX_CLIENTS);
            close(cli);
            continue;
        }

        /* The return buffer MUST be enlarged explicitly or AF_UNIX throughput
         * collapses below realtime decode speed.
         *
         * Upstream measurement (same binary, same hardware, 720p30 HEVC):
         * with the default AF_UNIX SO_SNDBUF/SO_RCVBUF of 229376 (224KB),
         * a single NV12 frame (720p = 1.38MB, 1080p = 3.11MB) does not fit,
         * so every frame filled the buffer over and over, blocking the
         * output thread -> MediaCodec output buffers not recycled -> input
         * slots exhausted -> "input buffers full" -> session abandoned
         * (0.92x realtime).  4MB holds a full 1080p NV12 frame with
         * headroom; the kernel doubles the SO_SNDBUF bookkeeping and caps
         * at net.core.{w,r}mem_max, so a failed setsockopt degrades to the
         * default and is intentionally ignored. */
        {
            int bufsz = 4 * 1024 * 1024;
            (void)setsockopt(cli, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
            (void)setsockopt(cli, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
        }

        Session *s = calloc(1, sizeof(Session));
        if (!s) {
            dlog(0, "session allocation failed");
            close(cli);
            client_release();
            continue;
        }
        s->fd       = cli;
        s->w        = 1920;   /* placeholder until the handshake / FORMAT_CHANGED */
        s->h        = 1080;
        s->vcl_in   = 1;      /* PTS tags start at 1: 0 is the "no PTS" sentinel */
        s->id       = next_id++;
        s->codec_id = CODEC_H264;
        s->mime     = codec_mime(CODEC_H264);
        s->xfer     = XFER_INLINE;
        s->shm_fd     = -1;   /* calloc zeroes; 0 is a legal fd, so set -1 here */
        s->shm_listen = -1;

        pthread_t th;
        if (pthread_create(&th, NULL, session_thread, s) != 0) {
            dlog(0, "cannot create session thread");
            close(cli);
            free(s);
            client_release();
            continue;
        }
        pthread_detach(th);
        dlog(1, "[%d] client connected", s->id);
    }
accept_loop_done:

    close(srv);
    /* Remove the socket file or the next start hits EADDRINUSE.  (Startup
     * also cleans a stale file - belt and braces for signal kills, which
     * never reach this point.) */
    unlink(sock_path);

    /* Let running sessions wind down so decoder resources are not yanked */
    for (int i = 0; i < 50; i++) {
        pthread_mutex_lock(&count_lock);
        int n = client_count;
        pthread_mutex_unlock(&count_lock);
        if (n == 0) break;
        usleep(100000);
    }

    dlog(1, "termux-va exiting");
    return 0;
}
