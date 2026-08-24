# 文档入口

第一次使用请从“用户路径”开始。需要开发或排障时，再进入“开发路径”和“协议路径”。

## 用户路径

| 文档 | 解决的问题 |
| --- | --- |
| [项目首页](../README.md) | 从下载固件到 H5 双向音频的最短闭环 |
| [完整体验流程](USER_EXPERIENCE_FLOW_CN.md) | 按真实使用顺序体验绑定、H5、小钛、微信 VoIP、设备互呼和 OTA |
| [从 Wi-Fi 到各项功能](GETTING_STARTED_CN.md) | 每个设备页面怎么操作，成功和失败分别看什么 |
| [烧录与 OTA](FLASH_AND_OTA_CN.md) | 从 GitHub Release 下载完整镜像，使用 Espressif ESP Tool 烧录 |

## 开发路径

| 文档 | 解决的问题 |
| --- | --- |
| [架构分层](architecture/layering.md) | UI、应用、服务、协议、驱动如何分层和持有资源 |
| [源码构建与配置](BUILD_AND_CONFIG_CN.md) | 环境版本、默认配置、干净构建、多地址网页烧录和构建排障 |
| [OTA 版本规则](release/OTA_VERSIONING_CN.md) | 应用版本、公开 Tag、完整镜像与 OTA app 如何保持一致 |
| [音频和媒体所有权](architecture/audio_media_ownership.md) | H5、AI、微信 VoIP、设备互呼如何共享音频；二维码扫描如何独立使用摄像头 |
| [串口诊断 CLI](BUILD_AND_CONFIG_CN.md#7-串口诊断-cli) | 网络、Socket、RTC、媒体、AEC、呼叫和内存怎么分阶段排查 |
| [版本信息](../VERSION.md) | 版本、源码快照、SDK、工具链和构建契约 |
| [变更记录](../CHANGELOG.md) | 当前版本新增能力和兼容性说明 |
| [小钛接入说明](ai_chat/README_CN.md) | AI 会话、字幕、联系人动作和资源释放 |
| [设备互呼流程](DEVICE_CALL_FLOW_CN.md) | 联系人、房间、主被叫、接听、拒接和异常恢复 |
| [ThingConnect 对齐说明](THING_CONNECT_ALIGNMENT_CN.md) | 设备代码与服务端协议、接口、消息和状态机的对应关系 |

## 协议路径

设备端示例和服务端使用同一套 ThingConnect 契约。完整字段、错误码和服务端行为以
[tangeai/tirtc-server-example](https://github.com/tangeai/tirtc-server-example)
为准：

- [设备上线与 MQTT 接入](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-integration.md)
- [H5 实时查看与按住说话](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-h5-live.md)
- [AI 对讲设备接入](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-ai.md)
- [微信 VoIP 对讲设备接入](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-voip.md)
- [设备呼设备接入](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-call.md)
- [统一会话模型](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-session-model.md)
- [API Reference](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/api-reference.md)

## 固件发布

完整镜像、OTA app、`SHA256SUMS.txt` 和 `release-manifest.json` 统一发布到
[tirtc-device-example Releases](https://github.com/tangeai/tirtc-device-example/releases)，
不提交到源码 Git 历史。文件选择、校验和烧录方法见
[烧录与 OTA](FLASH_AND_OTA_CN.md)。
