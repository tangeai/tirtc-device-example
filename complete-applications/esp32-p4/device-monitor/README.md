# TiRTC ESP32-P4 完整设备应用

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://github.com/tangeai/tirtc-device-example/blob/main/LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.5.4-E7352C?logo=espressif)](https://docs.espressif.com/projects/esp-idf/)
[![Chip](https://img.shields.io/badge/Chip-ESP32--P4-000000)](https://www.espressif.com/en/products/socs/esp32-p4)
[![TiRTC SDK](https://img.shields.io/badge/TiRTC%20SDK-2.3.0-1769AA)](https://docs.tange.ai/products/tirtc/en/overview/what-is-tirtc.html)
[![FreeRTOS Tick](https://img.shields.io/badge/FreeRTOS%20Tick-1%20kHz-00A98F)](sdkconfig.defaults)

这是面向 Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 的 TiRTC 完整设备应用。它把
P4 的摄像头、H264/JPEG 硬件能力、触摸屏和音频设备，与 C6 提供的远程 Wi-Fi、
TiRTC、ThingConnect、微信 VoIP 和 AI Chat 组合成一套可继续开发的产品级工程。

本版本只提供源码。请先在自己的 P4+C6 板卡上构建，再按本文烧录；不要拿其他项目的
BIN 代替本工程固件。

## 从这里开始

| 你现在要做什么 | 直接阅读 |
| --- | --- |
| 第一次拿到工程，完成环境、构建、烧录和首次联网 | [开发者上手指南](docs/GETTING_STARTED_CN.md) |
| 确认摄像头、编解码、PSRAM、AEC 和连接所有权 | [P4 媒体架构](docs/P4_MEDIA_ARCHITECTURE.md) |
| 核对应用、SDK、工具链和静态库哈希 | [VERSION.md](VERSION.md) |
| 核对公开源码来自哪个 Tag、哪些内容被排除 | [SOURCE_PROVENANCE.md](SOURCE_PROVENANCE.md) |

如果只是想跑起来，按这条主线走即可：

1. 准备 P4 主芯片与 C6 Wi-Fi 从芯片都可用的目标板。
2. 安装 ESP-IDF `5.5.4`，检出统一仓发布 Tag。
3. 在本目录执行 `idf.py set-target esp32p4` 和 `idf.py build`。
4. 把 `build/flasher_args.json` 中列出的 BIN 和 offset 填入
   [Espressif Web Serial 烧录工具](https://espressif.github.io/esptool-js/) 完成烧录。
5. 首次启动在屏幕上连接 2.4 GHz Wi-Fi，再按屏幕提示完成 6 位码绑定。
6. 先跑“网络测试”和“TiRTC 测试”，再分别进入 IPC、设备呼叫、微信 VoIP 和 AI Chat。

命令、预期现象和故障定位都在[开发者上手指南](docs/GETTING_STARTED_CN.md)中展开。

## 版本与平台

| 项目 | 当前值 |
| --- | --- |
| 应用版本 | `1.3.0` |
| 来源 Tag | `esp32-p4-device-app-v1.3.0` |
| 来源 commit | `dacf8e65b25ea4d0282fc9314e46a18607691fb4` |
| 目标开发板 | Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 |
| 主芯片 | ESP32-P4 |
| Wi-Fi 从芯片 | ESP32-C6，通过 ESP-Hosted/SDIO 连接 P4 |
| ESP-IDF | `5.5.4` |
| TiRTC SDK | `2.3.0` ESP32-P4 test package |
| FreeRTOS tick | `1000Hz` |
| 屏幕 | `480x320` 横屏触摸屏 |
| Flash | `16MB`，双 OTA 分区 |
| 发布形式 | 源码、必要配置和公开文档；不提供 P4 APP 预编译 BIN |

## P4+C6 的分工

ESP32-P4 不带原生 Wi-Fi。本项目使用 ESP32-C6 运行 ESP-Hosted slave，通过 4-bit、
40 MHz SDIO 为 P4 提供远程 Wi-Fi：

```text
OV5647 / Audio Codec / LCD / Touch
                 |
            ESP32-P4
      media + UI + TiRTC APP
                 |
      ESP-Hosted over SDIO
                 |
            ESP32-C6
              Wi-Fi
```

因此“P4 固件构建成功”和“Wi-Fi 实际可用”是两件事。P4 烧录只更新 P4；若 Wi-Fi 扫描
始终为空，要先确认 C6 已烧录与 ESP-Hosted `1.4.7` 兼容的 slave 固件、SDIO 连线和
复位脚工作正常。已经能稳定扫描 Wi-Fi 的板卡无需重复刷 C6。

## 核心能力

- **设备绑定与在线**：屏幕连接 Wi-Fi，自动获取 6 位绑定码，完成绑定后维持正式 MQTT 在线。
- **IPC 查看**：OV5647 采集 `1280x960` YUV420，P4 硬件 H264 编码后上行。
- **设备间呼叫**：设备联系人音频/视频呼叫、H264 双向视频和 PCMA 双向音频。
- **微信 VoIP**：设备侧 H264 上行、服务端 MJPEG 下行、硬件 JPEG 解码和全屏视频显示。
- **AI Chat**：独立音频流、可选 H264 视频流、字幕、打断，以及联系人查询和呼叫动作。
- **本地交互**：`480x320` 横屏 LVGL 界面，覆盖 Wi-Fi、绑定、联系人、呼叫、设置和 OTA。
- **媒体资源治理**：摄像头、显示、音频、AEC、PSRAM pool 和 TiRTC 连接按应用生命周期切换。

## 默认媒体参数

上行和下行都以 P4 设备为参照。

| 场景 | 当前源码默认值 |
| --- | --- |
| P4 -> 服务端，IPC | `1280x960@20fps`，`4Mbps`，H264 |
| P4 -> 服务端，设备呼叫/微信 VoIP | `480x320@15fps`，`800kbps` 起始，H264 |
| 服务端 -> P4，微信 VoIP | 请求 `640x480` MJPEG，P4 硬件 JPEG 解码后居中 `cover` 到 `480x320` |
| H264 GOP | `30` |
| AEC | 默认开启 |
| SDK 码率自适应 | 默认关闭 |
| 本地自动弱网降级 | 默认关闭 |

这里没有“微信上行 720p”的配置。P4 工程能确定的是设备侧上行和设备侧下行请求：
设备呼叫/微信设备上行为 `480x320` H264，微信服务端到 P4 的下行请求为 `640x480`
MJPEG。微信手机端采集分辨率由微信和服务端链路决定，不由本工程配置，也不能从 P4 源码
反推出 720p。

## 配置原则

正常开发无需把 Wi-Fi 密码或 TiRTC 密钥写进源码：

- Wi-Fi 在屏幕“设置 -> Wi-Fi 设置”中连接，并保存到设备 NVS。
- 设备身份由绑定流程下发并保存；“设置 -> TiRTC 配置”用于查看状态或重置绑定。
- AI Chat、微信联系人和设备联系人依赖平台侧账号、角色和授权关系。
- `main/application/app_config.h` 只提供空值和服务端默认入口，不应提交真实凭据。
- 媒体、AEC 和诊断开关位于 `main/Kconfig.projbuild`，稳定默认值位于 `sdkconfig.defaults`。

修改板型、SDIO 引脚、PSRAM、分区或编解码参数已经属于板级移植，不是普通业务配置。
在不同硬件上改这些值前，请先读[P4 媒体架构](docs/P4_MEDIA_ARCHITECTURE.md)。

## 构建与烧录摘要

本工程固定使用 ESP-IDF `5.5.4`。进入本目录后执行：

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py set-target esp32p4
idf.py reconfigure
idf.py build
```

打开 [Espressif Web Serial 烧录工具](https://espressif.github.io/esptool-js/)，按
`build/flasher_args.json` 添加构建生成的每个 BIN 及其 offset。不要把单独的应用 BIN
写到 `0x0`，也不要照抄其他工程的 offset。完整步骤见
[开发者上手指南](docs/GETTING_STARTED_CN.md)。

烧录完成后如需查看串口日志，可执行 `idf.py -p <PORT> monitor`。

## 第一次启动应该看到什么

串口先出现固件身份和系统就绪日志：

```text
firmware version: 1.3.0 project=tirtc_esp32p4_device_app ...
system ready: ESP32-P4 TiRTC dashboard
```

没有保存过网络时，屏幕会提示设置 Wi-Fi。连网后设备自动进入绑定流程，显示 6 位绑定码
和二维码；平台确认绑定后进入主界面。主界面提供查看、设备呼叫、微信呼叫、AI 对讲和设置。

看到主界面只证明 UI 和基础板级启动成功。Wi-Fi、绑定、TiRTC、音视频和 AEC 要按功能
逐项确认；本次公开发布没有把这些目标板运行结果写成已通过。

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
docs/                  开发者上手与媒体架构
tools/                 静态分析和媒体日志工具
```

UI 只展示状态和分发动作；应用层编排生命周期，服务层实现业务，协议层持有连接，媒体层
处理帧，驱动层持有硬件。遇到问题时沿这条边界定位，通常比直接在 UI 或 SDK 回调里加补丁
更快。

## 文档索引

- [开发者上手指南](docs/GETTING_STARTED_CN.md)
- [ESP32-P4 媒体架构](docs/P4_MEDIA_ARCHITECTURE.md)
- [应用、SDK 和工具链版本](VERSION.md)
- [源码来源与公开边界](SOURCE_PROVENANCE.md)
- [统一仓固件与 Release 说明](../../../docs/RELEASES_CN.md)
