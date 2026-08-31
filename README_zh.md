# Termux:VA

从 Termux 向 Linux 容器提供 MediaCodec 硬件视频解码能力。

`termux-va` 是 [DroidSpaces Media Decode Daemon](https://github.com/Re-s/droidspaces-media-decode) 的 Termux 移植版：一个小型 C 守护进程，通过 Unix socket 接收 H.264/HEVC/VP8/VP9 码流，用 Android MediaCodec API 硬件解码，回传 NV12 帧（内联传输，或经 memfd 槽位池零拷贝）。与 Termux 共享 tmp 目录的 Linux 容器内的应用通过标准 VA-API 使用它，无需任何改动：ffmpeg、Firefox、Chrome 均可用。

> English documentation: [README.md](README.md)

## 整体结构

移植思路沿用 [anland-termux](https://github.com/lfdevs/anland-termux) 确立的模式：**Termux daemon + 共享 tmp 中的 Unix socket + 容器侧 bridge**。

```
   Linux 容器（proot --shared-tmp）                     Termux
+---------------------------------------------+   +---------------------------+
|  ffmpeg / Firefox / Chrome / vainfo          |   |  termux-va (NDK r29, C11) |
|    libva -> Mesa VA 前端（桥接模式）           |   |  MediaCodec 硬解           |
|      -> 经 Unix socket 的 DMD v3 协议 --------+---+-> NV12 帧                  |
|         （内联，或 memfd + SCM_RIGHTS）        |   |                           |
+---------------------------------------------+   +---------------------------+
        socket: /tmp/termux-va/termux-va.sock == Termux $TMPDIR/termux-va/
```

- **Daemon（本仓库）**：`daemon/termux-va.c`，对上游 `decode-daemon.c` 的忠实移植，并移除了 TCP 传输——只监听路径式 Unix socket。使用 NDK **29.0.14206865** 构建（与 anland-termux 钉死的版本一致），API 29，arm64-v8a。
- **Mesa 桥（容器侧）**：位于 [mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container) 仓库的 `test/add-va-bridge` 分支（`src/gallium/frontends/va/tva_*`）。上游的独立伪 VA-API 驱动退役，桥在 Mesa 的 VA 前端内部使用同一线路协议。桥默认探测的 socket 是 `/tmp/termux-va/termux-va.sock`。

## 端点解析

| 优先级 | 来源 | 含义 |
|---|---|---|
| 1 | `--sock <路径\|目录>` | 命令行覆盖（目录会得到 `termux-va.sock`） |
| 2 | `TERMUX_VA_SOCKET` | 完整 socket 文件路径（双端） |
| 3 | `TERMUX_VA_SOCKET_DIR` | 目录；自动拼接 `termux-va.sock`（双端） |
| 4 | 默认 | `$TMPDIR/termux-va/termux-va.sock` |

`$TMPDIR` 依次回退到 `/tmp`、`/data/data/com.termux/files/usr/tmp`；adb 注入的 `/data/local/tmp` 视为未设置。使用 `proot-distro --shared-tmp` 时，Termux 的默认路径在容器内就是 `/tmp/termux-va/termux-va.sock`—— 一条路径，两端通用。

## 构建

```sh
# 在装有 Android NDK 的主机上（正式发布构建）：
ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/29.0.14206865 bash scripts/build-ndk.sh

# 在 Termux 内（开发便利）：
pkg install clang ndk-multilib
make -C daemon
```

## 运行

```sh
# Termux 内：
termux-va-start.sh                 # 或：termux-va [-v|-q] [--sock <路径|目录>]

# 带 shared tmp 进入容器：
proot-distro login debian --shared-tmp

# 容器内（Mesa 来自 mesa-for-android-container 的 test/add-va-bridge 分支）：
export LIBVA_DRIVER_NAME=termuxva
export TERMUX_VA_BRIDGE=1
export TERMUX_VA_GPU_BACKEND=kgsl  # KGSL Freedreno 路径
vainfo
ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi -i in.mp4 -f null -
```

将 `TERMUX_VA_GPU_BACKEND` 改为 `sw` 可强制 Mesa 使用 llvmpipe 的 surface 和帧拷贝路径；这不会关闭 Termux daemon 中的 MediaCodec 解码。当前 Mesa bridge 对外声明 H.264 和 VP9 Profile 0；daemon 虽然也接受 HEVC 和 VP8，但 Mesa bridge 尚未声明这两个 codec。

完整部署手册见 [doc/deploy_zh.md](doc/deploy_zh.md)，线路协议见 [doc/protocol_zh.md](doc/protocol_zh.md)。

## 仓库布局

```
common/tva_protocol.h   线路协议常量（镜像到 Mesa 桥）
daemon/termux-va.c      守护进程
daemon/Makefile         Termux 内构建入口
scripts/                NDK 构建、生命周期与看护脚本、镜像校验
tools/                  探活工具（tva-probe.c）、协议回归客户端（test_decode.py）
packages/termux-va/     termux-packages 配方
doc/                    协议与部署文档（中英双语）
```

## 许可证

本项目以 **GPL-3.0** 授权（见 [LICENSE](LICENSE)）。它基于以 Apache License 2.0 授权的 [DroidSpaces Media Decode Daemon](https://github.com/Re-s/droidspaces-media-decode)；所有由其衍生的文件均按 GPL-3.0 第 5 条要求带有显著的修改声明。
