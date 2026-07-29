# TiRTC ESP32-S3 设备端示例

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://github.com/tangeai/tirtc-device-example/blob/main/LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.5.4-E7352C?logo=espressif)](https://docs.espressif.com/projects/esp-idf/)
[![Chip](https://img.shields.io/badge/Chip-ESP32--S3-000000)](https://www.espressif.com/en/products/socs/esp32-s3)
[![TiRTC SDK](https://img.shields.io/badge/TiRTC%20SDK-2.2.0-1769AA)](https://docs.tange.ai/products/tirtc/en/overview/what-is-tirtc.html)
[![FreeRTOS Tick](https://img.shields.io/badge/FreeRTOS%20Tick-1%20kHz-00A98F)](sdkconfig.defaults)

面向 ESP32-S3 触摸屏设备的 TiRTC 参考工程。它与
[tangeai/tirtc-server-example](https://github.com/tangeai/tirtc-server-example)
使用同一套 ThingConnect 设备协议，演示设备如何完成**联网、6 位验证码绑定、H5
实时查看与对讲、AI 对讲、微信 IoT VoIP、设备间互呼和 OTA**。

本示例位于统一设备仓
[tangeai/tirtc-device-example](https://github.com/tangeai/tirtc-device-example)
的 `complete-applications/esp32-s3/device-monitor/`。源代码仓不保存固件 `.bin/.zip`；正式固件统一从
[GitHub Releases](https://github.com/tangeai/tirtc-device-example/releases) 下载。

---

## 首次体验：让设备在 H5 中出图、出声

第一次使用只走一条主线：

```text
下载固件 -> 0x0 烧录 -> 连接 2.4 GHz Wi-Fi -> 输入 6 位验证码 -> H5 查看音视频
```

完成这条链路，说明开发板、Wi-Fi、设备身份、MQTT、TiRTC 和音视频驱动已经端到端
工作。AI、微信 VoIP 和设备互呼应在此基础上逐项体验。

### 开始前

- 开发板：嘉立创/立创开发板“立创·实战派 ESP32-S3 开发板”，型号
  `LCKFB-SZPI-ESP32-S3-VA`。
- USB 数据线一根。只供电、不传数据的线无法烧录。
- 2.4 GHz Wi-Fi。当前开发板不使用 5 GHz Wi-Fi。
- Chrome 或 Edge 浏览器。
- 普通 ESP32-S3 DevKit 缺少本示例所需的屏幕、触摸、音频和摄像头，不能直接替代。

开发板资料：

- [立创开发板项目页](https://lckfb.com/project/detail/lckfb-esp32-s3-va?param=baseInfo)
- [立创商城](https://item.szlcsc.com/43285221.html)

### 步骤 1：下载当前固件

打开 [GitHub Releases](https://github.com/tangeai/tirtc-device-example/releases)，
进入 ESP32-S3 设备端示例 `1.7.6` 对应的 Release，下载：

```text
esp32s3-tirtc-device-monitor-full-v1.7.6.bin
```

这是写入地址为 `0x0` 的完整镜像。普通体验者不需要分别选择 bootloader、
partition table、app 和 storage。

**完成标志：** 文件名为 `esp32s3-tirtc-device-monitor-full-v1.7.6.bin`，并且
SHA-256 与同一 Release 中的 `SHA256SUMS.txt` 一致。名称含 `ota` 的文件不能用于
首次完整烧录。

### 步骤 2：使用官方网页烧录

1. 用 USB 数据线连接开发板和电脑。
2. 用 Chrome 或 Edge 打开 [Espressif ESP Tool](https://espressif.github.io/esptool-js/)。
3. 点击 `Connect`，选择开发板串口。
4. 点击 `Add File` 添加刚下载的完整镜像，`Flash Address` 填写 `0x0`。
5. 首次使用或设备状态不确定时，先执行“擦除闪存”。
6. 点击 `Program`，等待网页明确显示完成。
7. 按一下开发板 RESET，等待屏幕进入主页。

连接串口失败时，按住 BOOT，点一下 RESET，松开 RESET 后再松开 BOOT，然后重新连接。
更完整的说明见 [烧录与 OTA](docs/FLASH_AND_OTA_CN.md)。

**完成标志：** ESP Tool 明确显示烧录完成；按 RESET 后设备进入主页，设置页显示
固件版本 `1.7.6`。

### 步骤 3：连接 Wi-Fi

1. 在主页点击右下角箭头切换到第二页。
2. 点击“设置” -> “Wi-Fi 设置”。
3. 选择 2.4 GHz Wi-Fi，输入密码并点击连接。
4. 返回主页，确认顶部 Wi-Fi 图标已显示真实信号状态。

连接成功后 SSID 和密码会保存到 NVS，下次开机自动连接。

**完成标志：** 顶部 Wi-Fi 图标显示真实信号，设置页能看到 SSID 和 IP；重启后设备
可以自动重连。

### 步骤 4：用 6 位验证码绑定设备

设备首次联网后会自动显示 6 位验证码。浏览器打开
[ThingConnect H5](https://mqtt-demo.tange-ai.com/)，注册或登录账号，在设备页面输入
验证码。

绑定成功后，设备会自动保存 `device_id/device_key`、建立正式 MQTT 长连接并启动
TiRTC 常驻监听。普通体验者不需要手动输入设备 ID 或设备密钥。

**完成标志：** H5 设备列表出现当前设备，设备主页二维码下方显示正式设备 ID。

### 步骤 5：查看实时音视频

1. 在 H5 设备列表中找到刚绑定的设备。
2. 点击设备进入实时查看页。
3. 确认可以看到设备摄像头画面并听到设备麦克风音频。
4. 使用 H5 对讲按钮，确认设备扬声器可以播放网页端声音。
5. 关闭查看页，确认设备回到待机状态。

至此，首个完整闭环已经跑通。

### 第一次失败时怎么查

| 现象 | 先检查 |
| --- | --- |
| ESP Tool 没有串口 | USB 线是否支持数据、串口是否被其他工具占用、是否进入下载模式 |
| 烧录后仍显示旧版本 | 是否选择 `full-v1.7.6.bin`、地址是否为 `0x0`、是否按 RESET |
| Wi-Fi 无法连接 | 是否为 2.4 GHz、密码是否正确、路由器是否允许新设备接入 |
| 没有 6 位验证码 | 是否拿到 IP、系统时间是否同步、设备是否已经绑定 |
| H5 找不到设备 | 登录账号是否正确、绑定是否成功、设备是否保持在线 |
| 有画面但没有声音 | 浏览器音频权限、设备音量、H5 音频订阅是否开启 |

只排查当前失败的步骤，不要同时重置 Wi-Fi、设备身份和 TiRTC 配置。

---

## 下一步：逐项体验四类能力

设备侧功能与 ThingConnect 服务端文档按同一业务边界组织。遇到字段、接口或状态机
问题时，以右侧服务端文档为协议依据。

| 能力 | 设备端入口与完成标志 | 服务端协议文档 |
| --- | --- | --- |
| H5 实时查看与对讲 | H5 出图、出声，按住说话后设备扬声器播放 | [H5 实时查看与按住说话](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-h5-live.md) |
| AI 对讲 | 设备上行语音，收到字幕和 AI 语音回复，退出后资源释放 | [AI 对讲设备接入](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-ai.md) |
| 微信 IoT VoIP | 小程序授权后可双向呼叫、接听、挂断和刷新联系人 | [微信 VoIP 对讲设备接入](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-voip.md) |
| 设备间互呼 | 云端联系人可见，主被叫接通后双向音视频，结束后房间释放 | [设备呼设备接入](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-call.md) |

多业务共用一套音频、摄像头和 TiRTC 连接资源。应用层负责会话仲裁，H5 可在用户接听
来电时被抢占；AI、微信 VoIP 和设备互呼互斥。详细边界见
[ThingConnect 对齐说明](docs/THING_CONNECT_ALIGNMENT_CN.md)。

---

## 版本与构建环境

| 项目 | 当前值 |
| --- | --- |
| 示例版本 | `1.7.6` |
| 芯片 | ESP32-S3，16 MB Flash，8 MB PSRAM |
| 开发板 | `LCKFB-SZPI-ESP32-S3-VA` |
| ESP-IDF | `5.5.4` |
| TiRTC SDK | `2.2.0`，BuildInfo commit `1df9e045a9dc` |
| 工具链 | Xtensa ESP `14.2.0_20260121` |
| FreeRTOS tick | `1000 Hz` |

## 源码构建

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py build
```

命令行烧录和串口日志：

```powershell
idf.py -p COMx flash monitor
```

把 `COMx` 替换为实际串口。构建成功只证明静态集成和链接通过，不能代替 Wi-Fi、绑定、
H5、AI、微信 VoIP、设备互呼和 OTA 的真机验证。

## 工程结构

| 目录 | 责任 |
| --- | --- |
| `main/ui` | LVGL 页面和用户动作，不直接持有协议连接 |
| `main/application` | 应用生命周期、会话仲裁和资源申请/释放 |
| `main/connectivity` | 网络抽象和 Wi-Fi STA 管理 |
| `main/drivers` | 屏幕、触摸、音频和摄像头驱动 |
| `main/protocols` | HTTP、MQTT、RTC 和 TiRTC 适配 |
| `main/services` | 绑定、在线、H5、AI、微信 VoIP、设备互呼和 OTA |
| `components/tirtc_sdk` | TiRTC 头文件、静态库和版本契约 |
| `docs` | 上手、协议对齐、功能流程、架构和烧录说明 |
| `tools` | UI 资源、日志和 Release 打包工具 |

## 文档地图

- [文档入口](docs/README_CN.md)
- [完整用户体验流程](docs/USER_EXPERIENCE_FLOW_CN.md)
- [从 Wi-Fi 到各项功能](docs/GETTING_STARTED_CN.md)
- [烧录与 OTA](docs/FLASH_AND_OTA_CN.md)
- [设备互呼流程](docs/DEVICE_CALL_FLOW_CN.md)
- [ThingConnect 协议与功能对齐](docs/THING_CONNECT_ALIGNMENT_CN.md)
- [源码来源与验证边界](SOURCE_PROVENANCE.md)

## 固件发布规则

- Git 只管理源码、文档、脚本和必要的 SDK 静态库。
- `.bin/.zip` 构建产物不进入 Git 历史。
- 正式烧录包发布到 [GitHub Releases](https://github.com/tangeai/tirtc-device-example/releases)。
- OTA app 仍由 OTA 服务保存，GitHub Release 用于下载、归档和人工烧录。
- 每个 Release 必须附带 `SHA256SUMS.txt` 和 `release-manifest.json`。

## License

MIT © 探鸽智能
