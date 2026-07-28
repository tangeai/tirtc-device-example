# TiRTC ESP32-P4 Demo Version

| Field | Value |
|---|---|
| 示例工程 | ESP32-P4 最小 TiRTC 集成示例 |
| 示例版本 | 1.0.2 |
| 发布日期 | 2026-06-24 |
| TiRTC SDK 版本 | 0.1.4 |
| TiRTC SDK 来源 | ESP32-P4 ESP-IDF 5.5.4 package |
| TiRTC SDK MD5 | e818308a96f0a4d1d6a2266b5ba788f7 |
| TiRTC SDK 类型 | KCP single static library |
| 目标芯片 | ESP32-P4 |
| 目标 OS | FreeRTOS / ESP-IDF |
| 验证 ESP-IDF | 5.5.4 |
| 工具链 | riscv32-esp-elf-gcc-14.2.0_20260121 |

## 发布说明

- 提供 ESP32-P4 Wi-Fi 接入示例，启动后依次完成 Wi-Fi 联网、SNTP 时间同步和 TiRTC 上线。
- 支持远端呼入，也支持按 BOOT 键切换连接状态：未连接时主动连接 `TIRTC_REMOTE_DEVICE_ID`，已连接时主动断开。
- 主动连接时可通过本地签发 token 完成快速验证。
- 连接建立后默认主动发送本地 H264 视频和 PCMA 音频，可通过 `TIRTC_AUTO_PUSH_LOCAL_MEDIA_ON_CONNECT` 切换为等待订阅后推流。
- 保留常用控制接口：主动断开、订阅/取消订阅音视频、请求关键帧。
- 工程默认配置使用占位值，不包含真实 Wi-Fi、设备 ID、access_id 或 secret_key。

## 代码版本来源

固件启动时会打印 `main/app_version.h` 中的示例工程版本信息。发布新版本时，请同步更新：

- `main/app_version.h`
- `VERSION.md`
- `RELEASE.md`
- `README.md`
