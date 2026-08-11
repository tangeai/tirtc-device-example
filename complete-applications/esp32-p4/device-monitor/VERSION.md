# TiRTC ESP32-P4 完整设备应用版本

## 应用与平台

| 项目 | 内容 |
| --- | --- |
| 应用工程 | TiRTC ESP32-P4 Device App |
| 应用版本 | `1.3.2` |
| 发布日期 | `2026-08-11` |
| 来源 Tag | `esp32-p4-device-app-v1.3.2` |
| 来源 commit | `bc1ae8fbd9b64090503128985129a72e024c0551` |
| 公开项目 Tag | `esp32-p4-device-monitor-v1.3.2` |
| 发布范围 | 源码、公开文档和 `0x0` 完整烧录镜像 |
| 目标芯片 | ESP32-P4 |
| 目标开发板 | Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 |
| 网络架构 | ESP32-P4 host + ESP32-C6 ESP-Hosted/SDIO slave |
| ESP-IDF | `5.5.4` |
| Toolchain | `riscv32-esp-elf-gcc 14.2.0_20260121` |
| FreeRTOS tick | `1000Hz` |

## TiRTC SDK 契约

| 项目 | 内容 |
| --- | --- |
| API 版本 | `2.3.0` |
| 交付性质 | ESP32-P4 定制兼容快照，公开 `2.3.0` 头文件和 TGMP 回调契约不变 |
| Nano branch | `origin/tgmp` |
| Nano baseline | `aaad3da251bac90e0642b51b3279a1f40ca9fa9a` |
| 兼容回移 | 从 `fde4f1c58d2dc28f3d3d04e25dd49bc3a399fea6` 回移 HTTP DNS disable |
| tgwebrtc | `e39114731ad488c88573d16f0855a1326d97c989` |
| TGTRP | `v1.5.10` |
| TiRTC P4 library MD5 | `13c36e22805776d0f437f16dddda87ad` |
| TiRTC P4 library SHA-256 | `b0a38061b0c63ad0c556f73bb2ecc47c6fd84823b7524f774fe09916f577b4c6` |
| `tiRTC.h` SHA-256 | `b8338a07532e09a8f5ccd6f0270a9d1fa3f227f8bc1a57fd663115b222984801` |

HTTP DNS disable 回移让 `/v1/connect` 使用平台 DNS resolver，避免 SDK 自定义 DNS 缓存条目
过期时发生递归锁。这个快照不能仅凭版本号与其他 `2.3.0` 静态库互换；集成时必须同时核对
版本说明、目标芯片和上述哈希。

构建契约：

- `CONFIG_FREERTOS_HZ=1000`。
- FreeRTOS trace、stats formatting 和 runtime stats 关闭。
- `CONFIG_LWIP_MAX_SOCKETS=10`。
- `libwebrtc_nosctp.a` 已并入 `libTiRTC.a`，不能再次链接一份。
- `TIRTC_VIDEO_JPEG` 用于微信 VoIP MJPEG 下行。
- 启用 TGMP 码率控制时，在连接建立后注册 `TiRtcConnSetVideoBitrateParams()`。
- `on_video_bitrate_required()` 只向应用控制任务投递绝对目标码率，不在 SDK 回调线程中改硬件编码器。
- 文件级校验见 `components/tirtc_sdk/SHA256SUMS.txt`。

## 1.3.2 版本能力

- 新增 internal-RAM NVS worker。运行时持久化请求先复制名称、键和值，再由固定任务串行执行
  `open -> set/erase -> commit -> close`，避免 PSRAM task stack 直接进入 flash/NVS 操作。
- 设备 UUID、音量、AI 头像、RTC 凭证和绑定 pending session 统一复用该 worker；要求立即
  确认持久化的操作会等待 commit 完成。
- 绑定 token reset 回调只向 APP control queue 投递重绑定事件，绑定流程由应用生命周期层执行。
- WHIP 连接提交使用 attempt ID 原子占位，防止同一空闲窗口内并发提交多次 SDK connect。
- 连接接受区分正常拒绝和 stale-closing 回调；已经关闭中的句柄不会被第二次 disconnect。
- 按连接句柄断开时先原子脱离 active owner，再投递关闭请求；重复断连保持幂等。
- AI Chat 先等待 RTC 可建立新连接，再获取 Token，并用 generation 阻止过期任务继续连接。
- TiRTC SDK `2.3.0`、板级配置和默认媒体参数均未改变。

以下 `1.3.1` 能力继续保留：

- IPC H264 上行、设备间双向音视频、微信 H264 上行与 MJPEG 下行。
- AI Chat 音视频流、联系人状态查询，以及设备联系人和微信联系人的音频/视频呼叫。
- 摄像头按 V4L2 sequence 去重并排出旧完成帧；应用节拍按目标 fps 锁相推进。
- H264 GOP 固定为 2 秒：IPC `20fps` 为 `40` 帧，通话 `15fps` 为 `30` 帧。
- TinyH264 双任务 helper 的 core、优先级和同步阶段通知由应用显式约束。
- H264 下行输入为 `16 x 256KB` PSRAM slot；decoded 和 output 各 `4` 个 RGB565 slot。
- 本地视频发送、远端视频接收和 renderer 都提供分阶段 liveness 与周期统计。
- 内存按 internal、DMA、PSRAM 分别记录 free、largest block、minimum free 和分配失败计数，
  并按 `normal`、`warning`、`critical` 转换记录水位。
- 微信 SDK 耗时工作串行进入固定 worker；接听 worker 常驻 PSRAM 并用请求序号隔离过期任务。
- 媒体参数集中到 `main/media/media_tuning.h` 和
  `main/services/call_video_renderer_config.h`。
- AEC 使用 codec 同步参考，硬件参考不可用时使用 `80ms` 软件回退。
- SDK 码率自适应和旧的本地自动弱网降级默认关闭。

## 默认媒体参数

| 场景 | 参数 |
| --- | --- |
| IPC 上行 | `1280x960@20fps`，`4Mbps`，H264，GOP `40` 帧 / `2s` |
| 设备呼叫/微信上行 | `480x320@15fps`，`800kbps` 起始，H264，GOP `30` 帧 / `2s` |
| 微信下行 | 请求 `640x480` MJPEG，P4 JPEG 硬解后显示到 `480x320` |
| H264 output buffer | `1MB` |
| Max delta payload | `256KB` |
| Startup max delta payload | 首 `2500ms` 为 `128KB` |

## 安全与验证边界

源码默认不包含真实 Wi-Fi 密码、设备密钥、access key、token 或个人账号。设备凭证通过
绑定流程写入 NVS。

`1.3.2` 公开候选在 ESP-IDF `5.5.4` 的正式构建记录为：`project_version=1.3.2`，应用镜像
`6,927,360` bytes，SHA-256 `2df6d9d626a05f19a4fd1f15eb854c54119a32ccd475090f6713f2629afc90e2`。构建目录、ELF、MAP 和分片
产物不进入 Git。

Release 资产 `esp32p4-tirtc-device-monitor-full-v1.3.2.bin` 为 `16,777,216` bytes，
SHA-256 `87bfb67d1ba30d7f79663f63891e29f7f4f4367c80ff0d5cecb1b46f301d40e9`。它从 `0x0` 以 `16MB`、`DIO/80MHz` 烧录，只在 GitHub
Release 分发。完整烧录会清除已有 NVS、Wi-Fi 和绑定信息。

静态校验和构建记录不能代替烧录、ESP-Hosted/SDIO、联网、重复绑定或音视频运行证明。
本版新增 NVS 串行化和 RTC 并发门控后，尤其需要在目标板检查连续重绑定、快速进入/退出
AI Chat 与呼叫、重复 disconnect、网络中断恢复和长稳。TinyH264 永久阻塞时的安全回收也仍需
目标板长时间运行验证。详细来源与字节保持边界见
[SOURCE_PROVENANCE.md](SOURCE_PROVENANCE.md)。
