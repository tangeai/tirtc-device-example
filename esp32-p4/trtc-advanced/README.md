# TiRTC ESP32-P4 设备端示例

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](../../LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.5.4-E7352C?logo=espressif)](https://docs.espressif.com/projects/esp-idf/)
[![Chip](https://img.shields.io/badge/Chip-ESP32--P4-000000)](https://www.espressif.com/en/products/socs/esp32-p4)
[![TiRTC SDK](https://img.shields.io/badge/TiRTC%20SDK-2.2.0-1769AA)](https://docs.tange.ai/products/tirtc/overview/what-is-tirtc.html)

面向 Waveshare `ESP32-P4-WIFI6-Touch-LCD-3.5` 的 TiRTC 参考工程。它与
[tangeai/tirtc-server-example](https://github.com/tangeai/tirtc-server-example)
使用同一套 ThingConnect 设备协议，并把显示、触摸、摄像头、音频、ESP-Hosted
Wi-Fi、P4 H264 硬编和内存策略落实到 P4 平台。

固件 `.bin/.zip` 不提交到 Git 历史，正式固件从
[ESP32-P4 v1.1.0 Release](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-p4-trtc-advanced-v1.1.0)
下载。

---

## 首次体验：让 P4 在 H5 中出图、出声

### 开始前

- Waveshare `ESP32-P4-WIFI6-Touch-LCD-3.5` 开发板。
- 一根可以传输数据的 USB 线。
- 2.4 GHz Wi-Fi。
- Chrome 或 Edge 浏览器。

### 步骤 1：下载完整镜像

打开
[ESP32-P4 v1.1.0 Release](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-p4-trtc-advanced-v1.1.0)，
下载：

```text
tirtc-esp32p4-device-monitor-full-v1.1.0.bin
```

这是写入地址为 `0x0` 的 16 MiB 完整镜像。首次体验不要只烧录 app/OTA 文件。

**完成标志：** 文件名为 `tirtc-esp32p4-device-monitor-full-v1.1.0.bin`，大小为
`16777216` 字节。S3 固件不能烧到 P4。

### 步骤 2：使用官方网页烧录

1. 用 USB 数据线连接开发板和电脑。
2. 用 Chrome 或 Edge 打开 [Espressif ESP Launchpad](https://espressif.github.io/esp-launchpad/)。
3. 进入 DIY 烧录模式并选择开发板串口。
4. 添加完整镜像，地址填写 `0x0`。
5. 首次使用或设备状态不确定时，先擦除闪存。
6. 等待烧录完成，按一下 RESET。

找不到串口时，先检查 USB 线是否支持数据传输；仍然失败时，让开发板进入下载模式后
重新连接。

**完成标志：** ESP Launchpad 明确显示烧录完成；按 RESET 后设备进入主页，设置页显示
固件版本 `1.1.0`。

### 步骤 3：联网和绑定

1. 在设备主页进入“设置” -> “Wi-Fi 设置”。
2. 选择 2.4 GHz Wi-Fi 并连接。
3. 设备联网后会显示 6 位验证码。
4. 打开 [ThingConnect H5](https://mqtt-demo.tange-ai.com/)，登录后输入验证码。

绑定成功后，设备会自动保存身份并建立正式 MQTT 和 TiRTC 链路。普通体验者不需要
手动填写设备 ID 或设备密钥。

**完成标志：** 顶部 Wi-Fi 图标显示真实信号，H5 设备列表出现当前设备，设备主页显示
正式设备 ID。

### 步骤 4：查看实时音视频

1. 在 H5 设备列表中找到刚绑定的设备。
2. 打开实时查看页。
3. 确认 H264 画面和设备麦克风声音正常。
4. 使用 H5 对讲，确认设备扬声器正常播放。
5. 退出查看，确认设备回到待机状态。

首个闭环完成后，再按 AI 对讲、微信 VoIP、设备互呼、OTA 的顺序逐项验证。接口和
状态机以 [ThingConnect 文档](https://github.com/tangeai/tirtc-server-example/tree/main/thing-connect)
为准。

**完成标志：** H5 能看到实时画面并听到设备麦克风；使用 H5 对讲时，设备扬声器能
播放网页端声音；退出后设备回到待机状态。

### 第一次失败时怎么查

| 现象 | 先检查 |
| --- | --- |
| ESP Launchpad 没有串口 | USB 线是否支持数据、串口是否被占用、开发板是否进入下载模式 |
| 烧录后仍显示旧版本 | 是否选择 `full-v1.1.0.bin`、地址是否为 `0x0`、是否按 RESET |
| Wi-Fi 无法连接 | 是否为 2.4 GHz、密码是否正确、ESP-Hosted 网络是否启动 |
| 没有 6 位验证码 | 是否拿到 IP、系统时间是否同步、设备是否已经绑定 |
| H5 找不到设备 | 登录账号是否正确、绑定是否成功、设备是否保持在线 |
| 有画面但没有声音 | 浏览器音频权限、设备音量、H5 音频订阅是否开启 |

只排查当前失败的步骤，不要同时重置 Wi-Fi、设备身份和 TiRTC 配置。

---

## 下一步：逐项体验四类能力

设备侧功能与 ThingConnect 服务端文档按同一业务边界组织。字段、错误码和状态机以
服务端文档为协议依据。

| 能力 | 设备端完成标志 | 服务端协议文档 |
| --- | --- | --- |
| H5 实时查看与对讲 | H264 出图、设备音频上行、H5 对讲下行均正常 | [H5 实时查看与按住说话](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-h5-live.md) |
| AI 对讲 | 麦克风上行，收到字幕和 AI 语音，退出后资源释放 | [AI 对讲设备接入](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-ai.md) |
| 微信 IoT VoIP | 小程序授权后可双向呼叫、接听和挂断 | [微信 VoIP 对讲设备接入](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-voip.md) |
| 设备间互呼 | 联系人可见，主被叫双向音视频，结束后房间释放 | [设备呼设备接入](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-call.md) |

---

## 版本信息

| 项目 | 内容 |
| --- | --- |
| 示例版本 | 1.1.0 |
| 发布时间 | 2026-07-23 |
| 目标芯片 | ESP32-P4 |
| 目标开发板 | Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 |
| ESP-IDF | 5.5.4 |
| TiRTC SDK | 2.2.0 |
| 默认视频链路 | OV5647 YUV420 -> ESP32-P4 H264 -> TiRTC |
| 默认上行参数 | 1280x960, 20 fps, 4 Mbps |

## 核心能力

- 横屏 LVGL 设备端 UI，分辨率 `480x320`。
- Wi-Fi 连接、SNTP 校时、设备绑定、设备在线和状态上报。
- TiRTC 设备上线、远端呼入、连接断开、音视频订阅和关键帧请求。
- 摄像头 H264 上行：P4 侧使用 YUV420 输入和硬件 H264 编码，不启用本地摄像头预览抢占资源。
- 本机麦克风上行和远端音频播放。
- P4 设备间双向音视频通话：本机 H264 上行、远端 H264 解码、PSRAM 直通 LCD 显示和低延迟挂断。
- IPC 查看、设备通话、微信 VoIP 和 AI Chat 统一使用按业务所有权启停的 AEC 策略。
- 微信 VoIP 支持 PCMA 双向音频和本机摄像头 H264 主动上行，并提供 AI Chat、OTA、设置和调试信息页。
- IPC 查看页展示绑定二维码、设备 ID、分辨率、帧率和码率。

## 设备上线流程

1. Wi-Fi 联网并完成系统校时。
2. 从 `https://ep-open.tange-ai.com/services` 获取设备、MQTT、VoIP、AI、呼叫和 TiRTC 服务地址；发现失败时使用编译期备用地址。
3. 未绑定设备向 `/v1/device/report` 只上报物理 MAC，临时 MQTT 完成订阅后再展示 6 位绑定码。
4. 收到 `auth_grant` 后先发送 QoS 1 ACK，并等待 broker PUBACK，再保存设备凭证并关闭临时连接。
5. 已绑定设备使用 `X-Device-Id`、`X-Timestamp`、`X-Nonce`、`X-Signature` 和 `X-Mac` 获取 MQTT token。
6. 正式 MQTT 长连接订阅 `/cmd` 和 `/notify`，通过 `/ack` 确认命令，并使用 `/up` 上报心跳和状态。
7. 设备在线后设置 TiRTC 的 secret key 和物理 client ID，再以 device ID 调用 `TiRtcStart()`；以 `SYS_STARTED` 回调作为 SDK 已上线的判据。

服务端解绑通知会保留本地设备凭证，待正式 MQTT ACK 确认后走签名 Report 重新核对映射，不在设备端直接清除身份。

## 工程结构

```text
components/tirtc_sdk/        TiRTC SDK 头文件和 ESP32-P4 静态库
main/application/            应用生命周期、业务状态和配置持久化
main/ui/                     LVGL 界面和用户动作分发
main/services/               绑定、在线、音频、AI Chat、微信 VoIP、OTA、二维码等服务
main/protocols/tirtc/        TiRTC SDK 适配、连接状态、媒体发送队列和回调
main/media/                  摄像头 H264 pipeline、媒体策略和弱网配置接口
main/drivers/                显示、触摸、摄像头、音频等硬件驱动封装
main/hardware/               Waveshare P4 开发板初始化和 BSP 边界
docs/                        架构说明和发布说明
tools/                       本地构建、日志和 Release 打包工具
```

UI 只负责展示和触发动作，连接句柄、摄像头、音频、TiRTC SDK 生命周期分别由 application、media、drivers、protocols 分层持有。

## 源码构建

在 Windows PowerShell 中执行：

```powershell
cd $env:USERPROFILE\Desktop\tirtc-device-example\esp32-p4\trtc-advanced
cmd /c "C:\esp\v5.5.4\esp-idf\export.bat >nul && idf.py reconfigure build"
```

构建完成后，固件生成在本地 `build/`。正式发布资产以
`build/flasher_args.json` 的真实 offset 为准，不手写固定烧录地址，也不把构建产物
提交到 Git。

## 烧录

```powershell
cd $env:USERPROFILE\Desktop\tirtc-device-example\esp32-p4\trtc-advanced
cmd /c "C:\esp\v5.5.4\esp-idf\export.bat >nul && idf.py -p COMx flash monitor"
```

把 `COMx` 替换为当前开发板串口。

## 配置入口

默认源码不包含真实 Wi-Fi 密码、设备密钥、token 或私有账号。常用默认项在：

- `main/application/app_config.h`：Wi-Fi、OTA、设备绑定、TiRTC 服务默认地址。
- `main/application/app_rtc_config.c`：TiRTC 环境配置和 NVS 读取。
- `main/protocols/tirtc/tirtc_session_options.h`：TiRTC 发送缓冲、自动推流、音视频队列和日志等级。
- `main/Kconfig.projbuild`：P4 H264、DMA、摄像头 trace、调试屏幕服务和弱网开关。
- `sdkconfig.defaults`：发布构建默认配置。

当前默认策略：

- `CONFIG_FREERTOS_HZ=1000`
- `CONFIG_APP_RTC_H264_BITRATE=4000000`
- `CONFIG_APP_RTC_H264_FPS=20`
- `CONFIG_APP_RTC_H264_GOP=40`
- `CONFIG_APP_DEVICE_CALL_VIDEO_WIDTH=480`
- `CONFIG_APP_DEVICE_CALL_VIDEO_HEIGHT=320`
- `CONFIG_APP_DEVICE_CALL_VIDEO_FPS=15`
- `CONFIG_APP_DEVICE_CALL_VIDEO_BITRATE=2000000`
- `CONFIG_APP_AUDIO_AEC_ENABLE=y`
- `CONFIG_APP_RTC_VIDEO_AUTO_ADAPT_ENABLE` 默认关闭
- `CONFIG_APP_RTC_WAIT_VIDEO_SUBSCRIBE_BEFORE_CAPTURE` 默认关闭
- `TIRTC_AUTO_PUSH_LOCAL_MEDIA_ON_CONNECT=1`
- `TIRTC_SESSION_SDK_LOG_LEVEL=0`

弱网降级和分辨率/帧率调节接口已经放在 `main/media/media_governor.*`，默认不自动触发。上层后续可以按 TiRTC 或业务侧弱网信号调用该接口。

## 运行验证

建议按下面顺序看日志和界面：

1. 启动到首页，看到 `display ready`、`touch input ready` 和 `system ready`。
2. Wi-Fi 连接成功，日志出现 `wifi connected`、IP 和 RSSI。
3. 设备绑定页出现 6 位绑定码；绑定完成后 IPC 查看页显示设备 ID。
4. TiRTC 启动完成，日志出现 `TiRtcInit done`、`TiRtcStart accepted`、`rtc system started`。
5. Web 或手机端呼入后，设备进入连接态，日志出现 `conn accepted` 和 `peer_connection state`。
6. 视频链路出现 `H264 encoder ready`、`camera pipeline first upstream frame` 和周期性 `camera pipeline stats`。
7. 音频链路出现 `mic capture level`、`local audio tx` 和 `remote audio accepted`。
8. 返回首页后，状态快照应回到 `camera=0 rtc=0 fps=0.0 bitrate=0kbps`。

## 固件发布

- Git 只保存源码、文档、脚本和构建所需的 SDK 静态库。
- `.bin/.zip` 发布到
  [GitHub Releases](https://github.com/tangeai/tirtc-device-example/releases)，不进入 Git 历史。
- 普通体验者下载 `tirtc-esp32p4-device-monitor-full-v1.1.0.bin`，烧录地址 `0x0`。
- 维护者可以下载 web-flash 包，按包内 `flash_args.txt` 的真实 offset 烧录。
- Release 必须附带 `SHA256SUMS.txt` 和 `release-manifest.json`。

发布前确认 README、release note、版本号、TiRTC SDK 版本、构建产物和 SHA-256 一致。
仓库级规则见 [发布与二进制管理](../../docs/RELEASES_CN.md)。
