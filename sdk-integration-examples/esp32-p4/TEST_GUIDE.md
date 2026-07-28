# TiRTC ESP32-P4 Demo 验证说明

本文档用于验证 ESP32-P4 设备接入 TiRTC 后的基础联网、上线、主动连接、音视频发送和断开流程。

## 1. 前置条件

验证前请准备：

- 1 到 2 块 ESP32-P4 开发板，板卡带 PSRAM，Host Wi-Fi 可用。
- Windows 电脑已安装 ESP-IDF 5.5.4，并可在 ESP-IDF PowerShell 中执行 `idf.py`。
- 可访问公网的 2.4 GHz Wi-Fi。
- 每块设备对应的 `device_id` 和 `device_secret`。
- 如需板对板验证，A 设备需要知道 B 设备的 `device_id/device_secret`，B 设备也需要知道 A 设备的 `device_id/device_secret`。
- 本地签发 token 所需的 `access_id` 和 `secret_key`。
- 工程自带 `send_video.h264` 和 `send_audio.pcma`，烧录时会作为 SPIFFS 分区一起写入设备。

本地 token 签发机制仅用于快速验证。量产固件建议改为由业务服务端签发 token。

## 2. 参数配置

### 2.1 Wi-Fi 配置

修改 `main/app_config.h`：

```c
#define APP_WIFI_SSID "your_wifi_ssid"
#define APP_WIFI_PASSWORD "your_wifi_password"
```

### 2.2 TiRTC 配置

修改 `main/tirtc/tirtc_config.h`：

```c
#define TIRTC_DEVICE_ID "your_device_id"
#define TIRTC_DEVICE_SECRET_KEY "your_device_secret"

#define TIRTC_REMOTE_DEVICE_ID "peer_device_id"
#define TIRTC_REMOTE_DEVICE_SECRET_KEY "peer_device_secret_key"

#define TIRTC_AUTO_PUSH_LOCAL_MEDIA_ON_CONNECT 1

#define TIRTC_TOKEN_ACCESS_ID "your_token_access_id"
#define TIRTC_TOKEN_SECRET_KEY "your_token_secret_key"
```

`TIRTC_AUTO_PUSH_LOCAL_MEDIA_ON_CONNECT` 默认为 `1`，连接建立后立即推送本机 H264/PCMA；如需等待对端订阅后再推送本机测试媒体，可改为 `0`。

板对板验证时，两块板需要互相配置为对端：

| 设备 | 本机配置 | 对端配置 |
|---|---|---|
| A 设备 | A 的 `device_id/device_secret` | B 的 `device_id/device_secret` |
| B 设备 | B 的 `device_id/device_secret` | A 的 `device_id/device_secret` |

验证时建议只按其中一侧的 BOOT 键，避免两边同时发起连接。

## 3. 编译和烧录

在 ESP-IDF PowerShell 中进入工程目录后执行：

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py build
```

烧录并打开串口日志：

```powershell
idf.py -p COMx flash monitor
```

将 `COMx` 替换为实际串口号。建议使用 `idf.py flash`，这样会同时烧录应用固件和 `storage` SPIFFS 分区。

## 4. 验证步骤

### 4.1 启动上线

1. 烧录固件后重启设备。
2. 打开串口 monitor。
3. 等待设备连接 Wi-Fi。
4. 等待 SNTP 时间同步完成。
5. 等待 TiRTC 上线成功。

预期结果：

- Wi-Fi 能获取 IP。
- 系统时间同步成功。
- TiRTC SDK 版本打印为 `0.1.4`。
- 设备进入在线状态。

预期日志示例：

```text
Wi-Fi 已连接
系统时间同步完成
TiRTC 版本: 0.1.4
TiRTC 发送缓冲: 131072 bytes
TiRTC 服务地址: http://ep-tirtc.tange365.com
本地测试媒体已就绪: H264=... bytes PCMA=... bytes
TiRTC 启动请求已提交: device_id=...
TiRTC 已上线，可接收入站连接，也可主动连接远端设备
```

### 4.2 板对板连接和收流

以下以 A 设备主动连接 B 设备为例。

1. 确认 A、B 两台设备都已上线。
2. 按下 A 设备的 BOOT 键。
3. A 设备生成本次连接 token，并主动连接 `TIRTC_REMOTE_DEVICE_ID`。
4. 连接成功后，设备默认主动发送本地 H264/PCMA 测试媒体。
5. 需要看对端画面时，调用订阅接口订阅对端视频 `stream=11` 和音频 `stream=10`。
6. 对端收到订阅后也会发送本地 H264/PCMA 测试媒体。
7. 取消订阅会停止对应媒体，再次订阅会重新开始发送。

A 侧预期日志：

```text
收到 BOOT 键触发，切换 TiRTC 连接状态
BOOT 按键触发：当前无连接，主动连接目标设备
准备创建主动连接任务 remote_id=...
主动连接任务已创建
开始本地生成本次主动连接 token
本地 token 已生成 remote_id=... subject=... ttl=...
主动连接开始 remote_id=...
主动连接成功 hconn=...
当前连接已建立 hconn=...
```

订阅对端媒体时的预期日志：

```text
[CTRL][TX] 订阅对端视频 stream=11 ret=0 OK
[CTRL][TX] 订阅对端音频 stream=10 ret=0 OK
[RX][video] 接收统计：视频帧=25，流ID=11，长度=...字节，时间戳=...
[RX][audio] 接收统计：音频包=50，流ID=10，长度=...字节，时间戳=...
```

B 侧预期日志：

```text
TiRTC 收到远端连接 hconn=...
当前连接已建立 hconn=...
```

B 收到 A 的订阅后，预期日志：

```text
[CTRL][RX] 对端订阅本机视频 stream=11
[TX][video] 视频发送准备完成：格式=H264，流ID=11
[TX][video] 发流开始：视频=H264，目标帧率=25 fps，流ID=11
[CTRL][RX] 对端订阅本机音频 stream=10
[TX][audio] 音频发送准备完成：格式=PCMA，流ID=10
[TX][audio] 发流开始：音频=PCMA，采样=8k A-law，流ID=10
[TX][video] 发送统计：视频帧=25，流ID=11，发送缓冲=...
[TX][audio] 发送统计：音频包=50，流ID=10，发送缓冲=...
```

### 4.3 远端呼入

1. 确认 ESP32-P4 设备已经上线。
2. 使用远端设备或业务应用呼入该设备。
3. 等待连接建立。
4. 连接建立后默认开始发送本地 H264/PCMA 测试媒体。
5. 如需等待订阅后再发送，可将 `TIRTC_AUTO_PUSH_LOCAL_MEDIA_ON_CONNECT` 设为 `0`。

预期日志与 4.2 中 B 侧日志一致。

### 4.4 主动断开

连接状态下再次按下 BOOT 键。

预期日志：

```text
收到 BOOT 键触发，切换 TiRTC 连接状态
BOOT 按键触发：当前已有连接，主动断开两端连接
主动断开当前连接 ret=0 OK
TiRTC 连接断开 hconn=...
当前连接已清理 hconn=...
```

### 4.5 取消订阅和再次订阅

连接保持时，对端可以取消订阅本机视频或音频，再次订阅后本机会重新启动对应发送任务。

预期日志：

```text
[CTRL][RX] 对端取消订阅本机视频 stream=11
[TX][video] 本机 H264 发送任务已停止
[CTRL][RX] 对端订阅本机视频 stream=11
[TX][video] 视频发送准备完成：格式=H264，流ID=11
[TX][video] 发流开始：视频=H264，目标帧率=25 fps，流ID=11

[CTRL][RX] 对端取消订阅本机音频 stream=10
[TX][audio] 本机 PCMA 发送任务已停止
[CTRL][RX] 对端订阅本机音频 stream=10
[TX][audio] 音频发送准备完成：格式=PCMA，流ID=10
[TX][audio] 发流开始：音频=PCMA，采样=8k A-law，流ID=10
```

## 5. 异常排查

| 现象 | 检查项 |
|---|---|
| Wi-Fi 未连接 | 检查 `APP_WIFI_SSID`、`APP_WIFI_PASSWORD`、2.4 GHz 网络和信号强度 |
| 时间同步未完成 | 检查网络是否可访问公网 NTP 服务 |
| TiRTC 未上线 | 检查 `TIRTC_DEVICE_ID`、`TIRTC_DEVICE_SECRET_KEY` 和服务地址 |
| 主动连接未建立 | 检查 `TIRTC_REMOTE_DEVICE_ID`、对端在线状态和 token 签发参数 |
| 没有音视频发送 | 检查 `storage` 分区是否已烧录，以及是否出现 `[TX][video]`、`[TX][audio]` 发送日志 |
| 只能发送不能接收 | 检查对端是否发送媒体，以及本机是否已订阅对端音视频 |

保留完整串口输出时，建议覆盖 Wi-Fi、SNTP、TiRTC 启动、连接回调和 H264/PCMA 收发统计日志。
