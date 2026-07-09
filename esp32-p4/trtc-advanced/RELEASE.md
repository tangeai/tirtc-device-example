# TiRTC ESP32-P4 Device Monitor Demo 1.0.2 发布说明

## 版本定位

`1.0.2` 是 ESP32-P4 设备监控端演示版本，目标开发板为 Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5。该版本用于展示设备绑定、在线、TiRTC IPC 音视频、微信 VoIP、AI Chat、OTA 和横屏触摸 UI。

## 核心能力

- ESP32-P4 + ESP32-C6 Hosted Wi-Fi 联网。
- 横屏 `480x320` 设备端 UI。
- 6 位设备绑定码、绑定凭证保存、设备在线和 token 获取。
- TiRTC SDK 2.2.0 启动、监听、呼入、断开和媒体流。
- OV5647 摄像头采集，YUV420 输入，P4 H264 硬件编码后上行。
- 麦克风采集上行和远端音频播放。
- 微信 VoIP、AI Chat、OTA 和设置页。

## 关键默认参数

| 参数 | 当前值 |
| --- | --- |
| TiRTC SDK | 2.2.0 |
| FreeRTOS tick | 1000 Hz |
| RTC 视频 | 1920x1080, 20 fps, 6 Mbps |
| H264 GOP | 40 |
| H264 输出缓冲 | 4 MB |
| TiRTC 发送缓冲 | 1 MB |
| 视频发送池 | 6 slots, 1 MB/slot |
| 弱网自动降级 | 默认关闭 |
| 连接后主动推流 | 默认开启 |
| 调试屏幕服务 | 默认关闭 |

## 发布资产

发布资产位于：

```text
release_assets/web-flash/v1.0.2/
release_assets/web-install/v1.0.2/
```

`web-flash` 用于维护者按 offset 烧录，offset 来自当前 `build/flasher_args.json`。`web-install` 用于普通体验者烧录 0x0 完整镜像；如果本机环境无法合成完整镜像，该目录会在发布说明中标记未生成原因。

## 校验信息

| 资产 | 大小 | SHA-256 |
| --- | ---: | --- |
| `release_assets/web-flash/v1.0.2/tirtc_esp32p4_wifi_link_demo.bin` | 6111792 bytes | `F21720F41BF0BF5CA0A5B0E4EA478FDCC1FE56831E2F4BD7F371ED4B45430EFC` |
| `release_assets/web-flash/tirtc-esp32p4-device-monitor-webflash-v1.0.2.zip` | 4061104 bytes | `91C186DF96B8F06A7A6E35D70DCDDAAFD169E10ABB53DFE5AED9DC793B4B6500` |
| `release_assets/web-install/v1.0.2/tirtc-esp32p4-device-monitor-full-v1.0.2.bin` | 16777216 bytes | `F1A839DF65DEF2A25B43B33609F6307F65C500193D2DFD7A8EBF0C53665E8D49` |
| `release_assets/web-install/tirtc-esp32p4-device-monitor-webinstall-v1.0.2.zip` | 4069010 bytes | `F3AA44342D6F690BC233AD9AC53D685B7853B1DE9AEAD2B7030A63239CEABCF4` |

## 构建命令

```powershell
cd $env:USERPROFILE\Desktop\demo_p4
cmd /c "C:\esp\v5.5.4\esp-idf\export.bat >nul && idf.py reconfigure build"
```

## 验证记录

| 项目 | 状态 |
| --- | --- |
| ESP-IDF 构建 | 已通过：app `0x5d4230`，最小 app 分区 `0x730000`，剩余 `0x15bdd0` |
| 烧录验证 | 待当前硬件回归确认 |
| Wi-Fi 联网 | 待当前硬件回归确认 |
| 设备绑定 | 待当前硬件回归确认 |
| TiRTC 上线 | 待当前硬件回归确认 |
| 视频上行 | 待当前硬件回归确认 |
| 音频上行/播放 | 待当前硬件回归确认 |
| 返回释放 | 待当前硬件回归确认 |

## 注意事项

- 源码默认不包含真实 Wi-Fi 密码、设备密钥、token 或私有账号。
- TiRTC 凭证优先通过绑定流程写入 NVS。
- RTC 摄像头上行不做本地预览，避免显示刷新抢占 H264 和 Wi-Fi 资源。
- 弱网自动降级接口已保留，默认不启用；需要业务层或 TiRTC 弱网信号触发后再调用。
