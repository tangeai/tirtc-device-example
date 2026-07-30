# TiRTC ESP32-S3 AT ThingConnect Demo

这是一个与 `tirtc_esp32s3_wifi_link_demo` 平级的 AT 日志例子。设备通过
TiRTC 开发者平台完成发现、绑定、正式 MQTT 在线、AI 对讲和普通设备呼叫。
所有设备侧用户操作和状态查询都经 AT 指令完成；开发者平台账号登录与
6 位绑定码确认仍在 H5/浏览器完成。本例不包含微信 VoIP、SoftAP 网页配网、
LVGL、OTA 或设备端 token 生成器。

## 功能

- Wi-Fi STA 账号密码由 AT 写入 NVS，重启后自动连接。
- 使用稳定的 Wi-Fi STA MAC 作为物理身份和 TiRTC `client_id`。
- `AT+BUILD?` 查询运行中 app ELF、ESP-IDF 和 TiRTC SDK 的脱敏身份。
- 首次 Report、6 位绑定码、临时 MQTT、`auth_grant`、QoS1 ACK/PUBACK。
- `/v1/device/token` HMAC 鉴权、正式 MQTT、心跳、分片消息和 token 刷新。
- 平台 `unbind` 到达时立即关闭新业务门禁，并用保留的设备凭据签名 Report
  进入受控重绑。
- AI token、WHIP、`0x2100` 会话、caption、round、打断和设备动作回执。
- 内建 `call_device` 动作按联系人备注或设备 ID 解析目标，先可靠回执 AI，
  再把同一串行会话从 AI 原子交接到普通音频呼叫。
- 普通设备联系人、主被叫、接听、拒绝、取消、挂断及房间恢复。
- AI 与呼叫共享一个带 generation/deadline 的会话仲裁器。
- SPIFFS 预录 G711A/H264 上行；下行音视频计数并记录脱敏日志。

## 工程边界

本例定位是协议和状态机联调。预录上行与下行日志让用户在没有板级
麦克风、扬声器、摄像头和显示屏驱动时观察完整控制流程。真实媒体体验
仍需把 `media_runtime` 的 source/sink 替换为目标板驱动，并做真机验证。

## 构建

要求：

- ESP-IDF `v5.5.4`
- ESP32-S3，16 MB Flash，OPI PSRAM
- TiRTC SDK `v2.2.1`，commit `3a33bf4ae51b`

```powershell
. C:\esp\v5.5.4\esp-idf\export.ps1
cd <checkout>\sdk-integration-examples\esp32-s3-at-thingconnect
idf.py -B build build
```

工程会把 `media/` 生成到 `storage` SPIFFS 分区。原生 USB
Serial/JTAG CDC 默认作为 AT 控制面，诊断日志走 UART0，避免异步日志
破坏 AT 响应和 URC 的行协议。

OPI PSRAM 是硬件依赖。普通 `malloc/calloc` 以 512 字节为分配阈值让大块
优先进入 PSRAM，并为显式 INTERNAL/DMA 分配保留 96 KiB 内存；大 HTTP
响应和非实时 worker 栈会显式申请 PSRAM。mbedTLS 使用默认 allocator 和
系统 CA bundle，不关闭证书校验。这是内存共存策略，不表示所有 TLS 内存
都固定来自 internal RAM 或 PSRAM。

## 首次使用

1. 连接板载原生 USB Serial/JTAG 口作为 AT 口；需要内部诊断日志时，
   另接 UART0，参数为 `115200 8N1`。
2. 发送 `AT`，应返回 `OK`。
3. 发送 `AT+BUILD?`，记录运行 app 与 TiRTC SDK 身份。
4. 发送 `AT+WIFI="ssid","password"`。
5. 收到 `+WIFI:ONLINE` 后继续等待
   `+BIND:REQUIRED,<generation>,""`，再发送 `AT+BIND=START`；过早发送会
   因状态尚未进入 `NEED_BINDING` 而被拒绝。
6. 将 `+BIND:CODE,<generation>,"xxxxxx"` 中的验证码输入开发者平台
   H5 绑定页。
7. 等待 `+PLATFORM:MQTT,<generation>,0,"ONLINE"` 和
   `+TIRTC:READY,<generation>,0`。
8. 使用 `AT+AI=START` 或 `AT+CALL="target_device_id"`。AI 角色配置
   `call_device` 插件后，语音说出设备联系人备注即可自动转普通音频呼叫。
9. 用 `AT+SESSION?` 恢复完整会话快照，用 `AT+MEDIA?` 检查预录上行与
   下行回调的帧数、字节数和错误计数。

已有设备凭据时，固件先签名请求 `/v1/device/token`。服务端返回 6006 或
正式 MQTT 收到 `unbind` 时，固件会立即关闭新业务门禁，并自动使用保留
凭据执行签名 Report；此时主机只观察绑定 URC，不重复发送
`AT+BIND=START`。

同步 `OK` 只表示请求已受理，最终结果由 URC 给出。任何 AT 查询、URC
或日志都不会输出 Wi-Fi 密码、device_key、MQTT token、AI token 或呼叫
连接 token。

`tools/at_regression.py` 提供可重复的双串口探测、配网和证据采集。Wi-Fi
凭据只从 `TIRTC_TEST_WIFI_SSID`、`TIRTC_TEST_WIFI_PASSWORD` 环境变量
读取，发出的配网命令在证据文件中固定记为脱敏占位符。

两台设备完成绑定并互为普通联系人后，`tools/at_scenarios.py` 提供
`ai`、`ai-call-device`、`call-audio`、`call-video`、`call-reject`、`call-cancel`、
`call-busy`、`race-ai-stop`、`race-call-cancel`、`race-suite` 和整套
`suite`。preflight 先用 `AT+BUILD?` 把运行 app ELF/ESP-IDF/TiRTC SDK
与指定本地构建闭环；随后把同步 `OK`、`+REQUEST`、同请求 ID 的异步操作
阶段、会话快照、媒体代际、TiRTC 清理计数和串口健康分别校验。证据
`summary.json` 使用 schema v4，记录本轮 `run_id` 和原始 JSONL 的
SHA256；每次运行必须使用新的 `--artifact-dir`。写入证据前会脱敏
SSID/IP、设备和房间标识、绑定码、caption、action、AI JSON 和敏感 AT
命令。失败摘要只保留异常类型和不可逆指纹，不写入原始 operation payload
或状态快照。`ai-call-device` 只用 AT 临时设置目标联系人备注、启动 AI、
接听和挂断；测试素材必须预先包含与 `--target-alias` 一致的语音请求，
场景结束会恢复原备注。

`AT+BUILD?` 能证明运行 app 及其 TiRTC SDK 与指定本地 ELF/manifest
一致，不能证明板上的 bootloader、分区表、OTA data 或 `storage` 分区
与本地候选镜像一致。一次 `suite` 是功能 smoke，定向 race 的 20 次循环
也只证明对应竞态窗口；它们都不能替代完整 AI、普通呼叫和真实媒体的发布
验收门禁。

## 文档

- [使用说明](docs/USER_GUIDE_CN.md)
- [AI 呼叫设备体验流程](docs/EXPERIENCE_FLOW_CN.md)
- [架构与边界](docs/ARCHITECTURE_CN.md)
- [上游协议基线](docs/UPSTREAM_CONTRACT_CN.md)
- [AT 指令](docs/AT_COMMANDS_CN.md)
- [验证清单](docs/VALIDATION_CN.md)
- [版本闭环](VERSION.md)
- [来源边界](SOURCE_PROVENANCE.md)
- [测试媒体](media/README.md)
- [TiRTC SDK 交付说明](components/tirtc_sdk/README_CN.md)

## 验证声明

`idf.py build` 只证明源码、配置、SPIFFS 和 TiRTC 静态库链接闭环。烧录、
Wi-Fi、真实平台绑定、AI 云端、多轮 caption、双板呼叫和真实双向媒体必须
分别提供设备日志后才能算通过。仓内历史 artifact 也不能替代针对当前源码、
当前 ELF 和当前运行设备重新采集的证据。
