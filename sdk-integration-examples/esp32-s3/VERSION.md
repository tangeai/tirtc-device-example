# TiRTC ESP32-S3 Demo Version

| Field | Value |
|---|---|
| 示例工程 | ESP32-S3 最小 TiRTC 集成示例 |
| 示例版本 | 1.1.1 |
| 发布日期 | 2026-07-10 |
| TiRTC SDK 版本 | 2.2.1 |
| TiRTC SDK 来源 | tag `v2.2.1` / BuildInfo commit `3a33bf4ae51b` / tgtrp `v1.4.6` |
| TiRTC SDK MD5 | 2d8faeca3fcce4d80461b059cbfab821 |
| TiRTC SDK SHA256 | e039a074e04a0f963c95458ec77db4b5d6a9693ae7e6463e3c86fb66c39340be |
| TiRTC SDK 文件大小 | 4,723,856 bytes |
| TiRTC SDK 形态 | KCP single static library，`libwebrtc_nosctp.a` 已合入 `libTiRTC.a` |
| FreeRTOS Tick | 1000 Hz |
| 目标芯片 | ESP32-S3 |
| 目标 OS | FreeRTOS / ESP-IDF |
| 验证 ESP-IDF | 5.5.4 |
| 工具链 | xtensa-esp32s3-elf-gcc-14.2.0_20260121 |

## 发布说明

- 提供 ESP32-S3 Wi-Fi 接入示例，启动后依次完成 Wi-Fi 联网、SNTP 时间同步和 TiRTC 上线。
- 支持远端呼入，也支持按 BOOT 键切换连接状态：未连接时主动连接 `TIRTC_REMOTE_DEVICE_ID`，已连接时主动断开。
- BOOT 主动连接会按新版双校验规则在本地签发测试 token，仅用于联调。
- 远端呼入并收到对端订阅后发送本地 H264 视频和 PCMA 音频；BOOT 主动连接时订阅对端音视频。
- 示例保留常用控制接口：主动断开、订阅/取消订阅音视频、请求关键帧。
- 公开版本仅保留占位配置，不包含真实 Wi-Fi、设备 ID、access_id 或 secret_key。
- SDK 内部包含平台初始化入口 `SA_platInit()`，应用层只按示例调用 TiRTC API 即可。
- TiRTC 2.2.x 使用独立设备身份 option：`TiRtcInit()` 后设置 `TIRTC_OPT_DEVICE_SECRET_KEY` 和 `TIRTC_OPT_CLIENT_ID`，再调用 `TiRtcStart(device_id, callbacks)`；已绑定设备的 `client_id` 使用 `device_id`。

## 代码版本来源

固件启动时会打印 `main/app_version.h` 中的示例工程版本信息。发布新版本时，请同步更新：

- `main/app_version.h`
- `VERSION.md`
- `RELEASE.md`
- `README.md`
