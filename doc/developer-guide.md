# Termux:VA Developer Guide

**English** | [中文](developer-guide_zh.md)

---

This guide describes the Termux daemon, its container-side Mesa integration, the build and deployment workflow, and the invariants to preserve when changing the project. The wire format is documented separately in [protocol.md](protocol.md).

## System overview

Termux:VA exposes Android MediaCodec video decode and encode to Linux applications in a container. The design has three parts: a daemon running as the normal Termux app user, a Unix-domain socket in storage shared with the container, and a VA-API bridge integrated into Mesa in the container.

```
 Linux container (PRoot, chroot, or LXC)                  Termux
+------------------------------------------+   shared   +--------------------------+
| ffmpeg / Firefox / Chrome / vainfo       |   tmp dir  | termux-va                |
|   libva -> Mesa VA frontend              |<---------->| Android MediaCodec       |
|     -> termux-va bridge                  | Unix socket| decode / encode          |
|        inline frames or memfd slots      |            | NV12 frame transport    |
+------------------------------------------+            +--------------------------+
```

The container-side bridge is in [mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container), branch `dev/adreno-main`, under `src/gallium/frontends/va/tva_*`. Applications use the normal libva API; the bridge translates VA calls to the DMD v3 socket protocol. Decoder output is linear NV12. Inline mode sends frame bytes through the socket; SHM mode sends frame data through a memfd slot pool and uses the socket for control messages. The Mesa bridge stages received frames and copies them into retained Gallium resources on the application thread.

The Termux daemon is a port of [DroidSpaces Media Decode Daemon](https://github.com/Re-s/droidspaces-media-decode/tree/143610b816da5c612c325a2170dbc5621b5ce1eb) and owns MediaCodec sessions and the shared endpoint. It accepts decoder input units or packed NV12 encoder frames, configures Android MediaCodec, and returns decoded frames or encoded access units. The upstream TCP listener was removed; the daemon uses a path-based Unix socket only. Each client session has separate input and output threads so socket input, MediaCodec submission, output draining, and frame delivery can progress concurrently.

## Codec capabilities

The daemon protocol supports AVC, HEVC, VP8, VP9, and AV1 decoder sessions. The current Mesa VA bridge advertises AVC Constrained Baseline, Main, and High; HEVC Main; VP9 Profile 0; and AV1 Main for decode. The bridge exposes AVC Constrained Baseline, Main, and High; HEVC; and VP9 slice encoding. Device MediaCodec implementations decide which profiles and sizes actually work, so treat `vainfo` and full-stream tests on the target device as the authority.

The Mesa bridge can hide codec profiles with these container-side variables:

| Variable | Effect |
|---|---|
| `TERMUX_VA_DISABLE_AVC=1` or `TERMUX_VA_DISABLE_H264=1` | Hide AVC profiles; the H.264 name is a compatibility alias. |
| `TERMUX_VA_DISABLE_HEVC=1` | Hide HEVC profiles. |
| `TERMUX_VA_DISABLE_VP9=1` | Hide VP9 profiles. |
| `TERMUX_VA_DISABLE_AV1=1` | Hide AV1 profiles. |

The daemon and VA bridge must both contain the requested codec and profile support. In particular, explicit AVC Baseline, Main, and High encoder requests use protocol codec IDs added for those profiles; older daemons reject those IDs rather than silently selecting another profile. The Baseline request maps to Android Constrained Baseline, and the daemon checks the output SPS before returning packets.

## Repository layout

| Path | Purpose |
|---|---|
| `common/tva_protocol.h` | Shared socket protocol constants; mirrored byte-for-byte into the Mesa tree. |
| `daemon/termux-va.c` | MediaCodec daemon, session handling, endpoint resolution, and inline/SHM transport. |
| `daemon/Makefile` | Convenience build and install targets for Termux. |
| `scripts/build-ndk.sh` | Canonical host-side NDK cross-build for the daemon and probe. |
| `scripts/termux-va-start.sh` | Start or reuse a healthy daemon instance. |
| `scripts/termux-va-stop.sh` | Gracefully stop the daemon. |
| `scripts/termux-va-watchdog.sh` | Periodic real-decode health checks and bounded restart logic. |
| `scripts/check-mirror.sh` | Verify the protocol header mirror in Mesa. |
| `tools/tva-probe.c` | Liveness and decode health probe. |
| `tools/test_decode.py` | Protocol regression client for AVC, HEVC, VP8, and VP9 input samples. |
| `tools/test_encode.py` | Protocol encoder regression client, including AVC profile checks. |
| `packages/termux-va/build.sh` | Termux package recipe. |
| `.github/workflows/build-packages.yml` | Manual aarch64 package build and optional draft-release workflow. |
| `doc/protocol.md` and `doc/protocol_zh.md` | Wire-protocol reference. |
| `doc/developer-guide.md` and `doc/developer-guide_zh.md` | English and Chinese development, build, and deployment guides. |

## Endpoint discovery and shared storage

The daemon, start/stop scripts, probe, regression client, and Mesa bridge support the socket path overrides below. The daemon command-line option has the highest priority.

| Priority | Setting | Meaning |
|---|---|---|
| 1 | `termux-va --sock <path-or-directory>` | Use the supplied socket file or append `termux-va.sock` to a directory. |
| 2 | `TERMUX_VA_SOCKET` | Full socket-file path. |
| 3 | `TERMUX_VA_SOCKET_DIR` | Directory; the fixed socket name is appended. |
| 4 | Default | `$TMPDIR/termux-va/termux-va.sock`. |

If `TMPDIR` is empty or equals `/data/local/tmp`, the daemon uses `/tmp` when it exists and otherwise falls back to `/data/data/com.termux/files/usr/tmp`. The probe and test client use the same fallback. On a normal Termux session, `TMPDIR` points to Termux temporary storage.

With PRoot-Distro, enter the container with `proot-distro login <distro> --shared-tmp`. The Termux temporary directory is then visible at `/tmp` in the container, so the default endpoint is `/tmp/termux-va/termux-va.sock` on both sides. For chroot or LXC, bind-mount the Termux temporary **directory** to the container's `/tmp`.

Mount the containing directory, never the socket file itself. The daemon unlinks and recreates its socket on startup, which changes the socket inode; a file bind mount continues to refer to the old inode. The protocol v3 response includes the daemon endpoint's device and inode so clients can detect this stale mount condition. If using a custom path, configure the same path for the daemon and its clients.

The daemon creates its socket directory on startup. It also prepares the parent directory for access from a shared container; no root privileges are required to access Android MediaCodec from the normal Termux app domain.

## Protocol and cross-repository changes

The wire format remains byte-compatible with upstream DMD protocol v3 for decoder handshakes. Termux:VA accepts versions 2 through 3. Encoder bitrate, frame-rate, codec-profile, and raw-frame behavior are project extensions described in [protocol.md](protocol.md).

Treat `common/tva_protocol.h` as the single source of truth. When changing a constant or field that crosses the socket, update `src/gallium/frontends/va/tva_protocol.h` in the Mesa checkout in the same change set, then run:

```sh
MESA_REPO=../mesa-for-android-container bash scripts/check-mirror.sh
```

Preserve these compatibility rules:

- Do not change the meaning, order, or value of existing protocol fields and codec IDs; append new IDs or fields only.
- Keep the path-based Unix socket as the only control transport. Do not restore TCP or add a default port.
- Keep the legacy `DMD_*` environment names used by tools and the bridge.
- The only abstract socket is the per-session `dmd-shm-*` handoff used to transfer the memfd descriptor.
- When SHM transport is enabled, `SHM_SLOTS` (8) must remain at least as large as the Mesa bridge pipeline depth (default 6, configurable with `TERMUX_VA_PIPELINE_DEPTH`).
- Keep `SHM_SLOT_WAIT_MS` (15 seconds) well above the Mesa bridge frame timeout (5 seconds).

The bridge's synchronization and teardown are also protocol-adjacent behavior. Its reader thread receives and stages complete frames; it must not call `pipe_context` methods. The application thread performs the copy into retained Gallium resources from `fence_wait()`. Preserve the lock order `drv->mutex` then `context->mutex`, along with the pending-frame and fence teardown synchronization.

On the tested KGSL path, bridge surfaces are linear NV12 resources backed by shared DMA-BUFs. Freedreno export must populate `winsys_handle.size`; libva forwards this size to PRIME descriptors and downstream consumers may reject a zero-sized object. The bridge enables `FD_KGSL_ENABLE_DMABUF=1` before screen creation.

Turnip on KGSL must not advertise `VK_KHR_external_semaphore_fd` unless the OPAQUE_FD timeline semaphore path and DMA-BUF sync-file behavior have both been validated. FFmpeg requests that path; advertising the extension without it can corrupt imported surfaces. The bridge fence completes the CPU frame copy before export, so the tested path does not require that extension.

## Build

The supported target is Android arm64 on Android 10 or newer. The NDK is pinned to `29.0.14206865`, and the daemon is built with API level 29.

### Canonical NDK cross-build

Install the pinned NDK and build on a Linux host:

```sh
ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/29.0.14206865 bash scripts/build-ndk.sh
```

The script creates `build/termux-va` and `build/tva-probe`. The daemon is an Android aarch64 executable and cannot be run on the Linux build host; it requires Android MediaCodec and the Termux SELinux domain. Do not change the NDK pin without coordinating with the project.

### Build inside Termux

For quick development builds on the device:

```sh
pkg install clang ndk-multilib make
make -C daemon
make -C daemon install
```

The executable is installed as `$PREFIX/bin/termux-va`. The local build is a convenience path; release packages use the Termux package recipe and its pinned Android API level.

### Build the Termux package

The recipe lives at `packages/termux-va/build.sh` and expects the repository checkout to be visible next to the Termux Packages build tree. The project workflow builds only aarch64 and can be started manually from the GitHub Actions page:

- `ref` selects the branch, tag, or commit to build and defaults to `main`.
- `draft-release` requests a draft release. Release creation is skipped unless `ref` resolves to a tag.
- The Actions artifact contains the generated `.deb` and `sha256sums.txt`; a draft release contains only the package and a checksum section in its notes.

For a local package build from the Termux Packages checkout, make the Termux:VA source available at `../termux-va` and visible inside the builder container at `/home/builder/termux-va`, then run:

```sh
TERMUX_DOCKER_RUN_EXTRA_ARGS="--volume $(realpath ../termux-va):/home/builder/termux-va:ro" ./scripts/run-docker.sh ./build-package.sh -I -f -a aarch64 -j 4 ../termux-va/packages/termux-va
```

The extra Docker mount is applied only when `run-docker.sh` creates the builder container; an existing container must already have the source mounted. The build output is placed in the Termux Packages checkout's `output/` directory. The package installs the daemon, probe, and lifecycle scripts under `$PREFIX/bin`. The standalone Python regression clients are source-tree tools and are not included in the package.

## Install and run

### Install the Termux side

A prebuilt package is preferred for normal installation:

```sh
pkg install ./termux-va_<version>_aarch64.deb
```

To replace an existing package with a locally built package, use `pkg reinstall` with that file. The package includes `termux-va`, `tva-probe`, `termux-va-start`, `termux-va-stop`, and `termux-va-watchdog`.

To build and install directly from a checkout in Termux:

```sh
pkg install git clang ndk-multilib make
git clone https://github.com/lfdevs/termux-va
cd termux-va
make -C daemon
make -C daemon install
```

### Keep the daemon running

The service manager is recommended for long-running use because a background process may be terminated with its terminal session, and Termux may clear `TMPDIR` when a service is destroyed.

```sh
pkg install termux-services
mkdir -p $PREFIX/var/service/termux-va
cat > $PREFIX/var/service/termux-va/run <<'EOF'
#!/data/data/com.termux/files/usr/bin/sh
exec termux-va -q 2>&1
EOF
chmod +x $PREFIX/var/service/termux-va/run
sv-enable termux-va
```

Use `sv up termux-va` to start the service immediately when needed. For one-off sessions, run `termux-va-start`; it reuses a healthy instance, starts the daemon otherwise, and writes logs to `$TMPDIR/termux-va/termux-va.log` by default. Run `termux-va-stop` for a graceful shutdown.

The source scripts are named `scripts/termux-va-start.sh`, `scripts/termux-va-stop.sh`, and `scripts/termux-va-watchdog.sh`. The package installs command names without the `.sh` suffix. The daemon itself accepts `-v` for per-frame debug logs, `-q` for errors only, `-t` for timestamps, `--sock <path-or-directory>` for an endpoint override, and `-h` for help.

### Health checks

Run the installed probe in Termux:

```sh
tva-probe
tva-probe /path/to/termux-va.sock 3000
```

The probe connects, negotiates the protocol, submits an embedded 96x96 AVC stream, and requires decoded frames. Exit code 0 means healthy. Exit code 7 means the connected endpoint inode differs from the path's current inode; repair the shared mount rather than restarting the daemon. Exit code 8 means the handshake succeeded but no frame was decoded.

The optional watchdog probes every 5 seconds by default and restarts after five consecutive failures. It warns without restarting on endpoint inode mismatch; after the restart limit it pauses automatic restarts and continues periodic checks. Configure it with `TERMUX_VA_WD_INTERVAL`, `TERMUX_VA_WD_MAX_FAILS`, `TERMUX_VA_WD_COOLDOWN`, and `TERMUX_VA_WD_GRACE`; see the script header for their defaults. Run it from a long-lived Termux session or under a service manager.

## Configure the Linux container

The container must share Termux's temporary directory and have a Mesa build that includes the termux-va bridge. For PRoot-Distro:

```sh
proot-distro login debian --shared-tmp
```

Install the container-side VA-API tools and FFmpeg:

```sh
apt install libva2 vainfo ffmpeg
```

Install the Mesa build from [mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container), branch `dev/adreno-main`, using its matching package or CI archive. Configure the client environment in the container shell or desktop-session launcher:

```sh
export LIBVA_DRIVER_NAME=termuxva
export TERMUX_VA_BRIDGE=1
export TERMUX_VA_GPU_BACKEND=kgsl
export MESA_LOADER_DRIVER_OVERRIDE=kgsl
export TURNIP_KMD=kgsl
```

The supported `TERMUX_VA_GPU_BACKEND` values are `auto`, `kgsl`, `sw`, and `drm`. Use `kgsl` for the KGSL Freedreno path, `sw` with `MESA_LOADER_DRIVER_OVERRIDE=llvmpipe` to select Mesa's software surface and frame-copy path, or `drm` for the regular DRM path. The `sw` setting changes only the container-side graphics path; the daemon still uses Android MediaCodec. KGSL systems commonly expose `/dev/kgsl-3d0`; use the device node available on the target.

The termux-va bridge can also be activated by auto-detection; `TERMUX_VA_BRIDGE=1` forces it on, while `TERMUX_VA_BRIDGE=0` disables it. For a DRM-less PRoot native-Wayland Chromium session, build Mesa with `-Dtermux-va-wayland-shim=enabled` and preload `libtva_drm_shim_wayland.so` so Chromium's DRM probes can find the KGSL device. This shim is a Mesa integration aid and does not change the socket protocol.

## End-to-end validation

First verify the daemon from Termux:

```sh
tva-probe
```

Then verify that the container sees the VA driver and advertised profiles:

```sh
vainfo
```

Run a full decode test with FFmpeg; replace the input path and VA device with those available in the container:

```sh
ffmpeg -v error -xerror -vaapi_device /dev/kgsl-3d0 -hwaccel vaapi -hwaccel_output_format vaapi -i input.mp4 -map 0:v:0 -f null -
```

A transcode can use VA surfaces for decode, scale, and encode when the input and output profiles are supported:

```sh
ffmpeg -y -vaapi_device /dev/kgsl-3d0 -hwaccel vaapi -hwaccel_output_format vaapi -i input.mp4 -vf scale_vaapi=w=1280:h=720 -map 0:v:0 -map '0:a?' -c:v h264_vaapi -b:v 2M -c:a copy output.mp4
```

To exercise the raw protocol client directly, provide an Annex B AVC or HEVC stream, or an IVF VP8/VP9 sample:

```sh
python3 tools/test_decode.py clip.h264
python3 tools/test_decode.py clip.h264 h264 shm
python3 tools/test_decode.py clip.vp9.ivf vp9
```

The script is in the source checkout; it is not installed by the package. Its usage and framing requirements are documented in the script header. Encoder protocol testing is available with `python3 tools/test_encode.py --profile baseline --output baseline.h264`.

The upstream baseline reports roughly four-times-realtime 720p30 decode over the Unix socket with 4 MiB buffers, and about 19% lower daemon CPU use with SHM than inline transport. Treat these as reference results rather than device-independent performance guarantees; resolution, codec, SoC, thermal state, and the selected Mesa backend all affect results.

## Troubleshooting

| Symptom | Likely cause and next check |
|---|---|
| `vainfo` lists no profiles | Confirm the daemon is running with `tva-probe`; check `LIBVA_DRIVER_NAME`, `TERMUX_VA_BRIDGE`, and that the installed Mesa build contains the bridge. |
| `connect: Connection refused` | The daemon may be stopped, or the container may have a stale bind mount of the socket file. Share or bind-mount the containing directory and compare the endpoint path on both sides. |
| Probe reports endpoint inode mismatch (exit 7) | The mount points at an old socket inode. Recreate the directory mount; restarting the daemon alone cannot repair a file-level bind mount. |
| Decode runs but later frames are black | Check for an unexpected drain or flush in the daemon log; flush clears the codec reference chain, so decoding may remain black until a new IDR. |
| Socket directory disappears | Termux cleared temporary storage after service teardown. Restart the daemon; use termux-services for persistent operation. |
| Daemon exits immediately | Check the daemon log for an existing process holding the `<socket>.lock` file or for MediaCodec initialization errors. |
| Probe handshake succeeds but receives no frames (exit 8) | Inspect verbose daemon logs for codec configuration failures or repeated input-buffer exhaustion, and confirm the socket buffers are configured as expected. Confirm the device supports the stream profile and dimensions. |
| `input buffers full` persists | A full MediaCodec input queue is backpressure, not proof that SHM slots are full. The daemon waits while codec output buffers are being returned. If no input buffer becomes available and output makes no progress for the stall interval, it closes that decoder session and releases its codec and SHM after both workers exit; the log includes the number of busy SHM slots. |
| Container sees the daemon but GPU surfaces fail | Check the actual device node, Mesa build options, and `TERMUX_VA_GPU_BACKEND`; use `sw` to isolate DMA-BUF and KGSL issues from MediaCodec decoding. |

Use `TERMUX_VA_LOG=/path/to/log` to choose the start-script log file, `TERMUX_VA_START_WAIT` to change its startup wait, and `termux-va -v -t` for detailed timestamped daemon logs. Keep the full daemon log and the failing input's codec, profile, level, dimensions, and frame rate when reporting a reproducible issue.

`TERMUX_VA_STALL_TIMEOUT_MS` sets the decoder input stall interval and the socket send timeout, in milliseconds (default `15000`, accepted range `1000`–`600000`). For example, `TERMUX_VA_STALL_TIMEOUT_MS=15000 termux-va -t` uses 15 seconds. Returning an output buffer restarts the input stall timer; a completely closed peer cancels the session promptly. Idle sessions waiting for new input are not timed out, and a write-half close still requests normal EOS draining. Cleanup cancels only the affected session and wakes both workers before stopping/deleting MediaCodec and unmapping SHM. It does not forcibly reuse occupied slots or flush away reference frames; a failed session requires the application to reconnect/recreate its decoder. The separate SHM slot wait remains 15 seconds.

## Developer checklist

- Build the daemon with the pinned NDK and API level before relying on a host-only syntax check; Android MediaCodec symbols and bionic behavior are target-specific.
- If a change touches `common/tva_protocol.h`, update the Mesa mirror in the same change set and run `scripts/check-mirror.sh`.
- Test the relevant transport and operation end to end: probe, inline decode, SHM decode, VA profile enumeration, and FFmpeg decode or encode as applicable.
- Keep changes to the daemon, Mesa bridge, and regression clients aligned when changing codec IDs, packet framing, endpoint handling, or shared-memory behavior.
