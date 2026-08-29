# termux-va 部署手册

> English version: [deploy.md](deploy.md)

## 前提条件

- Android 10+（API 29）arm64 设备，装有 Termux。
- 与 Termux 共享 tmp 目录的 Linux 容器：PRoot 用
  `proot-distro login <distro> --shared-tmp`；chroot/LXC 把 `$PREFIX/tmp`
  bind mount 到容器的 `/tmp`。**必须挂目录**，不要挂单个 socket 文件
  （bind mount 绑的是 inode，daemon 每次启动都会重建 socket）。
- 容器侧：由 `mesa-for-android-container` 仓库 `test/add-va-bridge` 分支
  构建的 Mesa（含 `termux-va` VA 桥），以及 `libva2` 和 `vainfo`。
- Termux 侧：仅在本机构建时需要 `clang` + `ndk-multilib`；发布 deb 自带
  预编译二进制。

## 安装 daemon

首选（预编译 deb）：

```sh
pkg reinstall ./termux-va_0.1.0_aarch64.deb
```

或本机源码构建：

```sh
pkg install git clang ndk-multilib make
git clone https://github.com/lfdevs/termux-va && cd termux-va
make -C daemon && make -C daemon install
```

安装的文件：

| 文件 | 用途 |
|---|---|
| `$PREFIX/bin/termux-va` | 守护进程 |
| `$PREFIX/bin/tva-probe` | 探活工具（真实解码检查） |
| `$PREFIX/bin/termux-va-start` / `-stop` | 生命周期脚本 |
| `$PREFIX/bin/termux-va-watchdog` | 看护循环 |
| `$PREFIX/libexec/termux-va/test_decode.py` | 协议回归客户端 |

## 运行 daemon

### 方式 A：termux-services（推荐）

termux-services（runsv）能让 daemon 在会话关闭后继续存活、崩溃后自动
重启——这点很重要，因为 Termux 销毁服务时会清空 `$TMPDIR`，普通后台
进程可能随会话一起被杀。

```sh
pkg install termux-services
mkdir -p $PREFIX/var/service/termux-va
cat > $PREFIX/var/service/termux-va/run <<'EOF'
#!/data/data/com.termux/files/usr/bin/sh
exec termux-va -q 2>&1
EOF
chmod +x $PREFIX/var/service/termux-va/run
sv-enable termux-va    # 或：sv up termux-va
```

### 方式 B：启动脚本

```sh
termux-va-start        # 健康实例直接复用；日志在 $TMPDIR/termux-va/termux-va.log
termux-va-stop         # 优雅停止
```

### 看护（可选但推荐）

```sh
termux-va-watchdog     # 每 5 秒探活，连续 5 次失败自动重启
```

放在长驻会话里运行，或配合 termux-services。`tva-probe` 退出码 1/2/8
会触发重启；退出码 7（端点 inode 不匹配）只告警——那是挂载/配置问题，
重启 daemon 解决不了。

daemon 启动时会重建 socket 目录，所以被清空的 `$TMPDIR` 会在下次启动时
自愈。

## 容器配置

1. 带 shared tmp 进入容器：

   ```sh
   proot-distro login debian --shared-tmp
   ```

   之后 daemon 的默认 socket `$TMPDIR/termux-va/termux-va.sock` 在容器内
   就是 `/tmp/termux-va/termux-va.sock`。

2. 安装容器侧 Mesa（`test/add-va-bridge` 分支构建产物，来自其 CI 的
   tar.gz，装到 `/usr`），以及 libva 和工具：

   ```sh
   apt install libva2 vainfo
   ```

3. 设置消费侧环境变量（写进容器 shell profile 或桌面会话启动器）：

   ```sh
   export LIBVA_DRIVER_NAME=termuxva   # 选择 megadriver 的 termuxva 入口
   export TERMUX_VA_BRIDGE=1           # 显式启用桥（默认探测也可自动激活）
   # 可选覆盖：
   # export TERMUX_VA_SOCKET=/tmp/termux-va/termux-va.sock
   # export TERMUX_VA_SOCKET_DIR=/tmp/termux-va
   # export TERMUX_VA_DRM_DEVICE=/dev/dri/renderD128
   # export TERMUX_VA_GPU_BACKEND=drm|kgsl|swrast
   ```

## 端到端验证

```sh
# 1. daemon 健康（Termux 内）：
tva-probe                                   # 退出码 0，输出 probe: healthy frames=N

# 2. 跨界协议检查（容器内）：
python3 /data/data/com.termux/files/usr/libexec/termux-va/test_decode.py clip.h264
python3 .../test_decode.py clip.h264 h264 shm   # 零拷贝路径

# 3. VA-API 可见性：
vainfo                                      # 5 个 profile、VAEntrypointVLD、NV12

# 4. 真实解码：
ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi -i in.mp4 -f null -
```

较新的骁龙平台预期性能（上游基线）：720p30 经 Unix socket 约 4 倍实时
（4MB 缓冲）；SHM 模式相对内联为 daemon 省约 19% CPU。

## 故障排查

| 症状 | 诊断 |
|---|---|
| `vainfo` 列不出 profile | daemon 没在跑（Termux 内跑 `tva-probe`），或消费侧没导出 `LIBVA_DRIVER_NAME`/`TERMUX_VA_BRIDGE` |
| `connect: Connection refused` | daemon 重启过且 socket inode 变了，而有人挂载了单个文件——改挂目录 |
| `endpoint inode mismatch`（探针退出码 7） | 同上；两侧 `stat -c '%d:%i' /tmp/termux-va/termux-va.sock` 对比后重挂目录 |
| 解码一段时间后黑帧 | 触发了排空（flush 摧毁参考帧链）；正常播放不应发生——带 daemon `-v` 日志上报 |
| socket 目录消失 | Termux 清空了 `$TMPDIR`（服务被销毁）；重启 daemon，建议改用 termux-services |
| daemon 启动即退出 | 另一个实例持着锁（`<sock>.lock`）；查看 `termux-va.log` |
| 握手健康但 0 帧 | 正是 `tva-probe` 退出码 8 检测的故障；查 daemon 日志里是否刷"input buffers full"，并确认 4MB socket 缓冲已生效 |
