# 文档入口

第一次接入 TiRTC，建议先选择目标平台的最小 TiRTC 集成示例。需要完整交互设备或演示样机时，再进入
Device Monitor。

## SDK 接入路径

| 顺序 | 文档 | 解决的问题 |
| --- | --- | --- |
| 1 | [示例总览](../sdk-integration-examples/README.md) | 选择目标平台并了解验证边界 |
| 2 | [ESP32-S3 最小 TiRTC 集成示例](../sdk-integration-examples/esp32-s3/README.md) | ESP-IDF、芯片本地 Wi-Fi 与 TiRTC 基础链路 |
| 3 | [ESP32-P4 最小 TiRTC 集成示例](../sdk-integration-examples/esp32-p4/README.md) | ESP32-C6 Hosted/SDIO 与 P4 TiRTC 接入 |
| 4 | [G32S10X 最小 TiRTC 集成示例](../sdk-integration-examples/g32s10x/README.md) | 君正 SDK 集成、ATBM Wi-Fi 与测试媒体流 |

三个示例均包含来源说明。请先阅读项目中的 `SOURCE_PROVENANCE.md`，再根据 README 配置本地
凭据、构建并做真机验证。

## 完整应用路径

| 顺序 | 文档 | 解决的问题 |
| --- | --- | --- |
| 1 | [完整应用总览](../complete-applications/README.md) | 选择目标开发板 |
| 2 | [ESP32-S3 Device Monitor](../complete-applications/esp32-s3/device-monitor/README.md) | S3 烧录、绑定、功能和构建 |
| 3 | [ESP32-P4 Device Monitor](../complete-applications/esp32-p4/device-monitor/README.md) | P4 烧录、绑定、功能和构建 |
| 4 | [固件下载与校验](RELEASES_CN.md) | 固件在哪里下载、如何核验、为什么不放 Git |

完整应用首次体验路径：

```text
完整镜像 0x0 烧录 -> 连接 2.4 GHz Wi-Fi
-> H5 输入设备显示的 6 位验证码 -> 实时出图、出声
```

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

1. 在目标平台目录阅读 README 和版本说明。
2. 使用该目录声明的 ESP-IDF、工具链和 SDK 版本构建。
3. 最小 TiRTC 集成示例先验证当前网络实现、TiRTC 上线、连接、音视频和释放。
4. Device Monitor 再验证绑定、H5、AI、微信 VoIP、设备互呼和 OTA。
5. 分层记录编译证据与目标板证据，以对应日志、固件哈希和验证记录作为验收依据。

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
| [P4 TiRTC 适配](../complete-applications/esp32-p4/device-monitor/docs/TIRTC_P4_ADAPTATION.md) | P4 平台 SDK、网络和媒体适配 |
| [P4 测试指南](../complete-applications/esp32-p4/device-monitor/TEST_GUIDE.md) | 构建后如何做真机回归 |

## 版本与下载

- [版本与证据清单](VERSIONS_CN.md)
- [版本变更记录](CHANGELOG_CN.md)
- [固件下载与校验](RELEASES_CN.md)
