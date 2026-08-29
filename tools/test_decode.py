#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Minimal protocol regression client for the termux-va daemon.
#
# Copyright (C) 2026 lfdevs.  This program is free software: you can
# redistribute it and/or modify it under the terms of the GNU General Public
# License as published by the Free Software Foundation, version 3 of the
# License.
#
# ******************************************************************************
# MODIFICATION NOTICE (GPL-3.0 section 5)
#
# This file is a MODIFIED version of tools/test_decode.py from the
# droidspaces-media-decode project (Apache License, Version 2.0), ported to
# termux-va and relicensed under GPL-3.0.  Modifications:
#   - the transport is a path-based Unix socket (upstream spoke TCP); the
#     endpoint argument is optional and defaults to the daemon's resolution
#     order (TERMUX_VA_SOCKET > TERMUX_VA_SOCKET_DIR >
#     $TMPDIR/termux-va/termux-va.sock),
#   - the handshake speaks protocol v3 and reconciles the endpoint dev/ino
#     extension against stat() of the socket path,
#   - two upstream parser staleness bugs fixed: the CAP_FRAME_PTS capability
#     of the format block is honored (frame headers carry a 4th word), and
#     SHM control messages are consumed as 24 bytes (6 words) instead of 20,
#   - the "inline" transport spelling replaces "tcp" ("tcp" is still
#     accepted as an alias); comments and messages translated to English.
# ******************************************************************************
"""
Minimal reference implementation - verifies the termux-va wire protocol (v3).

No ffmpeg dependency; it only splits the input, speaks the protocol, and
reports statistics.  For production use see the Mesa bridge (container side)
or the upstream client/ reference implementation.

Usage:
    python3 test_decode.py [endpoint] <file> [codec] [inline|shm]

    endpoint: Unix socket path; omitted = auto-resolve like the daemon
              (TERMUX_VA_SOCKET > TERMUX_VA_SOCKET_DIR >
              $TMPDIR/termux-va/termux-va.sock)
    codec:    h264 (default) | hevc | vp9 | vp8
              h264/hevc  read a raw Annex B stream, split at start codes into
                         NALUs (start codes are REQUIRED)
              vp9/vp8    read an IVF container; each packet is a whole frame
                         (must NOT carry start codes)
    inline (default)  frame data comes back over the socket
    shm               frame data goes through a memfd slot pool

Examples:
    python3 test_decode.py test1080.h264
    python3 test_decode.py /tmp/termux-va/termux-va.sock test720.vp9.ivf vp9
    python3 test_decode.py test1080.h264 h264 shm
"""
import array
import mmap
import os
import socket
import struct
import select
import stat
import sys

HELLO_MAGIC = 0x444D4400
HELLO_VERSION = 3
TVA_VERSION_MIN = 2
FMTDESC_SENTINEL = 0xFFFFFFFF
SHMFRAME_SENTINEL = 0xFFFFFFFE
FMTDESC_BYTES = 32
CAP_FRAME_PTS = 0x00000001
SHM_CTRL_BYTES = 4096

XFER_INLINE = 0
XFER_SHM = 1

CODEC_IDS = {"h264": 0, "hevc": 1, "vp9": 2, "vp8": 3}

# Handshake rejection status codes
REJECT_REASONS = {
    1: "protocol version not supported",
    2: "codec not supported",
    3: "resolution out of hardware range",
    4: "handshake missing",
}


def resolve_endpoint():
    """Mirror the daemon's endpoint resolution order (daemon/termux-va.c)."""
    ep = os.environ.get("TERMUX_VA_SOCKET")
    if ep:
        return ep
    d = os.environ.get("TERMUX_VA_SOCKET_DIR")
    if d:
        return os.path.join(d, "termux-va.sock")
    tmp = os.environ.get("TMPDIR")
    if not tmp or tmp == "/data/local/tmp":
        tmp = "/tmp" if os.path.isdir("/tmp") else "/data/data/com.termux/files/usr/tmp"
    return os.path.join(tmp, "termux-va", "termux-va.sock")


def split_annexb(data):
    """Split at Annex B start codes, returning NALUs WITH their start codes.

    The daemon locates the nal_unit_header via the start code to identify
    parameter sets, so they must be preserved.
    """
    starts = []
    i = 0
    n = len(data)
    while i < n - 3:
        if data[i] == 0 and data[i + 1] == 0:
            if data[i + 2] == 1:
                starts.append(i)
                i += 3
                continue
            if i + 3 < n and data[i + 2] == 0 and data[i + 3] == 1:
                starts.append(i)
                i += 4
                continue
        i += 1
    return [data[starts[k]:(starts[k + 1] if k + 1 < len(starts) else n)]
            for k in range(len(starts))]


def split_ivf(data):
    """Parse an IVF container, returning frames (VP8/VP9 carry no start codes)."""
    if data[:4] != b"DKIF":
        raise SystemExit("not an IVF file (VP8/VP9 need an IVF container)")
    hdr_len = struct.unpack("<H", data[6:8])[0]
    frames = []
    pos = hdr_len
    while pos + 12 <= len(data):
        size = struct.unpack("<I", data[pos:pos + 4])[0]
        pos += 12                      # 4B length + 8B timestamp
        if pos + size > len(data):
            break
        frames.append(data[pos:pos + size])
        pos += size
    return frames


def guess_size(units, codec):
    """Roughly extract the resolution for the handshake declaration.

    The daemon returns the real size on FORMAT_CHANGED, so being wrong here
    does not affect correctness.  The IVF header carries the exact size; for
    Annex B we would have to parse the SPS - not worth it, fall back to 1080p.
    """
    if codec in ("vp9", "vp8"):
        return None                    # caller reads the IVF header
    return (1920, 1080)


def shm_attach(name):
    """Connect to the daemon's abstract socket, receive the memfd via
    SCM_RIGHTS, and mmap it.

    The name is chosen by the daemon and delivered in the handshake
    response - the client cannot know which connection number it is, and
    guessing would collide.  Abstract sockets belong to the network
    namespace, which the container and Termux share; path-based Unix
    sockets do not cross the mount namespace.
    """
    c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    c.settimeout(10)
    c.connect("\0" + name)                 # leading NUL = abstract namespace
    msg, anc, _flags, _addr = c.recvmsg(12, socket.CMSG_LEN(4))
    fds = array.array("i")
    for level, typ, data in anc:
        if level == socket.SOL_SOCKET and typ == socket.SCM_RIGHTS:
            fds.frombytes(data[:len(data) - (len(data) % 4)])
    c.close()
    if not fds:
        raise SystemExit("no memfd received")
    slots, slot_size, total = struct.unpack(">III", msg)
    mm = mmap.mmap(fds[0], total, mmap.MAP_SHARED,
                   mmap.PROT_READ | mmap.PROT_WRITE)
    os.close(fds[0])                       # fd no longer needed after mmap
    return mm, slots, slot_size


class Receiver:
    """Parses the daemon's downlink: sentinel frame headers introduce control
    messages, anything else is frame data."""

    def __init__(self, sock, shm=None, slots=0, slot_size=0):
        self.sock = sock
        self.buf = bytearray()
        self.frames = 0
        self.fmt = None
        self.fmt_caps = 0
        self.fmt_changes = 0
        self.last_dims = None
        self.shm = shm
        self.slots = slots
        self.slot_size = slot_size
        self.first_bytes = None

    def _release(self, slot):
        """Return a slot.  Not returning it makes the daemon wait out its
        slot timeout and judge the client stuck."""
        off = slot * 4
        self.shm[off:off + 4] = struct.pack("<I", 0)

    def _parse(self):
        while len(self.buf) >= 12:
            w, h, size = struct.unpack(">III", self.buf[:12])

            if size == SHMFRAME_SENTINEL:
                # 24-byte SHM control message:
                # [w][h][sentinel][slot][length][unit index]
                if len(self.buf) < 24:
                    return
                slot, dlen = struct.unpack(">II", bytes(self.buf[12:20]))
                del self.buf[:24]
                if self.shm is None or not (0 <= slot < self.slots):
                    raise SystemExit("illegal SHM slot: %d" % slot)
                base = SHM_CTRL_BYTES + slot * self.slot_size
                if self.first_bytes is None:
                    self.first_bytes = bytes(self.shm[base:base + 32])
                self.frames += 1
                self.last_dims = (w, h)
                self._release(slot)
                continue

            if size == FMTDESC_SENTINEL:
                if len(self.buf) < 12 + FMTDESC_BYTES:
                    return
                hdr2 = struct.unpack(">I", bytes(self.buf[4:8]))[0]
                body = struct.unpack(">8I", bytes(self.buf[12:12 + FMTDESC_BYTES]))
                del self.buf[:12 + FMTDESC_BYTES]
                prev = self.fmt
                self.fmt = body
                self.fmt_caps = hdr2
                self.fmt_changes += 1
                if prev and (prev[0], prev[1]) != (body[0], body[1]):
                    print("  mid-stream resolution change: %dx%d -> %dx%d"
                          % (prev[0], prev[1], body[0], body[1]))
                continue

            if not (0 < size <= 8 * 1024 * 1024):
                raise SystemExit("abnormal frame size: %d (protocol desync)" % size)
            # Inline frame header: [w][h][size] + a 4th word (unit index)
            # when the format block announced CAP_FRAME_PTS.
            hdr_len = 16 if (self.fmt_caps & CAP_FRAME_PTS) else 12
            if len(self.buf) < hdr_len + size:
                return
            del self.buf[:hdr_len + size]
            self.frames += 1
            self.last_dims = (w, h)

    def pump(self, block=False, timeout=5.0):
        """Read whatever is available.  block=True waits until the connection
        closes."""
        while True:
            if not block:
                ready, _, _ = select.select([self.sock], [], [], 0)
                if not ready:
                    return True
            else:
                ready, _, _ = select.select([self.sock], [], [], timeout)
                if not ready:
                    return False
            chunk = self.sock.recv(1 << 22)
            if not chunk:
                return False
            self.buf += chunk
            self._parse()
            if not block:
                return True


def main():
    args = [a for a in sys.argv[1:]]
    # The endpoint is optional and must look like a socket path; a bare
    # file argument that is a codec name means the endpoint was omitted.
    endpoint = None
    if args and args[0] not in CODEC_IDS and args[0] not in ("inline", "shm", "tcp"):
        endpoint = args.pop(0)

    if len(args) < 1:
        print(__doc__)
        return 1
    path = args[0]
    codec = args[1].lower() if len(args) > 1 else "h264"
    xfer_arg = args[2].lower() if len(args) > 2 else "inline"

    if codec not in CODEC_IDS:
        print("unknown codec: %s (supported: %s)" % (codec, "/".join(CODEC_IDS)))
        return 1
    if xfer_arg == "tcp":
        xfer_arg = "inline"            # legacy alias from the upstream script
    if xfer_arg not in ("inline", "shm"):
        print("unknown transport: %s (supported: inline/shm)" % xfer_arg)
        return 1
    want_xfer = XFER_SHM if xfer_arg == "shm" else XFER_INLINE

    if endpoint is None:
        endpoint = resolve_endpoint()
    if not os.path.exists(endpoint):
        print("endpoint does not exist: %s (is termux-va running?)" % endpoint)
        return 1

    data = open(path, "rb").read()

    if codec in ("vp9", "vp8"):
        units = split_ivf(data)
        width, height = struct.unpack("<HH", data[12:16])
        annexb = False
    else:
        units = split_annexb(data)
        width, height = guess_size(units, codec)
        annexb = True

    if not units:
        print("no data units parsed")
        return 1

    print("input: %s" % path)
    print("  codec %s, declared size %dx%d, %d %s"
          % (codec.upper(), width, height, len(units),
             "NALUs" if annexb else "frames"))

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    # The return buffer must be enlarged on the daemon side already; on the
    # client the default receive buffer only throttles, never breaks.
    sock.settimeout(15)
    sock.connect(endpoint)

    def read_exact(n):
        got = b""
        while len(got) < n:
            chunk = sock.recv(n - len(got))
            if not chunk:
                raise SystemExit("handshake response interrupted")
            got += chunk
        return got

    # The handshake is required: the daemon derives MIME, the initial size
    # and the transport mode from it.
    sock.sendall(struct.pack(">IIIIII", HELLO_MAGIC, HELLO_VERSION,
                             CODEC_IDS[codec], width, height, want_xfer))

    # The response is variable-length: [status][actual mode][name length(+ext flag)]
    status, mode, nlen_w = struct.unpack(">III", read_exact(12))

    if status != 0:
        print("handshake rejected: %s (status=%d)"
              % (REJECT_REASONS.get(status, "unknown reason"), status))
        return 1

    # v3 endpoint extension: reconcile against stat() of the path we used.
    st = os.stat(endpoint)
    if nlen_w & 0x80000000:
        ext = struct.unpack(">IIII", read_exact(16))
        ep_dev = (ext[0] << 32) | ext[1]
        ep_ino = (ext[2] << 32) | ext[3]
        if ep_dev != st.st_dev or ep_ino != st.st_ino:
            print("endpoint inode mismatch: stat(dev=%d,ino=%d) != daemon(dev=%d,ino=%d)"
                  % (st.st_dev, st.st_ino, ep_dev, ep_ino))
            print("the path does not lead to the endpoint that answered - "
                  "check mounts/single-file bind mounts")
            return 1
    nlen = nlen_w & 0x7FFFFFFF
    shm_name = read_exact(nlen).decode() if nlen else ""

    # The daemon may downgrade SHM; trust the granted mode, not the request.
    shm = None
    slots = slot_size = 0
    if mode == XFER_SHM:
        shm, slots, slot_size = shm_attach(shm_name)
        print("  transport SHM: %d slots x %d bytes (%s)" % (slots, slot_size, shm_name))
    else:
        print("  transport inline%s"
              % (" (SHM requested but downgraded)" if want_xfer == XFER_SHM else ""))

    rx = Receiver(sock, shm, slots, slot_size)

    # Send and receive interleaved: the daemon handles one session
    # serially, and send-only would block its writes on a full socket
    # buffer, deadlocking both sides and losing every decoded frame.
    for unit in units:
        sock.sendall(struct.pack(">I", len(unit)) + unit)
        if not rx.pump():
            break

    # Closing the write end triggers the flush; without it the frames still
    # queued inside the decoder never come out.
    sock.shutdown(socket.SHUT_WR)
    while rx.pump(block=True):
        pass
    sock.close()

    if rx.fmt:
        bw, bh, stride, slice_h, cl, ct, cr, cb = rx.fmt
        print("  final format: buffer %dx%d, stride=%d, slice_height=%d" % (bw, bh, stride, slice_h))
        print("                display area %dx%d (crop %d,%d - %d,%d)"
              % (cr - cl + 1, cb - ct + 1, cl, ct, cr, cb))
        if rx.fmt_changes > 1:
            print("  format changes: %d" % rx.fmt_changes)

    if shm is not None:
        shm.close()

    print("RESULT: %d frames decoded from %s" % (rx.frames, path))
    return 0 if rx.frames > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
