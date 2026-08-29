# termux-va 线路协议（DMD v3）

> English version: [protocol.md](protocol.md)

线路格式与 droidspaces-media-decode 协议 v3（`HELLO_MAGIC 0x444D4400`）逐字节兼容，上游回归工具可直接使用。常量的唯一事实来源是 [`common/tva_protocol.h`](../common/tva_protocol.h)，该文件镜像到 Mesa 桥（`src/gallium/frontends/va/tva_protocol.h`）并用 `scripts/check-mirror.sh` 校验。

## 传输

每个会话只有一条控制通道：路径式 Unix socket（默认 `$TMPDIR/termux-va/termux-va.sock`）。线路上全部使用大端字节序。termux-va 已移除 TCP；传输标识值 0（内联）与 1（SHM）为协议兼容而保留。

## 握手（必需）

客户端在发送任何数据前先发 24 字节：

```
[u32 magic = 0x444D4400][u32 version][u32 codec][u32 宽][u32 高][u32 xfer]
```

- `version`：客户端声明的协议版本。daemon 接受 `2..3` 并取最小值。版本语义：v2 增加 SHM 协商，v3 在响应中增加 endpoint 扩展。
- `codec`：`0=H.264 (video/avc), 1=HEVC (video/hevc), 2=VP9, 3=VP8, 4=AV1`（daemon 接受 AV1，但桥不会请求）。
- `宽/高`：初始分辨率，有效范围 96x96..8192x4320。
- `xfer`：请求的帧回传方式，`0=内联`，`1=SHM`。

daemon 回应变长消息：

```
[u32 status][u32 实际xfer][u32 名字长度]
[若 v3 且 status==0：16 字节 endpoint 扩展]
[若 名字长度>0：相应字节的 abstract socket 名字]
```

- `status`：`0` 接受，`1` 版本，`2` codec，`3` 分辨率越界，`4` 缺握手。错误响应恒为裸 12 字节。
- `实际xfer`：daemon 实际授予的方式——`0` 内联或 `1` SHM。客户端必须以此为准，而不是自己的请求。
- v3 扩展以 `名字长度` 字的 bit 31 标记（真实名字长度远小于 2^31）；扩展内容为 `[u32 dev_hi][u32 dev_lo][u32 ino_hi][u32 ino_lo]`——daemon 对监听 socket 的 `stat()` 结果。客户端对连接所用路径做 `stat()` 并对账：不一致说明该路径并未通向应答的端点（典型的单文件 bind mount 失效）。

## 格式描述块

在首帧之前（以及每次输出格式变化后）daemon 发送：

```
[u32 0][u32 能力位][u32 0xFFFFFFFF]        哨兵帧头
[u32 缓冲宽][u32 缓冲高][u32 stride][u32 slice_height]
[u32 crop_l][u32 crop_t][u32 crop_r][u32 crop_b]   32 字节内容
```

`能力位` bit 0（`CAP_FRAME_PTS`）：之后每个帧头都带第 4 个字——输入单元序号。缓冲几何反映解码器的真实输出（高通 Venus 宽按 128、高按 32 对齐）；crop 矩形是可见区域。

## 上行数据单元

```
[u32 长度][数据]
```

每个长度前缀只放一个单元：

- H.264 / HEVC：单个带 Annex B 起始码（3 或 4 字节）的 NALU。SPS/PPS（H.264 type 7/8，HEVC type 32/33/34）累积为 CSD，以 `FLAG_CODEC_CONFIG` 送入，不产出帧。
- VP8 / VP9：整帧，**不带**起始码。
- `长度 == 0`：可逆排空请求（见下）。
- `长度 > 8MB (MAX_FRAME)`：协议违规，会话结束。

daemon 为每个提交的 VCL 单元打标签 `presentationTimeUs = 单元序号 * 1000`（乘 1000 是为了在解码器按毫秒量化 PTS 后仍保持唯一）；该序号随对应的输出帧回传，消费者据此配对 surface，完全无需知道解码器的输出顺序。

### 可逆排空（`长度 == 0`）

送入一个带 EOS 标志的空缓冲；解码器吐出 EOS 后 daemon 调用 `AMediaCodec_flush`、重送 CSD，会话无需重连即可继续。flush 会摧毁参考帧链，排空后的帧在下一个 IDR 之前会黑屏——桥只在"等待已证明无效"时才触发排空。

## 下行帧

两种回传方式，由 daemon 在握手时决定：

### 内联

```
[u32 宽][u32 高][u32 大小][u32 单元序号]    帧头（CAP_FRAME_PTS 开启）
[NV12 数据，大小字节]
```

### SHM（对消费者零拷贝）

帧数据放在 memfd 槽位池里，socket 只传 24 字节控制消息：

```
[u32 宽][u32 高][u32 0xFFFFFFFE][u32 槽位][u32 长度][u32 单元序号]
```

memfd 在握手响应之后立即交接：daemon 在 abstract socket `dmd-shm-<pid>-<会话>-<8 位十六进制随机>` 上监听，名字通过握手响应送达（内联模式下 `名字长度` 恒为 0）。客户端连上后收到一条带 memfd 的 `SCM_RIGHTS` 消息，附带 `[u32 槽数][u32 单槽字节数][u32 总字节数]`（大端）。

池布局：

```
[控制区 4096 字节][槽位 0][槽位 1] ... [槽位 SHM_SLOTS-1=7]
```

每个槽位在控制区偏移 `槽位*4` 处有一个 u32 状态字：daemon 写完帧置 1（release 语义）；客户端消费完置 0（槽位归还）。单槽大小 = `align128(max(宽,1920)) * align32(max(高,1088)) * 1.5`，下限 64KB。`SHM_SLOTS`（8）必须 >= 桥的流水线深度（6），daemon 的槽位等待（15s）必须显著大于桥的取帧超时（5s）。

SHM 交接失败时双方都自动降级为内联，没有硬失败路径。

## 会话结束

客户端关闭写端（`shutdown(SHUT_WR)`）；daemon 送入带 EOS 标志的空缓冲，输出线程确定性地收完剩余帧。消费者拿够帧后直接关闭是正常收尾——daemon 把由此产生的 `EPIPE` 归类为"对端离开"，不当作错误。

## 常量表

| 常量 | 值 | 定义位置 |
|---|---|---|
| `HELLO_MAGIC` | `0x444D4400` | `common/tva_protocol.h` |
| `HELLO_VERSION` | 3 | 同上 |
| 接受的版本 | 2..3 | 同上 |
| `MAX_FRAME` | 8 MiB（上行单元上限） | 同上 |
| `MAX_CLIENTS` | 8 | 同上 |
| `SHM_SLOTS` | 8 | 同上 |
| `SHM_CTRL_BYTES` | 4096 | 同上 |
| `SHM_SLOT_WAIT_MS` | 15000 | 同上 |
| `CAP_FRAME_PTS` | `0x00000001` | 同上 |
| `FMTDESC_SENTINEL` | `0xFFFFFFFF` | 同上 |
| `SHMFRAME_SENTINEL` | `0xFFFFFFFE` | 同上 |
| `PTS_UNIT_SCALE` | 1000 | 同上 |
| socket 缓冲 | 4 MiB（`SO_SNDBUF`/`SO_RCVBUF`） | daemon + 桥 |
| socket 路径 | `$TMPDIR/termux-va/termux-va.sock` | 同上 |
| socket 文件名 | `termux-va.sock` | 同上 |
| socket 目录名 | `termux-va` | 同上 |
