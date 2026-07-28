# TiRTC ESP32-S3 Demo 测试说明

本文档用于指导客户验证 ESP32-S3 设备接入 TiRTC 后的基础联网、上线、主动连接、音视频发送和断开流程。

## 1. 前置条件

测试前请先确认：

- 已准备 1 到 2 块 ESP32-S3 开发板，且板卡带 PSRAM。
- Windows 电脑已安装 ESP-IDF 5.5.4，并可在 ESP-IDF PowerShell 中正常执行 `idf.py`。
- 测试 Wi-Fi 为 2.4GHz 网络，开发板可以访问公网。
- 已获取每块设备的 `device_id` 和 `device_secret`。
- 如需板对板测试，A 设备需要知道 B 设备的 `device_id/device_secret`，B 设备也需要知道 A 设备的 `device_id/device_secret`。
- 已获取测试用 `access_id` 和 `secret_key`。
- 工程自带 `send_video.h264` 和 `send_audio.pcma`，烧录时会作为 SPIFFS 分区一起写入设备。

本 demo 中的本地 token 签发机制仅用于联调。正式产品不建议把 `access_id`、`secret_key` 或对端设备密钥固化在固件中，应改为由业务服务端签发 token。

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
#define TIRTC_CLIENT_ID TIRTC_DEVICE_ID

#define TIRTC_REMOTE_DEVICE_ID "peer_device_id"
#define TIRTC_REMOTE_DEVICE_SECRET_KEY "peer_device_secret_key"

#define TIRTC_TOKEN_ACCESS_ID "your_token_access_id"
#define TIRTC_TOKEN_SECRET_KEY "your_token_secret_key"
```

板对板测试时，两块板需要互相配置为对端：

| 设备 | 本机配置 | 对端配置 |
|---|---|---|
| A 设备 | A 的 `device_id/device_secret` | B 的 `device_id/device_secret` |
| B 设备 | B 的 `device_id/device_secret` | A 的 `device_id/device_secret` |

这样 A 按 BOOT 可以主动连接 B，B 按 BOOT 也可以主动连接 A。测试时建议只按其中一侧，避免两边同时发起连接。

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

## 4. 测试步骤

### 4.1 启动上线测试

1. 烧录固件后重启设备。
2. 打开串口 monitor。
3. 等待设备连接 Wi-Fi。
4. 等待 SNTP 时间同步完成。
5. 等待 TiRTC 上线成功。

预期结果：

- Wi-Fi 能获取 IP。
- 系统时间同步成功。
- TiRTC SDK 版本打印为 `2.2.1`。
- 日志确认设备密钥 option 已在 `TiRtcStart()` 前设置成功。
- 设备进入在线状态。

预期日志示例：

```text
Wi-Fi 已连接
系统时间同步完成
TiRTC 版本: 2.2.1
本地测试媒体已就绪: H264=... bytes PCMA=... bytes
TiRTC 启动配置: device_id=... client_id=... secret_len=32
TiRTC 启动选项已设置: device_secret_key length=...
TiRTC 启动选项已设置: client_id length=...
TiRTC 启动请求已提交 device_id=...
TiRTC 已上线，可接收入站连接，也可主动连接远端设备
```

### 4.2 板对板连接和收流测试

以下以 A 设备主动连接 B 设备为例。

1. 确认 A、B 两台设备都已上线。
2. 按下 A 设备的 BOOT 键。
3. A 设备会本地生成测试 token，并主动连接 `TIRTC_REMOTE_DEVICE_ID`。
4. 连接成功后，A 自动订阅 B 的视频 `stream=11` 和音频 `stream=10`。
5. B 收到订阅后，开始发送本地 H264 视频和 PCMA 音频。
6. A 开始收到 B 发来的音视频数据。

A 侧预期日志：

```text
收到 BOOT 键触发，切换 TiRTC 连接状态
BOOT 按键触发：当前无连接，主动连接目标设备
准备创建主动连接任务 remote_id=...
主动连接任务已创建
开始本地生成本次主动连接 token
本地 token 已生成 remote_id=... subject=... ttl=...
主动连接开始 remote_id=... token=local
TiRTC 状态：已上线=1，连接=主动连接中
主动连接成功 hconn=...
当前连接已建立 hconn=...
主动连接侧作为观看端，只订阅对端音视频，不发送本地测试媒体
[CTRL][TX] 订阅对端视频 stream=11 ret=0
[CTRL][TX] 订阅对端音频 stream=10 ret=0
[RX][video] 接收统计：视频帧=25，流ID=11，长度=...字节，时间戳=...，关键帧=...
[RX][audio] 接收统计：音频包=50，流ID=10，长度=...字节，时间戳=...
```

B 侧预期日志：

```text
TiRTC 收到远端连接 hconn=...
当前连接已建立 hconn=...
等待对端订阅本机音视频后再开始发送测试媒体
[CTRL][RX] 对端订阅本机视频 stream=11
[TX][video] 视频发送准备完成：格式=H264，流ID=11
[TX][video] 发流开始：视频=H264，目标帧率=25 fps，流ID=11
[CTRL][RX] 对端订阅本机音频 stream=10
[TX][audio] 音频发送准备完成：格式=PCMA，流ID=10
[TX][audio] 发流开始：音频=PCMA，采样=8k A-law，流ID=10
[TX][video] 发送统计：视频帧=25，流ID=11，发送缓冲=...字节
[TX][audio] 发送统计：音频包=50，流ID=10，发送缓冲=...字节
```

连接稳定后，A 侧状态日志中的“接收视频帧”和“接收音频包”应持续增加。B 侧“发送缓冲”不应长期持续升高；如果持续升高，说明发送侧可能遇到网络拥塞或对端接收异常。

### 4.3 BOOT 断开测试

连接状态下，再次按下当前连接设备上的 BOOT 键。

预期结果：

- 本机主动断开当前 TiRTC 连接。
- 对端收到断开事件。
- 发送侧停止本地 H264/PCMA 测试媒体任务。
- 后续状态日志显示 `connection=idle`。

本机预期日志：

```text
收到 BOOT 键触发，切换 TiRTC 连接状态
BOOT 按键触发：当前已有连接，主动断开两端连接
主动断开当前连接 ret=0 OK
```

对端预期日志：

```text
TiRTC 连接断开 hconn=...
当前连接已清理 hconn=...
[TX][video] 本机 H264 发送任务已停止
[TX][audio] 本机 PCMA 发送任务已停止
```

## 5. 常见异常判断

### 5.1 Wi-Fi 连接失败

重点检查：

- `APP_WIFI_SSID` 和 `APP_WIFI_PASSWORD` 是否正确。
- 路由器是否为 2.4GHz Wi-Fi。
- 设备所在网络是否可以访问公网。

### 5.2 时间同步失败

token 中包含 `iat/exp` 时间戳，因此系统时间必须先同步成功。若 SNTP 失败，主动连接前的本地 token 生成会失败。

重点检查：

- Wi-Fi 是否已拿到 IP。
- 网络是否能访问 NTP 服务。
- 串口是否出现 `系统时间未同步`、`SNTP` 超时等日志。

### 5.3 TiRTC 未上线

重点检查：

- `TIRTC_DEVICE_ID` 是否为空或写错。
- `TIRTC_DEVICE_SECRET_KEY` 是否为空或写错。
- `TIRTC_CLIENT_ID` 是否为空；已绑定设备应与 `TIRTC_DEVICE_ID` 保持一致。
- 是否在 `TiRtcStart()` 前成功调用 `TiRtcSetOption(TIRTC_OPT_DEVICE_SECRET_KEY, ...)`；缺少该调用会返回 `-40014 TIRTC_E_NO_SECRET_KEY`。
- 是否在 `TiRtcStart()` 前成功调用 `TiRtcSetOption(TIRTC_OPT_CLIENT_ID, ...)`；缺少该调用时 `/v1/start` 会返回 `40003(X-Tg-Client-Id is required)`。
- `TIRTC_SERVICE_ENDPOINT` 是否为当前测试环境可访问的服务地址。

### 5.4 主动连接失败

重点检查：

- `TIRTC_REMOTE_DEVICE_ID` 是否是对端设备 ID。
- `TIRTC_REMOTE_DEVICE_SECRET_KEY` 是否是对端设备密钥。
- 对端设备是否已经上线。
- `TIRTC_TOKEN_ACCESS_ID` 和 `TIRTC_TOKEN_SECRET_KEY` 是否正确。
- 两边是否同时按下 BOOT 导致互相抢连。

主动连接发起后，状态日志会先显示：

```text
TiRTC 状态：已上线=1，连接=主动连接中
```

如果 SDK 长时间没有返回连接结果，demo 会在 30 秒后释放“主动连接中”状态：

```text
主动连接等待结果超时: 30000 ms
主动连接失败 error=-40005 TIRTC_E_TIMEOUTED
TiRTC 状态：已上线=1，连接=空闲
```

超时回到空闲后，可以再次按 BOOT 重新发起连接。

### 5.5 连接成功但没有音视频

重点看日志顺序：

1. A 侧是否打印 `[CTRL][TX] 订阅对端视频` 和 `[CTRL][TX] 订阅对端音频`。
2. B 侧是否打印 `[CTRL][RX] 对端订阅本机视频` 和 `[CTRL][RX] 对端订阅本机音频`。
3. B 侧是否打印 `[TX][video] 发流开始` 和 `[TX][audio] 发流开始`。
4. A 侧是否打印 `[RX][video] 接收统计` 和 `[RX][audio] 接收统计`。

如果 B 侧没有 `[CTRL][RX]`，说明订阅命令没有到达 B。  
如果 B 侧有 `[TX]` 但 A 侧没有 `[RX]`，需要继续检查网络链路、发送缓冲和 SDK 详细日志。

### 5.6 提示缺少 H264/PCMA 文件

说明 `storage` SPIFFS 分区没有正确烧录，或测试媒体文件没有被打包。

请使用：

```powershell
idf.py -p COMx flash monitor
```

不要只单独烧录 app bin，否则可能漏掉 `storage.bin`。

## 6. 需要保留的日志

如果测试失败，请保留两端完整串口日志，重点包含：

- 开机版本信息。
- Wi-Fi 连接结果。
- SNTP 时间同步结果。
- TiRTC 启动和上线日志。
- BOOT 按键触发日志。
- token 生成日志。
- 主动连接或远端呼入日志。
- `[CTRL][TX]`、`[CTRL][RX]` 控制日志。
- `[TX][video]`、`[TX][audio]` 发送日志。
- `[RX][video]`、`[RX][audio]` 接收日志。
- 断开或错误日志。
