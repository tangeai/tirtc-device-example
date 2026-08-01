# 小钛（AI 对讲）接入说明

本文档对应本工程的小钛设备端实现，覆盖 AI 对讲和联系人动作。代码入口位于
`main/services/ai_chat*`、`main/application/app_ai_device_action.c` 与 UI 模块。

参考官方文档：

- [AI Chat 开发文档](https://docs.tange.ai/products/ai-chat/)
- [总体流程](https://docs.tange.ai/products/ai-chat/guides/overall-flow.html)
- [设备端集成](https://docs.tange.ai/products/ai-chat/guides/device-integration.html)
- [事件协议](https://docs.tange.ai/products/ai-chat/api-reference/events.html)

## 角色边界

```mermaid
flowchart LR
    Device["ESP32-S3 设备端"] -->|device_id/device_key 签名| Thing["ThingConnect 业务服务"]
    Thing -->|mqtt_token| Device
    Device -->|mqtt_token 换 peer_id/token| Thing
    Device -->|使用 peer_id/token| TiRTC["TiRTC WHIP 连接"]
    TiRTC -->|音频/字幕/事件| Platform["AI Chat 云端"]
```

当前工程按 Python 演示的 ThingConnect 流程实现：设备保存绑定得到的 `device_id/device_key`，先签名请求 `/v1/device/token` 换取 `mqtt_token`，再用 `Authorization: Bearer mqtt_token` 请求 `/v1/ai/token` 获取短期 `peer_id/token`。设备端不再走旧版直连 token API，也不再持有 AI Chat 平台级服务密钥。

## 当前代码结构

| 文件 | 作用 |
| --- | --- |
| `main/services/ai_chat.c` | AI 对讲状态机，负责开关会话、WHIP 建连、`start_session`、心跳、打断、结束会话、音频上行。 |
| `main/services/ai_chat_events.c` | 解析云端下发的 JSON-RPC 事件，包括字幕、轮次开始/结束、打断、`device_action` 和结束。 |
| `main/services/ai_chat_token.c` | AI token 编排层：复用设备认证拿 `mqtt_token`，再向 `/v1/ai/token` 请求 `peer_id/token`。 |
| `main/application/app_ai_device_action.c` | 校验联系人动作、匹配联系人，并把设备呼叫或微信呼叫交给应用生命周期处理。 |
| `main/services/device_binding/device_auth_http.c` | 设备认证层：按 `device_id + timestamp + nonce` 做 HMAC-SHA256 签名，请求 `/v1/device/token`。 |
| `main/protocols/http/thing_http_client.c` | 通用 ThingConnect HTTP 客户端，统一 URL 拼接、HTTPS 证书包、超时和响应收集。 |
| `main/application/app.c` | 把设备配置、RTC 配置、小钛服务和应用生命周期连起来。 |
| `main/protocols/tirtc/tirtc_session.c` | TiRTC 连接层，新增 observer、WHIP connect、stream 1 音频订阅/发送接口。 |
| `main/drivers/audio/audio.c` | 麦克风采集和播放驱动，AI 对讲通过 capture observer 旁路采集音频。 |
| `main/services/rtc_media_bridge.c` | TiRTC 音频格式和本地播放格式转换，AI 下行 PCM 走这里播放。 |
| `main/ui/display.c` | 小钛页面，展示状态、ASR 字幕、TTS 字幕和开始新对话入口。 |

## 启动流程

```mermaid
sequenceDiagram
    participant UI as 小钛页面
    participant App as application/app.c
    participant AI as ai_chat.c
    participant Auth as device_auth_http.c
    participant Token as ai_chat_token.c
    participant RTC as tirtc_session.c
    participant Thing as ThingConnect
    participant Cloud as AI Chat 云端

    UI->>App: 打开小钛
    App->>AI: ai_chat_open()
    AI->>RTC: tirtc_session_start_if_ready()
    AI->>Token: 请求 AI join 凭证
    Token->>Auth: device_id/device_key/mac
    Auth->>Thing: POST /v1/device/token
    Thing-->>Auth: mqtt_token
    Token->>Thing: GET /v1/ai/token
    Thing-->>Token: peer_id + token
    AI->>RTC: tirtc_session_whip_connect(peer_id, token)
    RTC-->>AI: WHIP connect ok
    AI->>AI: 延迟约 300ms 等 KCP 数据通道稳定
    AI->>RTC: 发送 start_session
    Cloud-->>AI: start_session result
    AI->>RTC: 订阅音频 stream 1
    AI-->>UI: 状态切到会话中
```

## 语音上行

```mermaid
flowchart TD
    Start["start_session 成功"] --> Obs["启用 audio capture observer"]
    Obs --> Mic["麦克风 PCM"]
    Mic --> Resample["统一为 16 kHz / 16 bit / 单声道"]
    Resample --> Frame["TIRTC_AUDIO_PCM, stream_id=1"]
    Frame --> Send["tirtc_session_send_audio_frame()"]
    Send --> Cloud["AI Chat 云端 ASR/LLM/TTS"]
```

实现细节：

- 小钛对讲只有一种模式：会话建立后默认常听，用户直接说话即可。
- 实体侧键只做控制动作：AI 正在回复时按下发送 `interrupt`，清空本地远端媒体队列并停止旧回复播放。
- 会话空闲或异常断开后，小钛页面内按下实体侧键会重新发起一次新会话。
- 页面底部的“开始新对话”按钮和侧键重连走同一条启动流程。
- 当前固定协商 `pcm / 16000 Hz / 1 channel`，且 `start_session.input_audio`、`TIRTCFRAMEINFO.media` 和 `TIRTCFRAMEINFO.flags` 必须保持一致。
- `start_session` 使用 JSON-RPC 2.0，`id` 按官方事件协议使用字符串，例如 `start-session-001`；当前默认 `user_id=user-001`。`role_id` 以 `/v1/ai/token` 返回的 `peer_id` 查询参数为准，本地 `APP_CONFIG_AI_CHAT_ROLE_ID` 只作为兜底默认值。如果云端不回包，先确认设备日志里是否出现 `rtc command callback`。
- WHIP 连接回调后不会立刻发 `start_session`，当前延迟 300ms，和 Python demo 一致，用来等待 KCP 数据通道稳定。
- 必须等 `start_session` 成功响应后再启动麦克风上行；响应前推流会被云端丢弃。
- 如果底层采集格式不是 16 bit 单声道的 8 kHz 或 16 kHz PCM，本地会丢弃这一帧并计入丢帧，避免把伪静音发给云端。
- 打开会话、token 请求、WHIP 回调之间用 generation 做生命周期校验；用户退出页面后，未完成的旧启动流程不会再把设备拉回 AI 会话。

## 语音下行和字幕

```mermaid
flowchart LR
    Cloud["云端 TTS 音频 stream 1"] --> RTC["TiRTC on_audio"]
    RTC --> Bridge["rtc_media_bridge"]
    Bridge --> Sink["media_sink"]
    Sink --> Speaker["扬声器播放"]

    Cloud2["云端 caption 事件"] --> Cmd["命令字 0x2100"]
    Cmd --> Parser["ai_chat_events_parse"]
    Parser --> UI["小钛页面字幕"]
```

字幕处理规则：

- `caption_type=0` 展示为用户 ASR 字幕。
- `caption_type=1` 展示为云端回复字幕。
- `mode=1` 优先按增量追加；如果云端实际下发的是当前完整文本前缀，设备端会按全量文本替换，避免字幕重复。
- `mode=0` 用全量文本替换。
- 使用 `caption_type + utterance_id` 分组，避免不同轮次混在一起。

## 事件处理

| 事件 | 当前处理 |
| --- | --- |
| `start_session` result | 校验音频格式，订阅 stream 1，进入会话中。 |
| `caption` | 更新 ASR/TTS 字幕。 |
| `round_start` | UI 切到云端回复状态。 |
| `round_end` | UI 回到等待输入状态。 |
| `heartbeat` | 会话中每 30 秒发送一次。 |
| `interrupt` | 本地清空远端媒体队列并停止播放旧回复。 |
| `event` / `device_action` | 解析设备联系人状态查询、设备呼叫和微信呼叫动作，并返回结构化结果。 |
| `end_session` | 幂等关闭采集、清空播放队列、取消订阅和断开 TiRTC 连接。 |

## 联系人与呼叫动作

小钛可以通过 `device_action` 调用设备上的联系人能力：

- 查询设备联系人状态，支持 `online`、`offline` 和 `all` 过滤。
- 按设备 ID 或备注匹配设备联系人，发起音频或视频呼叫；未提供
  `call_type` 时按音频呼叫处理。
- 按微信 OpenID 或备注匹配微信联系人，并发起语音呼叫。微信联系人当前不提供在线状态
  查询，也不接受视频呼叫。
- 目标不存在、同名歧义、目标离线、网络离线、设备忙碌或呼叫类型不受支持时，设备会返回
  明确的失败结果，不会静默切换应用。

动作返回 `accepted` 时，只表示设备已经完成目标校验并接受应用生命周期切换请求，
不表示对端已经响铃、接听或建立媒体。后续进度仍由设备呼叫或微信呼叫状态机报告。

## 实现与验证边界

当前源码覆盖设备签名换取 `mqtt_token`、获取 AI `peer_id/token`、WHIP 建连、
`start_session`、PCM 上行、下行音频订阅、字幕展示和联系人动作路由。发布固件的构建
来源、大小与 SHA-256 以同一 Release 的 `release-manifest.json` 为准。

在目标板上集成时，建议依次确认系统时间与设备授权、`pcm / 16000 / 1` 协商、
ASR/TTS 字幕、扬声器连续播放，以及联系人查询和呼叫动作的成功、离线、歧义和忙线分支。
