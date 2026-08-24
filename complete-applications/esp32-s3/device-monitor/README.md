# TiRTC ESP32-S3 Device Monitor

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.5.4-E7352C?logo=espressif)](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/)
[![TiRTC SDK](https://img.shields.io/badge/TiRTC%20SDK-2.3.0%20mini-1769AA)](https://docs.tange.ai/products/tirtc/en/overview/what-is-tirtc.html)
[![Board](https://img.shields.io/badge/Board-LCKFB--SZPI--ESP32--S3--VA-2E7D32)](https://lckfb.com/project/detail/lckfb-esp32-s3-va?param=baseInfo)

这是面向立创·实战派 ESP32-S3 开发板的完整设备应用。它把 2.4 GHz Wi-Fi、ThingConnect
绑定与在线、TiRTC 实时音频、320 x 240 触摸屏、小钛、微信 VoIP、设备互呼和 OTA 放在
同一套应用生命周期里，适合直接体验，也适合作为带屏音频设备的工程参考。

当前版本为 `1.9.0`，公开项目 Tag 为 `esp32-s3-device-monitor-v1.9.0`，TiRTC SDK 为
`2.3.0 mini`。

> **1.9.0 产品边界**
>
> 本版本的 RTC 业务统一为双向音频：Web IPC、微信 VoIP、设备互呼和小钛发起的呼叫都不
> 发布或播放 RTC 视频。板载摄像头只用于扫描联系人二维码。这样可以让 UI、业务和协议层
> 对“这台设备能做什么”保持同一个答案。

## 先选你的路线

| 目标 | 最短入口 |
| --- | --- |
| 直接体验 | 下载完整镜像，使用 Espressif ESP Tool 从 `0x0` 烧录 |
| 修改源码 | 安装 ESP-IDF `5.5.4`，从公开 Tag 干净构建 |
| 接入自己的平台 | 先读 [ThingConnect 接入与实现对齐](docs/THING_CONNECT_ALIGNMENT_CN.md) |
| 排查音频或弱网 | 使用默认开启的串口诊断 CLI，并阅读 [音频和媒体所有权](docs/architecture/audio_media_ownership.md) |

## 直接烧录

从
[`esp32-s3-device-monitor-v1.9.0` Release](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-s3-device-monitor-v1.9.0)
下载：

```text
esp32s3-tirtc-device-monitor-full-v1.9.0.bin
SHA256SUMS.txt
release-manifest.json
```

先核对 SHA-256，再用 Chrome 或 Edge 打开
[Espressif ESP Tool](https://espressif.github.io/esptool-js/)，选择完整镜像并把烧录地址填写为
`0x0`。完整镜像会覆盖 16 MB Flash，并清除 NVS 中已有的 Wi-Fi、绑定和本地设置。

详细操作、OTA 文件区别和多地址烧录见 [烧录与 OTA](docs/FLASH_AND_OTA_CN.md)。

## 硬件要求

| 项目 | 要求 |
| --- | --- |
| 芯片 | ESP32-S3，双核 240 MHz |
| 开发板 | `LCKFB-SZPI-ESP32-S3-VA` |
| Flash / PSRAM | 16 MB / 8 MB |
| 显示 | 320 x 240 触摸屏 |
| 音频 | 板载麦克风、扬声器和音频 Codec |
| 摄像头 | 仅用于二维码扫描，不作为 RTC 视频源 |
| 网络 | 2.4 GHz Wi-Fi，可访问服务发现、ThingConnect 和 TiRTC 服务 |

普通 ESP32-S3 DevKit 缺少这块板的屏幕、触摸和音频外设，不能直接复现完整体验。

## 第一次启动

```text
烧录完整镜像
  -> 在屏幕上连接 2.4 GHz Wi-Fi
  -> 获取 6 位绑定码
  -> 在 ThingConnect H5 完成绑定
  -> 验证 Web IPC 双向音频
  -> 再体验小钛、微信 VoIP、设备互呼和 OTA
```

判断每一步时看真实现象：

1. 设置页显示版本 `1.9.0`。
2. Wi-Fi 页面显示已连接、IP 地址和信号状态。
3. 设备显示 6 位验证码，绑定后显示正式设备 ID。
4. H5 能听到设备麦克风音频，按住说话时设备扬声器能播放回传音频。
5. 小钛、微信和设备互呼分别完成进入、通话与退出，退出后可以正常进入下一项业务。

完整操作见 [从 Wi-Fi 到体验功能](docs/GETTING_STARTED_CN.md)。

## 当前能力

| 能力 | 设备端行为 |
| --- | --- |
| 设备接入 | 服务发现、6 位码绑定、设备身份保存、MQTT 在线和自动重连 |
| Web IPC | 设备麦克风上行、H5 音频下行；不启用 RTC 视频 |
| 小钛 | WHIP 音频、字幕、打断、联系人状态查询和音频呼叫动作 |
| 微信 VoIP | 联系人、备注、设备与微信双向发起的音频通话；主动呼叫使用体验版 `wx_version_type=2` |
| 设备互呼 | 音频呼叫、来电、接听、拒接、取消、挂断和异常房间恢复 |
| 摄像头 | 扫描设备联系人和微信联系人二维码 |
| OTA | 双 OTA app 分区，在线更新仅替换 app |
| 诊断 | 串口 AT 命令查看网络、Socket、RTC、音频路径、AEC、媒体统计和呼叫状态 |

多项 RTC 业务共享音频设备和 TiRTC 连接资源。应用层负责会话仲裁与资源交接；MQTT 和设备
在线属于常驻服务，不随单个页面销毁。

## 串口诊断 CLI

`CONFIG_APP_SERIAL_NET_CLI_ENABLE` 在公开默认配置中开启。连接 ESP-IDF Monitor 后输入
`AT+HELP` 可查看当前固件实际支持的命令，常用入口包括：

```text
AT+NET?       AT+WIFI?       AT+NETPROBE
AT+SOCKETS?   AT+MEDIA?      AT+AUDIOPATH?
AT+RTCLOG?    AT+RTCLINK?    AT+AUDIOCHECK?
AT+AECDUMP?   AT+CALL?       AT+HEAP?
```

CLI 使用固定缓冲区，不输出 Wi-Fi 密码、Token 或设备密钥。产品量产不需要串口诊断时，可在
`idf.py menuconfig` 的 `TiRTC Device Monitor -> Debug utilities` 中关闭
`APP_SERIAL_NET_CLI_ENABLE`，然后重新做完整干净构建。

## 从源码构建

```powershell
git clone https://github.com/tangeai/tirtc-device-example.git
cd tirtc-device-example
git checkout esp32-s3-device-monitor-v1.9.0
cd complete-applications/esp32-s3/device-monitor

. "$env:IDF_PATH\export.ps1"
idf.py -B build --no-ccache reconfigure build
```

版本和依赖必须同时满足：

| 项目 | 值 |
| --- | --- |
| 应用版本 | `1.9.0` |
| ESP-IDF | `5.5.4` |
| Xtensa 工具链 | `14.2.0_20260121` |
| TiRTC SDK | `2.3.0 mini`，BuildInfo `v2.3.0-1baf7c95` |
| `libTiRTC.a` | `8,079,682` bytes |
| `libTiRTC.a` SHA-256 | `43b06d1da421c7d24cc7fdb1385d600ecdffbfd2d3801f7faf0c540fb5cdbaa2` |
| OTA app 分区 | `0x770000` bytes |

`1.9.0` 的 SDK 和功能增量已在来源工程完成开发侧构建。统一公开代码快照
`d76e6e3b7f02f112b55ed917daa17f530b3c7a6b` 也已从全新目录完成 ESP-IDF `5.5.4`、
禁用 ccache 的正式干净构建；对应 app 大小
`7599904` bytes、SHA-256
`3cdebe0df0946fc7bee65c921f94796c080bc1e03025370b1b76a5cbe560d137`，分区剩余
`198880` bytes。当前容量余量较紧，继续增加图片、字体、日志或调试能力前，
请先重新检查 app 大小。

构建输入、配置和多地址烧录见 [源码构建与配置](docs/BUILD_AND_CONFIG_CN.md)。

## 版本来源

| 项目 | 值 |
| --- | --- |
| 开发来源 Tag | `v1.9.0` |
| 开发来源 commit | `a64422b0efdebe6c303370effafd52bbf51593d1` |
| 开发来源 tree | `b29d4080a43db0a2b8f2e35f095c5c45f3c1f4c7` |
| 公开项目 Tag | `esp32-s3-device-monitor-v1.9.0` |
| 比较基线 | `v1.8.0`，1.8.1 补丁历史继续保留在变更记录中 |

完整来源、筛选范围和证据分层见 [来源与验证边界](SOURCE_PROVENANCE.md)。

## 文档

- [文档入口](docs/README_CN.md)
- [从 Wi-Fi 到体验功能](docs/GETTING_STARTED_CN.md)
- [源码构建与配置](docs/BUILD_AND_CONFIG_CN.md)
- [烧录与 OTA](docs/FLASH_AND_OTA_CN.md)
- [ThingConnect 接入与实现对齐](docs/THING_CONNECT_ALIGNMENT_CN.md)
- [设备呼叫流程](docs/DEVICE_CALL_FLOW_CN.md)
- [小钛接入说明](docs/ai_chat/README_CN.md)
- [音频和媒体所有权](docs/architecture/audio_media_ownership.md)
- [变更记录](CHANGELOG.md)

## 证据边界

来源快照、SDK 文件、公开源码、正式构建和 Release 资产使用 commit、tree、文件清单和 SHA-256
相互绑定。构建通过证明这份源码能够完成编译链接；烧录、串口启动、Wi-Fi、云端在线、通话
效果和长时间稳定性要在目标板上分别观察，不能由构建结果代替。
