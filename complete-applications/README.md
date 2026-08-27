# 完整应用

本目录保存面向开发板或产品原型的完整应用。与最小 SDK 接入示例相比，完整应用通常还包括：

- Wi-Fi、时间同步和设备身份。
- ThingConnect HTTP/MQTT 协议。
- 设备绑定、在线状态和业务会话。
- 按平台使用麦克风、扬声器、显示、触摸和摄像头；具体媒体能力以项目 README 为准。
- H5 实时查看、AI 对讲、微信 VoIP、设备互呼和 OTA。
- 面向开发者的配置、构建、烧录和验证说明。

当前项目：

| 平台 | 项目 | 版本 | 入口 |
| --- | --- | --- | --- |
| ESP32-S3 | Device Monitor | `1.9.6` | [README](esp32-s3/device-monitor/README.md) |
| ESP32-P4 | Device Monitor | `1.5.0` | [README](esp32-p4/device-monitor/README.md) |
| G32S10X | Device Monitor | `0.1.1` | [README](g32s10x/device-monitor/README.md) |

ESP32-P4 Device Monitor `1.5.0` 面向 P4+C6 带屏设备，提供 IPC、AI 对讲、微信 VoIP、
设备互呼和真实摄像头/音频链路。设备互呼使用最高 `384x256` 双向 H264；微信下行请求
`640x480` MJPEG，并在 P4 上硬解、裁切和缩放到 `480x320`。TGMP 码率控制、持久 PSRAM
媒体池和跨 APP 生命周期按同一资源所有权运行。源码和 `0x0` 完整镜像按同一
项目版本发布；所有正式构建产物只通过
[GitHub Releases](https://github.com/tangeai/tirtc-device-example/releases) 分发，不进入 Git 历史。

ESP32-S3 Device Monitor `1.9.6` 是带屏双向音频设备应用：Web IPC、小钛、微信 VoIP 和
设备互呼使用真实麦克风与扬声器，摄像头只用于二维码扫描。它使用 TiRTC SDK
`2.3.0 mini`，改善冷启动呼叫准备、呼叫状态、联系人名称输入和音量触摸反馈；服务发现、
业务 HTTP、设备 MQTT 和 TiRTC HTTPS 均使用认证传输。Release 提供 `0x0` 完整镜像与 OTA app。
当前 Web IPC 和设备互呼仍可能听到轻微“沙沙电流声”，根因尚未闭环，详见项目 README。
