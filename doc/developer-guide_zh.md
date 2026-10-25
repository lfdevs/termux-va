# Termux:VA 开发者指南

[English](developer-guide.md) | **中文**

---

本文介绍 Termux 侧守护程序、容器侧 Mesa 集成、构建与部署流程，以及修改项目时需要保留的兼容约束。线路格式另见[protocol_zh.md](protocol_zh.md)。

## 系统结构

Termux:VA 将 Android MediaCodec 视频解码和编码能力提供给容器内的 Linux 应用。系统由三部分组成：以普通 Termux 应用用户身份运行的守护程序、位于 Termux 与容器共享目录中的 Unix Socket，以及容器内集成于 Mesa 的 VA-API 桥。

```
 Linux 容器（PRoot、chroot 或 LXC）                   Termux
+------------------------------------------+   共享 tmp  +--------------------------+
| ffmpeg / Firefox / Chrome / vainfo       |   目录      | termux-va                |
|   libva -> Mesa VA 前端                  |<---------->| Android MediaCodec       |
|     -> termux-va bridge                  | Unix Socket| 硬解码 / 硬编码           |
|        内联帧或 memfd 槽位                |            | NV12 帧传输              |
+------------------------------------------+            +--------------------------+
```

容器侧 bridge 位于 [mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container) 仓库的 `dev/adreno-main` 分支，源码在 `src/gallium/frontends/va/tva_*`。应用使用标准 libva API；bridge 将 VA 调用转换为 DMD v3 Socket 协议。解码输出为线性 NV12。内联模式通过 Socket 传输帧数据；SHM 模式通过 memfd 槽位池传输帧数据，并使用 Socket 传控制消息。Mesa bridge 会暂存收到的帧，再在应用线程中复制到保留的 Gallium 资源。

Termux daemon 移植自 [DroidSpaces Media Decode Daemon](https://github.com/Re-s/droidspaces-media-decode/tree/143610b816da5c612c325a2170dbc5621b5ce1eb)，负责 MediaCodec 会话和共享端点。它接收解码输入单元或打包的 NV12 编码帧，配置 Android MediaCodec，并返回解码帧或编码访问单元。上游的 TCP 监听器已移除；daemon 只使用路径式 Unix Socket。每个客户端会话使用独立的输入、输出线程，使 Socket 输入、MediaCodec 提交、输出排空和帧回传可以并行推进。

## 编解码能力

daemon 协议支持 AVC、HEVC、VP8、VP9 和 AV1 解码会话。当前 Mesa VA bridge 对外声明 AVC Constrained Baseline、Main、High，HEVC Main，VP9 Profile 0 和 AV1 Main 解码；编码侧声明 AVC Constrained Baseline、Main、High，HEVC 和 VP9 slice。具体可用的 profile、分辨率仍由设备上的 MediaCodec 实现决定，应以目标设备上的 `vainfo` 和完整视频流测试为准。

Mesa bridge 可通过容器侧环境变量隐藏 codec profile：

| 变量 | 作用 |
|---|---|
| `TERMUX_VA_DISABLE_AVC=1` 或 `TERMUX_VA_DISABLE_H264=1` | 隐藏 AVC profiles；H.264 名称是兼容别名。 |
| `TERMUX_VA_DISABLE_HEVC=1` | 隐藏 HEVC profiles。 |
| `TERMUX_VA_DISABLE_VP9=1` | 隐藏 VP9 profiles。 |
| `TERMUX_VA_DISABLE_AV1=1` | 隐藏 AV1 profiles。 |

daemon 和 VA bridge 必须同时包含所需的 codec 与 profile 支持。尤其是显式 AVC Baseline、Main、High 编码请求使用了新增的协议 codec ID；旧 daemon 会拒绝这些 ID，而不会静默选择其他 profile。Baseline 请求会映射为 Android Constrained Baseline，daemon 会检查输出 SPS 后再返回数据包。

## 仓库布局

| 路径 | 用途 |
|---|---|
| `common/tva_protocol.h` | Socket 协议常量的唯一源文件，并逐字节镜像到 Mesa 仓库。 |
| `daemon/termux-va.c` | MediaCodec daemon、会话处理、端点解析及内联/SHM 传输。 |
| `daemon/Makefile` | Termux 内的便捷构建与安装入口。 |
| `scripts/build-ndk.sh` | 主机侧 NDK 交叉构建 daemon 和探针的标准脚本。 |
| `scripts/termux-va-start.sh` | 启动 daemon，或复用健康实例。 |
| `scripts/termux-va-stop.sh` | 优雅停止 daemon。 |
| `scripts/termux-va-watchdog.sh` | 周期性真实解码探测及受限重启逻辑。 |
| `scripts/check-mirror.sh` | 校验 Mesa 仓库中的协议头镜像。 |
| `tools/tva-probe.c` | 存活与解码健康探针。 |
| `tools/test_decode.py` | AVC、HEVC、VP8、VP9 输入样本的协议回归客户端。 |
| `tools/test_encode.py` | 协议编码回归客户端，包含 AVC profile 检查。 |
| `packages/termux-va/build.sh` | Termux 软件包配方。 |
| `.github/workflows/build-packages.yml` | 手动触发的 aarch64 软件包构建及可选草稿发布工作流。 |
| `doc/protocol.md` 和 `doc/protocol_zh.md` | 线路协议参考。 |
| `doc/developer-guide.md` 和 `doc/developer-guide_zh.md` | 中英文开发、构建和部署指南。 |

## 端点解析与共享存储

daemon、启动/停止脚本、探针、回归客户端和 Mesa bridge 均支持以下 Socket 路径覆盖。daemon 的命令行选项优先级最高。

| 优先级 | 设置 | 含义 |
|---|---|---|
| 1 | `termux-va --sock <路径或目录>` | 使用指定 Socket 文件；若给出目录，则追加 `termux-va.sock`。 |
| 2 | `TERMUX_VA_SOCKET` | 完整 Socket 文件路径。 |
| 3 | `TERMUX_VA_SOCKET_DIR` | Socket 目录；自动追加固定文件名。 |
| 4 | 默认值 | `$TMPDIR/termux-va/termux-va.sock`。 |

若 `TMPDIR` 为空或等于 `/data/local/tmp`，daemon 会在 `/tmp` 存在时使用它，否则回退到 `/data/data/com.termux/files/usr/tmp`。探针和测试客户端使用相同的回退规则。普通 Termux 会话中的 `TMPDIR` 指向 Termux 临时目录。

使用 PRoot-Distro 时，通过 `proot-distro login <发行版> --shared-tmp` 进入容器。Termux 临时目录会映射为容器内的 `/tmp`，因此两端默认使用同一条 `/tmp/termux-va/termux-va.sock` 路径。chroot 或 LXC 环境应将 Termux 临时**目录** bind mount 到容器的 `/tmp`。

必须挂载 Socket 所在目录，不要挂载 Socket 文件本身。daemon 启动时会 unlink 并重建 Socket，inode 会随之改变；文件级 bind mount 仍指向旧 inode。协议 v3 响应携带 daemon 端点的设备号和 inode，客户端可以据此识别过期挂载。若使用自定义路径，daemon 和客户端必须配置成相同路径。

daemon 启动时会创建 Socket 目录，并准备共享容器访问其父目录所需的权限。以普通 Termux 应用身份访问 Android MediaCodec 不需要 root。

## 协议与跨仓库修改

解码握手在线路上仍与上游 DMD v3 协议兼容。Termux:VA 接受版本 2 到 3。编码码率、帧率、codec profile 和原始帧处理属于项目扩展，详见[protocol_zh.md](protocol_zh.md)。

将 `common/tva_protocol.h` 视为协议常量的唯一源文件。修改任何跨 Socket 的常量或字段时，必须在同一组更改中更新 Mesa 仓库的 `src/gallium/frontends/va/tva_protocol.h`，然后运行：

```sh
MESA_REPO=../mesa-for-android-container bash scripts/check-mirror.sh
```

需要保留以下兼容规则：

- 不要修改现有协议字段或 codec ID 的含义、顺序和值；新 ID 或字段只能追加。
- 路径式 Unix Socket 是唯一控制传输；不要恢复 TCP，也不要新增默认端口。
- 工具和 bridge 使用的旧式 `DMD_*` 环境变量名称需要保留。
- 唯一的 abstract socket 是用于传递 memfd 文件描述符的单会话 `dmd-shm-*` 交接通道。
- 启用 SHM 时，`SHM_SLOTS`（8）必须不小于 Mesa bridge 流水线深度（默认 6，可由 `TERMUX_VA_PIPELINE_DEPTH` 配置）。
- `SHM_SLOT_WAIT_MS`（15 秒）应明显大于 Mesa bridge 的帧等待超时（5 秒）。

bridge 的同步和销毁行为也与协议可靠性相关。reader 线程负责接收并暂存完整帧，不得调用 `pipe_context` 方法。应用线程在 `fence_wait()` 中将帧复制到保留的 Gallium 资源。需要保持 `drv->mutex` 后接 `context->mutex` 的锁顺序，并保留 pending 帧和 fence 销毁的同步逻辑。

已测试的 KGSL 路径使用线性 NV12 surface 和共享 DMA-BUF。Freedreno 导出资源时必须填写 `winsys_handle.size`；libva 会将该大小传给 PRIME descriptor，下游消费者可能拒绝大小为零的对象。bridge 会在创建 screen 前启用 `FD_KGSL_ENABLE_DMABUF=1`。

KGSL 上的 Turnip 不应声明 `VK_KHR_external_semaphore_fd`，除非已验证 OPAQUE_FD timeline semaphore 路径和 DMA-BUF sync-file 行为。FFmpeg 会请求该路径；缺少实现时错误声明扩展可能导致导入 surface 损坏。当前测试路径会在导出前由 bridge fence 等待 CPU 帧复制完成，因此不依赖该扩展。

## 构建

支持目标是 Android 10 或更新版本的 Android arm64 设备。NDK 固定为 `29.0.14206865`，daemon 使用 API level 29 构建。

### 使用 NDK 交叉构建

在 Linux 主机上安装固定版本的 NDK 后运行：

```sh
ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/29.0.14206865 bash scripts/build-ndk.sh
```

脚本生成 `build/termux-va` 和 `build/tva-probe`。daemon 是 Android aarch64 可执行文件，不能在 Linux 构建主机上运行；它依赖 Android MediaCodec 和 Termux SELinux domain。未经项目协同，不要更改 NDK 版本固定值。

### 在 Termux 内构建

在设备上进行快速开发构建：

```sh
pkg install clang ndk-multilib make
make -C daemon
make -C daemon install
```

程序会安装到 `$PREFIX/bin/termux-va`。该方式适合本机开发；发布软件包通过 Termux 配方并使用固定 Android API level 构建。

### 构建 Termux 软件包

配方位于 `packages/termux-va/build.sh`，构建时需要在 Termux Packages 构建树旁提供本仓库 checkout。项目工作流只构建 aarch64，可从 GitHub Actions 页面手动触发：

- `ref` 选择要构建的分支、tag 或 commit，默认是 `main`。
- `draft-release` 请求创建草稿 Release。仅当 `ref` 被识别为 tag 时才会创建。
- Actions artifact 包含生成的 `.deb` 和 `sha256sums.txt`；草稿 Release 只包含软件包文件，校验和放在 release notes 中。

在 Termux Packages checkout 中本地构建时，先让 Termux:VA 源码位于 `../termux-va`，并确保构建容器内将其挂载到 `/home/builder/termux-va`，再运行：

```sh
TERMUX_DOCKER_RUN_EXTRA_ARGS="--volume $(realpath ../termux-va):/home/builder/termux-va:ro" ./scripts/run-docker.sh ./build-package.sh -I -f -a aarch64 -j 4 ../termux-va/packages/termux-va
```

`run-docker.sh` 只会在创建构建容器时应用额外挂载参数；已有容器必须已经挂载源码。构建产物写入 Termux Packages checkout 的 `output/` 目录。软件包会将 daemon、探针和生命周期脚本安装到 `$PREFIX/bin`。独立 Python 协议回归工具只存在于源码树，不包含在软件包中。

## 安装与运行

### 安装 Termux 侧组件

日常使用建议安装预构建软件包：

```sh
pkg install ./termux-va_<版本>_aarch64.deb
```

替换已有安装时，对该软件包文件运行 `pkg reinstall`。软件包包含 `termux-va`、`tva-probe`、`termux-va-start`、`termux-va-stop` 和 `termux-va-watchdog`。

从 Termux 中的源码 checkout 直接构建并安装：

```sh
pkg install git clang ndk-multilib make
git clone https://github.com/lfdevs/termux-va
cd termux-va
make -C daemon
make -C daemon install
```

### 保持 daemon 运行

长期运行建议使用服务管理器，因为普通后台进程可能会随终端会话结束而被终止，Termux 销毁服务时也可能清空 `TMPDIR`。

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

需要立即启动服务时运行 `sv up termux-va`。临时会话可运行 `termux-va-start`；它会复用健康实例，否则启动 daemon，默认将日志写入 `$TMPDIR/termux-va/termux-va.log`。使用 `termux-va-stop` 优雅停止。

源码脚本名为 `scripts/termux-va-start.sh`、`scripts/termux-va-stop.sh` 和 `scripts/termux-va-watchdog.sh`。软件包安装后的命令名不带 `.sh` 后缀。daemon 支持 `-v`（逐帧调试日志）、`-q`（只输出错误）、`-t`（日志添加时间戳）、`--sock <路径或目录>`（覆盖端点）和 `-h`。

### 健康检查

在 Termux 中运行已安装的探针：

```sh
tva-probe
tva-probe /path/to/termux-va.sock 3000
```

探针会连接 daemon、协商协议、提交内置的 96x96 AVC 码流，并要求收到解码帧。退出码 0 表示健康。退出码 7 表示当前路径的 inode 与实际应答端点不同；应修复共享挂载，而不是重启 daemon。退出码 8 表示握手成功但没有解码帧。

可选 watchdog 默认每 5 秒探测一次，连续失败 5 次后重启。端点 inode 不匹配时只告警，不会尝试重启；达到重启上限后会暂停自动重启，但仍会定期检查。可通过 `TERMUX_VA_WD_INTERVAL`、`TERMUX_VA_WD_MAX_FAILS`、`TERMUX_VA_WD_COOLDOWN` 和 `TERMUX_VA_WD_GRACE` 配置；默认值见脚本头部。应在长驻 Termux 会话或服务管理器下运行。

## 配置 Linux 容器

容器必须与 Termux 共享临时目录，并安装包含 termux-va bridge 的 Mesa。PRoot-Distro 示例：

```sh
proot-distro login debian --shared-tmp
```

安装容器侧 VA-API 工具和 FFmpeg：

```sh
apt install libva2 vainfo ffmpeg
```

安装 [mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container) 仓库 `dev/adreno-main` 分支对应的 Mesa 构建，可使用匹配的软件包或 CI archive。在容器 shell 或桌面会话启动器中配置客户端环境：

```sh
export LIBVA_DRIVER_NAME=termuxva
export TERMUX_VA_BRIDGE=1
export TERMUX_VA_GPU_BACKEND=kgsl
export MESA_LOADER_DRIVER_OVERRIDE=kgsl
export TURNIP_KMD=kgsl
```

支持的 `TERMUX_VA_GPU_BACKEND` 值为 `auto`、`kgsl`、`sw` 和 `drm`。`kgsl` 使用 KGSL Freedreno 路径；`sw` 配合 `MESA_LOADER_DRIVER_OVERRIDE=llvmpipe` 使用 Mesa 软件 surface 和帧复制路径；`drm` 使用常规 DRM 路径。`sw` 只改变容器侧图形路径，daemon 仍通过 Android MediaCodec 工作。KGSL 系统通常提供 `/dev/kgsl-3d0`，实际应以目标设备上的节点为准。

termux-va bridge 也可自动探测激活；`TERMUX_VA_BRIDGE=1` 强制启用，`TERMUX_VA_BRIDGE=0` 禁用。对于 DRM-less PRoot native-Wayland Chromium，会需要以 `-Dtermux-va-wayland-shim=enabled` 构建 Mesa，并 preload `libtva_drm_shim_wayland.so`，使 Chromium 的 DRM 探测能够找到 KGSL device。该 shim 用于 Mesa 集成，不改变 Socket 协议。

## 端到端验证

先在 Termux 中检查 daemon：

```sh
tva-probe
```

再检查容器是否能看到 VA driver 和声明的 profiles：

```sh
vainfo
```

使用 FFmpeg 完整解码视频；将输入文件路径和 VA device 替换为容器中实际可用的值：

```sh
ffmpeg -v error -xerror -vaapi_device /dev/kgsl-3d0 -hwaccel vaapi -hwaccel_output_format vaapi -i input.mp4 -map 0:v:0 -f null -
```

输入格式和输出 profile 均受支持时，可以使用 VA surface 完成解码、缩放和编码：

```sh
ffmpeg -y -vaapi_device /dev/kgsl-3d0 -hwaccel vaapi -hwaccel_output_format vaapi -i input.mp4 -vf scale_vaapi=w=1280:h=720 -map 0:v:0 -map '0:a?' -c:v h264_vaapi -b:v 2M -c:a copy output.mp4
```

也可直接运行原始协议测试客户端。输入 AVC 或 HEVC Annex B 码流，或 VP8/VP9 IVF 样本：

```sh
python3 tools/test_decode.py clip.h264
python3 tools/test_decode.py clip.h264 h264 shm
python3 tools/test_decode.py clip.vp9.ivf vp9
```

脚本位于源码 checkout 中，不会由软件包安装。输入格式及参数顺序见脚本头部。编码协议回归测试可运行 `python3 tools/test_encode.py --profile baseline --output baseline.h264`。

上游基准数据是在较新的 Snapdragon 设备上以 4 MiB 缓冲经 Unix Socket 解码 720p30，速度约为四倍实时；与内联传输相比，SHM 模式的 daemon CPU 使用量约低 19%。这些数据仅作参考，不代表不同设备都能达到；分辨率、codec、SoC、温度状态和 Mesa backend 都会影响性能。

## 故障排查

| 现象 | 可能原因与检查方法 |
|---|---|
| `vainfo` 没有列出 profile | 用 `tva-probe` 确认 daemon 运行；检查 `LIBVA_DRIVER_NAME`、`TERMUX_VA_BRIDGE` 以及 Mesa 是否包含 bridge。 |
| `connect: Connection refused` | daemon 可能已停止，或容器绑定的是旧 Socket 文件。共享或 bind mount Socket 所在目录，并核对两侧端点路径。 |
| 探针报告端点 inode 不匹配（退出码 7） | 挂载指向旧 Socket inode。重新建立目录挂载；仅重启 daemon 无法修复文件级 bind mount。 |
| 解码一段时间后出现黑帧 | 检查 daemon 日志中是否发生意外 drain 或 flush；flush 会清除 codec 参考帧链，在新 IDR 前可能持续黑屏。 |
| Socket 目录消失 | Termux 在服务销毁后清除了临时目录。重启 daemon；长期运行建议使用 termux-services。 |
| daemon 启动后立即退出 | 检查日志中是否有其他进程持有 `<socket>.lock`，或有 MediaCodec 初始化错误。 |
| 探针握手成功但收不到帧（退出码 8） | 查看详细 daemon 日志中的 codec 配置错误或 input buffer 持续耗尽，并确认 Socket 缓冲区配置符合预期；同时确认设备支持该视频的 profile 和尺寸。 |
| 持续出现 `input buffers full` | MediaCodec 输入队列满属于背压，不能据此认定 SHM 槽位已满。只要 codec 输出缓冲仍在归还，daemon 就继续等待；如果整个停滞期限内既无法取得输入缓冲，输出也没有进展，则关闭该解码会话，在双线程退出后释放 codec 和 SHM，并在日志中记录占用的 SHM 槽位数。 |
| 容器能连接 daemon，但 GPU surface 失败 | 检查实际设备节点、Mesa 构建选项和 `TERMUX_VA_GPU_BACKEND`；用 `sw` 隔离 DMA-BUF/KGSL 问题与 MediaCodec 解码问题。 |

使用 `TERMUX_VA_LOG=/path/to/log` 指定启动脚本日志文件，使用 `TERMUX_VA_START_WAIT` 调整启动等待时间，或用 `termux-va -v -t` 获取带时间戳的详细 daemon 日志。提交可复现问题时，请附上完整 daemon 日志及输入视频的 codec、profile、level、尺寸和帧率。

`TERMUX_VA_STALL_TIMEOUT_MS` 设置解码器输入停滞期限和 Socket 发送超时，单位为毫秒，默认 `15000`，有效范围为 `1000`–`600000`。例如，`TERMUX_VA_STALL_TIMEOUT_MS=15000 termux-va -t` 使用 15 秒。每次归还输出缓冲会重新开始计算输入停滞时间；对端完全断连会及时取消会话。等待新输入的空闲会话不会因此超时，关闭写端仍表示正常 EOS 排空。清理只取消故障会话，先唤醒双线程，再停止并删除 MediaCodec、解除 SHM 映射；不会强制复用占用中的槽位或通过 flush 丢弃参考帧，失败后需要应用重新连接或重建解码器。独立的 SHM 槽位等待期限仍为 15 秒。

## 开发检查清单

- 在依赖主机语法检查之前，先使用固定 NDK 和 API level 构建 daemon；Android MediaCodec 符号和 bionic 行为需要目标工具链验证。
- 若修改 `common/tva_protocol.h`，需在同一组更改中更新 Mesa 镜像，并运行 `scripts/check-mirror.sh`。
- 按改动范围进行端到端验证：探针、inline 解码、SHM 解码、VA profile 枚举，以及相应的 FFmpeg 解码或编码。
- 修改 codec ID、数据包 framing、端点处理或共享内存行为时，要同步检查 daemon、Mesa bridge 和回归客户端。
