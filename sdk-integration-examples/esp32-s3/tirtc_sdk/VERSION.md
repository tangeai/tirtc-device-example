# TiRTC SDK Version

| Field | Value |
|---|---|
| 芯片原厂 | Espressif Systems / 乐鑫 |
| 芯片型号 | ESP32-S3 |
| SDK 版本 | 2.2.1 |
| SDK Tag / BuildInfo | tag `v2.2.1` / commit `3a33bf4ae51b` / tgtrp `v1.4.6` |
| 文件 | `lib/libTiRTC.a` |
| MD5 | `2d8faeca3fcce4d80461b059cbfab821` |
| SHA256 | `e039a074e04a0f963c95458ec77db4b5d6a9693ae7e6463e3c86fb66c39340be` |
| 文件大小 | 4,723,856 bytes / 4.51 MiB |
| 目标 OS | FreeRTOS / ESP-IDF |
| 工具链 | xtensa-esp32s3-elf-gcc-14.2.0_20260121 |
| FreeRTOS Tick | 1000 Hz |
| SDK 形态 | KCP single static library，`libwebrtc_nosctp.a` 已合入 `libTiRTC.a` |
| 初始化 | SDK 内部包含 `SA_platInit()`，应用侧无需额外初始化 |
| 设备启动契约 | `TiRtcInit()` -> `TIRTC_OPT_DEVICE_SECRET_KEY` -> `TIRTC_OPT_CLIENT_ID` -> `TiRtcStart(device_id)` |
