# ThingConnect 设备呼叫流程

本文以 ThingConnect 当前 `api-reference.md`、Python 设备模拟器和 C 设备模拟器为准，描述 ESP32-S3 固件中的设备呼叫流程。

## 1. 身份与常驻监听

- `device_id`：云端分配的 12 位设备身份，传给 `TiRtcStart(device_id, ...)`。
- `device_secret_key`：设备密钥，在 `TiRtcStart` 前通过 `TIRTC_OPT_DEVICE_SECRET_KEY` 设置。
- `client_id`：当前物理设备的稳定唯一标识，本工程使用无分隔符的大写 MAC，例如 `9C139E8BB4E8`。
- Wi-Fi、系统时间、`/v1/device/token` 鉴权和正式 MQTT 上线全部就绪后，固件才依次调用 `TiRtcInit`、设置 SDK 选项、`TiRtcStart`，等待 SDK 进入 started 状态。只有本地存在凭证还不够。
- `TiRtcStart` 成功后保持监听。一次通话结束只断开 P2P 连接，不执行 `TiRtcStop` 或 `TiRtcUninit`；SDK 完整停止只属于网络释放或应用生命周期结束。

`client_id` 必须始终使用同一个物理身份。收到 ThingConnect 的 MQTT `unbind` 或 `/v1/device/token` 返回未绑定时，固件关闭 TiRTC 身份门禁、停止当前 SDK 监听并保留 NVS 中的 `device_id/device_secret_key`，随后使用这组凭证发起签名 `Report`。用户重新绑定后仍使用原 `device_id`，正式 MQTT 再次上线后才恢复 TiRTC。

收到 `40305 TIRTC_SERVICE_CLIENT_ID_CONFLICT` 时，固件不会临时改用 `device_id` 或随机值重试，而是结束本次 SDK 初始化、等待 60 秒后使用原身份重新初始化。若同一物理 MAC 曾经被清空本地凭证并分配过新的 `device_id`，ThingConnect 页面可能已经显示新身份，但 TiRTC 服务仍保留旧的 `client_id -> device_id` 映射；此时必须由服务端释放或迁移旧映射，设备端无法可靠地自行修复。

## 2. 呼叫类型与动作来源

- 当前 S3 产品只支持 `call_type=audio`。呼叫请求会把该字段传给
  `POST /v1/call/request`，来电也会先校验再保存。
- 兼容没有 `call_type` 的旧来电时按 `audio` 处理；`video` 和其他未知值直接拒绝，避免
  云端房间、UI 和媒体资源进入不同模式。
- 用户可以从设备呼叫页面发起音频呼叫；小钛也可以按设备 ID 或备注发起音频呼叫。
  小钛返回 `accepted` 只表示设备接受了应用切换请求，不表示对端已经响铃或接听。
- 微信 VoIP 使用独立联系人和 WHIP 会话，当前设备侧同样只发起音频呼叫。

## 3. 主叫流程

1. 应用层准备音频和 RTC 资源。
2. 呼叫服务等待本机 TiRTC 监听状态 ready。
3. ready 后设置“下一条入站连接先接收、暂缓媒体”，再携带目标设备和 `call_type`
   调用 `POST /v1/call/request`。
4. 云端返回 `room_id`，本地进入 `OUTGOING`，启动 30 秒响铃计时。
5. 对端接听后，本机收到 MQTT `callee_answered`，进入 `CONNECTING` 并停止响铃超时判定。
6. 被叫使用业务接口取得 token，并主动 `TiRtcConnect` 到主叫。
7. 主叫接收到入站 TiRTC 连接，但在收到业务确认前不启动音频。
8. 被叫发送命令 `0x2000`，负载为 `{"room_id":"..."}`。
9. 主叫校验 `room_id`，匹配后进入 `IN_CALL` 并启动双向音频；不匹配
   则断开连接。

关键约束：如果 TiRTC 监听没有 ready，不能先创建云端房间。这样可以避免“云端已接听、本机却因 SDK 启动失败进入 ERROR”的半完成状态。

## 4. 被叫流程

1. MQTT 收到 `call_incoming`，保存 `room_id`、`caller_id` 和 `call_type`，显示接听弹窗。
2. 用户点击接听后，先确认本机 TiRTC 监听 ready。
3. ready 后调用 `POST /v1/call/device/info`，请求体包含 `device_id`、`room_id`、`purpose=call`。
4. 接口成功即代表业务侧已经接听，并会向主叫发送 `callee_answered`。
5. 被叫使用接口返回的 token 调用 `TiRtcConnect(caller_id, token)`。
6. P2P 连接成功后发送 `0x2000` 房间确认，进入 `IN_CALL` 并启动双向音频。

`POST /v1/call/device/info` 有接听副作用，因此不能在本机 RTC 未 ready 时提前调用。当前实现只使用该正式接口，不再回退到旧的 `/v1/device/info`。

## 5. 结束与异常补偿

- 主叫未接通时取消：`POST /v1/call/cancel`。
- 被叫拒接：`POST /v1/call/reject`，`reason` 只使用 `busy` 或 `decline`。
- 已接听或 P2P 建连失败：`POST /v1/call/hangup`，`reason` 只使用 `hangup` 或 `p2p_error`。
- 通话内主动挂断：先尽量发送 P2P 命令 `0x2001`，再断开连接并异步清理云端房间。
- 收到 `0x2001` 或 MQTT `room_cancel`：清理本地房间状态并断开 P2P，TiRTC SDK 回到常驻监听状态。
- 收到共享 RTC 命令 `0x2000` 时先按会话所有者分流；属于微信观察者的命令不会被普通
  设备呼叫状态机消费。
- `callee_answered` 只表示对端开始 P2P 连接，不等于媒体已经可用；真正进入通话以匹配的 `0x2000` 为准。
- 进程异常重启后可使用 `GET /v1/call/room` 查询云端残留房间。新呼叫收到 `40202` 时，固件会按返回的旧 `room_id` 先清理再重试。

## 6. 日志验收顺序

主叫成功链路应按顺序出现：

```text
stage=rtc_ready_wait_done ... ret=ESP_OK
stage=call_request_done ... call_type=audio ret=ESP_OK
stage=ringing ...
stage=callee_answered_rx ... matched=1
stage=in_call role=caller ... cmd=0x2000
```

被叫成功链路应按顺序出现：

```text
stage=incoming_received ...
stage=rtc_ready_wait_done ... ret=ESP_OK
stage=token_request_done ... path=/v1/call/device/info ret=ESP_OK
stage=p2p_connect_submitted ... ret=ESP_OK
stage=connected_notice_tx ... cmd=0x2000 ret=ESP_OK
stage=in_call ... role=callee
```

收到视频来电时，应看到 `reason=unsupported_call_type` 并由设备拒绝，不能继续创建本地视频
资源。看到 `40305` 时，本次日志中不应再出现 `/v1/call/request`，并且 `CALL_FLOW` 应保持
`active=0 role=none state=idle room=-`。如果仍然创建房间，说明业务建房和 RTC ready 门禁
没有生效。构建成功只能证明代码层通过，身份修复、连接、音频和二次呼叫仍需以服务端状态
及两台真机日志为准。
