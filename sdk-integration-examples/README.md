# SDK 接入与历史示例

本目录提供面向移植和 SDK 学习的最小 TiRTC 集成示例。它们聚焦联网、时间同步、TiRTC
初始化、上线、连接、音视频收发、订阅控制和资源释放，不包含完整产品业务。

## 当前示例

| 平台 | 示例 | 版本 | TiRTC SDK | 构建环境 | 候选验证状态 |
| --- | --- | --- | --- | --- | --- |
| ESP32-S3 | [最小 TiRTC 集成示例](esp32-s3/README.md) | `1.1.1` | `2.2.1` | ESP-IDF `5.5.4` | 头文件与 Tag `v2.2.1` 一致；静态库自报 commit `3a33bf4ae51b` |
| ESP32-P4 | [最小 TiRTC 集成示例](esp32-p4/README.md) | `1.0.2` | `0.1.4` | ESP-IDF `5.5.4` | 静态库与 2026-07-01 归档正式包逐字节一致 |
| G32S10X | [最小 TiRTC 集成示例](g32s10x/README.md) | `0.8.1` | `2.2.0` | Ingenic SDK `im_sdk_v0.4.0` | 已在隔离干净 SDK 副本构建；未真机验证 |

## 如何选择

- ESP32-S3 使用芯片本地 Wi-Fi，适合快速理解 ESP-IDF 下的 TiRTC 基础接入。
- ESP32-P4 通过 ESP32-C6 Hosted/SDIO 提供 Wi-Fi，适合 P4 网络和媒体链路移植。
- G32S10X 使用 ATBM Wi-Fi、lwIP/NTP 与内嵌 MJPEG/PCMA 测试流，供应商 SDK 和工具链需单独获取。

每个项目的 `SOURCE_PROVENANCE.md` 分开记录源路径、基线 commit、源工作树状态、公开筛选范围
和当前验证边界。三个候选都来自源工作树快照，不能只用源 HEAD 推断候选的完整内容。

项目中的凭据字段只保留占位符。请在本地配置自己的 Wi-Fi、设备身份和 Token 参数，真实
Secret 不应进入 Git。
