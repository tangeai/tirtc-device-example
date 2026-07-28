# TiRTC ESP32-P4 Device Monitor Demo 1.1.0

## 版本能力

- 设备绑定、设备在线、TiRTC IPC 查看、P4 设备间双向音视频通话、微信 VoIP、AI Chat 和 OTA。
- IPC 视频默认 `1280x960@20fps`、`4Mbps`，使用 OV5647 YUV420 和 ESP32-P4 H264 硬编码。
- P4 设备通话默认 `480x320@15fps`、`2Mbps`，支持远端 H264 解码和横屏 LCD 显示。
- IPC、设备通话、微信 VoIP 和 AI Chat 共享统一的音频所有权与 AEC 策略。
- 视频输入、解码帧和 RGB 显示池预分配到 PSRAM，内部 RAM 优先留给 DMA、ESP-Hosted 和实时任务。
- 弱网自动降级默认关闭，保留媒体策略接口供上层按需调用。

## 发布资产

| Release 附件 | 用途 |
| --- | --- |
| `tirtc-esp32p4-device-monitor-full-v1.1.0.bin` | 普通体验者从 `0x0` 烧录 |
| `tirtc-esp32p4-device-monitor-webflash-v1.1.0.zip` | 维护者按 offset 烧录 |
| `tirtc-esp32p4-device-monitor-webinstall-v1.1.0.zip` | 完整镜像和中文烧录说明 |
| `SHA256SUMS.txt`、`release-manifest.json` | 完整性和来源核验 |

附件位于
[ESP32-P4 v1.1.0 Release](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-p4-trtc-advanced-v1.1.0)，
不提交到 Git 历史。详细校验值记录在根目录 `RELEASE.md`。

## 验证边界

- ESP-IDF 5.5.4 完整构建已通过。
- app 大小为 `0x656160`，最小 app 分区为 `0x730000`。
- TiRTC SDK 2.2.0 的头文件、P4 静态库和版本清单已核对。
- 当前发布镜像的烧录、联网、绑定和双向音视频需要按 `TEST_GUIDE.md` 完成硬件回归。
