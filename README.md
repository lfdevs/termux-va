# Termux:VA

MediaCodec hardware video decoding for Linux containers, served from Termux.

`termux-va` is a Termux port of [DroidSpaces Media Decode Daemon](https://github.com/Re-s/droidspaces-media-decode): a small C daemon that receives H.264/HEVC/VP8/VP9 bitstreams over a Unix socket, decodes them with the Android MediaCodec API in hardware, and returns NV12 frames (inline, or zero-copy through a memfd slot pool). Applications inside a Linux container that shares Termux's tmp directory use it through the standard VA-API, without any modification: ffmpeg, Firefox and Chrome all work.

> 中文文档：[README_zh.md](README_zh.md)

## How it fits together

The porting model is the one established by [anland-termux](https://github.com/lfdevs/anland-termux): a Termux daemon, a Unix socket placed in the shared tmp directory, and a bridge on the container side.

```
   Linux container (proot --shared-tmp)                 Termux
+---------------------------------------------+   +---------------------------+
|  ffmpeg / Firefox / Chrome / vainfo          |   |  termux-va (NDK r29, C11) |
|    libva -> Mesa VA frontend (bridge mode)   |   |  MediaCodec hw decode     |
|      -> DMD v3 protocol over Unix socket ----+---+-> NV12 frames             |
|         (inline or memfd + SCM_RIGHTS)       |   |                           |
+---------------------------------------------+   +---------------------------+
        socket: /tmp/termux-va/termux-va.sock == Termux $TMPDIR/termux-va/
```

- **Daemon (this repository)**: `daemon/termux-va.c`, a faithful port of upstream `decode-daemon.c` with the TCP transport removed - it listens on a path-based Unix socket only.  Built with NDK **29.0.14206865** (the same version anland-termux pins), API 29, arm64-v8a.
- **Mesa bridge (container side)**: lives in [mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container), branch `test/add-va-bridge` (`src/gallium/frontends/va/tva_*`).  The upstream standalone pseudo VA-API driver is retired; the bridge speaks the same wire protocol from inside Mesa's VA frontend.  The default socket it probes is `/tmp/termux-va/termux-va.sock`.

## Endpoint resolution

| Priority | Source | Meaning |
|---|---|---|
| 1 | `--sock <path\|dir>` | CLI override (a directory receives `termux-va.sock`) |
| 2 | `TERMUX_VA_SOCKET` | full socket file path (both ends) |
| 3 | `TERMUX_VA_SOCKET_DIR` | directory; `termux-va.sock` is appended (both ends) |
| 4 | default | `$TMPDIR/termux-va/termux-va.sock` |

`$TMPDIR` falls back to `/tmp`, then `/data/data/com.termux/files/usr/tmp`; the adb-injected `/data/local/tmp` is treated as unset.  With `proot-distro --shared-tmp` the Termux default appears as `/tmp/termux-va/termux-va.sock` inside the container - one path, both worlds.

## Build

```sh
# On a host with the Android NDK (canonical release build):
ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/29.0.14206865 bash scripts/build-ndk.sh

# Inside Termux (development convenience):
pkg install clang ndk-multilib
make -C daemon
```

## Run

```sh
# In Termux:
termux-va-start.sh                 # or: termux-va [-v|-q] [--sock <path|dir>]

# Enter the container with the shared tmp:
proot-distro login debian --shared-tmp

# In the container (Mesa from mesa-for-android-container test/add-va-bridge):
export LIBVA_DRIVER_NAME=termuxva
export TERMUX_VA_BRIDGE=1
export TERMUX_VA_GPU_BACKEND=kgsl  # KGSL Freedreno path
vainfo
ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi -i in.mp4 -f null -
```

Set `TERMUX_VA_GPU_BACKEND=sw` instead to force Mesa's llvmpipe surface and frame-copy path; this does not disable MediaCodec decoding in the Termux daemon. The bridge currently advertises H.264 and VP9 Profile 0. The daemon also accepts HEVC and VP8, but those codecs are not yet advertised by the Mesa bridge.

See [doc/deploy.md](doc/deploy.md) for the full deployment manual and [doc/protocol.md](doc/protocol.md) for the wire protocol.

## Repository layout

```
common/tva_protocol.h   Wire protocol constants (mirrored into the Mesa bridge)
daemon/termux-va.c      The daemon
daemon/Makefile         On-device build for Termux
scripts/                NDK build, lifecycle and watchdog scripts, mirror check
tools/                  Health probe (tva-probe.c), regression client (test_decode.py)
packages/termux-va/     termux-packages recipe
doc/                    Protocol and deployment documentation (English + Chinese)
```

## License

This project is licensed under **GPL-3.0** (see [LICENSE](LICENSE)).  It is based on [DroidSpaces Media Decode Daemon](https://github.com/Re-s/droidspaces-media-decode), which is licensed under the Apache License 2.0; every file derived from it carries a prominent modification notice as required by GPL-3.0 section 5.
