# 版本信息

| 项目 | 当前值 |
| --- | --- |
| 应用版本 | `1.8.0` |
| 源标签 | `v1.8.0` |
| 源提交 | `8211e3bec7a40beff5cfe81dbf9cb8468016e786` |
| 比较基线 | `v1.7.6` / `04dfca5345f3b3ae6b0afa94e8a4d1d49986d214` |
| 发布日期 | `2026-07-31` |
| 芯片 | `ESP32-S3` |
| 开发板 | `LCKFB-SZPI-ESP32-S3-VA` |
| Flash / PSRAM | `16 MB / 8 MB` |
| ESP-IDF | `5.5.4` |
| 工具链 | `xtensa-esp32s3-elf 14.2.0_20260121` |
| TiRTC SDK | `2.2.0` |
| TiRTC BuildInfo commit | `1df9e045a9dc` |
| `libTiRTC.a` 大小 | `4,723,856` bytes |
| `libTiRTC.a` SHA-256 | `0686e5d7f5bfea18b7b3ee6ae1701061b3afcfb0ef2030642ebebab88af0413d` |
| FreeRTOS tick | `1000 Hz` |
| 服务发现 | `http://ep-open.tangeopen.com/services` |

## 构建契约

- 应用版本由 ESP-IDF 工程版本写入镜像描述信息；正式构建后应为 `1.8.0`。
- 两个 OTA app 分区大小均为 `0x770000`，OTA 固件不得超过该上限。
- TiRTC SDK 在本版本保持 `2.2.0`，静态库身份以上述 SHA-256 为准。
- 正式固件的实际大小、SHA-256、烧录地址和统一仓 commit 以当次
  `release-manifest.json` 为准。
