# ESP32-P4 最小 TiRTC 集成示例

本工程演示 ESP32-P4 设备通过 Wi-Fi 接入 TiRTC，完成设备上线、远端呼入、BOOT 按键主动连接、H264 视频发送、PCMA 音频发送和连接断开。

TiRTC 产品能力可参考官网文档：[TiRTC 产品介绍](https://docs.tange.ai/products/tirtc/overview/what-is-tirtc.html)。

## 功能概览

- 设备启动后自动连接 Wi-Fi。
- 联网后通过 SNTP 同步系统时间。
- 使用设备 ID 和设备密钥调用 `TiRtcStart()` 上线。
- 支持远端呼入连接。
- 支持 BOOT 按键切换连接状态：未连接时主动连接目标设备，已连接时主动断开。
- 连接建立后默认主动推送本地 H264 视频和 PCMA 音频。
- 支持通过配置宏切换为等待对端订阅后再推送本地媒体；需要看对端画面时调用订阅接口。
- 保留常用控制接口：断开连接、订阅/取消订阅音视频、请求关键帧。

## 目录说明

```text
tirtc_esp32p4_wifi_link_demo
├─ components/tirtc_sdk/              TiRTC SDK 头文件和 ESP32-P4 静态库
├─ dependencies.lock                  ESP-IDF managed component 版本锁定文件
├─ firmware/                          已编译好的烧录产物
├─ PACKAGE_CONTENTS.md                包内容说明
├─ main/app_main.c                    示例主流程
├─ main/app_config.h                  Wi-Fi 和 BOOT 键配置
├─ main/app_version.h                 示例版本信息
├─ main/system/                       Wi-Fi、时间同步、BOOT 按键
├─ main/tirtc/tirtc_config.h          TiRTC 接入参数
├─ main/tirtc/tirtc_app.c             TiRTC 上线、回调、连接和控制接口
├─ main/tirtc/tirtc_connect.c         主动连接和 token 获取入口
├─ main/tirtc/tirtc_token.c           本地 token 签发示例
├─ main/tirtc/local_media_stream.c    本地 H264/PCMA 测试媒体发送
├─ main/tirtc/h264_file_source.c      H264 文件逐帧读取
└─ main/tirtc/test_assets/            随 SPIFFS 烧录的测试媒体文件
```

## 快速接入

1. 修改 Wi-Fi 参数：`main/app_config.h`

```c
#define APP_WIFI_SSID "your_wifi_ssid"
#define APP_WIFI_PASSWORD "your_wifi_password"
```

2. 修改 TiRTC 参数：`main/tirtc/tirtc_config.h`

```c
#define TIRTC_DEVICE_ID "your_device_id"
#define TIRTC_DEVICE_SECRET_KEY "your_device_secret"

#define TIRTC_REMOTE_DEVICE_ID "peer_device_id"
#define TIRTC_REMOTE_DEVICE_SECRET_KEY "peer_device_secret_key"

#define TIRTC_AUTO_PUSH_LOCAL_MEDIA_ON_CONNECT 1

#define TIRTC_TOKEN_ACCESS_ID "your_token_access_id"
#define TIRTC_TOKEN_SECRET_KEY "your_token_secret_key"
```

`TIRTC_AUTO_PUSH_LOCAL_MEDIA_ON_CONNECT` 用于控制连接建立后的本机推流行为：默认值 `1` 表示主动连接或远端呼入建立后立即推送本机 H264/PCMA；设为 `0` 时，等待对端订阅后再推送本机 H264/PCMA。

`TIRTC_REMOTE_DEVICE_SECRET_KEY`、`TIRTC_TOKEN_ACCESS_ID` 和 `TIRTC_TOKEN_SECRET_KEY` 仅用于本地生成主动连接 token。量产固件建议改为由业务服务端签发 token，再下发给设备使用。

## 编译环境

- 芯片：ESP32-P4
- ESP-IDF：5.5.4
- 工具链：riscv32-esp-elf-gcc-14.2.0_20260121
- Flash：16 MB
- PSRAM：已启用
- P4 Wi-Fi：ESP-HOST-WIFI / ESP32-C6 Hosted SDIO

## 编译和烧录

在 ESP-IDF PowerShell 中进入工程目录后执行：

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py build
```

首次编译时，ESP-IDF 会根据 `main/idf_component.yml` 和 `dependencies.lock` 获取并锁定 P4 Wi-Fi 相关 managed components。工程根目录下的 `firmware/` 已包含随包提供的最新烧录产物。

烧录并查看串口日志：

```powershell
idf.py -p COMx flash monitor
```

将 `COMx` 替换为实际串口号。建议使用 `idf.py flash`，这样会同时烧录应用固件和 `storage` SPIFFS 分区。

## 预期启动日志

设备正常启动后，串口会看到类似日志：

```text
TiRTC ESP32-P4 Wi-Fi Link Demo v1.0.2 release=2026-06-24 sdk=0.1.4
Wi-Fi 已连接
系统时间同步完成
TiRTC 版本: 0.1.4
TiRTC 发送缓冲: 131072 bytes
TiRTC 服务地址: http://ep-tirtc.tange365.com
本地测试媒体已就绪: H264=... bytes PCMA=... bytes
TiRTC 启动请求已提交: device_id=...
TiRTC 已上线，可接收入站连接，也可主动连接远端设备
```

首行是当前固件保留的旧启动日志标识，不作为公开项目名称或目录类别。

## 主动连接流程

未连接时按下 BOOT 键，设备会连接 `TIRTC_REMOTE_DEVICE_ID`：

```text
收到 BOOT 键触发，切换 TiRTC 连接状态
BOOT 按键触发：当前无连接，主动连接目标设备
准备创建主动连接任务 remote_id=...
主动连接任务已创建
开始本地生成本次主动连接 token
主动连接开始 remote_id=...
主动连接成功 hconn=...
当前连接已建立 hconn=...
```

已连接时再次按下 BOOT 键，设备会主动断开当前连接：

```text
BOOT 按键触发：当前已有连接，主动断开两端连接
主动断开当前连接 ret=0 OK
```

## 音视频链路

示例工程把 `send_video.h264` 和 `send_audio.pcma` 打包到 SPIFFS。默认配置下，连接建立后会主动推送本地测试媒体：视频按 25 fps 发送 H264 帧，音频按 8 kHz A-law 每包约 20 ms 发送 PCMA 数据。若将 `TIRTC_AUTO_PUSH_LOCAL_MEDIA_ON_CONNECT` 设为 `0`，则等待对端订阅本机视频或音频后再开始推送；对端取消订阅后会停止对应发送任务，再次订阅会重新启动。

接入真实摄像头或麦克风时，主要替换 `main/tirtc/local_media_stream.c` 中读取本地文件的位置，保留 `TIRTCFRAMEINFO` 和发送 API 的写法：

```c
int ret = TiRtcSendVideoStream(conn, &frame, (void *)data);
int ret = TiRtcSendAudioStream(conn, &frame, packet);
```

## 常用接口位置

- 设备上线：`tirtc_start()`
- 释放资源：`tirtc_deinit()`
- 主动连接：`tirtc_connect_configured()`
- 主动断开：`tirtc_disconnect_current()`
- BOOT 连接状态切换：`tirtc_toggle_connection()`
- 订阅对端视频：`tirtc_subscribe_remote_video()`
- 取消订阅对端视频：`tirtc_unsubscribe_remote_video()`
- 订阅对端音频：`tirtc_subscribe_remote_audio()`
- 取消订阅对端音频：`tirtc_unsubscribe_remote_audio()`
- 请求关键帧：`tirtc_request_remote_key_frame()`

## 注意事项

- 默认关闭 Wi-Fi 省电，优先保证音视频链路稳定。
- 本地 token 签发只用于快速验证；量产固件建议改为业务服务端签发。
- `libTiRTC.a` 已放在 `components/tirtc_sdk/lib/esp32p4/`，工程会自动链接。
- 当前 `libTiRTC.a` 为 ESP32-P4 KCP 单库包，已包含 tgtrp/KCP 底层对象，接入工程不需要额外链接 `libwebrtc_nosctp.a`。
- SDK 初始化流程会先执行平台层初始化，再创建 TiRTC/tgtrp 运行资源。
- 测试媒体文件会通过 SPIFFS 随工程一起烧录，分区名为 `storage`。
- 示例包仅保留占位配置，不包含真实 Wi-Fi、设备 ID、access_id 或 secret_key。
- 更完整的验证步骤见 [TEST_GUIDE.md](TEST_GUIDE.md)。

## 候选来源

本目录来自含 TiRTC 头文件和静态库未提交修改的源工作树。当前版本声明与静态库契约仍需发布前
复核，复制范围和未验证边界见 [来源与验证边界](SOURCE_PROVENANCE.md)。
