# Termux:VA 兼容性

[English](compatibility.md) | **中文**

---

使用 Termux:VA 前，请查询您的设备可用的视频硬件解码器与编码器。一个快速的方法是使用该第三方 App：[Codec Info - Apps on Google Play](https://play.google.com/store/apps/details?id=com.parseus.codecinfo)

> [!NOTE]
> `c2.android` 开头的解码器为软件解码器，Termux:VA 暂不支持它们。
> ![[static/codec-info-app.jpg]]

## 应用

### FFmpeg

| 处理器 | Termux:X11 | Anland: Termux |
| :---: | :---: | --- |
| 骁龙 | ✔️ | ✔️ |
| 非骁龙 | ✔️ | ✔️ |

### Chromium

| 处理器 | Termux:X11 | Anland: Termux |
| :---: | :---: | --- |
| 骁龙 | ✔️ | ✔️ |
| 非骁龙 | ❌ | ❌ |

附注：Termux:X11 下，Chromium 的视频硬件编码暂不可用。

### Firefox

| 处理器 | Termux:X11 | Anland: Termux |
| :---: | :---: | --- |
| 骁龙 | ✔️ | ✔️ |
| 非骁龙 | ❌ | ❌ |

附注：Firefox 的视频硬件编码暂不可用。

### OBS Studio

需要使用打了补丁的版本：[OBS Studio for Termux:VA](https://github.com/lfdevs/obs-studio/releases/tag/release%2Ftermux-va)

| 处理器 | Termux:X11 | Anland: Termux |
| :---: | :---: | --- |
| 骁龙 | ✔️ | ✔️ |
| 非骁龙 | ❌ | ❌ |

### VLC Media Player

需要使用打了补丁的版本：[VLC Media Player for Termux:VA](https://github.com/lfdevs/vlc/releases/tag/release%2Ftermux-va)

| 处理器 | Termux:X11 | Anland: Termux |
| :---: | :---: | --- |
| 骁龙 | ✔️ | ✔️ |
| 非骁龙 | ❌ | ❌ |

## 硬件解码器 & 编码器

以下列举的硬件解码器和编码器仅代表在特定的 SoC 上通过了该格式某些 Profile & Level 视频的硬件解码、编码测试，不意味着一定支持所有 SoC 的同名解码器和编码器，或者该格式所有 Profile & Level 视频的硬件解码与编码。

### 高通

已测试的 SoC：`Snapdragon 8 Elite (SM8750)`、`Snapdragon 8+ Gen 1 (SM8475)`

1. 硬件解码器

   - `c2.qti.avc.decoder`
   - `c2.qti.hevc.decoder`
   - `c2.qti.vp9.decoder`
   - `c2.qti.av1.decoder`

2. 硬件编码器

   - `c2.qti.avc.encoder`
   - `c2.qti.hevc.encoder`

### 三星

已测试的 SoC：`Google Tensor (GS101)`

1. 硬件解码器

   - `c2.exynos.h264.decoder`
   - `c2.exynos.hevc.decoder`
   - `c2.exynos.vp9.decoder`

2. 硬件编码器

   - `c2.exynos.h264.encoder`
   - `c2.exynos.hevc.encoder`
   - `c2.exynos.vp9.encoder`

### 谷歌

已测试的 SoC：`Google Tensor (GS101)`

1. 硬件解码器

   - `c2.google.av1.decoder`

## 视频规格

### Profile & Level

根据 FFmpeg 实测结果，以下列出了特定 SoC 支持硬件解码的视频 Profile & Level 组合。

#### Snapdragon 8 Elite (SM8750)

1. AVC
   - **Constrained Baseline**: L1、L1.1、L1.2、L1.3、L2、L2.1、L2.2、L3、L3.1、L3.2、L4、L4.1、L4.2、L5、L5.1、L5.2、L6、L6.1、L6.2
   - **Main**: L1、L1.1、L1.2、L1.3、L2、L2.1、L2.2、L3、L3.1、L3.2、L4、L4.1、L4.2、L5、L5.1、L5.2、L6、L6.1、L6.2
   - **High**: L1、L1.1、L1.2、L1.3、L2、L2.1、L2.2、L3、L3.1、L3.2、L4、L4.1、L4.2、L5、L5.1、L5.2、L6、L6.1、L6.2

2. HEVC
   - **Main**: L1、L2、L2.1、L3、L3.1、L4、L4.1、L5、L5.1、L5.2、L6、L6.1、L6.2

3. VP9
   - **Profile 0**: L1、L1.1、L2、L2.1、L3、L3.1、L4、L4.1、L5、L5.1、L5.2、L6

4. AV1
   - **Main**: L2.0、L2.1、L3.0、L3.1、L4.0、L4.1、L5.0、L5.1、L5.2、L5.3、L6.0、L6.1

#### Snapdragon 8+ Gen 1 (SM8475)

1. AVC
   - **Constrained Baseline**: L1、L1.1、L1.2、L1.3、L2、L2.1、L2.2、L3、L3.1、L3.2、L4、L4.1、L4.2、L5、L5.1、L5.2、L6、L6.1、L6.2
   - **Main**: L1、L1.1、L1.2、L1.3、L2、L2.1、L2.2、L3、L3.1、L3.2、L4、L4.1、L4.2、L5、L5.1、L5.2、L6、L6.1、L6.2
   - **High**: L1、L1.1、L1.2、L1.3、L2、L2.1、L2.2、L3、L3.1、L3.2、L4、L4.1、L4.2、L5、L5.1、L5.2、L6、L6.1、L6.2

2. HEVC
   - **Main**: L1、L2、L2.1、L3、L3.1、L4、L4.1、L5、L5.1、L5.2、L6、L6.1、L6.2

3. VP9
   - **Profile 0**: L1、L1.1、L2、L2.1、L3、L3.1、L4、L4.1、L5、L5.1、L5.2、L6、L6.1、L6.2

#### Google Tensor (GS101)

1. AVC
   - **Constrained Baseline**: L1、L1.1、L1.2、L1.3、L2、L2.1、L2.2、L3、L3.1、L3.2、L4、L4.1、L4.2、L5、L5.1、L5.2、L6、L6.1、L6.2
   - **Main**: L1、L1.1、L1.2、L1.3、L2、L2.1、L2.2、L3、L3.1、L3.2、L4、L4.1、L4.2、L5、L5.1、L5.2、L6、L6.1、L6.2
   - **High**: L1、L1.1、L1.2、L1.3、L2、L2.1、L2.2、L3、L3.1、L3.2、L4、L4.1、L4.2、L5、L5.1、L5.2、L6、L6.1、L6.2

2. HEVC
   - **Main**: L1、L2、L2.1、L3、L3.1、L4、L4.1、L5、L5.1、L5.2、L6、L6.1、L6.2

3. VP9
   - **Profile 0**: L1、L1.1、L2、L2.1、L3、L3.1、L4、L4.1、L5、L5.1、L5.2、L6、L6.1、L6.2

4. AV1
   - **Main**: L2.0、L2.1、L3.0、L3.1、L4.0、L4.1、L5.0、L5.1、L5.2、L5.3

### 常见网站

已在真实设备通过以下常见网站的所列格式、分辨率视频的硬件解码测试。

#### YouTube

1. AVC

   ```text
   ID  EXT   RESOLUTION FPS CH │    FILESIZE   TBR PROTO │ VCODEC          VBR ACODEC      ABR ASR MORE INFO
   ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────
   160 mp4   256x128     24    │   112.58KiB   22k https │ avc1.4d400c     22k video only          144p, mp4_dash
   133 mp4   426x214     24    │   219.91KiB   43k https │ avc1.4d400d     43k video only          240p, mp4_dash
   134 mp4   640x320     24    │   378.64KiB   74k https │ avc1.4d4016     74k video only          360p, mp4_dash
   18  mp4   640x320     24  2 │     1.59MiB  316k https │ avc1.42001E         mp4a.40.2       44k 360p
   135 mp4   854x428     24    │   679.70KiB  133k https │ avc1.4d401e    133k video only          480p, mp4_dash
   136 mp4   1280x640    24    │     1.12MiB  224k https │ avc1.4d401f    224k video only          720p, mp4_dash
   137 mp4   1920x960    24    │     3.06MiB  612k https │ avc1.640028    612k video only          1080p, mp4_dash
   ```

2. VP9

   ```text
   ID  EXT   RESOLUTION FPS CH │    FILESIZE   TBR PROTO │ VCODEC          VBR ACODEC      ABR ASR MORE INFO
   ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────
   278 webm  256x128     24    │   209.79KiB   41k https │ vp9             41k video only          144p, webm_dash
   242 webm  426x214     24    │   218.85KiB   43k https │ vp9             43k video only          240p, webm_dash
   243 webm  640x320     24    │   441.30KiB   86k https │ vp9             86k video only          360p, webm_dash
   244 webm  854x428     24    │   594.73KiB  116k https │ vp9            116k video only          480p, webm_dash
   247 webm  1280x640    24    │  1006.75KiB  196k https │ vp9            196k video only          720p, webm_dash
   248 webm  1920x960    24    │     1.64MiB  327k https │ vp9            327k video only          1080p, webm_dash
   271 webm  2560x1280   24    │     8.73MiB 1744k https │ vp9           1744k video only          1440p, webm_dash
   313 webm  3840x1920   24    │    42.83MiB 8555k https │ vp9           8555k video only          2160p, webm_dash
   ```

3. AV1

   ```text
   ID  EXT   RESOLUTION FPS CH │    FILESIZE   TBR PROTO │ VCODEC          VBR ACODEC      ABR ASR MORE INFO
   ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────
   394 mp4   256x128     24    │   113.16KiB   22k https │ av01.0.00M.08   22k video only          144p, mp4_dash
   395 mp4   426x214     24    │   180.70KiB   35k https │ av01.0.00M.08   35k video only          240p, mp4_dash
   396 mp4   640x320     24    │   253.86KiB   50k https │ av01.0.01M.08   50k video only          360p, mp4_dash
   397 mp4   854x428     24    │   395.09KiB   77k https │ av01.0.04M.08   77k video only          480p, mp4_dash
   398 mp4   1280x640    24    │   636.34KiB  124k https │ av01.0.05M.08  124k video only          720p, mp4_dash
   399 mp4   1920x960    24    │  1009.71KiB  197k https │ av01.0.08M.08  197k video only          1080p, mp4_dash
   400 mp4   2560x1280   24    │     3.11MiB  622k https │ av01.0.12M.08  622k video only          1440p, mp4_dash
   401 mp4   3840x1920   24    │     8.42MiB 1682k https │ av01.0.12M.08 1682k video only          2160p, mp4_dash
   ```

#### Bilibili

1. AVC

   ```text
   ID     EXT RESOLUTION FPS │   FILESIZE   TBR PROTO │ VCODEC          VBR ACODEC      ABR
   ────────────────────────────────────────────────────────────────────────────────────────
   30016  mp4 360x640     30 │ ≈  2.00MiB  295k https │ avc1.64001E    295k video only
   30032  mp4 480x852     30 │ ≈  3.00MiB  442k https │ avc1.64001F    442k video only
   30064  mp4 720x1280    30 │ ≈  5.20MiB  767k https │ avc1.640028    767k video only
   30080  mp4 1080x1920   30 │ ≈ 10.31MiB 1521k https │ avc1.640032   1521k video only
   30112  mp4 1080x1920   30 │ ≈ 14.89MiB 2196k https │ avc1.640032   2196k video only
   30120  mp4 2160x3840   30 │ ≈ 55.99MiB 8262k https │ avc1.640033   8262k video only
   ```

2. HEVC

   ```text
   ID     EXT RESOLUTION FPS │   FILESIZE   TBR PROTO │ VCODEC          VBR ACODEC      ABR
   ────────────────────────────────────────────────────────────────────────────────────────
   30011  mp4 360x640     30 │ ≈  1.25MiB  184k https │ hvc1.1.6.L120  184k video only
   30033  mp4 480x852     30 │ ≈  1.76MiB  259k https │ hvc1.1.6.L120  259k video only
   30066  mp4 720x1280    30 │ ≈  2.92MiB  431k https │ hvc1.1.6.L120  431k video only
   30077  mp4 1080x1920   30 │ ≈  5.59MiB  825k https │ hvc1.1.6.L150  825k video only
   30102  mp4 1080x1920   30 │ ≈  9.60MiB 1417k https │ hvc1.1.6.L150 1417k video only
   30121  mp4 2160x3840   30 │ ≈ 21.84MiB 3223k https │ hvc1.1.6.L153 3223k video only
   ```

3. AV1

   ```text
   ID     EXT RESOLUTION FPS │   FILESIZE   TBR PROTO │ VCODEC          VBR ACODEC      ABR
   ────────────────────────────────────────────────────────────────────────────────────────
   100022 mp4 360x640     30 │ ≈  1.10MiB  163k https │ av01.0.08M.08  163k video only
   100023 mp4 480x852     30 │ ≈  1.54MiB  228k https │ av01.0.08M.08  228k video only
   100024 mp4 720x1280    30 │ ≈  2.48MiB  366k https │ av01.0.08M.08  366k video only
   100026 mp4 1080x1920   30 │ ≈  4.73MiB  698k https │ av01.0.08M.08  698k video only
   100027 mp4 1080x1920   30 │ ≈  8.28MiB 1222k https │ av01.0.08M.08 1222k video only
   100029 mp4 2160x3840   30 │ ≈ 16.11MiB 2377k https │ av01.0.12M.08 2377k video only
   ```

#### Tiktok

1. AVC

   ```text
   ID                      EXT RESOLUTION │  FILESIZE   TBR PROTO │ VCODEC       VBR ACODEC     MORE INFO
   ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────
   h264_540p_1169236-0     mp4 1356x576   │  24.50MiB 1169k https │ h264             aac
   h264_720p_3057165-0     mp4 1694x720   │  64.06MiB 3057k https │ h264             aac
   ```

2. HEVC

   ```text
   ID                      EXT RESOLUTION │  FILESIZE   TBR PROTO │ VCODEC       VBR ACODEC     MORE INFO
   ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────
   bytevc1_540p_1268298-0  mp4 1356x576   │  24.16MiB 1268k https │ h265       1268k video only Untested
   bytevc1_720p_1495035-0  mp4 1694x720   │  28.94MiB 1495k https │ h265       1495k video only Untested
   bytevc1_1080p_3057533-0 mp4 2542x1080  │  59.72MiB 3057k https │ h265       3057k video only Untested
   ```
