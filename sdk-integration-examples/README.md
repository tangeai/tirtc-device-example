# SDK 接入与历史示例

本目录提供面向移植和 SDK 学习的最小 TiRTC 集成示例，聚焦联网、时间同步、TiRTC 初始化、
上线、连接、音视频收发、订阅控制和资源释放。需要串口 AT 与结构化最小系统体验时，请进入
[最小系统例子](../minimal-system-examples/README.md)。

## 当前示例

| 平台 | 示例 | 版本 | TiRTC SDK | 构建环境 | 来源版本 |
| --- | --- | --- | --- | --- | --- |
| ESP32-S3 | [最小 TiRTC 集成示例](esp32-s3/README.md) | `1.2.0` | `2.2.1` | ESP-IDF `5.5.4` | Tag `v1.2.0` / commit `44b24a5` |
| ESP32-P4 | [最小 TiRTC 集成示例](esp32-p4/README.md) | `1.1.1` | `2.2.1` | ESP-IDF `5.5.4` | Tag `v1.1.1` / commit `0f36ddb` |
| G32S10X | [最小 TiRTC 集成示例](g32s10x/README.md) | `0.8.3` | `2.2.1` | Ingenic SDK `im_sdk_v0.4.0` | Tag `v0.8.3` / commit `b2abc22` |

## 如何选择

- ESP32-S3 使用芯片本地 Wi-Fi，适合快速理解 ESP-IDF 下的 TiRTC 基础接入。
- ESP32-P4 通过 ESP32-C6 Hosted/SDIO 提供 Wi-Fi，适合 P4 网络和媒体链路移植。
- G32S10X 使用 ATBM Wi-Fi、lwIP/NTP 与内嵌 MJPEG/PCMA 测试流，供应商 SDK 和工具链需单独获取。

每个项目的 `SOURCE_PROVENANCE.md` 分开记录不可移动的来源 Tag、完整 commit、公开筛选范围
和验证边界。正式构建产物只随
[GitHub Releases](https://github.com/tangeai/tirtc-device-example/releases) 分发，不进入 Git 历史。

项目中的凭据字段只保留占位符。请在本地配置自己的 Wi-Fi、设备身份和 Token 参数，真实
Secret 不应进入 Git。
