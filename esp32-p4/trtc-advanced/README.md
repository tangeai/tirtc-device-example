# TiRTC ESP32-P4 Device Monitor Demo

本工程是面向 Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 的 TiRTC 设备监控端示例。工程保持 S3 监控端的业务能力，并把显示、触摸、摄像头、音频、ESP-Hosted Wi-Fi、P4 H264 硬编和内存策略替换为 P4 平台实现。

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
release_assets/              发布时生成的烧录资产
```

UI 只负责展示和触发动作，连接句柄、摄像头、音频、TiRTC SDK 生命周期分别由 application、media、drivers、protocols 分层持有。

## 构建

在 Windows PowerShell 中执行：

```powershell
cd $env:USERPROFILE\Desktop\demo_p4
cmd /c "C:\esp\v5.5.4\esp-idf\export.bat >nul && idf.py reconfigure build"
```

构建完成后，固件会生成在 `build/`，同时复制一份到 `firmware/`。正式发布资产以 `build/flasher_args.json` 的真实 offset 为准，不手写固定烧录地址。

## 烧录

```powershell
cd $env:USERPROFILE\Desktop\demo_p4
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

## 发布资产

发布脚本会生成两类资产：

- `release_assets/web-flash/v1.1.0/`：维护者多地址烧录资产、`flash_args.txt`、`SHA256SUMS.txt`。
- `release_assets/web-install/v1.1.0/`：如本机 esptool 可用，会生成 0x0 完整镜像和一键烧录 zip。

发布前请确认 README、release note、版本号、TiRTC SDK 版本、构建产物和 SHA256 一致。
