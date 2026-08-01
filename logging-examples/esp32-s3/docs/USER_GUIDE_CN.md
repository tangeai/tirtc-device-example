# ESP32-S3 AT ThingConnect 日志示例：使用指南

本文面向需要完整协议字段、状态快照和自动化证据的开发者，因此使用
`AT+PROTO=RAW` 结构化调试模式。第一次拿到开发板时，先按[根 README](../README.md)
完成完整 BIN 烧录、配网和一次精简中文体验，再回到本文做深入联调，会更
容易定位每个阶段。完整结构化字段定义见
[AT_COMMANDS_CN.md](AT_COMMANDS_CN.md)。

| 现在要做什么 | 直接看这里 |
| --- | --- |
| 烧录、配网、绑定 | 第 2 至 4 节 |
| 建立联系人并配置 AI | 第 5 至 7 节 |
| 手工完成一次 AI 呼叫设备 | 第 8 节 |
| 普通设备呼叫 | 第 9 节 |
| 自动化和竞态回归 | 第 10 节 |
| 定位失败 | 第 11 节 |

## 1. 使用边界

- 设备侧控制入口是 AT 指令；本文在开始操作前显式切换到 RAW 模式。
- 账号登录和 6 位绑定码确认在开发者平台 H5/浏览器完成。
- 不使用设备端 token 生成器，也不在 AT 中配置长期 AI token 或 `role_id`。
- 示例不依赖麦克风、扬声器、摄像头、按键或屏幕。
- AI 和呼叫的上行媒体来自 SPIFFS 预录文件；下行媒体只统计帧数和字节数。
- `OK` 只表示命令已受理，最终结果必须以 URC 和状态快照为准。

## 2. 环境准备

| 项目 | 要求 |
| --- | --- |
| 芯片 | ESP32-S3 |
| Flash | 16 MB |
| PSRAM | OPI PSRAM |
| ESP-IDF | `v5.5.4` |
| TiRTC SDK | `v2.2.1`，commit `3a33bf4ae51b` |
| 浏览器烧录 | 支持 Web Serial 的桌面版 Chrome 或 Edge |
| AT 终端 | 支持文本串口和 `CR`、`LF` 或 `CRLF` |
| 网络 | 可访问 TiRTC 开发者平台和 TiRTC 服务 |
| 双机体验 | 两块已经连接并可分别访问 AT 口的 ESP32-S3 |

开发板通常会出现两个用途不同的串口：

- `<FLASH_PORT>`：下载固件和 UART0 诊断日志。
- `<AT_PORT>`：板载原生 USB Serial/JTAG CDC，只输出 AT 响应和 URC。

端口号由本机枚举决定，不要把文档里的占位符直接当成端口号。UART 后端
参数为 `115200 8N1`；原生 USB CDC 仍可在串口工具中选择 `115200 8N1`。

## 3. 准备并烧录固件

本文后续流程既适用于 Release 完整 BIN，也适用于当前源码构建结果。两种
来源选一种即可，不要把完整 BIN 与本地构建分片混合烧录。

### 3.1 Release 完整 BIN

从同一 GitHub Release 下载
`esp32s3-tirtc-logging-full-v0.3.0.bin` 和 `SHA256SUMS.txt`，核对哈希后
打开 [Espressif ESP Tool](https://espressif.github.io/esptool-js/)。添加完整
BIN，地址填写 `0x0`，连接下载串口并开始烧录。

完整 BIN 会覆盖 16 MB Flash，包括 `0x9000` 的 NVS。烧录完成后 Wi-Fi、
设备身份和绑定凭据为空，后续按第 4 节重新配置和绑定。

### 3.2 当前源码构建结果

```powershell
. "$env:IDF_PATH\export.ps1"
cd <checkout>\logging-examples\esp32-s3
idf.py -B build --no-ccache reconfigure build
```

构建完成后打开 `build/flasher_args.json`。在
[Espressif ESP Tool](https://espressif.github.io/esptool-js/) 中，把
`flash_files` 列出的每个 BIN 及其地址逐项添加后烧录。必须包含本次构建
生成的 `storage.bin`，这样 `media/` 中的预录输入才会进入 SPIFFS。

双机体验时，A、B 两块板使用同一种固件来源分别烧录。完成后，串口终端
连接对应的 `<AT_PORT_A>` 或 `<AT_PORT_B>`。

源码分片烧录是否保留 NVS 取决于 `flasher_args.json` 的实际写入范围。只有
明确需要重新初始化设备时才执行整片擦除；它会清除 Wi-Fi、设备身份和绑定
凭据。

## 4. 单板首次联网与绑定

每块板分别执行以下流程。

### 4.1 确认固件和当前状态

```text
AT
ATE0
AT+PROTO=RAW
AT+BUILD?
AT+STATUS?
```

期望：

```text
OK
+BUILD:1,...
+STATUS:1,...
```

记录 `AT+BUILD?` 中的 ELF SHA256、ESP-IDF 和 TiRTC SDK 版本。后续测试
必须确认两块板运行的是同一个候选固件。

### 4.2 写入 Wi-Fi

```text
AT+WIFI="<ssid>","<password>"
```

密码只用于写入 NVS，不会被查询命令、URC 或证据文件回显。等待：

```text
+SYSTEM:RESTARTING,<generation>,0,"wifi_config_changed"
```

该事件表示凭据已经写入并进入计划重启流程。等待设备重启，重新打开 AT
串口并发送 `AT` 完成同步，再等待：

```text
+WIFI:ONLINE,...
```

### 4.3 首次绑定

没有设备凭据时，继续等待：

```text
+BIND:REQUIRED,...
```

此后发送：

```text
AT+BIND=START
```

设备会输出：

```text
+BIND:CODE,<generation>,"<six_digit_code>"
```

在已经登录的开发者平台绑定页输入该 6 位码。成功标志为：

```text
+BIND:BOUND,...
+PLATFORM:MQTT,...,0,"ONLINE"
+TIRTC:READY,...,0
```

最后确认：

```text
AT+STATUS?
```

系统状态应为 `READY`。已有有效凭据时，设备会自动取 token 并上线，不应
重复绑定。

## 5. 建立两台设备的联系人关系

以下示例以 A 呼叫 B 为目标。

先分别查询两块板的平台状态和自身设备 ID：

```text
A: AT+PLATFORM?
B: AT+PLATFORM?
```

响应最后一个字段是当前设备的 `device_id`。记录为 `<DEVICE_ID_A>` 和
`<DEVICE_ID_B>`，不要把真实 ID 写入公开日志或文档。

先在 A 查询联系人：

```text
AT+CONTACTS?
```

如果列表里已有 B，并且 `online=1`，可直接进入备注设置。如果没有 B：

```text
AT+CONTACT=REQUEST,"<DEVICE_ID_B>"
```

在 B 查询待处理请求并接受：

```text
AT+PENDING?
AT+CONTACT=RESPOND,"<DEVICE_ID_A>",ACCEPT
```

回到 A 刷新联系人：

```text
AT+CONTACTS?
```

为 B 设置适合语音识别的唯一备注：

```text
AT+CONTACT=REMARK,"<DEVICE_ID_B>","测试设备"
AT+CONTACTS?
```

`REMARK` 只接受最近一次 `AT+CONTACTS?` 已确认的普通设备联系人。体验前
应确认目标满足：

- `type=device`
- `online=1`
- 备注在当前联系人列表中唯一
- 语音中说出的名称与备注一致

当前联系人合同不返回 `device_name`。因此“按设备名称呼叫”在本示例中应
通过联系人备注实现。

## 6. 配置 AI 的呼叫设备插件

在开发者平台中编辑设备端插件，并把插件绑定到 A 当前使用的 AI 角色或
Agent。

推荐配置：

| 字段 | 值 |
| --- | --- |
| 插件名称 | `呼叫设备` |
| Action | `call_device` |
| 输入参数 | `target` |
| 输入类型 | `string` |
| 必填 | 是 |
| 返回参数 | `ok:boolean`、`message:string` |

推荐描述：

```text
当用户明确希望呼叫一个已经添加的设备联系人时调用。
target 使用用户说出的联系人备注、设备名称或设备 ID。
目标不明确时先向用户确认，不要猜测或编造设备 ID。
```

设备收到的正式协议形态为：

```json
{
  "method": "device_action",
  "params": {
    "action": "call_device",
    "data": {
      "target": "测试设备"
    }
  }
}
```

`call_device` 由固件内建处理。看到对应 `+AI:ACTION` 后，不要再发送
`AT+AIACTION`，否则会返回 `action-owned-by-firmware`。

插件收到 `result.ok=true` 表示设备已经接受 AI 到 CALL 的交接请求，不表示
对方已经接听。真正接通必须以 B 执行 `AT+ACCEPT` 后两端都进入
`owner=call,state=in-call` 为准。

## 7. 准备 AI 可识别的预录语音

AT 只负责启动会话，不会把文字伪装成语音。AI 听到的内容来自
`media/media_profile.json` 指定的 G711A 文件。

默认音频规格：

| 项目 | 值 |
| --- | --- |
| 编码 | G711A / A-law |
| 采样率 | 8000 Hz |
| 声道 | 单声道 |
| 分包 | 20 ms |
| 时长 | 10 秒 |
| 包数 | 500 |
| 文件大小 | 80000 字节 |

若默认素材不包含“呼叫测试设备”之类的语句，可准备一个 10 秒 WAV，再用
FFmpeg 转换：

```powershell
ffmpeg -i .\prompt.wav -af "apad=pad_dur=10" -t 10 -ar 8000 -ac 1 `
  -c:a pcm_alaw -f alaw .\media\prompt_call_device.g711a
```

确认输出正好为 80000 字节，然后把
`media/media_profile.json` 的 `audio.file` 临时改为
`prompt_call_device.g711a`，重新执行构建和烧录。

`AT+BUILD?` 只能证明运行 app 和 SDK 与本地构建一致，不能证明板上的
`storage` 已经更新。修改语音后必须重新烧录包含该素材的 SPIFFS 分区；本
文推荐按 `build/flasher_args.json` 在
[Espressif ESP Tool](https://espressif.github.io/esptool-js/) 中重新写入本次
构建的全部分片，至少要确认新的 `storage.bin` 已按对应地址写入。

语音内容应短而明确，例如：

```text
请呼叫测试设备。
```

体验结束后，把 `audio.file` 恢复为原始素材并重新生成 `storage` 镜像。

## 8. 手工体验 AI 呼叫设备

### 8.1 启动前门禁

A、B 分别执行：

```text
AT+STATUS?
AT+SESSION?
AT+MEDIA?
```

开始新会话前必须满足：

- 系统为 `READY`
- `owner=none`
- `state=idle`
- `adapter_state=running`
- `connected=0`
- 最后 6 个异步清理计数均为 0

### 8.2 A 启动 AI

```text
AT+AI=START
```

依次观察：

```text
+REQUEST,...,"AI_START",...
+AI:STATE,...,"ai-connecting",...
+AI:STATE,...,"ai-starting",...
+AI:STATE,...,"ai-active",...
```

AI active 后，预录 G711A 会自动按 20 ms 音频帧上行。云端 ASR 识别语音，
AI 决定调用插件，然后 A 应看到：

```text
+AI:ACTION,...,"call_device",...
+AI:OP,...,"ai-call-device","contacts-refresh-submitted",""
+AI:OP,...,"ai-call-device","response-submitted",""
+AI:OP,...,"ai-call-device","response-drained",...
+AI:STATE,...,"ending",...,"ai-call-device-transfer"
+AI:STATE,...,"idle",...,"ai-call-device-transfer"
+AI:OP,...,"ai-call-device","adapter-drained",""
+CALL:OP,...,"call-start","accepted",...
```

### 8.3 B 接听

B 应收到：

```text
+CALL:INCOMING,...,"audio"
```

发送：

```text
AT+ACCEPT
```

两端最终应进入：

```text
+CALL:STATE,...,"in-call",...
```

### 8.4 查看媒体

A、B 分别多次查询：

```text
AT+MEDIA?
```

预录双向音频的成功判据：

- `active_profile=call`
- `tx_audio_frames` 持续增长
- `rx_audio_frames` 持续增长
- `send_errors=0`

没有扬声器 sink 是本日志示例的设计边界。下行音频被统计后丢弃，因此
“听不到声音”不代表网络媒体没有建立。

### 8.5 挂断并恢复空闲

任一通话中设备发送：

```text
AT+HANGUP
```

两端都应回到：

```text
+CALL:STATE,...,"idle",...
```

最后再次查询：

```text
AT+STATUS?
AT+SESSION?
AT+MEDIA?
```

确认系统仍为 `READY`，会话为 `none/idle`，异步清理计数全部归零。

## 9. 普通 AT 呼叫

不经过 AI 时，A 可以直接使用设备 ID：

```text
AT+CALL="<DEVICE_ID_B>"
```

B 使用：

```text
AT+ACCEPT
```

主叫尚未接通时可取消：

```text
AT+CANCEL
```

被叫响铃时可拒绝：

```text
AT+REJECT
```

通话建立后使用：

```text
AT+HANGUP
```

视频控制链路可用：

```text
AT+CALL="<DEVICE_ID_B>",VIDEO
```

视频下行只统计 H264 帧，不在本例中显示。

## 10. 自动化回归

脚本需要 Python 3 和 `pyserial`。可直接使用 ESP-IDF 的 Python 环境，或在
独立环境中安装：

```powershell
python -m pip install pyserial
```

每次运行使用新的证据目录。

### 10.1 双机预检

```powershell
$artifact = ".\artifacts\preflight-$(Get-Date -Format yyyyMMdd-HHmmss)"
python .\tools\at_scenarios.py preflight `
  --port-a <AT_PORT_A> --port-b <AT_PORT_B> `
  --build-dir .\build `
  --artifact-dir $artifact
```

### 10.2 真实 AI 呼叫设备

目标备注必须与预录语音一致：

```powershell
$artifact = ".\artifacts\ai-call-device-$(Get-Date -Format yyyyMMdd-HHmmss)"
python .\tools\at_scenarios.py ai-call-device `
  --port-a <AT_PORT_A> --port-b <AT_PORT_B> `
  --build-dir .\build `
  --target-alias "测试设备" `
  --artifact-dir $artifact
```

该场景只使用 AT 完成临时备注、AI 启动、B 接听、媒体检查、A 挂断和备注
恢复。它不会手工伪造 `call_device` action。

### 10.3 完整 smoke

```powershell
$artifact = ".\artifacts\suite-$(Get-Date -Format yyyyMMdd-HHmmss)"
python .\tools\at_scenarios.py suite `
  --port-a <AT_PORT_A> --port-b <AT_PORT_B> `
  --build-dir .\build `
  --artifact-dir $artifact
```

### 10.4 定向竞态

```powershell
$artifact = ".\artifacts\race-ai-stop-$(Get-Date -Format yyyyMMdd-HHmmss)"
python .\tools\at_scenarios.py race-ai-stop `
  --port-a <AT_PORT_A> --port-b <AT_PORT_B> `
  --build-dir .\build --iterations 20 `
  --artifact-dir $artifact

$artifact = ".\artifacts\race-call-cancel-$(Get-Date -Format yyyyMMdd-HHmmss)"
python .\tools\at_scenarios.py race-call-cancel `
  --port-a <AT_PORT_A> --port-b <AT_PORT_B> `
  --build-dir .\build --iterations 20 `
  --artifact-dir $artifact
```

有效结果必须同时满足：

- 命令退出码为 0
- `summary.json` 中 `status` 为 `passed`
- `iteration_progress.passed` 等于 `requested`
- `unexpected_restarts=0`
- `dropped_events=0`
- `framing_errors=0`
- 两端最终为 `READY` 和 `none/idle`

## 11. 常见问题

| 现象 | 判断与处理 |
| --- | --- |
| 浏览器没有列出开发板端口 | 关闭占用串口的终端，换一根确认支持数据的 USB 线；没有自动下载电路时，按住 BOOT、点按 RESET 后重连 |
| 烧录完成后 AT 无响应 | 确认打开的是原生 USB Serial/JTAG CDC，参数为 `115200 8N1`，行尾包含 CR 或 LF；UART0 只输出诊断日志 |
| 使用完整 BIN 后原绑定消失 | 完整 BIN 覆盖 16 MB Flash 并重置 NVS；重新配网和绑定即可 |
| 配网后串口断开 | 设备会计划重启并重新枚举 USB；关闭旧端口，等待新端口出现后重新发送 `AT` |
| `AT+BIND=START` 被拒绝 | 等待明确的 `+BIND:REQUIRED` 后再发送 |
| 一直没有 `+TIRTC:READY` | 依次查询 `AT+STATUS?`、`AT+PLATFORM?` 和 `AT+SESSION?`，先确认 Wi-Fi，再确认绑定和平台在线 |
| `AI=START` 被拒绝 | 用 `AT+STATUS?` 和 `AT+SESSION?` 检查系统是否 READY、是否已有 CALL 或交接 |
| AI active 但没有 action | 检查预录文件是否包含目标语句、`AT+MEDIA?` 的 AI 上行计数是否增长、插件是否绑定到当前 AI 角色 |
| 目标 unknown | 先 `AT+CONTACTS?`，确认目标备注或 ID 存在 |
| 目标 ambiguous | 给联系人设置唯一备注，不使用会命中多个联系人的子串 |
| 目标 offline | 让目标设备上线，并确认联系人 `online=1` |
| 收到 `action-owned-by-firmware` | `call_device` 不需要主机发送 `AT+AIACTION` |
| `response-drain-terminal-failed` | 固件会结束 AI 且不拨号；等待 idle 后重新检查网络和 TiRTC |
| 收到 `+AT:URC_OVERFLOW` | 当前事务证据不完整，立即查询 `STATUS?/SESSION?/MEDIA?` 恢复真值 |
| 收到 `+SYSTEM:RESTARTING` | 等待重新 READY，再重新核对 BUILD、STATUS、SESSION、MEDIA 和平台联系人 |
| 下行没有声音 | 本例没有扬声器 sink；以 `rx_audio_frames/bytes` 增长作为媒体证据 |
| 媒体计数始终为 0 | 确认烧录时包含了与应用同一次构建生成的 `storage.bin`；再检查当前会话是否已经接通 |

## 12. 安全约束

- 不在脚本参数、日志、截图或文档中记录真实 Wi-Fi 密码和设备密钥。
- 不把同步 `OK` 当成业务成功。
- 不跳过 `response-drained` 就宣称 AI 已可靠交接到 CALL。
- 不猜测联系人 ID；未知、歧义或离线目标必须失败关闭。
- 不复用旧 artifact 证明当前固件。
- 不把本示例的预录媒体计数等同于真实麦克风、扬声器或显示体验。
