# TiRTC ESP32-P4 Device Monitor Demo 1.1.0 发布说明

## 版本定位

`1.1.0` 面向 Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5，提供设备绑定、设备在线、TiRTC IPC 查看、P4 设备间双向音视频通话、微信 VoIP、AI Chat、OTA 和横屏触摸 UI。

## 核心能力

- ESP32-P4 主控与 ESP32-C6 Hosted Wi-Fi 联网。
- 横屏 `480x320` LVGL 界面和经过方向校准的触摸输入。
- 6 位绑定码、设备凭证持久化、服务发现和正式 MQTT 在线链路。
- TiRTC SDK 2.2.0 上线、呼入、主动连接、断开、订阅和媒体流。
- IPC 查看使用 OV5647 YUV420、P4 H264 硬编码和主动视频上行。
- P4 设备通话使用独立媒体档位，支持远端 H264 解码、RGB565 转换和 PSRAM 直通 LCD。
- 麦克风上行、远端扬声器播放，以及 IPC、设备通话、微信 VoIP、AI Chat 的统一 AEC 策略。
- 媒体资源按生命周期预热和复用，大块视频缓冲放入 PSRAM，内部 RAM 保留给 DMA、SDIO 和实时控制路径。

## 默认参数

| 参数 | 当前值 |
| --- | --- |
| TiRTC SDK | 2.2.0 |
| FreeRTOS tick | 1000 Hz |
| IPC 视频 | 1280x960, 20 fps, 4 Mbps |
| P4 设备通话视频 | 480x320, 15 fps, 2 Mbps |
| H264 GOP | 40 |
| AEC | 默认开启，按媒体所有权启停 |
| 弱网自动降级 | 默认关闭 |
| 连接后主动推流 | 默认开启 |
| 调试屏幕服务 | 默认关闭 |

## 发布资产

| 资产 | 大小 | SHA-256 |
| --- | ---: | --- |
| `release_assets/web-flash/v1.1.0/tirtc_esp32p4_wifi_link_demo.bin` | 6644064 bytes | `1CC65283663B16857343B2CD45F9EAA6C84726193542063EAAAF78C9F7822F85` |
| `release_assets/web-flash/tirtc-esp32p4-device-monitor-webflash-v1.1.0.zip` | 4234967 bytes | `FB5C88E4ACCE28053C719D7B915332B0074B4853F50711588E3F59C95AFD5892` |
| `release_assets/web-install/v1.1.0/tirtc-esp32p4-device-monitor-full-v1.1.0.bin` | 16777216 bytes | `9D0354B9E5E859DD67D23BD44D294AB8D8C7A409F8ED88E9ECDCF6BBB58B62EE` |
| `release_assets/web-install/tirtc-esp32p4-device-monitor-webinstall-v1.1.0.zip` | 4244092 bytes | `3B6F76CCE1B419969B49A51CCE34726AC0580355B52B5CE3B19B57409541BF07` |

多地址烧录 offset 来自本次构建的 `build/flasher_args.json`。完整镜像固定为 16 MiB，从 `0x0` 烧录。

## 构建与验证

| 项目 | 状态 |
| --- | --- |
| 源码基线 | `demo_p4` commit `4957a95` |
| ESP-IDF 构建 | 已通过，ESP-IDF 5.5.4 |
| app 大小 | `0x656160`，最小 app 分区 `0x730000`，剩余 `0xd9ea0` |
| SDK 契约 | TiRTC 2.2.0，P4 静态库 SHA-256 `97d4285474e301f5e2846c837241632b9d2bca09a64d96d2ec385c6c68c40ec6` |
| 当前发布镜像烧录回归 | 待硬件验证 |
| 联网、绑定、TiRTC、双向音视频回归 | 待硬件验证 |

## 注意事项

- 源码默认不包含真实 Wi-Fi 密码、设备密钥、token 或私有账号。
- TiRTC 凭证通过设备绑定流程写入 NVS。
- IPC 摄像头上行不启用本地预览，避免显示刷新抢占编码和网络资源。
- 弱网调节接口保留在媒体策略层，默认演示不会自动降低分辨率、帧率或码率。
