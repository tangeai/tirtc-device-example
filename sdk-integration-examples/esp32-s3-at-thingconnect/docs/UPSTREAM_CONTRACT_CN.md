# 上游协议基线

本例按 `tangeai/tirtc-server-example` 的 ThingConnect 文档与参考实现收口，
核对基线 commit 为 `593ac124fd56329a5babf99734b5797ac1f722f2`：

- [API Reference](https://github.com/tangeai/tirtc-server-example/blob/593ac124fd56329a5babf99734b5797ac1f722f2/thing-connect/api-reference.md)
- [Device Integration](https://github.com/tangeai/tirtc-server-example/blob/593ac124fd56329a5babf99734b5797ac1f722f2/thing-connect/device-integration.md)
- [Device AI](https://github.com/tangeai/tirtc-server-example/blob/593ac124fd56329a5babf99734b5797ac1f722f2/thing-connect/device-ai.md)
- [Device Call](https://github.com/tangeai/tirtc-server-example/blob/593ac124fd56329a5babf99734b5797ac1f722f2/thing-connect/device-call.md)
- [Session Arbiter](https://github.com/tangeai/tirtc-server-example/blob/593ac124fd56329a5babf99734b5797ac1f722f2/thing-connect/device-session-arbiter.md)
- [Error Policy](https://github.com/tangeai/tirtc-server-example/blob/593ac124fd56329a5babf99734b5797ac1f722f2/thing-connect/error-response-policy.md)

## 固件映射

| 上游能力 | 本例所有者 |
|---|---|
| HTTPS 服务发现、Report、设备 token | `platform_client` |
| 临时/正式 MQTT、绑定 ACK/PUBACK | `platform_client` |
| AI token、WHIP、JSON-RPC、`0x2100` | `session_ai` |
| 联系人、呼叫房间、`0x2000/0x2001` | `session_call` |
| `call_device` 目标解析与 AI -> CALL 交接 | `session_ai_call` |
| AI/CALL 单会话互斥与 generation | `session_runtime` |
| TiRTC 生命周期、连接、订阅、帧回调 | `tirtc_adapter` |
| 预录上行和下行日志统计 | `media_runtime` |
| 用户输入、状态查询和异步结果 | `app_at_commands` |

## 平台 API 清单

所有 path 都拼接到发现结果中的对应 service base URL。发现入口以及
device/AI/call 业务 base URL 必须使用 HTTPS。

| Service | Method / path | 用途 |
|---|---|---|
| discovery | `GET https://ep-open.tangeopen.com/services` | 获取 `device-srv`、`ai-srv`、`call-srv`、`mqtt-srv` 和可选 `tirtc-srv` |
| device | `POST /v1/device/report` | 首绑 unsigned Report，或保留凭据后的 signed Report |
| device | `POST /v1/device/token` | `device_id/device_key` HMAC 鉴权并获取短期 MQTT token |
| AI | `GET /v1/ai/token` | 获取当前平台角色对应的短期 AI token 与 WHIP 信息 |
| call | `POST /v1/call/request` | 创建设备呼设备房间 |
| call | `POST /v1/call/device/info` | 被叫接听后获取一次性连接 token |
| call | `GET /v1/call/room` | 启动或异常后的房间恢复 |
| call | `POST /v1/call/reject`, `/v1/call/cancel`, `/v1/call/hangup` | 房间拒绝、取消和挂断 |
| call | `GET /v1/call/device/contacts` | 普通设备联系人列表 |
| call | `GET /v1/call/device/contacts/pending` | 待处理联系人申请 |
| call | `POST /v1/call/device/contacts/request`, `/v1/call/device/contacts/respond` | 发起或处理联系人申请 |
| call | `PUT /v1/call/device/contacts/remark` | 修改联系人备注 |
| call | `DELETE /v1/call/device/contacts?peer_id=...` | 删除普通设备联系人 |

除 Report 与 device token 的 HMAC 流程外，AI/call 业务 HTTP 使用当前短期
MQTT token 作为 Bearer 凭据。固件不接受由 AT 注入的用户 JWT、AK/SK、
远端 device_key 或长期业务 token。

## AI 兼容边界

| 服务端字段 | 固件处理 |
|---|---|
| AI token `code` | 正式成功码 `200`；额外兼容旧服务的 `0` |
| `start_session.result.input_audio/output_audio` | 字段缺省时沿用本次请求格式；出现时严格核对 codec、采样率和声道 |
| G711A codec 名 | 接受合同名 `g711a` 和线上兼容名 `alaw` |
| caption 正文 | 正式读取 `text`，额外兼容旧字段 `caption` |
| ASR `mode=1` | 按线上实测的修订快照替换同一 utterance，TTS 仍保留防御性增量合并 |
| `update_config` | 只发送 `params.extra_params`，不能切换平台角色 |
| `device_action.id` | 接受非空字符串或数字，回执保留原 JSON 类型 |
| `device_action.params` | `call_device` 读取必填的 `data.target`；其他动作继续交给 AT 主机回执 |
| `call_device` result | 严格返回根级 `result.ok=true` 和 `result.message`，不私自增加插件未声明字段 |

`call_device` 每次都实时请求 `GET /v1/call/device/contacts`。固定合同只为
`type=device` 提供 `device_id`、`remark`、`source`、`online`，不提供
`device_name`；因此当前正式支持设备 ID 和联系人备注，设备名称需先同步
为备注。目标未知、歧义、离线或联系人快照不完整时返回 JSON-RPC error，
不结束 AI，也不创建呼叫房间。成功仅表示设备接受切换请求，不能描述为
已经接通；普通呼叫是否建立继续由 call-server 和 CALL 状态机决定。

## MQTT 与解绑

临时绑定 MQTT 固定使用 3.1.1；正式 MQTT 使用 v5，以便读取认证失败的
reason code。正式连接的凭据合同是
ClientID=`sn_<device_id>`、Username=`<device_id>`、
Password=`mqtt_token`。TiRTC 使用的 MAC 派生 `client_id` 与 MQTT
ClientID 是两个不同标识。

临时 `auth_grant` 必须先校验并持久化设备凭据，再发布 QoS1 应用 ACK，并
等待 PUBACK 后完成绑定。正式 MQTT command 中的精确 `type=unbind` 是
平台生命周期事件：固件立即关闭新业务门禁、尽力发送应用 ACK，并只推进
一次 `REBIND_REQUIRED`；controller 终止会话、停止正式连接并用保留凭据
执行 signed Report。服务端可以在发布 `unbind` 后立即踢线，因此该重绑
不能等待 ACK/PUBACK 才开始。

默认发现入口是 `https://ep-open.tangeopen.com/services`。HTTP 业务服务只
接受 HTTPS；MQTT 地址按上游合同接受 `mqtt://` 或 `mqtts://`，公开环境
当前返回 `mqtts://`。公开或生产部署必须使用 `mqtts://`；`mqtt://` 仅可
用于受信私网联调，因为 MQTT token 和业务负载不会获得传输加密。
`tirtc-srv` 是可选字段；缺省或为空时不设置 `TIRTC_OPT_SERVICE_ENDPOINT`，
由 TiRTC SDK 使用内置默认地址。

## VoIP 排除边界

本例不保存发现结果中的 `voip-srv`，不调用 `/v1/voip`，不注册微信 VoIP
AT 命令、状态或会话 owner。联系人响应只接受 `type=device`，`voip`
联系人会被静默过滤；备注、删除和申请响应还必须命中最近一次设备联系人
或待处理设备缓存，任意外部 peer ID 会失败关闭。TiRTC SDK 交付包中未被
应用引用的兼容 header 或符号不表示本例启用了 VoIP。

上游协议、本文档与代码发生冲突时，以当前服务端合同为准，升级时必须
重新固定 commit、更新本表并分别完成构建、真机绑定、AI 和双板呼叫验证。
