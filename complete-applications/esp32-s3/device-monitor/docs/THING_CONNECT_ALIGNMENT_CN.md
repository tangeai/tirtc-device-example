# ThingConnect 接入与实现对齐

本文说明 ESP32-S3 设备应用如何使用 ThingConnect 的设备身份、MQTT、TiRTC 和业务接口。
服务端字段、错误码与鉴权规则以
[tangeai/tirtc-server-example/thing-connect](https://github.com/tangeai/tirtc-server-example/tree/main/thing-connect)
为准。

## 1. 协议基线

- 服务发现入口：`https://ep-open.tangeopen.com/services`。
- 设备首次接入使用 6 位验证码绑定；设备端不要求用户手工录入设备密钥。
- 绑定后的 `device_id/device_key` 用于签名换取短期 `mqtt_token`。
- 正式 MQTT 承担设备在线、命令 ACK、业务通知和状态上报。
- TiRTC SDK 以 `db7290f2404b15f2ab8567dd221853ca2ec054ab` 为功能基础，BuildInfo 为
  `v2.3.0-db7290f`；HTTPS 服务端认证修复来自
  `13e34c3e3e3dc6776be4713b5c1e3c17bd282766`。
- 产品 RTC 能力为双向音频；摄像头只用于二维码扫描，不作为 RTC 视频源。

验证码语音接口 `GET /v1/device/tts?code=xxxxxx` 是可选能力。它不改变绑定、上线、
MQTT、TiRTC 或呼叫的必选流程。

### 1.1 传输安全

- 服务发现只接受 `https://` 服务地址和 `mqtts://` MQTT 地址。不安全结果不会覆盖编译期
  安全兜底，调用方收到明确失败后再重试发现。
- 通用 ThingConnect HTTP 客户端使用 ESP-IDF 证书包，校验证书链和 hostname，并禁用自动
  重定向，避免携带凭据的 HTTPS 请求被转发到 HTTP。
- 临时绑定 MQTT 与正式设备 MQTT 在创建客户端前再次检查 MQTTS。
- TiRTC SDK 自有 HTTP 传输使用 `MBEDTLS_SSL_VERIFY_REQUIRED`，挂接同一证书包并校验
  hostname。证书链、主机名、握手或 verify flags 失败时返回 SSL 错误，不降级明文协议。

## 2. 分层和所有权

| 层 | 工程位置 | 责任 |
| --- | --- | --- |
| 驱动层 | `main/drivers` | 屏幕、触摸、麦克风、扬声器，以及二维码扫描所需摄像头 |
| 连接层 | `main/connectivity` | Wi-Fi STA、IP 和网络状态 |
| 协议层 | `main/protocols` | HTTP、MQTT、TiRTC 的连接与传输适配 |
| 服务层 | `main/services` | 绑定、在线、H5、微信、小钛、设备呼叫和 OTA 状态机 |
| 应用层 | `main/application` | 前台应用切换、会话仲裁和硬件资源交接 |
| 调试层 | `main/debug/serial_net_cli` | 从串口查询网络、RTC、媒体、AEC 和呼叫状态 |
| UI 层 | `main/ui` | 展示状态并投递用户动作 |

正式 MQTT 和业务监听属于设备级服务，不随页面销毁。麦克风、扬声器以及当前 P2P/WHIP
会话属于前台应用资源，进入应用时申请，退出时释放。摄像头由联系人二维码扫描流程短时申请，
不参与 RTC 会话。

## 3. 设备身份与在线

### 3.1 首次绑定

1. Wi-Fi 获得 IP 后同步系统时间并执行服务发现。
2. 本地没有设备身份时，设备调用 `POST /v1/device/report` 获取 6 位绑定码、
   临时 MQTT 身份和有效期。
3. 用户在 ThingConnect H5 或配套入口输入绑定码。
4. 设备从临时 MQTT 收到 `auth_grant`，保存正式 `device_id/device_key`。
5. 设备签名调用 `POST /v1/device/token`，使用返回的 `mqtt_token` 正式上线。
6. 正式 MQTT 在线后，设备开启 TiRTC 身份监听和各业务消息入口。

### 3.2 解绑与重新绑定

收到 MQTT `unbind` 或 Token 返回未绑定状态后，设备保留原
`device_id/device_key`，停止旧 MQTT、TiRTC 和业务入口，再使用签名 Report 进入新的
6 位码流程。旧身份的排队消息按身份代次丢弃，不能恢复旧页面或联系人快照。

设备端不调用需要用户 JWT 的 `/v1/user/*` 接口。设置页“重置绑定”只用于重新核对
云端归属，不能绕过服务端身份关系。

## 4. 接口所有权

设备固件只调用设备身份可鉴权的接口：

- 绑定与在线：`POST /v1/device/report`、`POST /v1/device/token`。
- 微信 VoIP：`POST /v1/voip/device/profile`、
  `GET /v1/voip/device/contacts`、`POST /v1/voip/device/call`。
- 小钛：`GET /v1/ai/token`，随后使用返回的短期凭证建立 WHIP 会话。
- 设备呼叫：`/v1/call/request`、`/v1/call/device/info`、
  `/v1/call/reject`、`/v1/call/hangup`、`/v1/call/cancel`、
  `/v1/call/room` 和设备联系人接口。

`/v1/user/*`、`/v1/voip/user/*` 和 `/v1/call/user/*` 需要用户 JWT，属于网页或
小程序。设备端不因为请求字段可见就越过鉴权边界。

## 5. 业务能力

| 能力 | 设备端实现 |
| --- | --- |
| 服务发现 | 只接受 HTTPS/MQTTS；失败时使用同样安全的编译期兜底，网络恢复后允许重新发现 |
| 网络质量 | 20 次探测产生平均 RTT、相邻 RTT 平均抖动和丢包率；结构化字段贯穿网络层、应用快照、UI 与串口诊断 |
| 正式 MQTT | 常驻连接、QoS 1 ACK、心跳、Token 缓存和断线重连 |
| MQTT 分片 | 按 offset 重组长 `peer_id/token` 与授权消息 |
| Web IPC | 设备麦克风上行和 H5 回传音频使用独立流号；设备不发布或播放 RTC 视频 |
| 微信消息分流 | 正式消息按 `channel=wx` 分发，不消费 `channel=device` 的同名消息 |
| 微信联系人 | 兼容 `data.contacts`、`data.list` 和常用字段别名；NVS schema v2 缓存 |
| 微信备注 | 后台异步更新，成功后刷新服务端权威列表；UI 提供 9 个预设备注 |
| 微信呼叫 | 小程序和设备可双向发起音频通话；能力上报明确 `no_video=true`；设备主动呼叫使用体验版 `wx_version_type=2` |
| 小钛对讲 | WHIP 音频、ASR/TTS 字幕、打断和会话重建 |
| 小钛联系人动作 | 查询设备联系人状态；发起设备或微信联系人的音频呼叫 |
| 设备呼叫 | 只接受 `call_type=audio`，覆盖来电铃声、接听、拒接、取消、挂断和房间恢复 |
| 多业务仲裁 | H5 可在接听来电时被抢占；小钛、微信和设备呼叫互斥 |

小钛动作返回 `accepted` 时，只表示目标校验和应用生命周期切换请求已被设备接受，
不表示对端已经响铃、接听或建立媒体。联系人不存在、备注歧义、目标离线、网络离线、
设备忙碌和不支持的呼叫类型会返回明确结果。

## 6. 联系人兼容性

普通设备联系人和微信联系人按业务类型分流：

- 设备呼叫页面只接收 `type=device`，目标使用 12 位 `device_id`。
- 微信联系人接受 `data.contacts` 或 `data.list`。
- 微信 OpenID、ModelID、AppID 和备注兼容服务端常用字段别名。
- 微信备注可选妈妈、爸爸、爷爷、奶奶、外公、外婆、哥哥、姐姐或朋友；更新后以服务端
  返回列表为权威来源。
- 微信联系人不提供在线状态查询，设备侧当前只发起音频呼叫。

## 7. 会话边界

- 同一时间只允许一个前台媒体会话持有音频和 RTC 连接。
- Web IPC、微信 VoIP、小钛和设备呼叫不复用对方的媒体流号或缓冲区。
- 所有 RTC 业务只获取音频资源。来自云端的设备视频呼叫请求会按不支持的类型拒绝，不会
  静默降级成音频。
- 来电振铃阶段不提前释放当前页面；用户接听后由应用层统一切换资源。
- 忙线来电立即返回 busy，不保存第二个 pending 会话。
- `call_reject` 是单个被叫的局部事件；`room_cancel`、`0x2001` 或本地挂断才是
  房间终态。
- 共享 RTC 命令 `0x2000` 按当前会话所有者分发，微信观察者的命令不会被普通设备呼叫
  状态机消费。
- MQTT、RTC 和 UI 回调只上报事件；跨业务选择和硬件资源交接由应用层完成。

## 8. 音频与网络数据契约

- Web IPC、设备呼叫和微信 VoIP 的设备上行使用
  `8 kHz / 16 bit / mono / G.711 A-law / 20 ms / 160 bytes`。显式 CALL gate 与 built-in
  microphone 路径不能声明同一产品格式却发送不同 Codec。
- Device Call 独立使用有界自适应播放缓冲和约 `1.25%` 的水位回中速率微调；Web IPC、
  微信和小钛保留各自 profile。云端媒体到达异常仍通过丢包、缺失、乱序、缓冲水位和下溢
  统计表达，不能由本地变速改写成“网络正常”。
- 普通设备来电铃声是设备呼叫服务的本地资源。接听、拒接、取消、挂断和身份重置都要停止，
  不由 ThingConnect 云端保留另一份铃声状态。
- 网络测试的抖动定义是相邻成功 RTT 差值绝对值的平均数，与音频播放缓冲的 jitter depth
  不是同一个指标。UI 阈值只用于短窗口提示，不代替 RTC 媒体质量判断。

## 9. 串口诊断边界

公开默认配置开启 `CONFIG_APP_SERIAL_NET_CLI_ENABLE`。输入 `AT+HELP` 可查看实际命令集；
`AT+NET?`、`AT+NETPROBE`、`AT+SOCKETS?`、`AT+MEDIA?`、`AT+RTCLOG?`、
`AT+AUDIOPATH?`、`AT+AUDIOCHECK?`、`AT+AECDUMP?` 和 `AT+CALL?` 可以帮助定位第一处
失败阶段。

CLI 不回显 Wi-Fi 密码、Token 或设备密钥。它是诊断入口，不是业务旁路：不会伪造在线、
绕过服务端鉴权或把失败状态改成成功。量产不需要现场诊断时，可以在 Kconfig 关闭并重新构建。

## 10. 集成验证建议

在目标板上依次验证：

1. 首次绑定、重启、云端解绑和保留身份重新绑定。
2. Web IPC 设备上行音频和 H5 回传音频，并确认没有意外启用 RTC 视频资源。
3. 微信联系人两种列表结构、备注更新、来电和设备侧体验版 `wx_version_type=2` 语音呼叫。
4. 小钛连续进入退出、字幕、语音播放、联系人状态查询和呼叫动作。
5. 设备音频呼叫、来电铃声停止、视频类型拒绝、拒接、取消、挂断和异常房间恢复。
6. H5 被来电接听抢占，以及小钛、微信和设备呼叫之间的忙线分支。
7. 断网重连、长 MQTT 分片和旧身份消息隔离。
8. 有效证书连接成功，并在可控环境确认错误证书或错误 hostname 连接失败；两类结果都保留日志。

构建、烧录、联网、服务端状态、音频效果和目标板业务现象是独立证据。发布固件的来源、
大小和 SHA-256 以同一 Release 的 `release-manifest.json` 为准。

当前 Web IPC 和设备互呼的人耳试听仍可能感知轻微“沙沙电流声”。线上格式核对和 A-law
编解码自检没有发现异常，但底噪根因尚未证实；集成验收应把这个现象作为独立已知问题记录，
不能因为 ThingConnect、TiRTC 或媒体包计数成功就写成音质已经通过。
