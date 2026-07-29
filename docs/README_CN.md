# 文档入口

第一次接入 TiRTC，建议先选择目标平台的最小 TiRTC 集成示例。需要完整交互设备或演示样机时，
再进入对应平台的完整应用。

本次统一 Release 覆盖 ESP32-S3、ESP32-P4、G32S10X 三个平台，共三个最小 TiRTC 集成示例
和三个完整应用。六个项目的来源 Tag、commit 和版本均已锁定；五个固件项目从统一源码
commit 完成正式干净构建，P4 APP 按源码范围交付。

## SDK 接入路径

| 顺序 | 文档 | 版本 | TiRTC SDK | 解决的问题 |
| --- | --- | --- | --- | --- |
| 1 | [示例总览](../sdk-integration-examples/README.md) | - | - | 选择目标平台并了解验证边界 |
| 2 | [ESP32-S3 最小 TiRTC 集成示例](../sdk-integration-examples/esp32-s3/README.md) | `1.2.0` | `2.2.1` | ESP-IDF、芯片本地 Wi-Fi 与 TiRTC 基础链路 |
| 3 | [ESP32-P4 最小 TiRTC 集成示例](../sdk-integration-examples/esp32-p4/README.md) | `1.1.1` | `2.2.1` | ESP32-C6 Hosted/SDIO 与 P4 TiRTC 接入 |
| 4 | [G32S10X 最小 TiRTC 集成示例](../sdk-integration-examples/g32s10x/README.md) | `0.8.3` | `2.2.1` | 君正 SDK 集成、ATBM Wi-Fi 与测试媒体流 |

三个示例均包含来源说明。请先阅读项目中的 `SOURCE_PROVENANCE.md`，再根据 README 配置本地
凭据、构建并做真机验证。

## 完整应用路径

| 顺序 | 文档 | 版本 | TiRTC SDK | 解决的问题 |
| --- | --- | --- | --- | --- |
| 1 | [完整应用总览](../complete-applications/README.md) | - | - | 选择目标开发板 |
| 2 | [ESP32-S3 Device Monitor](../complete-applications/esp32-s3/device-monitor/README.md) | `1.7.6` | `2.2.0` | S3 绑定、交互、媒体和 OTA |
| 3 | [ESP32-P4 Device App](../complete-applications/esp32-p4/device-monitor/README.md) | `1.2.3` | `2.3.0` | P4 Hosted 网络、交互和媒体链路 |
| 4 | [G32S10X Device Monitor](../complete-applications/g32s10x/device-monitor/README.md) | `0.1.1` | `2.2.1` | G32S10X 完整设备能力与君正平台适配 |
| 5 | [固件下载与校验](RELEASES_CN.md) | - | - | Release 下载、烧录入口和一致性校验 |

完整应用的典型体验路径：

```text
从 GitHub Releases 获取目标板资产 -> 使用平台烧录工具
-> 按项目说明联网和绑定 -> 验证对应业务功能
```

ESP32-S3/P4 使用 [Espressif ESP Tool](https://espressif.github.io/esptool-js/)；
G32S10X 使用君正 Cloner。烧录文件和地址以当次 Release 的校验清单及项目说明为准。

## 功能和协议路径

设备端示例与
[tangeai/tirtc-server-example](https://github.com/tangeai/tirtc-server-example)
使用同一套 ThingConnect 契约。功能说明、接口字段、错误码和服务端行为以以下文档为准：

- [设备上线与 MQTT 接入](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-integration.md)
- [H5 实时查看与按住说话](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-h5-live.md)
- [AI 对讲设备接入](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-ai.md)
- [微信 VoIP 对讲设备接入](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-voip.md)
- [设备呼设备接入](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-call.md)
- [统一会话模型](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-session-model.md)
- [API Reference](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/api-reference.md)

## 开发者验证

1. 在目标平台目录阅读 README、版本说明和来源信息。
2. 核对项目版本、TiRTC SDK、来源 Tag 和 commit。
3. 使用该目录声明的 ESP-IDF、供应商 SDK 或工具链构建。
4. 最小 TiRTC 集成示例先验证当前网络实现、TiRTC 上线、连接、音视频和释放。
5. 完整应用再验证绑定、媒体与各项业务能力。
6. 分层记录静态来源、干净构建、固件资产和目标板证据，以实际日志、固件哈希和验证记录
   作为验收依据。

### ESP32-S3 深入文档

| 文档 | 解决的问题 |
| --- | --- |
| [架构分层](../complete-applications/esp32-s3/device-monitor/docs/architecture/layering.md) | UI、应用、服务、协议和驱动的职责边界 |
| [音频与媒体所有权](../complete-applications/esp32-s3/device-monitor/docs/architecture/audio_media_ownership.md) | H5、AI、微信 VoIP、设备互呼如何共享硬件资源 |
| [AI 对讲](../complete-applications/esp32-s3/device-monitor/docs/ai_chat/README_CN.md) | AI 凭证、WHIP 会话、字幕和媒体链路 |
| [ThingConnect 对齐](../complete-applications/esp32-s3/device-monitor/docs/THING_CONNECT_ALIGNMENT_CN.md) | 设备实现与服务端接口、消息和状态机的对应关系 |

### ESP32-P4 深入文档

| 文档 | 解决的问题 |
| --- | --- |
| [P4 媒体架构](../complete-applications/esp32-p4/device-monitor/docs/P4_MEDIA_ARCHITECTURE.md) | 摄像头、H264、解码、显示和内存边界 |
| [P4 项目说明](../complete-applications/esp32-p4/device-monitor/README.md) | P4 平台 SDK、Hosted 网络、功能和构建入口 |
| [P4 版本契约](../complete-applications/esp32-p4/device-monitor/VERSION.md) | 应用版本、TiRTC SDK 和来源信息 |

## 版本与下载

- [版本与证据清单](VERSIONS_CN.md)
- [版本变更记录](CHANGELOG_CN.md)
- [固件下载与校验](RELEASES_CN.md)
- [第三方组件与 SDK](THIRD_PARTY_NOTICES_CN.md)
