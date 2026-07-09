# TiRTC ESP32-P4 Device Monitor Demo 1.0.2 发布说明

## 版本定位

本版本用于 ESP32-P4 设备监控端演示，目标开发板为 Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5。功能覆盖设备绑定、设备在线、TiRTC IPC 音视频、微信 VoIP、AI Chat、OTA 和横屏 UI。

## 核心能力

- ESP32-P4 主控，ESP32-C6 Hosted Wi-Fi。
- 横屏 `480x320` LVGL 界面。
- 6 位绑定码、绑定凭证持久化和设备在线。
- TiRTC SDK 2.2.0，上线、呼入、断开、订阅和媒体流。
- 摄像头 YUV420 输入，ESP32-P4 H264 硬件编码，上行到 TiRTC。
- 麦克风采集上行和远端音频播放。
- 微信 VoIP、AI Chat、OTA 和设置页。

## 默认参数

| 参数 | 当前值 |
| --- | --- |
| 视频目标 | 1920x1080@20fps |
| H264 目标码率 | 6Mbps |
| H264 GOP | 40 |
| H264 输出缓冲 | 4MB |
| TiRTC 发送缓冲 | 1MB |
| FreeRTOS tick | 1000Hz |
| 弱网自动降级 | 默认关闭 |
| 连接后主动推流 | 默认开启 |

## 发布资产

| 目录 | 用途 |
| --- | --- |
| `release_assets/web-flash/v1.0.2/` | 多地址烧录资产，面向维护者 |
| `release_assets/web-install/v1.0.2/` | 0x0 完整镜像和一键包，面向体验者 |

每个发布目录都应包含 `README_FLASH_CN.md` 或 `README_INSTALL_CN.md`、`SHA256SUMS.txt` 和 zip 包。

## 校验信息

| 资产 | 大小 | SHA-256 |
| --- | ---: | --- |
| `release_assets/web-flash/v1.0.2/tirtc_esp32p4_wifi_link_demo.bin` | 6111792 bytes | `F21720F41BF0BF5CA0A5B0E4EA478FDCC1FE56831E2F4BD7F371ED4B45430EFC` |
| `release_assets/web-flash/tirtc-esp32p4-device-monitor-webflash-v1.0.2.zip` | 4061104 bytes | `91C186DF96B8F06A7A6E35D70DCDDAAFD169E10ABB53DFE5AED9DC793B4B6500` |
| `release_assets/web-install/v1.0.2/tirtc-esp32p4-device-monitor-full-v1.0.2.bin` | 16777216 bytes | `F1A839DF65DEF2A25B43B33609F6307F65C500193D2DFD7A8EBF0C53665E8D49` |
| `release_assets/web-install/tirtc-esp32p4-device-monitor-webinstall-v1.0.2.zip` | 4069010 bytes | `F3AA44342D6F690BC233AD9AC53D685B7853B1DE9AEAD2B7030A63239CEABCF4` |

## 验证记录

| 项目 | 状态 |
| --- | --- |
| 构建 | 已通过：app `0x5d4230`，最小 app 分区 `0x730000`，剩余 `0x15bdd0` |
| 烧录 | 待当前硬件回归确认 |
| 联网 | 待当前硬件回归确认 |
| 绑定 | 待当前硬件回归确认 |
| TiRTC 上线 | 待当前硬件回归确认 |
| 视频上行 | 待当前硬件回归确认 |
| 音频上行/播放 | 待当前硬件回归确认 |
| 返回释放 | 待当前硬件回归确认 |

## 发布注意

- 源码不包含真实 Wi-Fi 密码、设备密钥、token 或个人账号。
- 烧录 offset 以当前 `build/flasher_args.json` 为准。
- RTC 摄像头上行不启用本地预览。
- 弱网调节接口已保留，默认演示不自动降帧率、降分辨率或降码率。
