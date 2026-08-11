# TiRTC ESP32-P4 完整设备应用

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://github.com/tangeai/tirtc-device-example/blob/main/LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.5.4-E7352C?logo=espressif)](https://docs.espressif.com/projects/esp-idf/)
[![Chip](https://img.shields.io/badge/Chip-ESP32--P4-000000)](https://www.espressif.com/en/products/socs/esp32-p4)
[![TiRTC SDK](https://img.shields.io/badge/TiRTC%20SDK-2.3.0-1769AA)](https://docs.tange.ai/products/tirtc/en/overview/what-is-tirtc.html)

这是面向 Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 的完整设备应用。它把 P4 的摄像头、
H264/JPEG 编解码、触摸屏和音频设备，与 C6 提供的远程 Wi-Fi、TiRTC、ThingConnect、
微信 VoIP 和 AI Chat 组合成一套可以继续开发的工程。

本项目同时提供源码和 `0x0` 完整烧录镜像。想先体验功能可以直接下载 BIN；需要改代码时，
再按 ESP-IDF 流程构建。BIN 只在 GitHub Release 中分发，不进入 Git 历史。

## 从这里开始

| 你现在要做什么 | 直接阅读 |
| --- | --- |
| 下载完整镜像，直接烧录体验 | [固件下载与校验](../../../docs/RELEASES_CN.md) |
| 配环境、构建、烧录并完成首次联网 | [开发者上手指南](docs/GETTING_STARTED_CN.md) |
| 理解摄像头、编解码、PSRAM、AEC 和连接归属 | [P4 媒体架构](docs/P4_MEDIA_ARCHITECTURE.md) |
| 核对应用、SDK、工具链和静态库哈希 | [VERSION.md](VERSION.md) |
| 核对源码来自哪个 Tag、公开时排除了什么 | [SOURCE_PROVENANCE.md](SOURCE_PROVENANCE.md) |

直接体验：

1. 确认 P4 主芯片和 C6 Wi-Fi 从芯片都能正常上电。
2. 从 [`esp32-p4-device-monitor-v1.3.2` Release](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-p4-device-monitor-v1.3.2) 下载
   `esp32p4-tirtc-device-monitor-full-v1.3.2.bin` 并核对 SHA-256。
3. 在 [Espressif Web Serial 烧录工具](https://espressif.github.io/esptool-js/)中选择 P4 端口，
   从 `0x0` 以 `16MB`、`DIO/80MHz` 烧录。
4. 重启后重新连接 2.4 GHz Wi-Fi 并完成绑定，再运行“网络测试”和“TiRTC 测试”。

完整镜像会清除现有 NVS、Wi-Fi 和绑定信息。需要开发时，检出项目 Tag
`esp32-p4-device-monitor-v1.3.2`，再按[开发者上手指南](docs/GETTING_STARTED_CN.md)
完成源码构建和分片烧录。

## 版本与平台

| 项目 | 当前值 |
| --- | --- |
| 应用版本 | `1.3.2` |
| 来源 Tag | `esp32-p4-device-app-v1.3.2` |
| 来源 commit | `bc1ae8fbd9b64090503128985129a72e024c0551` |
| 目标开发板 | Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 |
| 主芯片 | ESP32-P4 |
| Wi-Fi 从芯片 | ESP32-C6，通过 ESP-Hosted/SDIO 连接 P4 |
| ESP-IDF | `5.5.4` |
| TiRTC SDK | `2.3.0` ESP32-P4 定制兼容快照 |
| FreeRTOS tick | `1000Hz` |
| 屏幕 | `480x320` 横屏触摸屏 |
| Flash | `16MB`，双 OTA 分区 |
| 发布形式 | 源码与 `esp32p4-tirtc-device-monitor-full-v1.3.2.bin` 完整镜像 |

## P4 和 C6 各自负责什么

ESP32-P4 不带原生 Wi-Fi。本项目让 ESP32-C6 运行 ESP-Hosted slave，通过 4-bit、40 MHz
SDIO 为 P4 提供远程 Wi-Fi：

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

因此“P4 固件构建成功”和“Wi-Fi 实际可用”是两项证据。P4 烧录只更新 P4；如果 Wi-Fi
扫描始终为空，先检查 C6 slave 固件、SDIO 连线和 reset，不要先改 TiRTC 或摄像头代码。

## 主要能力

- 屏幕连接 Wi-Fi，自动获取 6 位绑定码，绑定后维持 ThingConnect 和 TiRTC 在线。
- IPC 使用 OV5647 `1280x960` YUV420 和 P4 H264 硬件编码上行。
- 设备间呼叫支持音频/视频呼叫、H264 双向视频和 PCMA 双向音频。
- 微信 VoIP 支持设备侧 H264 上行、服务端 MJPEG 下行和 PCMA 双向音频。
- AI Chat 支持独立音频流、可选 H264 视频流、字幕、打断，以及联系人查询和呼叫动作。
- `480x320` LVGL 界面覆盖 Wi-Fi、绑定、联系人、呼叫、设置和 OTA。
- 应用生命周期统一管理摄像头、显示、音频、AEC、PSRAM pool 和 TiRTC 连接。

## 1.3.2 重点变化

本版集中收紧持久化和 RTC 连接生命周期。TiRTC SDK 仍为 `2.3.0` 定制兼容快照，摄像头、
编解码和默认媒体参数没有变化。

- 新增 `main/platform/platform_nvs_async.c`。运行时 NVS 写入统一进入 internal-RAM worker，
  每个请求按 `open -> set/erase -> commit -> close` 串行完成；调用方即使使用 PSRAM task stack，
  也不会直接在该栈上执行 flash/NVS 操作。
- 设备 UUID、音量、AI 头像、RTC 设备身份和绑定 pending session 均复用这条持久化路径。
  需要立即确认落盘的配置使用等待接口；凭证 blob 提交成功后才更新应用状态。
- 服务端要求重置绑定时，回调只投递 `DEVICE_REBIND_REQUIRED` 事件，真正的重绑定由 APP
  control task 执行，避免服务回调跨层进入绑定和 RTC 生命周期。
- WHIP 提交增加 attempt ID 和原子占位。同一时刻只允许一次连接提交；SDK 回调返回后才释放
  该占位，网络离线或 SDK 停止时也会清理它。
- 连接接受路径区分“应拒绝的新连接”和“已经进入关闭流程的过期回调”。后者直接忽略，
  不再次销毁同一 SDK 连接；按具体句柄断连时，重复请求保持幂等。
- AI Chat 在申请 Token 前先等待 RTC 进入可建立新连接的状态，并持续核对 session generation；
  已取消或已过期的启动流程不能继续提交连接。

`1.3.1` 已有的媒体节拍、TinyH264 同步保护、分阶段 liveness 和内存水位诊断继续保留。
这些连接与持久化改动已完成源码静态收口；烧录、重复绑定、连续呼叫和长稳结果仍应在目标板
逐项记录，本文不把未执行的真机检查写成已通过。

## 默认媒体参数

上行和下行均以 P4 设备为参照。

| 场景 | 当前源码默认值 |
| --- | --- |
| P4 -> 服务端，IPC | `1280x960@20fps`，`4Mbps`，H264，GOP `40` 帧 / `2s` |
| P4 -> 服务端，设备呼叫/微信 VoIP | `480x320@15fps`，`800kbps` 起始，H264，GOP `30` 帧 / `2s` |
| 服务端 -> P4，微信 VoIP | 请求 `640x480` MJPEG，P4 JPEG 硬解后居中 `cover` 到 `480x320` |
| H264 下行池 | 输入 `16 x 256KB`；decoded/output 各 `4` 个 RGB565 slot |
| AEC | 默认开启 |
| SDK 码率自适应 | 默认关闭 |
| 本地自动弱网降级 | 默认关闭 |

这里没有“微信上行 720p”的配置。P4 能确定的是设备侧上行和设备侧下行请求；微信手机端
采集分辨率由微信和服务端链路决定，不能从 P4 源码反推出 720p。

## 配置、构建和烧录

普通使用无需把 Wi-Fi 密码或 TiRTC 密钥写进源码：

- Wi-Fi 在屏幕“设置 -> Wi-Fi 设置”中连接并保存到 NVS。
- 设备身份由绑定流程下发；“设置 -> TiRTC 配置”用于查看状态或重置绑定。
- `main/application/app_config.h` 只保留空值和服务入口，不应提交真实凭据。
- 硬件和构建开关位于 `main/Kconfig.projbuild` 与 `sdkconfig.defaults`。
- 运行时媒体参数位于上述两个集中配置头文件，修改后需要重新构建。

进入 ESP-IDF `5.5.4` 环境后执行：

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py set-target esp32p4
idf.py reconfigure
idf.py build
```

烧录时打开 [Espressif Web Serial 烧录工具](https://espressif.github.io/esptool-js/)，按
`build/flasher_args.json` 添加全部 BIN 和 offset。不要把单独的应用 BIN 写到 `0x0`，
也不要照抄其他项目的 offset。完整步骤和常见错误见
[开发者上手指南](docs/GETTING_STARTED_CN.md)。

## 第一次启动应该看到什么

串口先确认固件身份：

```text
firmware version: 1.3.2 project=tirtc_esp32p4_device_app ...
system ready: ESP32-P4 TiRTC dashboard
```

屏幕随后进入 Wi-Fi 或绑定流程。`1.3.2` 正式构建的应用镜像大小为
`6,927,360` bytes，SHA-256 为 `2df6d9d626a05f19a4fd1f15eb854c54119a32ccd475090f6713f2629afc90e2`；由同一构建生成的 `16MB`
完整镜像大小为 `16,777,216` bytes，SHA-256 为 `87bfb67d1ba30d7f79663f63891e29f7f4f4367c80ff0d5cecb1b46f301d40e9`。完整镜像
只上传 GitHub Release，不进入 Git。Wi-Fi、绑定、TiRTC、首帧、双向音频和长稳仍要在目标板
逐项验证。

## 目录

```text
components/tirtc_sdk/  TiRTC 头文件、静态库和版本契约
main/application/      生命周期、业务状态和资源所有权
main/connectivity/     网络状态和 Wi-Fi 管理
main/drivers/          音频、摄像头、显示及测试媒体驱动
main/hardware/         Waveshare P4 板级初始化
main/media/            摄像头 pipeline、媒体调优和像素转换
main/platform/         存储、时间、日志和内存水位策略
main/protocols/        HTTP、MQTT、RTC 和 TiRTC 适配
main/services/         绑定、在线、呼叫、VoIP、AI、IPC 和 OTA
main/ui/               LVGL 页面、布局和资源
docs/                  开发者上手与媒体架构
tools/                 静态分析和媒体日志工具
```

UI 只展示状态和分发动作；应用层编排生命周期，服务层实现业务，协议层持有连接，媒体层
处理帧，驱动层持有硬件。遇到问题时沿这条链路找第一处异常，比直接在 UI 或 SDK 回调中
增加特殊判断更可靠。

## 文档索引

- [开发者上手指南](docs/GETTING_STARTED_CN.md)
- [ESP32-P4 媒体架构](docs/P4_MEDIA_ARCHITECTURE.md)
- [应用、SDK 和工具链版本](VERSION.md)
- [源码来源与公开边界](SOURCE_PROVENANCE.md)
- [固件与 Release 说明](../../../docs/RELEASES_CN.md)
