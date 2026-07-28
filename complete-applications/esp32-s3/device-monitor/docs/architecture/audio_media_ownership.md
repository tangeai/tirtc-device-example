# 音频媒体所有权约束

这份约束用于后续维护 IPC 查看、AI 对讲、微信 VoIP 三条音频链路。任何改动音频、TiRTC 状态机、应用切换、内存迁移或媒体策略的代码，都要先按这里检查。

## 三条链路

| 应用 | 音频 owner | 采集入口 | 上传/发送方式 | 关键边界 |
| --- | --- | --- | --- | --- |
| IPC 查看 | TiRTC 内建媒体桥 | 主麦克风 callback | TiRTC local audio stream | 进入 IPC 必须恢复 built-in owner，不能继承 AI/VoIP 的外部音频状态 |
| AI 对讲 | AI Chat 服务 | 麦克风 observer | AI Chat 自己发 command/audio frame | 不接管 TiRTC 主采集，不改 IPC 的 built-in owner 持久状态 |
| 微信 VoIP | WeChat VoIP 服务 | 麦克风 observer | 外部 WHIP/external audio | 通话期间标记 external audio，退出必须解除外部音频状态 |

## 修改门禁

1. 先确认当前改动属于哪条链路，不允许用一个全局开关顺手修所有应用。
2. 进入应用时必须显式恢复自己的 owner：
   - IPC：恢复 TiRTC built-in media，打开本地音频发送意图，再应用 IPC 音量策略。
   - AI 对讲：只启动自己的 observer/连接流程，不抢 TiRTC 主采集。
   - 微信 VoIP：只在入会期间进入 external audio，结束时退出。
3. 退出应用时只释放自己拥有的资源，不能破坏下一条链路的入口恢复。
4. 任何修复都至少验证这些切换：
   - IPC 查看 -> 返回主页 -> AI 对讲 -> 返回主页 -> IPC 查看
   - IPC 查看 -> 返回主页 -> 微信 VoIP -> 返回主页 -> IPC 查看
   - AI 对讲 -> 返回主页 -> 微信 VoIP
   - 微信 VoIP -> 返回主页 -> AI 对讲

## 判定日志

判断 IPC 没声音时按这三层拆：

| 现象 | 应看日志 | 含义 |
| --- | --- | --- |
| 麦克风是否真的采集 | `mic capture level` | 有这条说明底层采集任务有数据 |
| IPC 是否接管 TiRTC 主采集 | `rtc microphone capture enabled ... owner=tirtc` | 有这条说明 IPC built-in owner 生效 |
| 是否上传到云端 | `local audio first packet` / `local audio tx` | 有这条说明音频帧已经进入 TiRTC 发送链路 |

如果只有第一层有，问题在 owner/媒体策略；如果前两层有但没有上传，问题在 call active、stream id、bootstrap 或 peer audio request；如果三层都有但 Web 无声，问题再看云端/Web 解码链路。

## 当前代码锚点

- `main/application/app.c`：`app_apply_device_ipc_audio_profile`
- `main/protocols/tirtc/tirtc_session.c`：`tirtc_session_use_builtin_media`
- `main/protocols/tirtc/tirtc_session.c`：`tirtc_session_complete_call_response`
- `main/protocols/rtc/rtc_transport.c`：`rtc_transport_use_builtin_media`

## 原则

三条链路必须一劳永逸地兼容。后续修一个应用时，不能让另外两个应用靠运气保持可用；要靠入口恢复、退出释放和日志证据闭环。
