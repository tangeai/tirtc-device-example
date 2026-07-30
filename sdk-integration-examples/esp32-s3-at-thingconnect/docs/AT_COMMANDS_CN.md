# AT 指令

默认 AT 端口是板载原生 USB Serial/JTAG CDC；UART 后端参数为
`115200 8N1`。行结束符接受 `CR`、`LF` 或 `CRLF`。ESP-IDF/TiRTC
诊断日志默认走 UART0。

## 通用规则

- `AT`：链路探测。
- `ATE0`：确认关闭回显。为避免配网密码泄露，`ATE1` 固定返回
  `ESP_ERR_NOT_SUPPORTED`，本例不允许打开回显。
- `AT+HELP`：列出命令名。
- `AT+BUILD?`：查询运行固件和 TiRTC SDK 的脱敏构建身份。
- `AT+STATUS?`：查询不含密码和 token 的系统快照；其中仍包含 SSID、IP
  和设备 ID，公开证据必须继续脱敏。
- `AT+SESSION?`：查询版本化的完整会话快照。
- `AT+MEDIA?`：查询当前或最近一代会话的媒体计数。
- 查询命令会先同步输出查询行，再返回 `OK`。改变业务状态的命令返回
  `OK` 只表示语法正确且意图已放入控制队列；语法或本地队列错误返回
  `ERROR:<code>`。
- 业务状态拒绝使用
  `+ERROR:<generation>,<request_id>,<code>,"REQUEST_REJECTED","operation"`。
- 已受理意图使用
  `+REQUEST,<generation>,<request_id>,0,"operation","",""`，最终状态继续由
  业务 URC 报告。
- 业务 URC 的数字 `request_id` 来自 AT intent。AI caption 的
  `utterance_id` 是字幕分组键；device_action 的 `id` 才是 JSON-RPC
  ID。后两者均接受字符串或数字并以可显示字符串出现在 URC 中，不能与
  AT `request_id` 混用；动作回执仍保留上游 JSON-RPC ID 的原始类型。
- 字段内的 `"`、`\`、CR、LF 会转义。
- Wi-Fi 密码、device_key 和任何 token 均不可查询或回显。
- `+AT:URC_OVERFLOW,<count>` 表示有界 URC 队列在一次突发中丢弃了事件；
  主机必须把本轮事务判为不完整，并立即用 `STATUS?/SESSION?/MEDIA?`
  恢复可恢复状态。

## 固件身份

```text
AT+BUILD?
+BUILD:1,"<project_name>","<project_version>","<idf_version>",
  "<app_elf_sha256>","<tirtc_version>","<tirtc_build_info_json>"
```

`project_name` 来自 ESP app descriptor，最长为 31 个可显示字符，因此本
工程的长项目名会在运行响应中按 descriptor 上限截断。`app_elf_sha256`
是当前运行 app 对应 ELF 的 SHA256；TiRTC BuildInfo JSON 包含 SDK 版本、
commit 和下层版本，作为一个已转义的 CSV 字符串返回。响应不包含密码、
device_key 或 token。

场景工具会把这些字段与指定构建目录中的 ELF、ESP-IDF 元数据和 TiRTC
manifest 逐项比对。匹配只证明运行 app 及其 SDK 与该本地构建一致，不证明
板上的 bootloader、分区表、OTA data 或 `storage` 分区与本地候选镜像一致。

## Wi-Fi 与平台

```text
AT+WIFI?
AT+WIFI="ssid","password"
AT+WIFI=CLEAR
AT+BIND?
AT+BIND=START
AT+PLATFORM?
AT+RESTART
```

典型 URC：

```text
+WIFI:CONNECTING,<generation>,"ssid","0"
+WIFI:ONLINE,<generation>,"192.0.2.10","0"
+BIND:REQUIRED,<generation>,""
+BIND:CODE,<generation>,"xxxxxx"
+BIND:BOUND,<generation>,"device_id"
+PLATFORM:MQTT,<generation>,0,"ONLINE"
+TIRTC:READY,<generation>,0
```

Wi-Fi、绑定、平台和 TiRTC URC 都携带当前 generation；平台与 TiRTC
URC 还携带状态码。主机应按完整字段数解析，不能只匹配前缀后的单个值。

首次没有设备凭据时，收到 `+WIFI:ONLINE` 后必须继续等待
`+BIND:REQUIRED`，再发送 `AT+BIND=START`。账号登录和 6 位码确认在
开发者平台 H5/浏览器完成；设备侧不提供网页或其他交互入口。已有凭据的
token 返回 6006，或正式 MQTT command 收到精确 `type=unbind` 时，固件
会立即关闭新业务门禁并自动执行签名 Report 重绑。该路径可同样输出
`+BIND:REQUIRED/PROGRESS/CODE`，主机只观察进度，不重复发送
`AT+BIND=START`。

## 可恢复快照

```text
AT+SESSION?
+SESSION:3,<app_generation>,<revision>,<session_generation>,<request_id>,
  <owner>,<state>,<deadline_remaining_ms>,<pending_incoming_call>,<caller>,
  <audio|video|none>,
  "<room_id>","<peer_id>","<ai_session_id>",
  <ai_update_pending>,<ai_update_deadline_remaining_ms>,
  <ai_action_pending>,"<ai_action_id>",
  <ai_action_deadline_remaining_ms>,
  <ai_call_handoff_pending>,
  "<none|resolving|action-response|response-drain|adapter-drain>",
  <ai_call_handoff_deadline_remaining_ms>

AT+MEDIA?
+MEDIA:1,<adapter_state>,<connected>,<active_profile>,<measured_profile>,
  <active_session_generation>,<measured_session_generation>,
  <connection_generation>,<tx_audio_frames>,<tx_audio_bytes>,
  <tx_video_frames>,<tx_video_bytes>,<rx_audio_frames>,<rx_audio_bytes>,
  <rx_video_frames>,<rx_video_bytes>,<send_errors>,
  <first_tx_ms>,<last_tx_ms>,<first_rx_ms>,<last_rx_ms>,
  <connect_request_pending>,<connect_callback_pending>,
  <accept_callbacks_pending>,<disconnects_pending>,<connection_users>,
  <incoming_armed>
```

`SESSION` schema v3 固定为 22 个 CSV 字段（包含开头的 schema 版本）。
相对 v2，最后新增 `call_device` 交接的 pending、phase 和 deadline；
主机应拒绝未知版本或错误字段数。`pending_incoming_call` 只表示
待处理来电，不是任意异步请求的总 pending 标志。没有相应 AI pending 时，
deadline 为 `0`，`ai_action_id` 为空字符串。

`revision` 每次发布会话快照时递增。所有 deadline 返回剩余毫秒，不暴露
设备单调时钟绝对值。这些 v3 字段用于在 URC 丢失后恢复当前 AI/呼叫真值。
媒体计数在新会话取得媒体所有权时清零，结束后保留到下一代，便于主机在
回到 idle 后取证。`MEDIA` 最后 6 个字段用于判断 SDK
异步生命周期是否真正收口：进入稳定 idle 时，请求、回调、断开和 handle
使用计数必须为 0，`incoming_armed` 也必须为 0；任一字段非 0 都表示
仍处在禁止启动下一会话的清理窗口。
`send_errors` 只统计仍属于当前媒体路由的发送故障；连接关闭类返回值由
会话状态和结束原因记录，不重复计入媒体错误。

## AI 对讲

```text
AT+AI?
AT+AI=START
AT+AI=STOP
AT+AI=INTERRUPT
AT+AI=SUBMIT
AT+AI=UPDATE,"{\"latitude\":31.2304,\"longitude\":121.4737}"
AT+AIACTION="action_id",OK[,"result-json-or-message"]
AT+AIACTION="action_id",ERROR[,"reason"]
```

角色由开发者平台绑定关系和 `/v1/ai/token` 决定，设备端不接受 `role_id`
或长期 AI token。`AI=UPDATE` 只在 `ai-active` 中更新 `extra_params`，
参数必须是 JSON object，同一时刻只允许一个请求等待上游 response，
匹配 response 的超时为 10 秒。受 AT 行和意图队列边界约束，反转义后的
UPDATE/AIACTION payload 最多 128 字节；本例已按线上服务验证经纬度字段。

典型 URC：

```text
+AI:STATE,<generation>,<request_id>,<status>,"ai-connecting","","reason"
+AI:STATE,<generation>,<request_id>,<status>,"ai-starting","","reason"
+AI:STATE,<generation>,<request_id>,<status>,"ai-active","","reason"
+AI:OP,<generation>,<request_id>,<status>,"operation","phase","json"
+AI:CAPTION,<generation>,<request_id>,<type>,<mode>,<seq>,"utterance_id",<final>,"text"
+AI:EVENT,<generation>,<request_id>,<status>,"round_start","","params_json"
+AI:ACTION,<generation>,<request_id>,"action_id","action","json"
+AI:STATE,<generation>,<request_id>,<status>,"ending","","reason"
+AI:STATE,<generation>,<request_id>,<status>,"idle","","reason"
```

AI session id 从 `AT+SESSION?` 的版本化快照读取。`AI:OP` 的常见
operation/phase 包括 `ai-start accepted/completed`、
`ai-update-config submitted/completed/rejected/response-timeout`、
`ai-interrupt submitted`、`ai-submit-speech submitted` 和
`ai-action-result submitted/action-timeout`；第三个字符串保留上游原始
JSON，不能把同步 `OK` 当成业务完成。

`AI:EVENT` 的第二个字符串是协议保留位；当前通知类 method（包括
`round_start`）没有 JSON-RPC request id，因此固定为空，最后一项是
上游 `params` JSON。`AI:CAPTION` 的字符串 ID 是 `utterance_id`。
URC JSON 完整值最多 1024 字节，超出时明确替换为
`{"truncated":true}`，不会静默输出空串。

设备同一时刻只保留一个 pending device_action。主 action 成功或失败回包
提交前，新的并发动作会串行回复 busy；已完成的 busy ID 仍用于去重，第三个
不同 ID 超出串行槽时会终止 AI 与交接。进入成功回包排空阶段后，不再向
即将关闭的连接追加任何带 ID 的 JSON-RPC request response。除
`call_device` 外，收到 `+AI:ACTION` 后，主机必须在 15 秒内用
`AT+AIACTION` 回执；超时会自动发送错误 response。`OK` payload 若是
合法 JSON，会进入 `result.data`，否则作为 `result.message`；`ERROR`
使用 JSON-RPC error code `-32000`，payload 作为 message。字符串和数字
action id 在回执中保持原 JSON 类型。

`call_device` 是固件内建动作，正式参数形态为
`params.data.target` 必填字符串。固件仍输出 `+AI:ACTION` 供主机观察，
但主机不得也不需要发送 `AT+AIACTION`；解析期间发送手工回执会得到
`action-owned-by-firmware`。固件实时刷新普通设备联系人，按设备 ID、
联系人备注和唯一子串匹配在线目标。当前平台联系人合同不返回
`device_name`，因此需要按设备名称呼叫时，应先用
`AT+CONTACT=REMARK` 把该名称设为备注。成功后会依次输出：

```text
+AI:OP,...,"ai-call-device","contacts-refresh-submitted",""
+AI:OP,...,"ai-call-device","response-submitted",""
+AI:OP,...,"ai-call-device","response-drained","{\"send_buffer_bytes\":0}"
+AI:STATE,...,"ending","","ai-call-device-transfer"
+AI:STATE,...,"idle","","ai-call-device-transfer"
+AI:OP,...,"ai-call-device","adapter-drained",""
+CALL:OP,...,"call-start","accepted","..."
```

目标缺失、未知、歧义、离线或联系人刷新失败会以同一个 JSON-RPC id 返回
error，并保持 AI active。`response-submitted` 只表示 SDK 接受回包，
`response-drained` 表示同代发送缓冲区已归零；平台是否消费该结果还需
开发者平台 action trace 或后续 AI 行为证明。发送缓冲区在 5 秒内未归零
或查询失败会输出 `response-drain-terminal-failed`，结束 AI 且不发起
CALL。远端 `end_session`、连接断开或其他路径若先于显式
`response-drained` 结束 AI，会清除交接目标且不会发起 CALL。交接期间其他
action 的 busy error 首次发送失败会保留同一 ID 做有界重试，同 ID 重发
不会产生第二份 response，主 action 的总 deadline 不会被辅助重试延长；
adapter 清理与 CALL 启动的重试预算彼此独立，任一重试槽溢出都会明确结束
AI 与交接，不会静默丢请求。

`AI=STOP` 会先尝试发送 `end_session`；`+AI:OP` 以
`ai-stop/end-submitted` 或 `ai-stop/end-send-failed` 报告结果，同时
设备无论发送成功与否都会继续执行本地资源清理。

由 AT 主动触发的状态和操作沿用对应的 `<request_id>`；平台推送、远端来电
等没有本地请求来源的被动事件使用 `0`。

## 普通设备呼叫

```text
AT+CALL?
AT+CALL="target_device_id"
AT+CALL="target_device_id",VIDEO
AT+ACCEPT
AT+REJECT
AT+CANCEL
AT+HANGUP

AT+CONTACTS?
AT+PENDING?
AT+CONTACT=REQUEST,"target_device_id"
AT+CONTACT=RESPOND,"peer_device_id",ACCEPT
AT+CONTACT=RESPOND,"peer_device_id",REJECT
AT+CONTACT=REMARK,"peer_id","remark"
AT+CONTACT=DELETE,"peer_id"
```

省略第二个 `CALL` 参数时默认音频呼叫；`VIDEO` 会同时启用预录 H264
上行并请求视频下行。日志例子只统计和记录下行帧，不做显示解码。

呼叫连接 token 只用于一次 `TiRtcConnect` 提交；同步提交失败、异步失败
或超时都会结束本次会话并走平台清理，不使用同一 token 重试。如果 SDK
异步失败回调没有可断开的 connection handle，adapter 会发布失败并在回调
完成后请求受控整机重启。主机会先收到
`+SYSTEM:RESTARTING,...,"tirtc_failed_connect_transport"`；重新连接同一
AT 端口后，必须重新核对 `AT+BUILD?`、`AT+STATUS?`、`AT+SESSION?`、
`AT+MEDIA?` 和平台 HTTPS canary，才能开始下一会话。

典型 URC：

```text
+CALL:INCOMING,<generation>,<request_id>,"caller_id","room_id","audio|video"
+CALL:STATE,<generation>,<request_id>,<status>,"calling","room_id","reason"
+CALL:STATE,<generation>,<request_id>,<status>,"call-connecting","room_id","reason"
+CALL:STATE,<generation>,<request_id>,<status>,"in-call","room_id","reason"
+CALL:STATE,<generation>,<request_id>,<status>,"ending|idle","room_id","reason"
+CALL:OP,<generation>,<request_id>,<status>,"operation","phase","json"
+CONTACT:OP,<generation>,<request_id>,<status>,"operation","phase","json"
+CONTACT,<request_id>,<index>,<online>,"peer_id","remark","json"
+PENDING,<request_id>,<index>,0,"peer_id","created_at","json"
+CONTACTS:DONE,<request_id>,<count>,<status>
+PENDING:DONE,<request_id>,<count>,<status>
```

联系人中的 `voip` 类型会被静默过滤，不会暴露为普通联系人，也不会进入
会话状态机。`REMARK`、`DELETE` 只接受最近一次 `AT+CONTACTS?` 中确认
为 `type=device` 的前 32 个设备 ID；`RESPOND` 只接受最近一次
`AT+PENDING?` 返回的前 32 个待处理设备 ID。缓存未命中时命令失败关闭，
主机应先刷新相应列表；这也阻止主机把已知的任意 VoIP peer ID 送入通用
联系人接口。
