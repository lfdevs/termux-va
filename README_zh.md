# Termux:VA

[English](README.md) | **中文**

---

通过 Mesa VA-API bridge 向 Linux 容器提供 Android MediaCodec 硬件视频解码和编码能力。

## 特性

- Termux:VA 自身的运行无需 Root 权限，同时支持 PRoot、Chroot、LXC 和 Droidspaces 容器。
- 提供标准的 Mesa VA driver ABI，FFmpeg、Chromium、Firefox 等应用无需修改即可使用。
- 相较于软件解码和编码，可在一定程度上降低 CPU 占用率。
- 理论上支持 4K 60fps 乃至更高规格视频的硬件解码。
- 支持 Android 10 及更高版本的系统。

## 兼容性

在处理器支持的前提下，Termux:VA 目前支持以下的 SDR 视频格式：

| | AVC | HEVC | VP9 | AV1 |
| :-: | :-: | :-: | :-: | :-: |
| 解码 | ✔️ | ✔️ | ✔️ | ✔️ |
| 编码 | ✔️ | ✔️ | ✔️ | ❌ |

> [!NOTE]
> 上表所列的视频格式为 Termux:VA 的理论能力，实际的支持情况仍取决于具体的处理器、应用、视频规格等。推荐在使用 Termux:VA 前先查阅[详细的兼容性文档](doc/compatibility_zh.md)。

## 用户指南

Termux:VA 的安装、使用方法请查阅[用户指南](doc/user-guide_zh.md)。

## 其他文档

- [开发者指南](doc/developer-guide_zh.md)：系统结构、端点配置、构建、安装、部署、验证和故障排查。
- [线路协议](doc/protocol_zh.md)：Socket 消息、codec ID、内联与 SHM 传输及协议兼容性。

## 致谢

- [DroidSpaces Media Decode Daemon](https://github.com/Re-s/droidspaces-media-decode/tree/143610b816da5c612c325a2170dbc5621b5ce1eb): 本项目基于它进行移植与扩展。
- [FFmpeg](https://www.ffmpeg.org/): 世界上最知名的多媒体框架。它为本项目的实现提供了重要参考。

## 许可证

本项目以 **GPL-3.0** 授权（见 [LICENSE](LICENSE)）。它基于以 Apache License 2.0 授权的 [DroidSpaces Media Decode Daemon](https://github.com/Re-s/droidspaces-media-decode/tree/143610b816da5c612c325a2170dbc5621b5ce1eb)；所有由其衍生的文件均按 GPL-3.0 第 5 条要求带有显著的修改声明。
