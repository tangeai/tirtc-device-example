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
2. 从 `tirtc-device-examples-v2026.08.02.1` Release 下载
   `esp32p4-tirtc-device-monitor-full-v1.3.1.bin` 并核对 SHA-256。
3. 在 [Espressif Web Serial 烧录工具](https://espressif.github.io/esptool-js/)中选择 P4 端口，
   从 `0x0` 以 `16MB`、`DIO/80MHz` 烧录。
4. 重启后重新连接 2.4 GHz Wi-Fi 并完成绑定，再运行“网络测试”和“TiRTC 测试”。

完整镜像会清除现有 NVS、Wi-Fi 和绑定信息。需要开发时，检出统一仓 Tag
`tirtc-device-examples-v2026.08.02.1`，再按[开发者上手指南](docs/GETTING_STARTED_CN.md)
完成源码构建和分片烧录。

## 版本与平台

| 项目 | 当前值 |
| --- | --- |
| 应用版本 | `1.3.1` |
| 来源 Tag | `esp32-p4-device-app-v1.3.1` |
| 来源 commit | `739146438dd4b65512bb8198731bd2c8a1eb1275` |
| 目标开发板 | Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 |
| 主芯片 | ESP32-P4 |
| Wi-Fi 从芯片 | ESP32-C6，通过 ESP-Hosted/SDIO 连接 P4 |
| ESP-IDF | `5.5.4` |
| TiRTC SDK | `2.3.0` ESP32-P4 定制兼容快照 |
| FreeRTOS tick | `1000Hz` |
| 屏幕 | `480x320` 横屏触摸屏 |
| Flash | `16MB`，双 OTA 分区 |
| 发布形式 | 源码与 `esp32p4-tirtc-device-monitor-full-v1.3.1.bin` 完整镜像 |

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

## 1.3.1 重点变化

这次更新主要收紧媒体节拍、故障定位和资源边界，没有把“多打日志”当成修复：

- 摄像头帧按 V4L2 `sequence` 去重并排出旧完成帧；应用节拍按目标 fps 锁相推进，避免积压帧
  被当作新帧发送，也避免每次延迟后重新起算造成节拍漂移。
- GOP 统一按 2 秒计算：IPC `20fps` 对应 `40` 帧，通话 `15fps` 对应 `30` 帧。
- TinyH264 双任务模式增加同步通知保护。helper 优先级高于同步等待它的 decoder caller，通知忙时
  等待并发送原阶段值，避免直接覆盖或丢失阶段通知。
- 本地上行、远端接收和 renderer 增加分阶段 liveness：能区分采集、发送队列、SDK API、
  transport 首包、renderer 提交和 H264 decode 卡在哪一段。
- 内存策略增加 `normal`、`warning`、`critical` 水位，只在状态变化、恢复或新分配失败时记录，
  同时保留 internal、DMA、PSRAM 的 free、largest block 和历史低水位。
- 微信耗时操作改由固定 worker 串行处理；接听 worker 的大栈常驻 PSRAM，并用请求序号隔离
  已取消或已过期的接听任务。
- 可调媒体值集中到 `main/media/media_tuning.h` 和
  `main/services/call_video_renderer_config.h`。Kconfig 继续管理构建组成和硬件开关，不再让
  生成的 `sdkconfig` 充当运行时媒体参数的唯一来源。
- TiRTC SDK 保持公开 `2.3.0` 头文件契约，使用 Nano 基线 `aaad3da...` 并回移
  `fde4f1...` 的 HTTP DNS disable 修正，避免缓存 DNS 过期时出现递归锁。

TinyH264 当前可以识别超过 2 秒的 decode 阻塞并隔离后续媒体；只有阻塞调用最终返回后，
才能安全销毁 decoder 并从新 IDR 重建。第三方 decoder 永久不返回时的强制回收仍需长时间
真机验证，本文不会把这一边界写成已经解决。

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
firmware version: 1.3.1 project=tirtc_esp32p4_device_app ...
system ready: ESP32-P4 TiRTC dashboard
```

屏幕随后进入 Wi-Fi 或绑定流程。公开候选已在 ESP-IDF `5.5.4` 完成一次干净构建，应用镜像
为 `6,924,512` bytes，未出现编译错误。由该构建生成的 `16MB` 完整镜像只上传 GitHub
Release，不进入 Git。Wi-Fi、绑定、TiRTC、首帧、双向音频和长稳仍要在目标板逐项验证。

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
- [统一仓固件与 Release 说明](../../../docs/RELEASES_CN.md)
