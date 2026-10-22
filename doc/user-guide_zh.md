# Termux:VA 用户指南

[English](user-guide.md) | **中文**

---

本文将逐步介绍 [Termux:VA](https://github.com/lfdevs/termux-va) 的安装和使用方法。

## 前提

Termux:VA 实际的支持情况取决于具体的处理器、应用、视频规格等。推荐在使用 Termux:VA 前先查阅[详细的兼容性文档](compatibility_zh.md)。

## 安装

1. 在 Termux 安装 [Termux:VA 的最新版本](https://github.com/lfdevs/termux-va/releases/latest)，如 `termux-va_0.1.0_aarch64.deb`。

   ```sh
   pkg install ./termux-va_0.1.0_aarch64.deb
   ```

2. 在 Linux 容器中安装 [Mesa for Android Container](https://github.com/lfdevs/mesa-for-android-container) 驱动的最新版本。请按照该页面的说明进行安装：<https://github.com/lfdevs/mesa-for-android-container/releases/latest>

## 使用

### Termux 侧

1. 使用以下命令来启动 Termux:VA 守护程序：

   ```bash
   termux-va-start
   ```

   > 如需结束 Termux:VA 守护程序，请运行命令 `termux-va-stop`。

2. 将 Termux 的 `$TMPDIR/termux-va` 目录绑定挂载到 Linux 容器内部的 `/tmp/termux-va`。

   比如 PRoot-Distro 容器在登录时可添加 `--shared-tmp` 选项：

   ```sh
   proot-distro login debian --shared-tmp
   ```

   Chroot、LXC 和 Droidspaces 容器可能需要额外添加 SELinux 规则。

### Linux 容器侧

#### 基础环境变量

要让 Linux 应用通过 Termux:VA 进行硬件解码或编码，首先需要声明这些基础环境变量。你可以将它们加入 `/etc/environment` 作为全局环境变量。如果加入这些全局环境变量后其他应用出现问题，也可只为特定的应用声明。

- 对于骁龙处理器：`MESA_LOADER_DRIVER_OVERRIDE=kgsl LIBVA_DRIVER_NAME=termuxva TERMUX_VA_BRIDGE=1 TERMUX_VA_GPU_BACKEND=kgsl`
- 对于非骁龙处理器：`MESA_LOADER_DRIVER_OVERRIDE=llvmpipe LIBVA_DRIVER_NAME=termuxva TERMUX_VA_BRIDGE=1 TERMUX_VA_GPU_BACKEND=sw`

选择性添加以下环境变量来禁用处理器不支持的硬件解码器。检查方法可参考[兼容性文档](compatibility_zh.md) 开头的说明。

- AVC: `TERMUX_VA_DISABLE_AVC=1`
- HEVC: `TERMUX_VA_DISABLE_HEVC=1`
- VP9: `TERMUX_VA_DISABLE_VP9=1`
- AV1: `TERMUX_VA_DISABLE_AV1=1`

#### FFmpeg

以下的 FFmpeg 命令示例将一个视频转码为 HEVC 格式，并将分辨率设置为 `1280x720`，码率设置为 `6M`。环境变量 `DISPLAY` 的值请根据实际进行调整。

1. Termux:X11
   - 对于骁龙处理器：

     ```bash
     env LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libtva_drm_shim_wayland.so TERMUX_VA_X11_DRI3=1 DISPLAY=:0 ffmpeg -vaapi_device /dev/kgsl-3d0 -hwaccel vaapi -hwaccel_output_format vaapi -i input.mp4 -vf scale_vaapi=w=1280:h=720 -map 0:v:0 -map 0:a? -c:v hevc_vaapi -b:v 6M -c:a copy output.mp4
     ```

   - 对于非骁龙处理器：

     ```bash
     env DISPLAY=:0 ffmpeg -vaapi_device :0 -hwaccel vaapi -hwaccel_output_format vaapi -i input.mp4 -vf scale_vaapi=w=1280:h=720 -map 0:v:0 -map 0:a? -c:v hevc_vaapi -b:v 6M -c:a copy output.mp4
     ```

2. Anland: Termux
   - 对于骁龙处理器：

     ```bash
     env LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libtva_drm_shim_wayland.so ffmpeg -vaapi_device /dev/kgsl-3d0 -hwaccel vaapi -hwaccel_output_format vaapi -i input.mp4 -vf scale_vaapi=w=1280:h=720 -map 0:v:0 -map 0:a? -c:v hevc_vaapi -b:v 6M -c:a copy output.mp4
     ```

   - 对于搭载 Mali GPU 的非骁龙处理器：

     ```bash
     env LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libtva_drm_shim_wayland.so ffmpeg -vaapi_device /dev/mali0 -hwaccel vaapi -hwaccel_output_format vaapi -i input.mp4 -vf scale_vaapi=w=1280:h=720 -map 0:v:0 -map 0:a? -c:v hevc_vaapi -b:v 6M -c:a copy output.mp4
     ```

#### Chromium

目前，通过 Termux:VA 在 Chromium 开启硬件视频解码和编码加速仅对骁龙处理器可用。

1. Termux:X11

   Termux:X11 下硬件视频解码加速可用，硬件视频编码加速暂不可用。

   ```bash
   env LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libtva_drm_shim_wayland.so TERMUX_VA_X11_DRI3=1 chromium --no-sandbox --ozone-platform=x11 --use-gl=angle --use-angle=gles-egl --disable-gpu-rasterization --enable-features=VaapiVideoDecoder,VaapiVideoDecodeLinux,PlatformHEVCDecoderSupport,AcceleratedVideoEncoder,AcceleratedVideoDecoder,AcceleratedVideoDecodeLinuxGL --disable-features=AcceleratedVideoDecodeLinuxZeroCopyGL,VaapiVideoMinResolutionForPerformance,PreferMappableSharedImage --enable-gpu-compositing --enable-zero-copy --enable-native-gpu-memory-buffers --ignore-gpu-blocklist --hardware-video-device-path=/dev/kgsl-3d0
   ```

2. Anland: Termux

   Anland: Termux 下，硬件视频解码和编码加速均可用。

   ```bash
   env -u ANLAND_NO_DRM_DEVICE LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libtva_drm_shim_wayland.so chromium --no-sandbox --ozone-platform=wayland --use-gl=angle --use-angle=gles --render-node-override=/dev/kgsl-3d0 --hardware-video-device-path=/dev/kgsl-3d0 --enable-features=VaapiIgnoreDriverChecks,AcceleratedVideoEncoder,AcceleratedVideoDecoder,AcceleratedVideoDecodeLinuxGL,AcceleratedVideoDecodeLinuxZeroCopyGL --ignore-gpu-blocklist
   ```

#### Firefox

目前，通过 Termux:VA 在 Firefox 开启硬件视频解码加速仅对骁龙处理器可用，硬件视频编码加速暂不可用。需要提前在 `about:config` 页面修改如下的几个选项：

- `media.hardware-video-decoding.force-enabled` 改为 `true`
- `gfx.webrender.all` 改为 `true`
- `widget.dmabuf.force-enabled` 改为 `true`
- `layers.acceleration.force-enabled` 改为 `true`
- `security.sandbox.gpu.level` 改为 `0`

1. Termux:X11

   ```bash
   env LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libtva_drm_shim_wayland.so TERMUX_VA_X11_DRI3=1 MOZ_FAKE_NO_SANDBOX=1 MOZ_DRM_DEVICE=/dev/kgsl-3d0 firefox-esr
   ```

2. Anland: Termux

   ```bash
   env LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libtva_drm_shim_wayland.so MOZ_ENABLE_WAYLAND=1 MOZ_FAKE_NO_SANDBOX=1 MOZ_DRM_DEVICE=/dev/kgsl-3d0 firefox-esr
   ```

#### OBS Studio

目前，通过 Termux:VA 在 OBS Studio 开启硬件视频解码和编码加速仅对骁龙处理器可用。需要使用打了补丁的版本：[OBS Studio for Termux:VA](https://github.com/lfdevs/obs-studio/releases/tag/release%2Ftermux-va)

使用以下命令启动 OBS Studio 后，在“输出”设置中将“输出模式”切换到“高级”，然后在下方“视频编码器”的下拉菜单中即可选择“FFmpeg VAAPI H.264”或“FFmpeg VAAPI HEVC”。对于媒体源，可以在它的设置中开启“在可用时使用硬件解码”。

1. Termux:X11

   ```bash
   env LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libtva_drm_shim_wayland.so TERMUX_VA_X11_DRI3=1 obs
   ```

2. Anland: Termux

   ```bash
   env LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libtva_drm_shim_wayland.so QT_QPA_PLATFORM=wayland obs
   ```

#### VLC Media Player

目前，通过 Termux:VA 在 VLC Media Player 开启硬件视频解码加速仅对骁龙处理器可用。需要使用打了补丁的版本：[VLC Media Player for Termux:VA](https://github.com/lfdevs/vlc/releases/tag/release%2Ftermux-va)

1. Termux:X11

   ```bash
   vlc --avcodec-hw=vaapi --gl=egl_x11
   ```

2. Anland: Termux

   ```bash
   env LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libtva_drm_shim_wayland.so vlc --avcodec-hw=vaapi --gl=egl_x11
   ```
