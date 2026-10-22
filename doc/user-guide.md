# Termux:VA User Guide

**English** | [中文](user-guide_zh.md)

---

This guide walks you through installing and using [Termux:VA](https://github.com/lfdevs/termux-va).

## Prerequisites

Actual support depends on the processor, application, video specifications, and other factors. See the [detailed compatibility guide](compatibility.md) before using Termux:VA.

## Installation

1. Install the [latest Termux:VA release](https://github.com/lfdevs/termux-va/releases/latest) in Termux, for example `termux-va_0.1.0_aarch64.deb`.

   ```sh
   pkg install ./termux-va_0.1.0_aarch64.deb
   ```

2. Install the latest [Mesa for Android Container](https://github.com/lfdevs/mesa-for-android-container) driver in your Linux container. Follow the instructions on the [latest release page](https://github.com/lfdevs/mesa-for-android-container/releases/latest).

## Usage

### On the Termux side

1. Start the Termux:VA daemon with:

   ```bash
   termux-va-start
   ```

   > To stop the daemon, run `termux-va-stop`.

2. Bind-mount Termux's `$TMPDIR/termux-va` directory at `/tmp/termux-va` inside the Linux container.

   For example, add the `--shared-tmp` option when logging in to a PRoot-Distro container:

   ```sh
   proot-distro login debian --shared-tmp
   ```

   Chroot, LXC, and Droidspaces containers may require additional SELinux rules.

### On the Linux container side

#### Basic environment variables

To let Linux applications use Termux:VA for hardware video decoding or encoding, first set the following environment variables. You can add them to `/etc/environment` to make them global. If this causes problems with other applications, you can instead set them only for specific applications.

- Snapdragon processors: `MESA_LOADER_DRIVER_OVERRIDE=kgsl LIBVA_DRIVER_NAME=termuxva TERMUX_VA_BRIDGE=1 TERMUX_VA_GPU_BACKEND=kgsl`
- Non-Snapdragon processors: `MESA_LOADER_DRIVER_OVERRIDE=llvmpipe LIBVA_DRIVER_NAME=termuxva TERMUX_VA_BRIDGE=1 TERMUX_VA_GPU_BACKEND=sw`

You can optionally set the following variables to disable hardware decoders that your processor does not support. See the beginning of the [compatibility guide](compatibility.md) for how to check decoder availability.

- AVC: `TERMUX_VA_DISABLE_AVC=1`
- HEVC: `TERMUX_VA_DISABLE_HEVC=1`
- VP9: `TERMUX_VA_DISABLE_VP9=1`
- AV1: `TERMUX_VA_DISABLE_AV1=1`

#### FFmpeg

The following FFmpeg examples convert a video to HEVC at `1280x720` and a bitrate of `6M`. Adjust `DISPLAY` to match your setup.

1. Termux:X11
   - Snapdragon processors:

     ```bash
     env LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libtva_drm_shim_wayland.so TERMUX_VA_X11_DRI3=1 DISPLAY=:0 ffmpeg -vaapi_device /dev/kgsl-3d0 -hwaccel vaapi -hwaccel_output_format vaapi -i input.mp4 -vf scale_vaapi=w=1280:h=720 -map 0:v:0 -map 0:a? -c:v hevc_vaapi -b:v 6M -c:a copy output.mp4
     ```

   - Non-Snapdragon processors:

     ```bash
     env DISPLAY=:0 ffmpeg -vaapi_device :0 -hwaccel vaapi -hwaccel_output_format vaapi -i input.mp4 -vf scale_vaapi=w=1280:h=720 -map 0:v:0 -map 0:a? -c:v hevc_vaapi -b:v 6M -c:a copy output.mp4
     ```

2. Anland: Termux
   - Snapdragon processors:

     ```bash
     env LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libtva_drm_shim_wayland.so ffmpeg -vaapi_device /dev/kgsl-3d0 -hwaccel vaapi -hwaccel_output_format vaapi -i input.mp4 -vf scale_vaapi=w=1280:h=720 -map 0:v:0 -map 0:a? -c:v hevc_vaapi -b:v 6M -c:a copy output.mp4
     ```

   - Non-Snapdragon processors with a Mali GPU:

     ```bash
     env LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libtva_drm_shim_wayland.so ffmpeg -vaapi_device /dev/mali0 -hwaccel vaapi -hwaccel_output_format vaapi -i input.mp4 -vf scale_vaapi=w=1280:h=720 -map 0:v:0 -map 0:a? -c:v hevc_vaapi -b:v 6M -c:a copy output.mp4
     ```

#### Chromium

Currently, Termux:VA hardware video decoding and encoding in Chromium are available only on Snapdragon processors.

1. Termux:X11

   Hardware video decoding works under Termux:X11; hardware video encoding is currently unavailable.

   ```bash
   env LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libtva_drm_shim_wayland.so TERMUX_VA_X11_DRI3=1 chromium --no-sandbox --ozone-platform=x11 --use-gl=angle --use-angle=gles-egl --disable-gpu-rasterization --enable-features=VaapiVideoDecoder,VaapiVideoDecodeLinux,PlatformHEVCDecoderSupport,AcceleratedVideoEncoder,AcceleratedVideoDecoder,AcceleratedVideoDecodeLinuxGL --disable-features=AcceleratedVideoDecodeLinuxZeroCopyGL,VaapiVideoMinResolutionForPerformance,PreferMappableSharedImage --enable-gpu-compositing --enable-zero-copy --enable-native-gpu-memory-buffers --ignore-gpu-blocklist --hardware-video-device-path=/dev/kgsl-3d0
   ```

2. Anland: Termux

   Hardware video decoding and encoding both work under Anland: Termux.

   ```bash
   env -u ANLAND_NO_DRM_DEVICE LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libtva_drm_shim_wayland.so chromium --no-sandbox --ozone-platform=wayland --use-gl=angle --use-angle=gles --render-node-override=/dev/kgsl-3d0 --hardware-video-device-path=/dev/kgsl-3d0 --enable-features=VaapiIgnoreDriverChecks,AcceleratedVideoEncoder,AcceleratedVideoDecoder,AcceleratedVideoDecodeLinuxGL,AcceleratedVideoDecodeLinuxZeroCopyGL --ignore-gpu-blocklist
   ```

#### Firefox

Currently, Termux:VA hardware video decoding in Firefox is available only on Snapdragon processors. Hardware video encoding is currently unavailable. Before launching Firefox, change these options on the `about:config` page:

- Set `media.hardware-video-decoding.force-enabled` to `true`.
- Set `gfx.webrender.all` to `true`.
- Set `widget.dmabuf.force-enabled` to `true`.
- Set `layers.acceleration.force-enabled` to `true`.
- Set `security.sandbox.gpu.level` to `0`.

1. Termux:X11

   ```bash
   env LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libtva_drm_shim_wayland.so TERMUX_VA_X11_DRI3=1 MOZ_FAKE_NO_SANDBOX=1 MOZ_DRM_DEVICE=/dev/kgsl-3d0 firefox-esr
   ```

2. Anland: Termux

   ```bash
   env LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libtva_drm_shim_wayland.so MOZ_ENABLE_WAYLAND=1 MOZ_FAKE_NO_SANDBOX=1 MOZ_DRM_DEVICE=/dev/kgsl-3d0 firefox-esr
   ```

#### OBS Studio

Currently, Termux:VA hardware video decoding and encoding in OBS Studio are available only on Snapdragon processors. Use the patched [OBS Studio for Termux:VA](https://github.com/lfdevs/obs-studio/releases/tag/release%2Ftermux-va).

After starting OBS Studio with one of the commands below, switch **Output Mode** to **Advanced** in the **Output** settings. Then select **FFmpeg VAAPI H.264** or **FFmpeg VAAPI HEVC** from the **Video Encoder** list. For a media source, enable **Use hardware decoding when available** in its settings.

1. Termux:X11

   ```bash
   env LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libtva_drm_shim_wayland.so TERMUX_VA_X11_DRI3=1 obs
   ```

2. Anland: Termux

   ```bash
   env LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libtva_drm_shim_wayland.so QT_QPA_PLATFORM=wayland obs
   ```

#### VLC Media Player

Currently, Termux:VA hardware video decoding in VLC Media Player is available only on Snapdragon processors. Use the patched [VLC Media Player for Termux:VA](https://github.com/lfdevs/vlc/releases/tag/release%2Ftermux-va).

1. Termux:X11

   ```bash
   vlc --avcodec-hw=vaapi --gl=egl_x11
   ```

2. Anland: Termux

   ```bash
   env LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libtva_drm_shim_wayland.so vlc --avcodec-hw=vaapi --gl=egl_x11
   ```
