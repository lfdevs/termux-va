# termux-va Wire Protocol (DMD v3)

**English** | [中文](protocol_zh.md)

---

The wire format is byte-compatible with droidspaces-media-decode protocol v3 (`HELLO_MAGIC 0x444D4400`), so the upstream regression tools work unchanged.  The single source of truth for the constants is [`common/tva_protocol.h`](../common/tva_protocol.h), mirrored into the Mesa bridge (`src/gallium/frontends/va/tva_protocol.h`) and verified with `scripts/check-mirror.sh`.

## Transport

Exactly one control channel per session: a path-based Unix socket (`$TMPDIR/termux-va/termux-va.sock` by default).  Byte order on the wire is big-endian throughout.  TCP was removed in termux-va; the transport identifier values 0 (inline) and 1 (SHM) are kept for protocol compatibility.

## Handshake (required)

The client sends 24 bytes before anything else:

```
[u32 magic = 0x444D4400][u32 version][u32 codec][u32 width][u32 height][u32 xfer]
```

- `version`: protocol version, client-declared.  The daemon accepts `2..3` and takes the minimum.  Version semantics: v2 added SHM negotiation, v3 added the endpoint extension in the response.
- `codec`: `0=AVC (video/avc), 1=HEVC (video/hevc), 2=VP9, 3=VP8, 4=AV1` (AV1 accepted by the daemon, never requested by the bridge).
- `width/height`: initial resolution; valid range 96x96..8192x4320.
- `xfer`: requested frame-return transport, `0=inline`, `1=SHM`.

The daemon responds with a variable-length message:

```
[u32 status][u32 actual_xfer][u32 namelen]
[if v3 and status==0: 16B endpoint extension]
[if namelen>0: namelen bytes of the abstract socket name]
```

- `status`: `0` accepted, `1` version, `2` codec, `3` resolution out of range, `4` handshake missing.  Error responses are always a bare 12 bytes.
- `actual_xfer`: what the daemon granted - `0` inline or `1` SHM.  The client must honor this, not its request.
- The v3 extension is marked by bit 31 of the `namelen` word (real name lengths are far below 2^31); the extension is `[u32 dev_hi][u32 dev_lo][u32 ino_hi][u32 ino_lo]` - the daemon's `stat()` of the listening socket.  The client `stat()`s the path it connected to and reconciles: a mismatch means the path does not lead to the endpoint that answered (classic single-file bind-mount staleness).

## Format descriptor block

Before the first frame (and again after every output-format change), the daemon sends:

```
[u32 0][u32 caps][u32 0xFFFFFFFF]        sentinel header
[u32 buf_w][u32 buf_h][u32 stride][u32 slice_height]
[u32 crop_l][u32 crop_t][u32 crop_r][u32 crop_b]   32-byte body
```

`caps` bit 0 (`CAP_FRAME_PTS`): every following frame header carries a 4th word - the input unit index.  The buffer geometry reflects the decoder's real output (Qualcomm Venus aligns width to 128 / height to 32); the crop rectangle is the visible area.

## Uplink units

```
[u32 length][data]
```

Exactly one unit per length prefix:

- AVC / HEVC: a single Annex B NALU **with** its start code (3 or 4 bytes).  SPS/PPS (AVC type 7/8, HEVC type 32/33/34) accumulate into the CSD and are submitted with `FLAG_CODEC_CONFIG`; they produce no frames.
- VP8 / VP9: one whole frame, **without** start codes.
- `length == 0`: reversible drain request (see below).
- `length > 8MB (MAX_FRAME)`: protocol violation, session ends.

The daemon tags every submitted VCL unit with `presentationTimeUs = unit_index * 1000` (the x1000 survives the decoder's millisecond PTS quantization); the index comes back on the matching output frame, which lets the consumer pair surfaces without knowing the decoder's output order.

### Reversible drain (`length == 0`)

Queues an empty buffer flagged EOS; when the decoder emits EOS the daemon calls `AMediaCodec_flush`, re-sends the CSD, and the session continues without a reconnect.  The reference chain is destroyed by the flush, so frames after a drain stay black until the next IDR - the bridge triggers a drain only when waiting is provably futile.

## Downlink frames

Two transports, chosen by the daemon during the handshake:

### Inline

```
[u32 w][u32 h][u32 size][u32 unit_index]    frame header (CAP_FRAME_PTS on)
[NV12 data, size bytes]
```

### SHM (zero-copy to the consumer)

Frame data lives in a memfd slot pool; the socket carries a 24-byte control message only:

```
[u32 w][u32 h][u32 0xFFFFFFFE][u32 slot][u32 length][u32 unit_index]
```

The memfd is handed over right after the handshake response: the daemon listens on an abstract socket `dmd-shm-<pid>-<session>-<8 hex random>`, whose name is delivered in the handshake response (that is what `namelen` carries in inline mode: always 0).  The client connects and receives one `SCM_RIGHTS` message carrying the memfd plus `[u32 slots][u32 slot_bytes][u32 total_bytes]` (big-endian).

Pool layout:

```
[control area 4096 bytes][slot 0][slot 1] ... [slot SHM_SLOTS-1=7]
```

Each slot has a u32 state word in the control area at offset `slot*4`: the daemon sets it to 1 (release semantics) after writing the frame; the client resets it to 0 after consuming (the slot is then free).  Slot size is `align128(max(w,1920)) * align32(max(h,1088)) * 1.5` with a 64KB floor. `SHM_SLOTS` (8) must stay >= the bridge's pipeline depth (6), and the daemon's slot wait (15s) must stay well above the bridge's frame timeout (5s).

A failed SHM handoff downgrades the session to inline automatically on both sides; there is no hard failure path.

## End of session

The client closes its write end (`shutdown(SHUT_WR)`); the daemon queues an EOS-flagged empty buffer and the output thread drains deterministically. A consumer closing after taking enough frames is normal - the daemon classifies the resulting `EPIPE` as "peer gone", not as an error.

## Constants

| Constant | Value | Defined in |
|---|---|---|
| `HELLO_MAGIC` | `0x444D4400` | `common/tva_protocol.h` |
| `HELLO_VERSION` | 3 | same |
| accepted versions | 2..3 | same |
| `MAX_FRAME` | 8 MiB (uplink unit cap) | same |
| `MAX_CLIENTS` | 8 | same |
| `SHM_SLOTS` | 8 | same |
| `SHM_CTRL_BYTES` | 4096 | same |
| `SHM_SLOT_WAIT_MS` | 15000 | same |
| `CAP_FRAME_PTS` | `0x00000001` | same |
| `FMTDESC_SENTINEL` | `0xFFFFFFFF` | same |
| `SHMFRAME_SENTINEL` | `0xFFFFFFFE` | same |
| `PTS_UNIT_SCALE` | 1000 | same |
| socket buffer size | 4 MiB (`SO_SNDBUF`/`SO_RCVBUF`) | daemon + bridge |
| socket path | `$TMPDIR/termux-va/termux-va.sock` | same |
| socket file name | `termux-va.sock` | same |
| socket dir name | `termux-va` | same |
