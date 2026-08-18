# 完整应用

本目录保存面向开发板或产品原型的完整应用。与最小 SDK 接入示例相比，完整应用通常还包括：

- Wi-Fi、时间同步和设备身份。
- ThingConnect HTTP/MQTT 协议。
- 设备绑定、在线状态和业务会话。
- 摄像头、麦克风、扬声器、显示和触摸。
- H5 实时查看、AI 对讲、微信 VoIP、设备互呼和 OTA。
- 面向开发者的配置、构建、烧录和验证说明。

当前项目：

| 平台 | 项目 | 版本 | 入口 |
| --- | --- | --- | --- |
| ESP32-S3 | Device Monitor | `1.8.1` | [README](esp32-s3/device-monitor/README.md) |
| ESP32-P4 | Device App | `1.3.2` | [README](esp32-p4/device-monitor/README.md) |
| G32S10X | Device Monitor | `0.1.1` | [README](g32s10x/device-monitor/README.md) |

ESP32-P4 Device App `1.3.2` 提供源码和 `0x0` 完整镜像。所有正式构建产物只通过
[GitHub Releases](https://github.com/tangeai/tirtc-device-example/releases) 分发，不进入 Git 历史。
