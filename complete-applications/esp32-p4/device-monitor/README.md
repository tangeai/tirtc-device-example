# TiRTC ESP32-P4 Device Monitor

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://github.com/tangeai/tirtc-device-example/blob/main/LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.5.4-E7352C?logo=espressif)](https://docs.espressif.com/projects/esp-idf/)
[![Chip](https://img.shields.io/badge/Chip-ESP32--P4-000000)](https://www.espressif.com/en/products/socs/esp32-p4)
[![TiRTC SDK](https://img.shields.io/badge/TiRTC%20SDK-2.3.0-1769AA)](https://docs.tange.ai/products/tirtc/en/overview/what-is-tirtc.html)

这是面向 Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 的完整设备应用。ESP32-P4 负责摄像头、
音频、H264/JPEG 编解码、触摸屏和业务 UI；ESP32-C6 通过 ESP-Hosted/SDIO 提供 Wi-Fi。
工程包含设备绑定、ThingConnect、IPC、设备互呼、微信 VoIP、AI Chat 和 OTA。

项目同时提供源码和 `0x0` 完整烧录镜像。BIN 只放在 GitHub Release，不进入 Git 历史。

## 从这里开始

| 目标 | 入口 |
| --- | --- |
| 下载 16 MiB 完整镜像并直接体验 | [固件下载与校验](../../../docs/RELEASES_CN.md) |
| 安装环境、构建、烧录和完成首次联网 | [开发者上手指南](docs/GETTING_STARTED_CN.md) |
| 理解视频、音频、PSRAM、SDIO 和连接归属 | [P4 媒体架构](docs/P4_MEDIA_ARCHITECTURE.md) |
| 核对应用、SDK、工具链和静态库哈希 | [版本契约](VERSION.md) |
| 核对开发来源、公开筛选和验证边界 | [源码来源](SOURCE_PROVENANCE.md) |

直接体验时，从项目 [GitHub Releases](https://github.com/tangeai/tirtc-device-example/releases)
下载 `esp32p4-tirtc-device-monitor-full-v1.5.0.bin`、`SHA256SUMS.txt` 和
`release-manifest.json`。先核对 SHA-256，再用
[Espressif ESP Tool](https://espressif.github.io/esptool-js/) 从 `0x0` 烧录完整镜像。

需要改代码时，从公开 Tag 开始：

```powershell
git clone https://github.com/tangeai/tirtc-device-example.git
cd tirtc-device-example
git checkout esp32-p4-device-monitor-v1.5.0
cd complete-applications/esp32-p4/device-monitor
```

## 版本身份

| 项目 | 当前值 |
| --- | --- |
| 应用版本 | `1.5.0` |
| 发布日期 | `2026-08-28` |
| 开发来源 | `esp32-p4-device-app-v1.5.0` / `97331363a0d3c4e6f89b5b93d39561e74087ba2a` |
| 公开 Tag | `esp32-p4-device-monitor-v1.5.0` |
| 目标板 | Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 |
| 网络 | ESP32-C6 + ESP-Hosted/SDIO |
| ESP-IDF | `5.5.4` |
| TiRTC SDK | `2.3.0` 官方源码重建版，含 P4 传输稳定性修复 |
| 屏幕 | `480x320` 横屏触摸屏 |
| Flash | `16MB`，双 OTA 分区 |

ESP32-P4 没有原生 Wi-Fi。P4 固件构建和烧录只更新 P4；Wi-Fi 扫描始终为空时，应先检查
C6 slave 固件、SDIO 连线和 reset，不要先改 TiRTC 或摄像头代码。

## 主要能力

- 屏幕配网，自动获取绑定码，绑定后维持 ThingConnect、MQTT 和 TiRTC 在线。
- IPC 使用 OV5647 `1280x960` YUV420 与 P4 H264 硬件编码上行。
- 设备视频呼叫使用最高 `384x256` 的双向 H264，PCMA 双向音频。
- 微信 VoIP 使用 `480x320@15fps` H264 上行、`640x480` MJPEG 下行和 PCMA 双向音频。
- AI Chat 支持音频、可选 H264 视频、字幕、打断、联系人查询和呼叫动作。
- `480x320` LVGL 页面覆盖 Wi-Fi、绑定、联系人、呼叫、设置和 OTA。
- 应用生命周期统一管理摄像头、显示、音频、AEC、PSRAM 池和 TiRTC 连接。

## 1.5.0 更新

- TiRTC `2.3.0` P4 重建版接入 TGMP 码率反馈。SDK 只给出绝对目标码率，应用控制任务再调整
  H264 编码器；旧的本地队列压力自动降级保持关闭，避免两套控制器争夺码率。
- 跨 APP 切换和重复媒体会话共用一套资源所有权。摄像头 USERPTR、H264 参考帧/去块、解码、
  显示和 AEC 大块工作区使用持久 PSRAM 池，退出后按会话代际清理状态而不是反复申请大块内存。
- 设备呼叫视频档位为 `384x256@12fps`、`256kbps`；H264 压缩输入池扩展为
  `24 x 256KB`，解码池 4 帧、RGB 输出池 20 帧，自适应播放队列最大深度 16 帧。
- 微信 VoIP 设备上行使用 `480x320@15fps`、`480kbps` H264；下行仍请求 `640x480` MJPEG，
  P4 硬件解码后一次 `cover` 到 `480x320`。控制层 5 秒后隐藏，点击视频恢复。
- 音频链路补充采集增益、AEC 双讲近端保护和播放优化；设备与微信联系人增加待确认、备注和
  删除流程，二维码预览兼容灰度、RGB565 与 packed YUV420。
- Wi-Fi 后台恢复、结构化网络时延/抖动/丢包指标以及默认开启的串口回归 CLI，便于从网络、
  APP、呼叫、媒体和资源水位逐层定位第一处异常。

SDK 源码基线为 `tag.v1.5.12`，但当前静态库的嵌入 TGTRP BuildInfo 仍显示
`tagv1.5.11`。本项目把源码基线、运行时 BuildInfo 和静态库 SHA-256 分开记录，不用一个
版本字符串替代完整 SDK 身份。

## 默认媒体参数

方向均以 P4 设备为参照。

| 场景 | 当前源码默认值 |
| --- | --- |
| IPC 上行 | `1280x960@20fps`，`4Mbps`，H264，名义 GOP `40` 帧 / `2s` |
| 设备呼叫上行 | `384x256@12fps`，`256kbps`，H264，名义 GOP `192` 帧 / `16s` |
| 设备呼叫下行 | constrained-baseline H264，解码上限 `384x256`，显示到 `480x320` |
| 微信上行 | `480x320@15fps`，`480kbps`，H264，名义 GOP `30` 帧 / `2s` |
| 微信下行 | 请求 `640x480` MJPEG，P4 JPEG 硬解后 `cover` 到 `480x320` |
| 设备 H264 下行池 | 输入 `24 x 256KB`，decoded `4` 帧，output `20` 帧，均在 PSRAM |
| AEC | 默认开启 |
| SDK/TGMP 码率控制 | 默认开启；设备呼叫注册范围 `96-256kbps` |
| 本地自动弱网降级 | 默认关闭 |

16 秒是设备呼叫的名义 GOP。流开始、订阅恢复、传输恢复和对端请求仍会触发关键帧，不能把
名义 GOP 理解成故障时必须等待 16 秒。微信手机端的采集分辨率由微信和服务端决定，P4 源码
没有配置或证明手机端为 720p。

## 配置与构建

普通使用不需要把凭据写进源码：Wi-Fi 由屏幕配置并保存到 NVS，设备身份由绑定流程下发。
`main/application/app_config.h` 只保留空凭据和服务入口。硬件开关在
`main/Kconfig.projbuild`，稳定默认值在 `sdkconfig.defaults`，媒体参数在
`main/media/media_tuning.h`。

进入 ESP-IDF `5.5.4` 环境后执行：

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py set-target esp32p4
idf.py reconfigure
idf.py build
```

本地开发烧录必须读取当前 `build/flasher_args.json` 的文件和 offset。应用 BIN 不能单独写到
`0x0`；只有 Release 中 16 MiB 的 `full` 镜像可从 `0x0` 直接烧录。

## 启动与验证

复位后先确认身份：

```text
firmware version: 1.5.0 project=tirtc_esp32p4_device_app ...
system ready: ESP32-P4 TiRTC dashboard
```

开发侧已有双设备呼叫、AI、IPC 重复切换和内存恢复记录。最新持久 PSRAM 池调整后，没有
重新完成微信小程序外部实呼和弱网矩阵；当前连接在 COM7/COM11 上的启明板仍是旧版
`1.3.2`，也不属于本版本证据。公开 Release 的正式构建与这些开发侧运行记录分开验收。

正式 Release 的源码 commit、构建命令、解析依赖、应用镜像、分区余量、完整镜像和 SHA-256
统一记录在 `release-manifest.json`。构建成功仍需与烧录、C6/SDIO、联网、绑定、媒体和长稳
分别验收。

## 目录

```text
components/tirtc_sdk/  TiRTC 头文件、静态库和版本契约
main/application/      生命周期、业务状态和资源所有权
main/connectivity/     网络状态和 Wi-Fi 管理
main/drivers/          音频、摄像头、显示及测试媒体驱动
main/hardware/         Waveshare P4 板级初始化
main/media/            摄像头 pipeline、媒体调优和像素转换
main/platform/         存储、时间、日志和内存水位策略
main/protocols/        HTTPS、MQTTS、RTC 和 TiRTC 适配
main/services/         绑定、在线、呼叫、VoIP、AI、IPC 和 OTA
main/ui/               LVGL 页面、布局和资源
docs/                  上手指南和媒体架构
tools/                 静态分析和媒体日志工具
```

UI 只展示状态和分发动作；应用层编排生命周期，服务层实现业务，协议层持有连接，媒体层
处理帧，驱动层持有硬件。排障时沿这条链路找第一处异常，比在 UI 或 SDK 回调中增加特殊判断
更可靠。
