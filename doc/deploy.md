# termux-va Deployment Manual

> 中文版：[deploy_zh.md](deploy_zh.md)

## Prerequisites

- An Android 10+ (API 29) arm64 device with Termux installed.
- A Linux container sharing Termux's tmp directory: PRoot via `proot-distro login <distro> --shared-tmp`, or a chroot/LXC with `$PREFIX/tmp` bind-mounted to the container's `/tmp`.  Mount the **directory**, never the single socket file (a bind mount pins the inode; the daemon re-creates the socket on every start).
- Container side: Mesa built from `mesa-for-android-container` branch `test/add-va-bridge` (which contains the `termux-va` VA bridge), plus `libva2` and `vainfo`.
- Termux side: `clang` + `ndk-multilib` only if building on-device; the release deb carries prebuilt binaries.

## Install the daemon

Preferred (prebuilt deb):

```sh
pkg reinstall ./termux-va_0.1.0_aarch64.deb
```

Or build from source on-device:

```sh
pkg install git clang ndk-multilib make
git clone https://github.com/lfdevs/termux-va && cd termux-va
make -C daemon && make -C daemon install
```

Installed files:

| File | Purpose |
|---|---|
| `$PREFIX/bin/termux-va` | the daemon |
| `$PREFIX/bin/tva-probe` | health probe (real decode check) |
| `$PREFIX/bin/termux-va-start` / `-stop` | lifecycle scripts |
| `$PREFIX/bin/termux-va-watchdog` | watchdog loop |
| `$PREFIX/libexec/termux-va/test_decode.py` | protocol regression client |

## Run the daemon

### Option A: termux-services (recommended)

termux-services (runsv) keeps the daemon alive across session closes and restarts it on crash - important because Termux clears `$TMPDIR` when its service is destroyed, and because a plain background process may be killed with its session.

```sh
pkg install termux-services
mkdir -p $PREFIX/var/service/termux-va
cat > $PREFIX/var/service/termux-va/run <<'EOF'
#!/data/data/com.termux/files/usr/bin/sh
exec termux-va -q 2>&1
EOF
chmod +x $PREFIX/var/service/termux-va/run
sv-enable termux-va    # or: sv up termux-va
```

### Option B: start script

```sh
termux-va-start        # reuses a healthy instance, logs to $TMPDIR/termux-va/termux-va.log
termux-va-stop         # graceful shutdown
```

### Watchdog (optional but recommended)

```sh
termux-va-watchdog     # probes every 5s, restarts after 5 consecutive failures
```

Run it from a long-lived session or together with termux-services.  Exit codes 1/2/8 of `tva-probe` trigger a restart; exit code 7 (endpoint inode mismatch) only warns - it is a mount/configuration problem that a restart cannot fix.

The daemon creates its socket directory on startup, so a wiped `$TMPDIR` self-heals on the next (re)start.

## Container setup

1. Enter the container with the shared tmp:

   ```sh
   proot-distro login debian --shared-tmp
   ```

   The daemon's default socket `$TMPDIR/termux-va/termux-va.sock` then appears as `/tmp/termux-va/termux-va.sock` inside the container.

2. Install the container-side Mesa build of branch `test/add-va-bridge` (tar.gz from its CI, installed to `/usr`), plus libva and tools:

   ```sh
   apt install libva2 vainfo
   ```

3. Set the consumer environment (put it in the container shell profile or the desktop-session launcher):

   ```sh
   export LIBVA_DRIVER_NAME=termuxva   # select the megadriver's termuxva entry
   export TERMUX_VA_BRIDGE=1           # explicit bridge activation (auto-detect also works)
   # optional overrides:
   # export TERMUX_VA_SOCKET=/tmp/termux-va/termux-va.sock
   # export TERMUX_VA_SOCKET_DIR=/tmp/termux-va
   # export TERMUX_VA_DRM_DEVICE=/dev/dri/renderD128
   # export TERMUX_VA_GPU_BACKEND=drm|kgsl|swrast
   ```

## End-to-end verification

```sh
# 1. Daemon health, from Termux:
tva-probe                                   # exit 0, prints probe: healthy frames=N

# 2. Cross-boundary protocol check, from the container:
python3 /data/data/com.termux/files/usr/libexec/termux-va/test_decode.py clip.h264
python3 .../test_decode.py clip.h264 h264 shm   # zero-copy path

# 3. VA-API visibility:
vainfo                                      # 5 profiles, VAEntrypointVLD, NV12

# 4. Real decode:
ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi -i in.mp4 -f null -
```

Expected performance on a recent Snapdragon (upstream baseline): 720p30 at ~4x realtime over the Unix socket with 4MB buffers; the SHM mode saves the daemon ~19% CPU versus inline.

## Troubleshooting

| Symptom | Diagnosis |
|---|---|
| `vainfo` reports no profiles | daemon not running (`tva-probe` in Termux), or `LIBVA_DRIVER_NAME`/`TERMUX_VA_BRIDGE` not exported in the consumer's environment |
| `connect: Connection refused` | the daemon restarted and the socket inode changed while something bind-mounted the single file - mount the directory instead |
| `endpoint inode mismatch` (probe exit 7) | same as above; stat both sides (`stat -c '%d:%i' /tmp/termux-va/termux-va.sock`) and re-mount the directory |
| decode works for a while then black frames | a drain was triggered (flush destroys the reference chain); should not happen in steady playback - report it with the daemon `-v` log |
| socket directory disappears | Termux cleared `$TMPDIR` (service destroyed); restart the daemon, prefer termux-services |
| daemon exits right after start | another instance holds the lock (`<sock>.lock`); check `termux-va.log` |
| 0 frames despite healthy handshake | exactly what `tva-probe` exit code 8 detects; check the daemon log for `input buffers full` storms and confirm the 4MB socket buffers are in effect |
