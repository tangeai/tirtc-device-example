# 版本信息

- 最小系统例子：`0.8.0`
- 发布阶段：正式发布
- 目标芯片：ESP32-S3
- Flash：4 MB 或更大
- PSRAM：必需
- ESP-IDF：`v5.5.4`
- TiRTC SDK：`v2.2.1`
- TiRTC SDK commit：`3a33bf4ae51b3ab9eb246648adb274d0fae32ebf`
- TiRTC SDK BuildInfo：
  `{"tirtc":"v2.2.1","commit":"3a33bf4ae51b","tgtrp":"v1.4.6"}`
- TiRTC 静态库 SHA-256：
  `dc9f869194584fd43fe091f887342d170a1422e657ce1fc2a144c90a3ade1f8e`
- 公开 Tag：`esp32-s3-minimal-system-v0.8.0`

本版本提供中文精简 AT 交互、开发者平台绑定、网页远程查看、预录音视频、
设备间呼叫与消息、联系人备注、AI 对讲，以及 AI 按联系人备注呼叫设备。

本版本补全经过脱敏的 30 张操作截图，并把对外分类、目录、Tag 和固件名统一为“最小系统例子”。
功能代码、TiRTC SDK、媒体输入和板级配置相对上一公开版本保持不变。构建、烧录和真机运行是
相互独立的验证层；实际完成范围以 Release 自带的 `release-manifest.json` 为准。
