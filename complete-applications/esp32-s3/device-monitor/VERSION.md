# 版本信息

| 项目 | 当前值 |
| --- | --- |
| 应用版本 | `1.9.0` |
| 源标签 | `v1.9.0` |
| 源 Tag object | `bd0c800cef4186a74b94a0b696a9c3075db1aca9` |
| 源提交 | `a64422b0efdebe6c303370effafd52bbf51593d1` |
| 源 tree | `b29d4080a43db0a2b8f2e35f095c5c45f3c1f4c7` |
| 比较基线 | `v1.8.0` / `8211e3bec7a40beff5cfe81dbf9cb8468016e786` |
| 公开项目 Tag | `esp32-s3-device-monitor-v1.9.0` |
| 发布日期 | `2026-08-24` |
| 芯片 | `ESP32-S3` |
| 开发板 | `LCKFB-SZPI-ESP32-S3-VA` |
| Flash / PSRAM | `16 MB / 8 MB` |
| ESP-IDF | `5.5.4` |
| 工具链 | `xtensa-esp32s3-elf 14.2.0_20260121` |
| TiRTC SDK | `2.3.0 mini` |
| TiRTC BuildInfo | `v2.3.0-1baf7c95` |
| TiRTC BuildInfo commit | `1baf7c95f3ca715c9367b9c998417f647934dc35` |
| TGTRP BuildInfo | `tagv1.5.11-41c9a257` |
| `libTiRTC.a` 大小 | `8,079,682` bytes |
| `libTiRTC.a` SHA-256 | `43b06d1da421c7d24cc7fdb1385d600ecdffbfd2d3801f7faf0c540fb5cdbaa2` |
| FreeRTOS tick | `1000 Hz` |
| 服务发现 | `http://ep-open.tangeopen.com/services` |
| 设备主动呼叫微信版本类型 | 体验版（`2`） |
| RTC 产品能力 | 双向音频；摄像头仅用于二维码扫描 |
| 串口诊断 CLI | 默认开启；量产配置可关闭 |

## 构建契约

- 应用版本由 ESP-IDF 工程版本写入镜像描述信息；本次正式构建已核对为 `1.9.0`，后续重建也
  必须保持一致。
- 两个 OTA app 分区大小均为 `0x770000`，OTA 固件不得超过该上限。
- 正式公开构建应从最终公开 commit 的全新目录执行，不复用来源工程或旧版本 `build/`。
- 正式 app 大小为 `7599904` bytes，SHA-256 为
  `3cdebe0df0946fc7bee65c921f94796c080bc1e03025370b1b76a5cbe560d137`，分区剩余
  `198880` bytes。
- `1.9.0` 的 app 余量较紧；增加图片、字体、日志、调试功能或 SDK 代码后必须重新检查分区。
- TiRTC SDK 的头文件、静态库、组件配置和 `components/tirtc_sdk/VERSION.md` 必须来自同一
  `2.3.0 mini` 包，不能只改版本文字。
- 正式完整镜像大小、SHA-256、烧录地址和公开 commit 以同一 Release 的
  `release-manifest.json` 为准。
