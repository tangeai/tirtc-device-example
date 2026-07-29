# TiRTC ESP32-P4 完整设备应用

面向 Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 的 TiRTC 设备端完整应用。工程包含设备绑定
与在线、IPC 查看、设备间呼叫、微信 VoIP、AI Chat、OTA，以及 `480x320` 横屏触摸界面。

## 版本与平台

| 项目 | 当前值 |
| --- | --- |
| 应用版本 | `1.2.3` |
| 来源 Tag | `esp32-p4-device-app-v1.2.3` |
| 来源 commit | `c1af1eb1bcfa9da4b359ee24d25afb30f75d24b0` |
| 目标芯片 | ESP32-P4 |
| 目标开发板 | Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 |
| ESP-IDF | `5.5.4` |
| TiRTC SDK | `2.3.0` ESP32-P4 test package |
| FreeRTOS tick | `1000Hz` |
| 屏幕 | `480x320` 横屏 |
| 发布范围 | 源码、必要配置和公开文档 |

完整版本和 SDK 哈希见 [VERSION.md](VERSION.md)，源码筛选范围和验证边界见
[SOURCE_PROVENANCE.md](SOURCE_PROVENANCE.md)。

## 源码交付

本版本以源码形式进入 TiRTC 设备示例统一仓。开发者可以审查代码、按目标环境配置并自行构建；
当前 P4 APP 发布范围不生成或上传预编译 BIN。统一仓中的其他项目固件与本项目源码版本相互独立，
不能替代本项目构建产物。

## 主要能力

- ESP32-C6 + ESP-Hosted/SDIO Wi-Fi，支持服务发现、设备绑定、正式 MQTT 和 TiRTC 上线。
- IPC 使用 OV5647 YUV420 采集和 ESP32-P4 H264 硬件编码，不启用本地摄像头预览。
- 设备间呼叫支持 H264 上行、constrained-baseline H264 下行和双向音频。
- 微信 VoIP 支持 H264 上行、服务端 MJPEG 下行和 PCMA 双向音频。
- AI Chat 支持独立音频流与可选 H264 视频流。
- IPC、设备呼叫、微信 VoIP 和 AI Chat 由统一媒体所有权策略控制 AEC。
- 大块媒体缓冲和后台任务栈优先使用 PSRAM，内部 RAM 保留给 DMA 和实时控制路径。
- TiRTC TGMP 码率反馈接口由应用控制任务处理；SDK 码率自适应和旧的本地自动弱网降级默认关闭。

## 目录

```text
components/tirtc_sdk/  TiRTC 头文件、静态库和版本契约
main/application/      生命周期、业务状态和资源所有权
main/connectivity/     网络状态和 Wi-Fi 管理
main/drivers/          音频、摄像头、显示及测试媒体驱动
main/hardware/         Waveshare P4 板级初始化
main/media/            摄像头 pipeline、像素转换和媒体策略
main/platform/         存储、时间、日志和内存策略
main/protocols/        HTTP、MQTT、RTC 和 TiRTC 适配
main/services/         绑定、在线、呼叫、VoIP、AI、IPC 和 OTA
main/ui/               LVGL 页面、布局和资源
docs/                  架构与平台说明
tools/                 静态分析和媒体日志工具
```

UI 只负责展示状态和分发动作，不持有摄像头、音频设备或 TiRTC 连接。应用层编排业务生命周期，
服务层实现业务，协议层持有连接，媒体层处理帧，驱动层持有硬件。

## 默认媒体参数

| 场景 | 参数 |
| --- | --- |
| IPC 上行 | `1280x960@20fps`, `4Mbps`, H264 |
| 设备呼叫/微信上行 | `480x320@15fps`, `800kbps` 起始，H264 |
| 微信下行 | MJPEG -> P4 JPEG 硬解 -> RGB565 |
| H264 GOP | `30` |
| AEC | 默认开启 |
| SDK 码率自适应 | 默认关闭 |
| 本地自动弱网降级 | 默认关闭 |

具体链路、内存归属和失败边界见
[P4_MEDIA_ARCHITECTURE.md](docs/P4_MEDIA_ARCHITECTURE.md)。

## 配置与构建

准备以下环境：

- Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5。
- ESP-IDF `5.5.4`。
- 与目标板匹配的 ESP32-C6 ESP-Hosted/SDIO 固件。
- 2.4 GHz Wi-Fi。

配置入口位于 `main/Kconfig.projbuild` 和 `sdkconfig.defaults`。Wi-Fi 与 TiRTC 凭据默认留空，
由设备界面或本地安全配置写入 NVS。进入本目录后，在 ESP-IDF 环境执行：

```powershell
idf.py reconfigure build
```

源码仓只维护代码、配置和必要文档。`build/`、固件镜像、烧录包、日志和本地 IDE 配置
不进入版本管理。

## 相关文档

- [版本和 SDK 契约](VERSION.md)
- [源码来源与发布边界](SOURCE_PROVENANCE.md)
- [ESP32-P4 媒体架构](docs/P4_MEDIA_ARCHITECTURE.md)
- [统一仓固件下载与校验](../../../docs/RELEASES_CN.md)
