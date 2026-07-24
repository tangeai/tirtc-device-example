# 文档入口

第一次拿到设备，请先走“普通体验路径”。需要编译、适配或发布时，再进入开发者路径。

## 普通体验路径

| 顺序 | 文档 | 解决的问题 |
| --- | --- | --- |
| 1 | [仓库首页](../README.md) | 选择开发板，从下载固件到 H5 出图、出声 |
| 2 | [ESP32-S3 使用说明](../esp32-s3/trtc-advanced/README.md) | S3 开发板、烧录、绑定、功能和构建 |
| 3 | [ESP32-P4 使用说明](../esp32-p4/trtc-advanced/README.md) | P4 开发板、烧录、绑定、功能和构建 |
| 4 | [发布与二进制管理](RELEASES_CN.md) | 固件在哪里下载、如何核验、为什么不放 Git |

第一次体验只需要完成：

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

## 开发者路径

1. 在目标平台目录阅读 README 和版本说明。
2. 使用该目录声明的 ESP-IDF、工具链和 SDK 版本构建。
3. 先完成基础联网、绑定和 H5 闭环。
4. 再按 H5、AI、微信 VoIP、设备互呼、OTA 的顺序逐项验证。
5. 发布前区分编译证据和真机证据，未执行的项目明确标记“未验证”。

### ESP32-S3 深入文档

| 文档 | 解决的问题 |
| --- | --- |
| [架构分层](../esp32-s3/trtc-advanced/docs/architecture/layering.md) | UI、应用、服务、协议和驱动的职责边界 |
| [音频与媒体所有权](../esp32-s3/trtc-advanced/docs/architecture/audio_media_ownership.md) | H5、AI、微信 VoIP、设备互呼如何共享硬件资源 |
| [AI 对讲](../esp32-s3/trtc-advanced/docs/ai_chat/README_CN.md) | AI 凭证、WHIP 会话、字幕和媒体链路 |
| [ThingConnect 对齐](../esp32-s3/trtc-advanced/docs/THING_CONNECT_ALIGNMENT_CN.md) | 设备实现与服务端接口、消息和状态机的对应关系 |

### ESP32-P4 深入文档

| 文档 | 解决的问题 |
| --- | --- |
| [P4 媒体架构](../esp32-p4/trtc-advanced/docs/P4_MEDIA_ARCHITECTURE.md) | 摄像头、H264、解码、显示和内存边界 |
| [P4 TiRTC 适配](../esp32-p4/trtc-advanced/docs/TIRTC_P4_ADAPTATION.md) | P4 平台 SDK、网络和媒体适配 |
| [P4 测试指南](../esp32-p4/trtc-advanced/TEST_GUIDE.md) | 构建后如何做真机回归 |

## 发布路径

- 源码、文档和脚本进入 Git。
- 固件 `.bin/.zip` 进入 [GitHub Releases](https://github.com/tangeai/tirtc-device-example/releases)。
- OTA app 和 manifest 进入 OTA 服务。
- 临时 CI 构建结果可以使用 Actions Artifact，但不能作为正式下载入口。
