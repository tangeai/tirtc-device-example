# ESP32-S3 AT ThingConnect 日志示例：架构

这个工程是 ESP32-S3 的 TiRTC AT ThingConnect 日志示例。它以开发者平台的
ThingConnect 设备身份为唯一鉴权来源，覆盖 AI 对讲和普通设备呼叫，
不包含微信 VoIP。

默认对外控制面是 `AT+TIRTC` 精简中文指令。本文出现的 `AT+BUILD?`、
`AT+SESSION?` 等结构化指令用于协议联调和自动化验证，需要先切换到
`AT+PROTO=RAW`；两种模式共享同一业务状态机。

## 分层

```text
AT command plane
  -> at_transport            USB Serial/JTAG / UART 可替换传输
  -> app_at_commands          语法、参数校验、意图投递、URC 格式化
  -> app_controller           启动门禁、身份代际、统一状态快照
  -> session_runtime          AI/CALL 唯一会话写者和资源仲裁
  -> platform_client          服务发现、绑定、正式 MQTT、业务 HTTP
  -> tirtc_adapter            TiRTC 单实例和回调事件化
  -> media_runtime            SPIFFS 预录上行、下行统计日志
  -> ESP-IDF / TiRTC SDK
```

`wifi_manager` 和 `runtime_config` 是独立基础服务。AT 层不直接执行
HTTP、MQTT、TiRTC 断开或 NVS 清理；SDK、MQTT 和网络回调只产生定长
事件，由控制任务串行处理。

## 启动门禁

```text
NVS -> AT ready -> Wi-Fi STA -> SNTP/service discovery
    -> existing credentials: signed /v1/device/token
       -> success: formal MQTT
       -> code 6006: automatic signed Report with retained credentials
    -> no credentials: NEED_BINDING / +BIND:REQUIRED
       -> AT+BIND=START -> unsigned Report
       -> temporary MQTT -> auth_grant -> persist -> QoS1 ACK/PUBACK
    -> formal MQTT online -> TiRTC SYS_STARTED -> session ready
```

TiRTC 使用的物理 `client_id` 由 Wi-Fi STA MAC 派生，绑定、解绑和重启后
保持稳定。正式 MQTT 使用 ClientID=`sn_<device_id>`、
Username=`<device_id>`、Password=`mqtt_token`；它与 MAC 派生的 TiRTC
`client_id` 是不同标识。正式连接使用 MQTT v5 以读取认证 reason code，
临时绑定 MQTT 固定使用 3.1.1。

平台长期机密只持久化 `device_id/device_key`；另保存非机密的 MAC 派生
`client_id`、可选 TiRTC service endpoint，并独立保存 Wi-Fi 配置。设备
不保存或接受用户 JWT、AK/SK、远端 device_key、MQTT token、AI token
或呼叫连接 token。

正式 MQTT command 收到精确 `type=unbind` 时，`platform_client` 不依赖
session 队列，立即以一次性状态关闭 `ready/client_active`，尽力发送 QoS1
应用 ACK，并向 controller 报告 `REBIND_REQUIRED`。上游可能在发布后立即
踢线，因此 ACK/PUBACK 不是重绑的生命周期前提。controller 随后终止当前
会话、停止正式平台连接，并使用保留凭据执行签名 Report；notify topic
中的 `unbind` 会被丢弃。

平台请求在提交时固定 `client_epoch` 和 `auth_generation`。stop、rebind
或更新一代 token 后，旧 HTTP 响应仍会对原 callback 恰好回调一次，但
body 固定为空，不能改变新一代业务。MQTT init/start、stop/destroy、
heartbeat 和 token 缓冲区由同一操作锁保护；MQTT callback 只提交带
client/epoch 的状态事件，platform worker 串行核对代次后再通知 observer。
因此旧 SUBACK 不能在 stop/rebind 后重新发布 ONLINE，observer 也不会看到
`REBIND_REQUIRED` 后又被旧代 `MQTT_CONNECTED` 覆盖。

## 凭据保护边界

示例用版本化、带 CRC 的 NVS 记录保证 Wi-Fi 和设备身份写入完整性，并在
RAM 中尽快擦除密码、device_key 和短期 token。为了保持普通开发板可直接
烧录，本工程默认没有启用 Flash/NVS 加密，因此它不提供静态存储机密性。
产品固件应在量产烧录流程中启用 Secure Boot、Flash Encryption 和 NVS
Encryption；在此之前，不应把示例分区镜像当作可公开分发的生产固件。

## 会话仲裁

`session_runtime` 持有唯一 owner：

- `NONE`：允许启动 AI 或呼叫，也允许接收来电。
- `AI`：新来电以 `busy` 异步拒绝。
- `CALL`：AI 启动请求返回忙。

每个会话带 generation、room/session id 和单调时钟 deadline。旧代回调、
错误 room、重复结束和迟到的 HTTP/MQTT/RTC 事件不会改变当前会话。
所有结束入口进入同一幂等清理路径。断开时 adapter 先在短临界区摘除
connection handle，等已经取得该 handle 的 send/subscribe 调用归还后，
再由低优先级 worker 调用 SDK Disconnect，业务和 SDK 回调栈都不直接
执行物理断开。每个 handle 先进入唯一的 disconnect ownership registry，
直到 SDK `on_disconnected` 才释放所有权；同步无效句柄则立即完成。

主动连接同时维护 request generation 和 callback generation。取消只撤销
业务请求，回调槽仍保持占用，迟到的同代 callback 到达后再清零并回收其
handle，因此旧回调不能清掉新请求，也不能让下一会话越过 SDK 清理窗口。
`AT+MEDIA?` 暴露连接请求、回调、断开、handle 使用和来电许可计数，主机
以这些结构化字段而非固定延时判断是否回到稳定 idle。

### AI 呼叫设备交接

`call_device` 是 `session_runtime` 内建动作，不由 AT 层解释目标，也不从
SDK 回调直接启动新连接。单写者按以下阶段推进：

```text
AI active
  -> resolving: 实时 GET device contacts，提交完整快照后解析目标
  -> action-response: 串行提交并发 busy 回包与主 action 回包
  -> response-drain: 暂停预录上行，以原 JSON-RPC id 回 result
  -> adapter-drain: 发送 end_session，清理 AI connection/profile
  -> CALL: 使用解析出的 device_id 进入普通音频呼叫状态机
```

解析优先级为设备 ID 精确匹配、联系人备注/可选设备名精确匹配、唯一备注/
可选设备名子串匹配。未知、歧义、离线、畸形或超过 32 个设备联系人的快照
均失败关闭并保持 AI 会话，不猜测设备 ID。当前正式联系人合同没有
`device_name` 字段，因此设备名称只有同步为联系人备注时才可匹配；代码只
为上游未来明确增加该字段保留兼容位置。

每次联系人刷新都有独立 request cookie；同一 AI generation 内前一次超时
响应不能误归属给下一次 action。成功或错误 response 发送失败都会进入有界
`action-response` 重试，期间固件继续拥有该 action；同 JSON-RPC id 的重发
按重复请求忽略，不产生 busy 与最终结果两份矛盾响应。主成功或失败回包提交
前，另一个 action 的 busy error 与主回包严格串行：首次发送失败会保留原始
ID 并有界重试，发送完成后的 busy ID 也会留作去重，同 ID 重发只复用现有
结果，主状态机的联系人和总 deadline 仍继续推进。单槽被占用时再出现第三个
不同 ID，会终止 AI 与交接，而不是静默丢失 JSON-RPC 请求。

成功回执严格使用插件声明的 `result.ok` 和 `result.message`。只有回执被
SDK 接受、同一 connection generation 的发送缓冲区归零后，固件才提交
AI 清理；这证明设备发送队列已排空，不单独等价于平台已经消费结果，平台
action trace 或后续 AI 行为仍是云端闭环证据。主成功回包提交后进入
`response-drain` 冻结区，不再向同一连接追加任何带 ID 的 JSON-RPC request
response；此时到达的新 request 只记录为 closing ignored，并由即将关闭的
AI 连接明确失败，避免辅助回包污染主回包的排空门槛。只有同代发送缓冲区
查询成功且为零的显式路径才把阶段推进到 `adapter-drain` 并保留交接目标；
远端 `end_session`、连接断开或其他提前结束路径都会清除 reservation，不能
继续发起 CALL。交接预留期间，新 AI/CALL 意图被拒，来电按 busy 处理，旧
HTTP/RTC 回调不能抢占目标。adapter 的连接、回调、断开和 handle 使用计数
全部归零后，普通 CALL 才取得新 generation。
发送缓冲区在 5 秒内没有归零或查询失败时，交接以明确 terminal operation
结束并清理 AI，不会在回执不可证明已排空时继续发起 CALL。
如果 adapter 未在首个 10 秒窗口内安静，固件保留目标并继续执行有界清理
重试；只有 adapter 完全安静且普通 CALL 请求已成功提交后才释放 reservation。
adapter 清理和 CALL 启动各自使用独立重试计数，前一阶段的失败不会消耗后
一阶段预算。任一阶段重试耗尽都会输出明确 terminal operation，不会先清
目标再伪装成已启动呼叫。

呼叫连接 token 是一次性的：一次 token 只允许一次 `TiRtcConnect` 提交。
同步提交失败、异步失败或超时会结束会话并执行平台清理，不能用同一 token
重试。SDK 异步失败可能返回 `NULL` handle；此时没有合法 handle 可调用
Disconnect。adapter 会先发布失败事件，在终态 callback 完成后进入
`ERROR` 并标记需要整机恢复。controller 中止会话、尽力 stop/deinit TiRTC，
随后执行受控 `esp_restart()`；这是该无句柄失败的资源释放边界，避免 SDK
内部 transport socket 跨多次进程内 recycle 累积。主机只把
`+SYSTEM:RESTARTING,...,"tirtc_failed_connect_transport"` 识别为此保护
路径，并在同一 AT 端口恢复后重新核对 BUILD、READY、canonical idle 与
平台 HTTPS canary。回归工具每次只消费一条 status=0、generation 匹配的
RESTARTING；串口保持时要求同传输上的 BOOTING，串口恢复时要求一次明确
reconnect（主动重开允许没有 read failure，真实断口必须有一次 failure）
并完成 READY canary。历史重启计数不能掩盖后续故障。其他 adapter 错误
仍走进程内 stop/deinit/restart。

lwIP socket 上限配置为 16。正式 MQTT、TiRTC 监听/ICE 和短时开发者平台
HTTPS 在会话结束阶段可能同时存在；扩大描述符预算只负责容纳这个有界
重叠，不能替代上述 handle 所有权和回调代际约束。

## PSRAM 与 TLS

OPI PSRAM 是本例的硬件依赖。普通 `malloc/calloc` 以 512 字节为阈值让
大块分配优先使用 PSRAM，并为显式 INTERNAL/DMA 分配保留 96 KiB internal
RAM；允许非实时任务使用外部栈。平台 HTTP 的大响应缓冲区优先显式申请
PSRAM，platform/session 等非实时 worker 栈也放在 PSRAM。显式 PSRAM
响应缓冲区申请失败时可回退普通 allocator，但显式外部任务栈没有 internal
fallback，启动失败会作为内存错误上报。

mbedTLS 使用 ESP-IDF 默认 allocator 和系统 CA bundle，HTTPS 与 MQTTS
均执行证书校验，不使用跳过校验的开发选项。默认 allocator 会同时受上述
heap 策略影响，因此不能宣称所有 TLS 内存固定在 internal RAM 或 PSRAM。
Flash/NVS 访问可能暂时关闭外部内存 cache，所以 bootstrap 把 NVS 读写
委托给短生命周期的 internal-stack worker；NVS 操作期间的 worker 工作副本
位于 internal RAM。返回后配置会复制回 bootstrap 的 PSRAM 栈，并在使用完后
清零。

## 媒体边界

本例用于协议联调和日志验证：

- 上行使用 `media/` 中的预录 G711A/H264 素材循环发送。
- AI 音频 stream id 为 `1`，视频 stream id 为 `0`。
- 普通呼叫音频 stream id 为 `10`，视频 stream id 为 `11`。
- 下行帧做代际校验和计数后记录日志，不接扬声器或显示屏。

因此，构建和云端信令成功不等于真实麦克风、扬声器或屏幕已经验证。
接入具体板卡时只替换 media source/sink，不能把板级驱动反向放进
session、platform 或 AT 层。

## 交互边界

设备侧唯一用户控制面是 AT；账号登录和 6 位绑定码确认仍在开发者平台
H5/浏览器完成。默认后端为板载原生 USB Serial/JTAG CDC，UART0 作为
可替换 AT 后端保留；ESP-IDF 和 TiRTC 诊断日志默认走 UART0，避免日志与
AT 响应互相污染。USB 驱动由固定核的 AT task 打开和关闭，
`at_server` 不感知底层读写实现。AT task 是传输的唯一写者：命令处理器
同步写查询响应，其他任务只向有界队列投递已经格式化的 URC，AT task 在
两条命令之间冲刷队列，避免异步事件切断响应和最终 `OK/ERROR`。队列丢失
时输出 `+AT:URC_OVERFLOW` 并以版本化快照恢复真值。同步 `OK` 只表示
命令已受理；业务完成、状态变化和错误通过 URC 报告。URC 和日志均不得
输出密码或 token。

每个改变状态的 AT intent 都产生独立的数字 `request_id`。这个 ID 与
AI JSON-RPC 的 ID（字符串或数字，保留原类型）分离，并沿
`AT -> controller -> session -> HTTP/RTC callback -> URC` 传播。查询层由
controller 聚合 `session_runtime` 与 `tirtc_adapter` 的只读快照，AT
格式层不跨层读取业务内部结构。

`AT+BUILD?` 是独立的只读身份面：它从运行 app descriptor 与 TiRTC
BuildInfo 读取 project/version、ESP-IDF、app ELF SHA256 和 SDK 版本。
场景工具据此绑定运行 app 与指定本地 ELF/manifest；bootloader、分区表、
OTA data 和 SPIFFS `storage` 仍需用各自镜像哈希和烧录记录证明。

AI 角色归开发者平台所有，由设备 token 接口选择。AT 的 `AI=UPDATE`
只更新当前 AI 会话的 `extra_params`，不会绕过平台切换角色。字幕的
`utterance_id` 只用于分组与修订；device_action 的 JSON-RPC id 则同时
保存显示值和原始 JSON 表示，使 AT 回执能把字符串或数字 id 原样映射回
上游 response。动作和 UPDATE 各自只有一个有界 pending 槽及独立超时。
`ai-stop` operation 还会在 session task 实际处理 STOP 的时刻记录
connect request/callback pending 布尔值。竞态回归以这份原子处理点证据
判断 STOP 是否真正撞上异步连接，不用先查询 `AT+MEDIA?` 的旧快照代替。
