# TiRTC ESP32-P4 完整设备应用版本

| 项目 | 内容 |
| --- | --- |
| 应用工程 | TiRTC ESP32-P4 Device App |
| 应用版本 | `1.3.0` |
| 发布日期 | `2026-07-31` |
| 来源 Tag | `esp32-p4-device-app-v1.3.0` |
| 来源 commit | `dacf8e65b25ea4d0282fc9314e46a18607691fb4` |
| 发布范围 | 源码、必要配置和公开文档 |
| 目标芯片 | ESP32-P4 |
| 目标开发板 | Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 |
| 网络架构 | ESP32-P4 host + ESP32-C6 ESP-Hosted/SDIO slave |
| ESP-IDF | `5.5.4` |
| TiRTC SDK | `2.3.0` |
| SDK 来源标识 | ESP32-P4 test package |
| TiRTC Nano | `origin/tgmp` / `aaad3da251bac90e0642b51b3279a1f40ca9fa9a` |
| tgwebrtc | `e39114731ad488c88573d16f0855a1326d97c989` |
| TGTRP | `v1.5.10` |
| TiRTC P4 library MD5 | `ead6bc4172ba949adc3ec91d9efeb0d0` |
| TiRTC P4 library SHA-256 | `70b54792399ab983b8ec5b6ccdd295eab7b0c13e98c4769e17b82a3c05eace89` |
| `tiRTC.h` SHA-256 | `b8338a07532e09a8f5ccd6f0270a9d1fa3f227f8bc1a57fd663115b222984801` |

## SDK 契约

- `TIRTC_VERSION_MAJOR/MINOR/PATCH` 为 `2.3.0`。
- FreeRTOS tick 为 `1000Hz`，trace、stats formatting 和 runtime stats 关闭。
- `CONFIG_LWIP_MAX_SOCKETS=10`。
- 启用 SDK 码率自适应时，在连接建立后注册 `TiRtcConnSetVideoBitrateParams()`。
- 启用后，`on_video_bitrate_required()` 只投递绝对目标码率到应用控制任务，不在 SDK
  回调线程内调整编码器。
- `TIRTC_VIDEO_JPEG` 用于微信 VoIP MJPEG 下行。
- 详细文件级校验见 `components/tirtc_sdk/SHA256SUMS.txt`。

## 版本能力

- IPC H264 上行、设备间双向音视频、微信 H264 上行与 MJPEG 下行。
- AI Chat H264 视频流和音频流分离发送，并支持联系人状态查询、设备联系人及微信联系人
  的音频/视频呼叫。
- AI 设备动作通过应用生命周期异步路由，呼叫类型从 AI 请求贯通到设备呼叫和微信 VoIP。
- 微信联系人兼容 `data.contacts`、`data.list` 和字段别名，支持备注更新与权威列表刷新。
- 设备呼叫和微信 VoIP 场景中，P4 设备向服务端发送 `480x320@15fps` H264；微信下行
  请求服务端提供 `640x480` MJPEG，并居中 `cover` 显示到 `480x320` 屏幕。
- IPC 场景保持 `1280x960@20fps`、`4Mbps` H264 上行。微信手机端采集分辨率不由本工程
  配置，本版本没有把它声明为 720p。
- 微信远端视频订阅成功后若 `1s` 内没有首个视频包，仅补发一次幂等订阅请求。
- AEC 使用 codec 同步参考，硬件参考不可用时使用 `80ms` 软件回退。
- 音频播放使用自适应缓冲控制器。
- TiRTC 2.3 TGMP 码率反馈接口已经接入；SDK 码率自适应和旧的本地自动弱网降级默认关闭。
- JPEG 解码器、H264 编码器、RTC 媒体池和 AEC 在启动早期预热。

## 安全边界

源码默认不包含真实 Wi-Fi 密码、设备密钥、access key、token 或个人账号。设备凭证通过
绑定流程或设备本地配置写入 NVS。

## 发布边界

本版本按源码范围交付，不生成或发布 P4 APP BIN。来源选择、公开筛选和静态核验结果见
[SOURCE_PROVENANCE.md](SOURCE_PROVENANCE.md)。来源 Tag 固定源码、配置和 SDK 文件；构建
成功可提供编译与链接证据，烧录、ESP-Hosted/SDIO、联网和业务运行结果由目标板验证单独确认。

从源码构建、烧录和首次联网的完整步骤见
[开发者上手指南](docs/GETTING_STARTED_CN.md)。
