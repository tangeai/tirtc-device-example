# TiRTC ESP32-P4 Demo Version

| Field | Value |
|---|---|
| 示例工程 | TiRTC ESP32-P4 Wi-Fi Link Demo |
| 示例版本 | 1.1.1 |
| 发布日期 | 2026-07-30 |
| TiRTC SDK 版本 | 2.2.1 |
| TiRTC SDK 来源 | `ESP32-P4_v2.2.1_20260729` |
| TiRTC SDK Source commit | `3a33bf4ae51b3ab9eb246648adb274d0fae32ebf` |
| TiRTC SDK MD5 | d2c0755049868129c42de5ca72de686a |
| TiRTC SDK SHA256 | a8eadc99e97e9d6fcc7d871963d3456484ed3625469804a7ff6718218b117d65 |
| TiRTC SDK 类型 | KCP single static library |
| 目标芯片 | ESP32-P4 |
| 目标 OS | FreeRTOS / ESP-IDF |
| 验证 ESP-IDF | 5.5.4 |
| 工具链 | riscv32-esp-elf-gcc-14.2.0_20260121 |

## 发布说明

- 提供 ESP32-P4 Wi-Fi 接入示例，启动后依次完成 Wi-Fi 联网、SNTP 时间同步和 TiRTC 上线。
- 按 TiRTC C SDK 2.2.x 设备端契约，在 `TiRtcStart()` 前设置 `device_secret_key` 和必填 `client_id`。
- 支持远端呼入，也支持按 BOOT 键切换连接状态：未连接时主动连接 `TIRTC_REMOTE_DEVICE_ID`，已连接时主动断开。
- 主动连接时可通过本地签发 token 完成快速验证。
- 连接建立后默认主动发送本地 H264 视频和 PCMA 音频，可通过 `TIRTC_AUTO_PUSH_LOCAL_MEDIA_ON_CONNECT` 切换为等待订阅后推流。
- 保留常用控制接口：主动断开、订阅/取消订阅音视频、请求关键帧。
- 启动日志打印 SDK version 和 build info，便于核对运行时 SDK 与交付包一致。
- 工程默认配置使用占位值，不包含真实 Wi-Fi、设备 ID、client_id、access_id 或 secret_key。

## 代码版本来源

固件启动时会打印 `main/app_version.h` 中的示例工程版本信息。发布新版本时，请同步更新：

- `main/app_version.h`
- `VERSION.md`
- `README.md`
