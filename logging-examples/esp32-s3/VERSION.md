# 版本信息

- 日志示例：`0.3.0`
- 目标芯片：ESP32-S3
- ESP-IDF：`v5.5.4`
- TiRTC SDK：`v2.2.1`
- TiRTC SDK commit：`3a33bf4ae51b3ab9eb246648adb274d0fae32ebf`
- TiRTC SDK BuildInfo：
  `{"tirtc":"v2.2.1","commit":"3a33bf4ae51b","tgtrp":"v1.4.6"}`
- TiRTC 静态库 SHA-256：
  `dc9f869194584fd43fe091f887342d170a1422e657ce1fc2a144c90a3ade1f8e`
- AT 响应 schema：`BUILD=1`、`SESSION=3`（22 个 CSV 字段）、`MEDIA=1`
- 硬件证据摘要 schema：`4`
  - 每条 JSONL 记录使用同一个随机 `run_id`
  - `summary.json.raw_evidence` 记录同一 `run_id`、原始 JSONL 路径、
    大小、记录数和 SHA-256

`0.3.0` 是 ESP32-S3 AT ThingConnect 日志示例的正式源码版本，包含可由
AT 选择的 AI 提示、精简中文日志、最终字幕类型校验和联系人备注持久化。
旧版 `0.2.0` 及其公开 Tag 保持不变。

这些字段用于识别源码、SDK 和证据格式。构建、烧录、平台绑定、AI、设备
呼叫、竞态和真实媒体验证仍按各自证据独立记录。
