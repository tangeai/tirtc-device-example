# ThingConnect 对齐审计

本文记录 ESP32-S3 固件与 ThingConnect 官方仓库的契约对齐结果，避免后续只按单份旧文档修改，或把网络入口、业务会话和硬件资源混成同一个生命周期。

## 1. 审计基线

- 审计日期：2026-07-24。
- 官方仓库：<https://github.com/tangeai/tirtc-server-example/tree/main/thing-connect>
- 官方基线提交：`0a71139aa259cc59d56d3ef299cd25c3d717ec0f`。
- 主要契约：`device-integration.md`、`device-session-model.md`、`device-call.md`、`api-reference.md`。
- 参考实现优先级：正式契约文档、当前 `device-sim/device-sim-c`、ESP32-S3 参考实现、Python 模拟器。后加入的 Demo 不能覆盖正式契约；文档和实现不一致时必须记录差异，不能静默猜测。

官方服务端已经在 `api-reference.md` 中正式列出验证码语音接口：
`GET /v1/device/tts?code=xxxxxx`。它使用同一次 Report 返回的 `temp_token` 鉴权，默认
返回 8 kHz、单声道、S16LE PCM，`fmt=wav` 返回 WAV。它是可选交互能力，不改变绑定、
上线、MQTT、TiRTC 或呼叫的必选契约。

最新基线新增 `device-sim/device-sim-esp32` 和 TiRTC SDK `2.2.1` 包。它给出了单一 SessionManager、H5 可抢占、通话忙线、SDK 外置目录以及 FreeRTOS 构建契约等可复用原则。本工程当前 SDK 仍是 `2.2.0`（BuildInfo commit `1df9e045a9dc`）；两版公开头文件除版本号外没有 API 差异，但静态库哈希不同，因此不能在未构建、未真机回归时静默替换。

### 1.1 上游实现差异

最新 ESP32-S3 Demo 的 `session_runtime.c` 收到 `unbind` 后会清除 NVS 凭证并重启；这与 `device-integration.md` 和 `api-reference.md` 的正式流程冲突。正式流程要求保留 `device_id/device_key`，Token 返回 `6006` 后使用四个签名 Header 调 Report，并在空 `auth_grant` 后继续使用原凭证。本工程以正式流程为准，保留身份，不复制 Demo 的清凭证分支。

ESP32-S3 Demo 每 2 秒轮询一次 `/v1/call/room`。本工程采用正式 MQTT 事件驱动，并只在启动恢复和 `40202` 残留房间补偿时查询房间，避免设备常态轮询服务器。两种实现都覆盖房间恢复，本工程保留更低流量的事件驱动方案。

## 2. 分层和所有权

| 层 | 本工程位置 | 只负责 |
| --- | --- | --- |
| 驱动层 | `main/drivers` | 屏幕、触摸、麦克风、扬声器、摄像头的初始化、读写和反初始化 |
| 连接层 | `main/connectivity` | Wi-Fi STA、IP 和网络状态 |
| 协议层 | `main/protocols` | HTTP、MQTT、TiRTC 的连接、订阅、命令和传输适配 |
| 服务层 | `main/services` | 绑定、在线、设备呼叫、微信 VoIP、AI、OTA 等业务状态机 |
| 应用层 | `main/application` | 前台应用切换、硬件资源所有权、跨服务编排和异常恢复 |
| UI 层 | `main/ui` | 展示快照和投递用户动作，不直接停止 MQTT、TiRTC 或硬件驱动 |

长期在线的正式 MQTT 和业务消息入口属于设备级服务，不跟页面销毁。麦克风、扬声器、摄像头、当前 P2P/WHIP 会话属于前台应用资源，进入应用时申请，退出时挂断并释放。

## 3. 主流程

### 3.1 启动和绑定

1. Wi-Fi 使用 NVS 中保存的配置自动连接。
2. IP 就绪后校准系统时间。
3. 调用服务发现；失败时使用编译期兜底地址，后续网络重新上线事件允许再次发现。
4. 本地无 `device_id/device_key` 时，无签名调用 `POST /v1/device/report`。
5. 保存服务端返回的 6 位码、`temp_client_id`、`temp_token` 和过期时间。
6. 使用临时身份连接 MQTT，订阅临时 `cmd`，网络瞬断按 3 秒间隔自动重连；收到 `auth_grant` 后按 QoS 1 等待 PUBACK。
7. 新设备保存下发的 ID/KEY；保留身份重新绑定时，空 `auth_grant` 也视为成功。

### 3.2 正式在线

1. 使用设备 KEY 签名请求 `/v1/device/token`，token 在有效期内从缓存读取。
2. 正式 MQTT 使用 `client_id=sn_{device_id}`、`username=device_id`、`password=mqtt_token`。
3. 常驻订阅 `cmd` 和 `notify`，断线按 3 秒间隔自动重连；`cmd` 必须回复 ACK，状态和心跳走 `up`。
4. MQTT 因 `0x98/0x99` 断开时清除 token，再获取新 token 后重连。
5. 正式 MQTT 在线后才打开 TiRTC 身份门禁并启动监听。
6. 正式 MQTT 的离线到在线跃迁是统一补偿点：重做房间恢复、联系人刷新、当前前台应用服务启动和 RTC 准备；监听器在连接前注册，恢复动作在连接后执行。
7. Token HTTP 或刷新失败后，在线服务按 5 秒到 60 秒退避常驻自愈；快速断网重连发生在旧任务退出期间时，记录重启请求，旧任务退出后自动接续，不能停在“本地有凭证但无在线任务”。

签名 Token/Report 的 HTTP 传输层不直接重放同一个请求，因为 nonce 只能使用一次。需要重试时由设备在线/绑定服务重新发起完整操作，生成新的时间戳、nonce 和签名。

### 3.3 解绑和重新绑定

1. MQTT 收到 `unbind` 后先等待该命令 ACK 的 PUBACK。
2. UI 或 MQTT 回调只投递控制事件，由应用控制任务统一暂停微信/设备呼叫消息入口、停止正式 MQTT 和 TiRTC 身份；绑定状态变化再异步驱动页面回主界面并释放页面硬件资源。
3. 本地保留 `device_id/device_key`，不清空成新设备。
4. 使用保留凭证发起带四个签名 Header 的 `/v1/device/report`。
5. `auth_grant` 先持久化凭证并把绑定状态提交为 `BOUND`，绑定任务完全退出后才重启正式 MQTT、微信常驻入口和 TiRTC 身份监听，避免“MQTT 上线事件早于绑定完成”的竞态。

平台真正解绑是用户鉴权接口 `DELETE /v1/user/device/reset`，设备端没有用户 JWT，不能自行调用该接口。因此设置页“重置绑定”只强制重新请求 `/v1/device/token` 校验云端归属：仍绑定时恢复正式 MQTT；只有收到 `HTTP 410 + code=6006` 或正式 MQTT 的 `unbind` 后，才进入保留 ID/KEY 的签名 Report 和 6 位码流程。UI 不再提前伪造“未绑定/正在 Report”状态。

身份切换必须先把旧正式 MQTT 完整停稳，再建立临时 MQTT；两种连接不能并存。设备呼叫和微信队列都带身份代次，旧身份已排队的消息在新身份生效后必须丢弃。微信授权联系人还要在身份代次变化时清除内存和 NVS 快照，不能只按 `device_id` 判断，因为解绑后可能由新用户绑回同一个 ID；微信入口启动、停止和联系人 NVS 擦除/加载由同一生命周期锁串行化，保证旧快照擦除完成后才允许新身份加载。UI 判断“已绑定”只看绑定状态机，不能因为 Flash 仍保留 ID/KEY 就继续隐藏 6 位码。

### 3.4 HTTP 接口所有权

设备固件只调用设备身份可鉴权的接口：

- 绑定与在线：`POST /v1/device/report`、`POST /v1/device/token`；需要语音播报码时可选
  调用 `GET /v1/device/tts`。
- 微信 VoIP：`POST /v1/voip/device/profile`、`GET /v1/voip/device/contacts`、`POST /v1/voip/device/call`。旧地址 `device/callers` 只作为服务端兼容别名，设备端使用当前正式地址。
- AI：`GET /v1/ai/token`。
- 设备呼设备：`/v1/call/request`、`/v1/call/device/info`、`/v1/call/reject`、`/v1/call/hangup`、`/v1/call/cancel`、`/v1/call/room`、`/v1/call/device/contacts`，以及联系人 `POST request`、`POST respond`、`PUT remark` 接口。

`/v1/user/*`、`/v1/voip/user/*`、`/v1/call/user/*` 需要用户 JWT，属于网页或小程序；内部和管理接口属于服务端。设备端不得因为请求体字段看起来足够就越过鉴权边界直接调用。验证码 TTS 已进入正式 API 清单，但仍是可选设备能力，不作为绑定闭环的前置条件。

## 4. 功能对齐矩阵

| 功能 | 当前状态 | 本轮结论 |
| --- | --- | --- |
| 服务发现 | 源码已对齐，待真机 | 失败结果不再永久锁死，后续可重试 |
| 首次绑定 | 源码已对齐，待真机 | 无签名 Report、临时 MQTT 3 秒自动重连、凭证保存和过期会话恢复均已实现 |
| 保留身份重绑 | 源码已对齐，待真机 | signed Report、空 `auth_grant`、ID/KEY 保留均已实现 |
| 身份切换隔离 | 源码已对齐，待真机 | UI 异步投递；旧入口按代次失效；正式 MQTT 停稳后才启动临时 MQTT；凭证保存、`BOUND` 提交、任务退出、正式服务恢复按顺序执行 |
| 正式 MQTT | 源码已对齐，待真机 | 常驻连接、3 秒自动重连、ACK、Unix 时间心跳、token 缓存、`0x98/0x99` 自愈、HTTP 退避重试和快速断网重启接续均已实现 |
| MQTT 分片消息 | 源码已对齐，待真机 | 首片保存 topic 和消息类型，后续无 topic 分片按 offset 续接，长 `peer_id/token` 不再被静默丢弃 |
| 云端解绑 | 源码已对齐，待真机 | 等待 ACK 后进入保留 ID/KEY 的 signed Report；本地“重置绑定”只做 Token 归属校验，两者入口不同但由 6006/`unbind` 收敛到同一重绑流程 |
| Web IPC | 源码已对齐，待真机 | 视频 11、设备上行音频 10、H5 回传音频 14 分开管理 |
| 微信 VoIP profile | 源码已对齐，待真机 | 使用 `up_video_mt/down_video_mt/down_audio_mt`，不再混用旧 `video_mt` |
| 微信 MQTT 分流 | 源码已对齐，待真机 | 正式 `call_incoming/call_cancel/callers_update` 只接受 `channel=wx`；仅旧版微信别名兼容缺失 channel，避免 `channel=device` 被微信链路重复消费 |
| 微信授权联系人 | 源码已对齐，待真机 | 设备只拉取 `device/contacts`、处理 `callers_update` 和调用 `device/call`；完整列表成功返回后原子替换本地快照，服务端撤销的授权不会残留；联系人 HTTP 刷新由独立常驻任务执行并校验身份代次，不阻塞 `call_incoming/call_cancel` 信令队列，也不会让解绑前的旧响应覆盖新身份；NVS 读取结果落地前再次核对设备身份，启动/停止还通过生命周期锁保证旧快照擦除先于新身份加载；已移除无 user JWT 的 `report-auth/delete-auth` 调用，扫码或输入 OpenID 只核对既有授权 |
| 微信来电入口 | 源码已对齐，待真机 | MQTT 入口常驻；页面只拥有 RTC 会话和音频资源，主界面也能收到来电 |
| AI 对讲 | 源码已对齐，待真机 | 长 `peer_id/token` 使用 PSRAM 大缓冲；媒体就绪后等待 300 ms 再发 `0x2100` |
| 设备呼叫核心链路 | 源码已对齐，待真机 | request/info、接听、拒接、取消、挂断、`0x2000/0x2001` 和 MQTT 路由均已实现 |
| 呼叫回调线程边界 | 源码已对齐，待真机 | MQTT/SDK 回调只更新状态并投递会话结束；RTC 断开和音频释放由应用控制任务执行，不在协议回调中递归 teardown |
| 设备呼叫身份恢复 | 源码已对齐，待真机 | 首次绑定、扫码换凭证和保留身份重绑都会重新打开设备呼叫 MQTT 入口，旧联系人任务不能覆盖新身份 |
| 多目标呼叫拒接 | 源码已对齐，待真机 | 单个 `call_reject` 只更新提示，只有 `room_cancel` 才终止整个房间 |
| 房间查询和恢复 | 源码已对齐，待真机 | 监听器先注册，正式 MQTT 上线后执行一次房间和联系人恢复；`40202` 时再次查询，按 caller/callee 和 active/answered 选择 cancel/reject/hangup，不恢复已失效媒体 |
| 联系人申请、响应和备注 | 服务接口已对齐，待真机 | request/respond/remark 均为异步服务接口，成功后刷新联系人；320x240 UI 暂不增加没有产品入口的审批和编辑页面 |
| 多业务会话仲裁 | 源码已对齐，待真机 | H5 可被来电接听抢占；AI、微信 VoIP、设备呼设备互斥；忙线来电主动回 reject，不静默排队或切换 |
| ESP32-S3 构建契约 | 配置已对齐 | 1 kHz tick、trace/stats 关闭、ESP-IDF 5.5.x 与上游 2.2.1 包要求一致 |
| TiRTC SDK 版本 | 待单独升级 | 当前 `2.2.0` 与最新参考 `2.2.1` 静态库不同；升级必须单独做构建、启动、监听、H5/AI/两类呼叫回归 |
| 验证码 TTS | 可选未接入 | 服务端已提供 PCM/WAV；当前绑定弹窗已有文字和二维码，后续需要语音播报时应由应用层申请扬声器资源后接入 |

### 4.1 明确保留的兼容差异

官方最新 API 文档把 MAC 示例写成 `AA:BB:CC:DD:EE:FF`。本工程历史设备已经用 12 位大写无冒号格式建立服务端指纹，当前继续保持该格式，避免已部署设备在 signed Report 时触发 MAC 不一致。该项不能只改固件；若平台决定统一冒号格式，必须先完成服务端兼容/迁移，再切换设备端。

`GET /v1/call/device/contacts` 会同时返回 `device` 与 `voip` 联系人。本工程在设备呼设备页面明确只接收 `type=device`；微信联系人仍由微信 VoIP 服务单独维护，防止把 openid 当设备 ID 调 `/v1/call/request`。这属于按业务类型分流，不是字段遗漏。

官方通用 SessionManager 把 H5 实时流定义为开机默认后台流，并建议前台会话结束后自动恢复。本产品按 320x240 前台应用生命周期管理：回到主界面必须释放页面拥有的 RTC 会话和音频/摄像头驱动，只保留设备级 TiRTC 监听与正式 MQTT；用户重新进入 IPC 应用时再申请资源。因此本工程只沿用“H5 可被前台通话抢占”的优先级，不自动在主界面恢复 IPC 硬件。这是产品资源策略差异，不是协议缺口。

`api-reference.md` 的设备接听接口说明允许被叫切换到另一个房间，但统一会话模型和最新 ESP32-S3 参考实现采用单会话忙线拒绝。本产品需要防止 AI、微信、设备呼叫和 H5 争抢同一套音频/RTC 资源，因此采用忙线拒绝，不自动释放正在进行的前台会话。该策略必须由应用层统一仲裁，不能在 MQTT 回调里直接切换。

## 5. 业务会话边界

- 同一时间只允许一个前台媒体会话拥有音频和 RTC 连接。
- `STREAM`/Web IPC、微信 VoIP、AI 对讲和设备呼叫不能互相复用对方的媒体流号或缓冲区。
- Web IPC 待机配置的远端音频流号为 14；设备间呼叫在发起连接前配置为 10。
- “下一条连接的流号”和“当前连接实际订阅的流号”必须分开保存，退出时按当前连接的实际值反订阅。
- 微信 MQTT listener 常驻，但退出微信页面必须挂断 WHIP/P2P 并释放音频；解绑时还必须暂停 listener，待正式 MQTT 重新上线后恢复。
- `call_reject` 是某个被叫的局部事件；`room_cancel`、`0x2001` 或本地 hangup 才是房间终态。
- H5/IPC 是可抢占业务。来电振铃阶段不提前释放当前会话；用户接听时由应用层统一切换所有权并释放旧连接。结束后按本产品页面生命周期回主界面，不在后台私自重新初始化硬件。
- AI、微信 VoIP、设备呼设备是同级独占业务。任一业务处于启动、振铃、连接或通话状态时，其他来电必须立即忙线拒接，不能保存成第二个 pending 会话。
- MQTT、RTC 和 UI 回调只上报事件或调用服务接口；跨业务选择和硬件资源交接由应用层统一决定。
- 资源切换只回滚本次新申请成功的资源；申请目标资源失败时不能误释放切换前已持有的驱动或网络资源。

## 6. 仍需真机验证

静态对齐不能代替设备验证。下一轮烧录后至少覆盖：

1. 首次绑定、重启、云端解绑、保留 ID 再绑定。
2. 主界面收到微信来电，接听后自动切入微信应用，挂断返回主界面后再次来电。
3. Web IPC 双向音频确认下行订阅流 14，设备间呼叫确认流 10；两种业务交替两轮。
4. 多设备群呼中一台拒接不结束房间，全部拒接后收到 `room_cancel` 才结束。
5. AI 对讲连续进入退出，确认 `0x2100` 在媒体建立约 300 ms 后发送。
6. 服务发现首次失败后恢复网络，确认能再次发现而不是永久使用兜底地址。
7. 用超过单个 MQTT buffer 的微信 `peer_id/token` 和绑定 `auth_grant` 验证分片重组，确认只分发一次且 ACK 正常。
8. 首次绑定和保留身份重绑完成后，不进入任何页面，直接从另一台设备呼叫本机，确认设备呼叫入口已经恢复。
9. 在任意前台应用中触发云端解绑，确认自动回主界面、旧会话释放、6 位码出现，并且旧身份排队消息不会恢复旧页面。
10. H5 查看中分别接收微信和设备来电：先振铃，接听时再中断 H5；挂断后资源可再次被 H5 使用。
11. AI、微信、设备呼设备分别占用会话时注入另外两类来电，确认对端收到 busy，设备端不出现第二个 pending 或自动切页。
12. 断网启动后延迟恢复正式 MQTT，确认上线后仍执行一次房间恢复；覆盖 active/caller、active/callee、answered 三种服务端状态。
13. 通过服务接口各执行一次联系人 request、respond 和 remark，连续注入 `callers_update`、`call_incoming`、`call_cancel`，确认联系人 HTTP 慢响应不阻塞呼叫信令；分别在 HTTP 刷新、联系人 NVS 读取和旧快照擦除途中解绑/重绑，确认旧身份返回的数据都被丢弃，并且同一 `device_id` 重新绑定也不会恢复旧用户联系人。
14. 升级 TiRTC SDK `2.2.1` 时单独记录静态库哈希、BuildInfo、构建结果和真机监听/H5/AI/VoIP/设备呼叫证据，不与普通业务修改混发。
15. 在小程序分别创建和取消微信 VoIP 授权，确认设备仅通过 `callers_update`/`device/contacts` 更新列表，固件不请求任何 `/v1/voip/user/*` 接口。
16. 向同一正式 MQTT 连接注入同名但不同 channel 的消息，确认 `channel=wx` 只进入微信链路、`channel=device` 只进入设备呼叫链路。

验收时要同时保留设备日志、服务端房间/MQTT 状态和对端现象。仅构建通过不能证明绑定归属、在线状态、音频流号或二次进入已经闭环。
