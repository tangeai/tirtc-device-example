# 验证清单

## 静态与构建

1. 使用 ESP-IDF `v5.5.4`，并用 `-B build-hardware-final` 隔离构建产物。
   目录名可替换，但不能复用其他 checkout 的构建目录；工程根
   `sdkconfig` 与 `sdkconfig.defaults` 是本 checkout 的配置闭环。
2. 核对 TiRTC `v2.2.1`、commit 和静态库 SHA256。
3. 确认目标为 ESP32-S3、16 MB Flash、OPI PSRAM，tick 为 1000 Hz、
   FreeRTOS trace/stats 关闭、`sizeof(StaticSemaphore_t)==84`，
   `CONFIG_LWIP_MAX_SOCKETS=16`，原生 USB AT 与 UART0 日志互相隔离。
4. 核对 PSRAM/TLS 策略：
   `CONFIG_SPIRAM_USE_MALLOC=y`、512 字节分配阈值、96 KiB internal
   reserve、允许外部任务栈、mbedTLS default allocator 和系统 CA bundle。
   不能把 default allocator 误写成“所有 TLS 分配固定在 PSRAM”。
5. 核对 app、bootloader、分区表、OTA data、SPIFFS `storage` 和
   `flasher_args.json` 都来自同一构建目录，且 storage 不早于预录素材。
6. 扫描源码中的固定凭据模式，并把已知旧例子的密码、device_id、
   device_key 逐项与生成固件做二进制比对；短 SSID 只作人工复核，
   不能单独作为泄漏结论。
7. 确认 main/platform/session 不引用 VoIP header、service、命令、状态或
   `/v1/voip`；SDK 交付包自带但未引用的兼容 header 不代表本例启用了
   VoIP。

静态检查和可选的已知凭据二进制比对：

```powershell
.\tools\validate_example.ps1 `
  -ReferenceLocalConfig ..\tirtc_esp32s3_wifi_link_demo\main\local_config.h `
  -FirmwarePath .\build-hardware-final\tirtc_esp32s3_at_thingconnect_demo.bin `
  -BuildConfigPath .\sdkconfig
```

不传 `ReferenceLocalConfig` 与 `FirmwarePath` 时，脚本只执行 SDK/素材
哈希、配置和源码规则检查；`BuildConfigPath` 省略时默认使用工程根
`sdkconfig`。传入固件时，脚本还会核对 checkout/build/config/defaults
所有权、target/ESP-IDF、生成配置、16 MB/OPI/console 设置、完整 flash
资产、storage 新鲜度和固件 SHA，不能用另一份配置替旧固件背书。
当前脚本对 PSRAM/TLS 只覆盖其中一部分配置，交付前仍需逐项核对本节第 4
项。脚本也不会把“没有正则命中”当成任意未知高熵密钥都不存在的证明。

## 单板

1. USB Serial/JTAG CDC 只输出 AT 响应和 URC，UART0 只输出内部日志。
2. `AT+BUILD?` 的 schema、project descriptor、IDF、app ELF SHA256 和
   TiRTC BuildInfo 与本地指定构建一致。
3. AT 配网、断电重启和清除配置。
4. 首次绑定：`+WIFI:ONLINE` 后先等待 `+BIND:REQUIRED`，再发送
   `AT+BIND=START`；验证绑定码、auth_grant、凭据持久化、ACK PUBACK 和
   正式 MQTT。
5. 已绑定启动：先 HMAC 签名取 token，不重复发绑定码。
6. token 6006：保留设备凭据并自动 signed Report；正式 MQTT command
   `type=unbind` 到达时立即关闭新业务门禁，尽力 ACK，终止会话并自动
   signed Report。两条路径都不能要求主机再次发送 `AT+BIND=START`。
7. 临时 MQTT 确认为 3.1.1；正式 MQTT 确认为 v5，且实际公开/生产连接
   使用 `mqtts://`。
8. TiRTC `SYS_STARTED` 以后才允许业务会话。
9. 每个异步操作的业务 URC 使用与 `+REQUEST` 一致的 `request_id`；
   丢失 URC 后，`AT+SESSION?` schema v3 的 22 个字段能恢复
   owner/state/peer/room/deadline、AI pending 和 `call_device` 交接状态。
10. `AT+MEDIA?` 能区分上下行音频/视频帧、字节和发送错误；连接终态由
    会话结束原因取证，不重复计入媒体发送错误。
11. 呼叫连接 token 只提交一次，同一 token 不因同步失败、异步失败或超时
    重试。异步失败返回 `NULL` handle 时不得伪造 Disconnect；应观察到
    失败事件、指定原因的受控整机重启；AT 恢复后 BUILD 身份不变、会话为
    canonical idle、adapter 为 `running`，且平台 HTTPS canary 成功。

## AI 云端

验证 token、WHIP、匹配的 JSON-RPC response、协商音频、首个上行包、
首个下行包、caption 修订、round、interrupt、submit、device_action
回执、end 和超时。至少重复 20 轮。

### AI 呼叫设备

1. 在 AI 角色中启用 action=`call_device`、必填字符串 `target`、返回
   `ok:boolean` 和 `message:string` 的设备插件。
2. 用 `AT+CONTACTS?` 确认目标是唯一在线普通设备联系人；需要按设备名称
   呼叫时，用 `AT+CONTACT=REMARK` 把名称设为联系人备注。
3. SPIFFS 预录 G711A 素材说出“呼叫 + 目标备注”，只用
   `AT+AI=START` 触发真实 AI action，不手工发送 `AT+AIACTION`。
4. 核对同一 action 的联系人刷新、response submitted、同代发送缓冲区
   drained、AI ending/idle、adapter drained 和普通 CALL accepted 顺序。
   缓冲区未归零或查询失败的 terminal 路径必须结束 AI 且不出现 CALL
   accepted；交接期间并发 action 的 busy error 首次发送失败必须保留原
   JSON-RPC ID 有界重试，主成功和失败回包都必须等待该 busy 回包完成；已经
   完成的 busy ID 重发不得产生重复 response，第三个不同 ID 必须明确终止
   交接，辅助重试不得暂停联系人或主回包 deadline。主成功回包提交后必须
   冻结所有带 ID 的 JSON-RPC request response，保证发送缓冲区排空只为已
   串行提交的回包收口；显式 drain 成功前若远端结束或断线，必须清除目标且
   不得发起 CALL。adapter 清理和 CALL 启动的重试预算还需分别耗尽验证。
5. 第二块板只用 `AT+ACCEPT`、`AT+REJECT`，主叫只用 `AT+CANCEL`、
   `AT+HANGUP`，覆盖接通、拒绝、取消并验证双向预录音频计数。
6. 分别验证设备 ID、备注、唯一子串、未知、歧义、离线、联系人 HTTP
   失败、交接期间 STOP/新 action/来电、连续多轮和超时清理。
7. 串口发送缓冲区归零不等价于平台已消费 JSON-RPC response；还需保存
   开发者平台 action trace/reveal，或结果驱动的后续 AI 行为证据。

## 双板呼叫

验证联系人、主叫、被叫、接听、拒绝、取消、挂断、错误 room、
`0x2000/0x2001`、房间恢复、双向音频和可选视频。至少重复 20 轮。

## 并发与故障

验证 AI 中来电忙拒、呼叫中 AI 返回忙、Wi-Fi/MQTT 断线、HTTP 超时、
畸形或分片 MQTT、队列压力、旧 generation、重复结束、连接 callback 返回
错误且 handle 为 `NULL`、正式 MQTT `unbind` 和平台立即踢线。

构建只能证明源码与 SDK 链接闭环。没有真实设备、账号、H5、AI 云端和
第二块板的日志时，不得把对应项报告为已验证。

## 证据层级

串口证据可用以下工具采集；配网命令在 JSONL 中会被脱敏。场景工具要求
两台设备已经绑定、同属可互呼账号并互为普通设备联系人，首次 H5 登录和
输入绑定码仍是显式人工/浏览器步骤。以下命令中的 `suite --iterations 1`
是功能 smoke，两个 20 次命令是定向 race，不等价于 AI 或普通呼叫的完整
20 轮发布门禁：

```powershell
$portA = '<AT_PORT_A>'
$portB = '<AT_PORT_B>'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$artifactRoot = ".\artifacts\candidate-$stamp"
$env:TIRTC_TEST_WIFI_SSID = '<ssid>'
$env:TIRTC_TEST_WIFI_PASSWORD = '<password>'
python .\tools\at_regression.py probe --port $portA --port $portB `
  --artifact-dir "${artifactRoot}-probe"
python .\tools\at_regression.py wifi --port $portA --port $portB `
  --artifact-dir "${artifactRoot}-wifi"
python .\tools\at_scenarios.py preflight --port-a $portA --port-b $portB `
  --build-dir .\build-hardware-final `
  --artifact-dir "${artifactRoot}-preflight"
python .\tools\at_scenarios.py suite --port-a $portA --port-b $portB `
  --build-dir .\build-hardware-final --media-seconds 8 --iterations 1 `
  --artifact-dir "${artifactRoot}-suite-smoke"
python .\tools\at_scenarios.py ai-call-device `
  --port-a $portA --port-b $portB --target-alias '<contact-remark>' `
  --build-dir .\build-hardware-final --media-seconds 8 --iterations 1 `
  --artifact-dir "${artifactRoot}-ai-call-device"
python .\tools\at_scenarios.py race-ai-stop --port-a $portA `
  --build-dir .\build-hardware-final --iterations 20 `
  --artifact-dir "${artifactRoot}-race-ai-stop-20"
python .\tools\at_scenarios.py race-call-cancel `
  --port-a $portA --port-b $portB --build-dir .\build-hardware-final `
  --iterations 20 --artifact-dir "${artifactRoot}-race-call-cancel-20"
```

`at_regression.py wifi` 会把 `WIFI_SET` 请求与同 generation 的
`wifi_config_changed` 计划重启绑定；它只证明配置已写入并进入重启流程。
紧随其后的 `at_scenarios.py` preflight 负责重新打开串口、等待平台上线，
再用 `AT+BUILD?` 把每块板运行的 app ELF、
ESP-IDF 和 TiRTC SDK 与 `--build-dir` 的本地 ELF/manifest 绑定，再进行
业务测试。这个比对不证明板上的 bootloader、分区表、OTA data 和 storage
镜像；它们必须由刷写记录和各自 SHA256 补齐。工具还会拒绝复用已有证据
目录，并在场景结束时确认本地源码/构建身份没有在测试中途变化。

工具不以“出现 `OK`”作为业务通过。它关联 `+REQUEST` 与同 request ID 的
`AI:OP/CALL:OP`，再核对两端 SESSION、当前媒体 generation、相对基线
增长、忙线/拒绝原因及回到 idle 后的 AT 可恢复性。JSONL 保存 transport
epoch、接收序号和单调时间；丢事件、断帧、传输重连或读错误都会使场景
失败。竞态场景以 `AT+MEDIA?` 最后 6 个异步计数全部归零、adapter 仍为
`running`、平台错误为 0 作为下一轮门槛；固定 sleep 不作为清理完成证据。

20 轮 AI stop race 覆盖 6 个请求/连接窗口，并从 `ai-stop` operation
读取 session task 处理 STOP 当刻的 adapter 状态；门禁要求同时命中
`local-cleanup-no-connection` 与 `end-submitted`，覆盖连接建立前后两侧。
内部 connect pending 标志仍保留为诊断字段，但同步 SDK 提交窗口不要求
能被单写者任务观测。20 轮 call cancel race 覆盖两方向、音频/视频和 5 个计划
延迟；每一路必须同时出现 cancel-won 与 connect-won，后者严格要求
`no-outgoing-call/status=259` 后再 HANGUP，并核对实际延迟没有严重漂移。
受控重启每次只能消费一条 generation/status/reason 完整匹配的事件。若
原串口保持打开，必须在同一传输上看到 `BOOTING`；若恢复流程重开串口，
必须恰好增加一次 reconnect，主动重开时允许没有 read failure，真实断口
则必须恰好增加一次 failure。两种重开路径都必须完成完整 READY 恢复。

证据输出会脱敏 Wi-Fi SSID/IP、MAC、设备/房间/AI session id、绑定码、
caption、action、AI JSON 和敏感 AT 命令；内存中的原始 AT 行只用于本轮
断言，不写入 JSONL 或 summary。失败路径只记录异常类型和不可逆指纹；
原始 operation payload、状态快照和清理异常文本不写入证据或
`summary.error`。

`summary.json` schema v4 保存 iteration progress、race coverage、本地
build identity 和各板 running build。`raw_evidence` 记录原始
`serial-events.jsonl` 的绝对路径、随机 `run_id`、字节数、记录数和
SHA256；JSONL 每条记录以及 summary 中抽取的 step 都携带同一个
`run_id`。归档时必须同时保留 summary 与原始 JSONL，并重新计算 SHA256
比对；不得追加、覆盖或把不同 run 的文件拼在一起。

完整发布门禁还必须单独证明本工具没有覆盖的项目。AI 至少包括 caption
修订、round、device_action 的 OK/ERROR 与字符串/数字 ID、UPDATE/action
超时、远端 end 和真实上下行媒体；普通呼叫至少包括联系人全部操作、错误
room、`0x2000/0x2001`、房间恢复和真实双向媒体。每项至少连续 20 轮，并
保留平台日志、两端串口原始证据和失败轮次。任何 smoke、race 或 summary
显示 `passed`，都不能自动提升为完整发布通过。
