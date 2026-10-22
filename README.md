# Termux:VA

**English** | [中文](README_zh.md)

---

Provide Android MediaCodec hardware video decoding and encoding to Linux containers through Mesa's VA-API bridge.

## Features

- Termux:VA runs without root and supports PRoot, Chroot, LXC, and Droidspaces containers.
- It provides the standard Mesa VA driver ABI, so applications such as FFmpeg, Chromium, and Firefox can use it without modification.
- Hardware decoding and encoding can reduce CPU usage compared with software processing.
- Hardware decoding of 4K 60 fps and higher-specification video may be possible, depending on the device.
- Supports Android 10 and later.

## Compatibility

When supported by the processor, Termux:VA currently supports the following SDR video formats:

| | AVC | HEVC | VP9 | AV1 |
| :-: | :-: | :-: | :-: | :-: |
| Decode | ✔️ | ✔️ | ✔️ | ✔️ |
| Encode | ✔️ | ✔️ | ✔️ | ❌ |

> [!NOTE]
> The table describes Termux:VA's theoretical capabilities. Actual support depends on the processor, application, video specifications, and other factors. See the [detailed compatibility guide](doc/compatibility.md) before using Termux:VA.

## User guide

See the [user guide](doc/user-guide.md) for installation and usage instructions.

## Other documentation

- [Developer guide](doc/developer-guide.md): architecture, endpoint configuration, building, installation, deployment, validation, and troubleshooting.
- [Wire protocol](doc/protocol.md): socket messages, codec IDs, inline and SHM transport, and protocol compatibility.

## Acknowledgments

- [DroidSpaces Media Decode Daemon](https://github.com/Re-s/droidspaces-media-decode/tree/143610b816da5c612c325a2170dbc5621b5ce1eb): this project is ported and extended from it.
- [FFmpeg](https://www.ffmpeg.org/): a widely used multimedia framework and an important reference for this project.

## License

This project is licensed under **GPL-3.0** (see [LICENSE](LICENSE)). It is based on [DroidSpaces Media Decode Daemon](https://github.com/Re-s/droidspaces-media-decode/tree/143610b816da5c612c325a2170dbc5621b5ce1eb), which is licensed under the Apache License 2.0; every file derived from it carries a prominent modification notice as required by GPL-3.0 section 5.
