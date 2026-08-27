# TiRTC ESP32-P4 Device Monitor 版本契约

## 应用与平台

| 项目 | 内容 |
| --- | --- |
| 应用工程 | TiRTC ESP32-P4 Device Monitor |
| 应用版本 | `1.5.1` |
| 发布日期 | `2026-08-28` |
| 来源 Tag | `esp32-p4-device-app-v1.5.1` |
| 来源 Tag object | `4a0fa821b53551432c8341763cf68a61c81285b5` |
| 来源 commit | `8d26a2bc5267f6bf1db721730a210ce8bc2f7ccc` |
| 比较基线 | `esp32-p4-device-app-v1.5.0` / `97331363a0d3c4e6f89b5b93d39561e74087ba2a` |
| 公开项目 Tag | `esp32-p4-device-monitor-v1.5.1` |
| 发布范围 | 源码、公开文档和 `0x0` 16 MiB 完整镜像 |
| 目标板 | Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 |
| 网络架构 | ESP32-P4 host + ESP32-C6 ESP-Hosted/SDIO slave |
| ESP-IDF | `5.5.4` |
| Toolchain | `riscv32-esp-elf-gcc 14.2.0_20260121` |
| FreeRTOS tick | `1000Hz` |

## TiRTC SDK

| 项目 | 内容 |
| --- | --- |
| API 版本 | `2.3.0` |
| 交付性质 | 官方源码重建版，含 P4 ICE/TGTRP 稳定性修复 |
| Nano source | `v2.3.0` / `1baf7c95f3ca715c9367b9c998417f647934dc35` |
| TGWebRTC source | `tag.v1.5.12` / `41c9a25768ffe265c07f17ef78a6439607b19364` |
| 嵌入 TGTRP BuildInfo | `tagv1.5.11` |
| 官方归档 SHA-256 | `6daa39e04edf552283360f6a7defa6d12de8c8dd8d8094f8a6bbbdbb64a3f190` |
| P4 `libTiRTC.a` 大小 | `1,827,850` bytes |
| P4 `libTiRTC.a` MD5 | `7e5fe37e9530bcaffce015583ae8cfb0` |
| P4 `libTiRTC.a` SHA-256 | `6dc4d437ea444761ca21e203fc9babb1799bb1f7fc261d7c523248fde0a96e67` |
| `tiRTC.h` SHA-256 | `a53fa3392f71c8fd15c77891a772cc20939b5d253b995b3382486e514c134473` |

`tag.v1.5.12` 是 SDK 重建使用的 TGWebRTC 源码 Tag，`tagv1.5.11` 是当前库运行时嵌入的
TGTRP BuildInfo。两项来自不同元数据面，本文如实并列；集成时以源码 commit、静态库哈希和
`components/tirtc_sdk/SHA256SUMS.txt` 共同确定 SDK 身份。

构建契约：

- `CONFIG_FREERTOS_HZ=1000`，FreeRTOS trace/runtime stats 关闭。
- `CONFIG_LWIP_MAX_SOCKETS=16`。
- ESP32-C6 通过 ESP-Hosted/SDIO 提供 Wi-Fi；P4 不使用原生 Wi-Fi。
- `libwebrtc_nosctp.a` 已并入 `libTiRTC.a`，不能重复链接。
- `TIRTC_VIDEO_JPEG` 用于微信 VoIP MJPEG 下行。
- SDK 对外回调名为 `on_update_bitrate()`；应用内部 observer 名仍是
  `on_video_bitrate_required`。
- SDK/TGMP 码率控制默认开启；旧本地队列压力自动降级默认关闭。

## H264 组件

`components/espressif__esp_h264/` 以 Espressif `1.3.8`、upstream commit
`8e86030d` 为基线。三个新增文件与该 commit 的 blob 完全一致；项目同时保留 4 个 P4 硬件
编码源文件补丁和本地 `idf_component.yml`，用于输出长度边界、参考帧池与码率控制。
因此准确表述是“Espressif 1.3.8 基线 + P4 项目补丁”，不是整个组件逐字节等同上游。

## 1.5.1 变更

- 绑定弹窗中的文字网址和二维码使用独立的 ThingConnect 设备管理入口
  `https://demo-open.tange-ai.com/devices`。
- JSON 服务发现继续使用 `https://ep-open.tangeopen.com/services`，设备业务 API 继续使用
  `https://srv-open.tangeopen.com`；本补丁没有改变服务发现或业务 API 契约。
- TiRTC SDK、第三方组件、媒体档位、分区表和硬件配置保持 `1.5.0` 基线不变。

## 1.5.0 能力基线

- P4 ICE/TGTRP 继续使用有界接收和公平调度；音频抖动单轮最多处理 4 个有序工作项，ICE
  预算耗尽后让出 1 tick，避免媒体回调长期占用 RTC 线程。
- SDK/TGMP 反馈经应用控制任务调整编码器；设备呼叫 compact 范围为 `96-256kbps`，连接以
  正常 `256kbps` 档启动，注册起点为 `224kbps`，收到传输反馈后才调整。
- 摄像头 USERPTR、H264 reference/deblocking、解码、显示和 AEC 使用持久 PSRAM 池；跨 APP
  预热和会话代际清理减少连续进入、退出媒体业务时的大块内存碎片。
- 设备呼叫、微信联系人、二维码、铃声和呼叫 UI 完成一轮交互收口；二维码扫描预览支持
  灰度、RGB565 和 packed YUV420。
- 网络测试提供结构化时延、抖动和丢包率；ESP-Hosted Wi-Fi 后台恢复和串口回归 CLI 默认启用。

## 默认媒体参数

| 场景 | 参数 |
| --- | --- |
| IPC 上行 | `1280x960@20fps`，`4Mbps`，H264，GOP `40` / `2s` |
| 设备呼叫上行 | `384x256@12fps`，`256kbps`，H264，GOP `192` / `16s` |
| 设备呼叫下行 | constrained-baseline H264；解码 `384x256`，显示 `480x320` |
| 微信上行 | `480x320@15fps`，`480kbps`，H264，GOP `30` / `2s` |
| 微信下行 | 请求 `640x480` MJPEG，P4 JPEG 硬解后 `cover` 到 `480x320` |
| H264 输入/输出池 | 输入 `24 x 256KB`；decoded `4` 帧；output `20` 帧；playout 深度上限 `16` |
| 控制层自动隐藏 | `5s`，点击视频恢复 |

## 证据边界

来源 Tag 工作树、版本入口、源 SDK 7 项 SHA-256、P4 库哈希、凭据范围和 Git diff 已完成
静态核对。统一公开候选已完成唯一一次 ESP-IDF `5.5.4`、`--no-ccache` 干净构建：
`1837/1837` 个步骤，warning/error/ICE 均为 0；应用镜像 `6,955,776` bytes，SHA-256
`1542c19052ae9bdb8804e38916c54f928e405ac06e6ae796faa5d46cd7c7e515`，分区剩余
`580,864` bytes（`7.71%`）；16 MiB 完整镜像 SHA-256
`019eed4e047f9d1a1809c5fa0e441b94d2a509fe83c885e5eafc21adb89c0a7a`。

最新持久池修改后尚未重新完成微信外部实呼和弱网矩阵；COM7/COM11 上的启明板是旧版
`1.3.2`，不属于本版本证据。上面的正式构建证明候选可以编译和链接；本轮没有烧录或真机
验证，不能据此宣称绑定门户、网络或媒体已在目标板验收。

微信小程序 VoIP 和外部 IPC/H5 对端本轮没有重新完成人工端到端验证。

构建、烧录、C6/SDIO、联网、绑定、真实媒体和长期运行是不同证据层，不能互相替代。
