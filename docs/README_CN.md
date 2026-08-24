# 文档入口

第一次接入 TiRTC，建议按“最小集成 -> 最小系统体验 -> 完整应用”逐步选择：

1. 最小 TiRTC 集成示例用于理解 SDK 生命周期和基础音视频链路。
2. 最小系统例子用于观察 ThingConnect、AI 对讲和设备呼叫的协议状态。
3. 完整应用用于带屏设备、真实音频硬件和完整业务体验；摄像头是否参与 RTC 由各项目说明。

八个项目分别使用独立的“平台 + 项目 + 版本号”Tag。某个项目更新时，只增加该项目的新
Release，不再用日期批次同时代表整仓内容。当前版本和下载入口见[版本与证据清单](VERSIONS_CN.md)
与[固件下载与校验](RELEASES_CN.md)。

## 按手头任务找文档

| 你现在要做什么 | 从这里开始 |
| --- | --- |
| 下载已经构建好的 ESP32-S3/P4 固件 | [固件下载与校验](RELEASES_CN.md) |
| 第一次移植 TiRTC SDK | [最小 TiRTC 集成](../sdk-integration-examples/README.md) |
| 用串口观察配网、绑定、AI 和呼叫协议 | [ESP32-S3 最小系统例子](../minimal-system-examples/esp32-s3/README.md) |
| 在带屏 S3 板上运行完整业务 | [ESP32-S3 Device Monitor](../complete-applications/esp32-s3/device-monitor/README.md) |
| 在 P4+C6 板上构建完整业务 | [ESP32-P4 Device App](../complete-applications/esp32-p4/device-monitor/README.md) |
| 核对源码、SDK 和构建证据 | [版本与证据清单](VERSIONS_CN.md) |

每个项目 README 是主入口。先沿着其中的“准备 -> 配置 -> 构建或下载 -> 烧录 -> 首次启动”完成
一遍，再按需阅读架构和协议专题；这样不会一上来就掉进实现细节里。

## 最小 TiRTC 集成

| 文档 | 版本 | TiRTC SDK | 解决的问题 |
| --- | --- | --- | --- |
| [示例总览](../sdk-integration-examples/README.md) | - | - | 选择目标平台并了解验证边界 |
| [ESP32-S3 最小示例](../sdk-integration-examples/esp32-s3/README.md) | `1.2.0` | `2.2.1` | 芯片本地 Wi-Fi 与 TiRTC 基础链路 |
| [ESP32-P4 最小示例](../sdk-integration-examples/esp32-p4/README.md) | `1.1.1` | `2.2.1` | ESP32-C6 Hosted/SDIO 与 P4 TiRTC 接入 |
| [G32S10X 最小示例](../sdk-integration-examples/g32s10x/README.md) | `0.8.3` | `2.2.1` | 君正 SDK、ATBM Wi-Fi 与测试媒体流 |

## 最小系统体验

| 文档 | 版本 | TiRTC SDK | 解决的问题 |
| --- | --- | --- | --- |
| [最小系统例子总览](../minimal-system-examples/README.md) | - | - | 选择串口 AT 与完整协议体验入口 |
| [ESP32-S3 最小系统例子](../minimal-system-examples/esp32-s3/README.md) | `0.8.0` | `2.2.1` | 烧录、配网、绑定、网页查看、AI 对讲和设备呼叫 |
| [ESP32-P4 最小系统例子](../minimal-system-examples/esp32-p4/README.md) | `0.2.0` | `2.3.0` | P4+C6/C61、串口 AT、网页查看、AI 对讲和设备呼叫 |

## 完整应用

| 文档 | 版本 | TiRTC SDK | 解决的问题 |
| --- | --- | --- | --- |
| [完整应用总览](../complete-applications/README.md) | - | - | 选择目标开发板 |
| [ESP32-S3 Device Monitor](../complete-applications/esp32-s3/device-monitor/README.md) | `1.9.0` | `2.3.0 mini` | 绑定、H5 双向音频、小钛、微信、设备音频呼叫、二维码扫描和 OTA |
| [ESP32-P4 Device App](../complete-applications/esp32-p4/device-monitor/README.md) | `1.3.2` | `2.3.0` 定制兼容快照 | Hosted 网络、P4 媒体链路和完整业务 |
| [G32S10X Device Monitor](../complete-applications/g32s10x/device-monitor/README.md) | `0.1.1` | `2.2.1` | G32S10X 完整设备能力与君正平台适配 |
| [固件下载与校验](RELEASES_CN.md) | - | - | Release 下载、烧录入口和一致性校验 |

完整应用的典型体验路径：

```text
从 GitHub Releases 获取目标板资产 -> 使用平台烧录工具
-> 按项目说明联网和绑定 -> 验证对应业务功能
```

ESP32 使用 [Espressif ESP Tool](https://espressif.github.io/esptool-js/)；G32S10X 使用君正
Cloner。烧录文件和地址以当次 Release 的清单及项目说明为准。

## 功能和协议

设备端示例与
[tangeai/tirtc-server-example](https://github.com/tangeai/tirtc-server-example)
使用同一套 ThingConnect 契约。功能说明、字段、错误码和服务端行为以服务端仓文档为准：

- [设备上线与 MQTT 接入](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-integration.md)
- [H5 实时查看与按住说话](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-h5-live.md)
- [AI 对讲设备接入](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-ai.md)
- [微信 VoIP 对讲设备接入](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-voip.md)
- [设备呼设备接入](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-call.md)
- [统一会话模型](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-session-model.md)

## 验证边界

1. 先核对项目版本、TiRTC SDK、来源 Tag 和 commit。
2. 使用项目声明的 ESP-IDF、供应商 SDK 或工具链做干净构建。
3. 分开记录静态来源、构建、附件哈希、烧录、联网和业务验证。
4. 构建成功不能替代目标板、网络或音视频运行证明。

## 版本与下载

- [版本与证据清单](VERSIONS_CN.md)
- [版本变更记录](CHANGELOG_CN.md)
- [固件下载与校验](RELEASES_CN.md)
- [第三方组件与 SDK](THIRD_PARTY_NOTICES_CN.md)
