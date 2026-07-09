# TiRTC ESP32-P4 Device Monitor Demo Version

| 项目 | 内容 |
| --- | --- |
| 示例工程 | TiRTC ESP32-P4 Device Monitor Demo |
| 示例版本 | 1.0.2 |
| 发布时间 | 2026-07-09 |
| 目标芯片 | ESP32-P4 |
| 目标开发板 | Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 |
| ESP-IDF | 5.5.4 |
| TiRTC SDK 版本 | 2.2.0 |
| TiRTC SDK MD5 | 23661a4ee27debc667f904f7118dd270 |
| TiRTC SDK SHA256 | 97d4285474e301f5e2846c837241632b9d2bca09a64d96d2ec385c6c68c40ec6 |

## 版本能力

- 设备绑定、在线、TiRTC IPC、微信 VoIP、AI Chat、OTA 和设置页。
- P4 横屏 UI，物理显示 `320x480`，UI 视口 `480x320`。
- 摄像头链路使用 YUV420 到 H264 的 P4 硬编路径。
- 默认 RTC 视频目标为 `1920x1080@20fps`，目标码率 `6Mbps`。
- 默认关闭弱网自动降级，保留手动调节分辨率、帧率、码率接口。
- 默认开启连接后主动推流。

## 安全说明

仓库内默认配置只保留空值或服务默认地址，不提交真实 Wi-Fi 密码、设备密钥、access key、token 或个人账号。
