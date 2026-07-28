# TiRTC ESP32-P4 Device Monitor Demo Package Contents

本目录包含 ESP32-P4 设备监控端示例源码、TiRTC SDK 组件、P4 硬件适配、文档和
Release 资产生成流程。

## 主要内容

| 路径 | 内容 |
| --- | --- |
| `components/tirtc_sdk/` | TiRTC 2.2.0 头文件和 ESP32-P4 静态库 |
| `main/application/` | 应用生命周期、配置和业务状态 |
| `main/ui/` | 横屏 LVGL UI 和用户动作分发 |
| `main/services/` | 绑定、在线、音频、AI Chat、微信 VoIP、OTA、二维码等服务 |
| `main/protocols/tirtc/` | TiRTC SDK 适配、连接管理和媒体发送 |
| `main/media/` | 摄像头 H264 pipeline 和媒体策略 |
| `main/drivers/` | 摄像头、显示、触摸、音频等驱动封装 |
| `main/hardware/` | Waveshare P4 开发板初始化 |
| `docs/` | 架构说明和发布说明 |
| `tools/` | 本地构建、日志和 Release 打包工具 |

## 不包含

- 真实 Wi-Fi SSID 或密码。
- 真实设备 ID、设备密钥、access key、token。
- 串口日志、临时调试包、IDE 缓存和构建目录。
- 固件 `.bin/.zip`；正式固件位于 GitHub Releases。

## 发布产物

正式
[ESP32-P4 v1.1.0 Release](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-p4-trtc-advanced-v1.1.0)
应包含：

- 面向体验者的 `0x0` 完整镜像。
- 面向维护者的多地址 web-flash 压缩包。
- OTA app。
- `SHA256SUMS.txt`
- `release-manifest.json`
- 中文烧录说明。
- 当前源码 commit。
