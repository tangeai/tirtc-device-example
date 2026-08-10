# TiRTC ESP32-S3 AT ThingConnect 日志示例

这个示例把 TiRTC 的联网、平台绑定、AI 对讲和设备呼叫整理成一组可直接
操作的串口 AT 指令。你不需要先接摄像头、麦克风、扬声器或屏幕，就能看清
每一步发生了什么，也能通过字幕和媒体计数判断链路是否真的在工作。

| 项目 | 版本或规格 |
| --- | --- |
| 日志示例 | `0.3.0` |
| TiRTC SDK | `2.2.1` |
| ESP-IDF | `5.5.4` |
| 目标芯片 | ESP32-S3，16 MB Flash，OPI PSRAM |
| 公开 Release | [`esp32-s3-logging-v0.3.0`](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-s3-logging-v0.3.0) |

## 1. 先选一条使用路径

| 你的目标 | 建议入口 |
| --- | --- |
| 尽快体验功能 | 从 [`esp32-s3-logging-v0.3.0` Release](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-s3-logging-v0.3.0) 下载完整 BIN，按本文第 3 节烧录 |
| 修改代码、AT 指令或板级配置 | 获取源码，按本文第 4 节构建并烧录 |
| 接入自动化测试或解析完整字段 | 先完成基本体验，再读 [结构化 AT 指令](docs/AT_COMMANDS_CN.md) 和 [验证清单](docs/VALIDATION_CN.md) |

当前可体验的内容：

- Wi-Fi 配网和开发者平台绑定
- AI 对讲、讲故事、讲笑话和天气查询
- 用户与 AI 的实时中文字幕
- AI 按“小张”“小李”等联系人备注发起设备呼叫
- 来电接听、拒接、主叫取消和通话挂断
- 音视频链路及媒体收发状态

固件内置了预录 G711A 音频和 H264 视频。它们用于驱动协议和媒体链路，
因此这个示例适合先验证“能否接入、能否呼叫、是否有媒体数据”，不用于
评价真实摄像头、麦克风、扬声器或画面质量。

## 2. 开始前准备

### 硬件

- 一块 ESP32-S3 开发板或自研最小系统
- 16 MB Flash 和 OPI PSRAM
- 一根支持数据传输的 USB 线
- 可用的 2.4 GHz Wi-Fi 网络
- 双机呼叫时准备两块满足相同条件的板卡

本版本使用 GPIO19、GPIO20 原生 USB Serial/JTAG 作为 AT 控制口。UART0
可输出精简中文诊断日志。板卡的 Flash、PSRAM 或 USB 接法不同时，请先
从源码调整板级配置，再为自己的硬件重新构建。

### 软件和账号

- 体验完整 BIN：支持 Web Serial 的桌面版 Chrome 或 Edge
- 源码构建：ESP-IDF `v5.5.4`
- 串口终端：支持 UTF-8 和 `115200 8N1`
- 一个可登录 TiRTC 开发者平台的账号

开发板可能枚举出两个串口：

| 端口 | 用途 |
| --- | --- |
| 原生 USB Serial/JTAG CDC | AT 命令、响应和异步消息 |
| UART0 对应的 USB 转串口 | 精简诊断日志；也可按板卡下载电路进入 Bootloader |

端口号由操作系统分配。无法确认时，拔下开发板，记下消失的端口，再重新
插入逐一确认。AT 控制口上电后发送 `AT` 应返回 `OK`。

## 3. 使用 Release 完整 BIN

### 3.1 下载并核对文件

在当前版本的 GitHub Release 中下载：

```text
esp32s3-tirtc-logging-full-v0.3.0.bin
SHA256SUMS.txt
```

Windows PowerShell 可这样计算文件哈希：

```powershell
Get-FileHash .\esp32s3-tirtc-logging-full-v0.3.0.bin -Algorithm SHA256
```

结果应与同一 Release 中 `SHA256SUMS.txt` 的对应记录完全一致。文件名、
版本或哈希有任何一项对不上，先停止烧录并重新确认下载来源。

### 3.2 在浏览器中烧录

1. 打开 [Espressif ESP Tool](https://espressif.github.io/esptool-js/)。
2. 连接开发板的下载串口。浏览器弹出端口列表时，只选择当前开发板端口。
3. 添加 `esp32s3-tirtc-logging-full-v0.3.0.bin`，地址填写 `0x0`。
4. 开始烧录，等待页面明确报告完成后再复位开发板。
5. 重新选择原生 USB Serial/JTAG CDC，在串口终端中发送 `AT`。

没有自动下载电路时，按住 BOOT 或保持 GPIO0 为低电平，再按一下 RESET
进入下载模式；烧录结束后松开 BOOT 并重新复位。不同板卡的按键名称可能
不同，以板卡原理图为准。

这个完整 BIN 是 16 MB 整片镜像，会写入 Bootloader、分区表、OTA data、
应用和预录媒体，也会把位于 `0x9000` 的 NVS 恢复为空白状态。烧录后需要
重新配置 Wi-Fi，并重新完成平台绑定。

## 4. 从源码构建

### 4.1 准备 ESP-IDF

使用 ESP-IDF `v5.5.4`，在已经加载 ESP-IDF 环境的 PowerShell 中执行：

```powershell
. "$env:IDF_PATH\export.ps1"
cd <checkout>\logging-examples\esp32-s3
idf.py -B build --no-ccache reconfigure build
```

这一条命令只使用 `build/` 作为当前源码快照的构建目录。不要把其他分支、
其他 checkout 或旧版本留下的 `build/` 当作当前源码的输出。

工程会把 `media/` 中的预录输入生成到 `storage` SPIFFS 分区。构建完成后，
先确认以下文件来自同一个 `build/`：

- `flasher_args.json`
- Bootloader、分区表和 OTA data BIN
- 应用 BIN
- `storage.bin`

### 4.2 烧录源码构建输出

仍使用 [Espressif ESP Tool](https://espressif.github.io/esptool-js/)。打开
`build/flasher_args.json`，把其中 `flash_files` 列出的每个 BIN 及其地址
逐项添加到网页工具，再开始烧录。地址以这次构建生成的文件为准，不照抄
其他版本，也不要把 Release 完整 BIN 与本机构建分片混用。

普通更新可以只写 `flasher_args.json` 中的构建分片；是否保留 NVS 取决于
实际写入范围。只有明确需要清除 Wi-Fi、设备身份和绑定凭据时，才执行整片
擦除。

源码中不保存 Wi-Fi、设备密钥或长期 token。Wi-Fi 在固件运行后通过 AT
写入 NVS，平台设备身份通过六位绑定码取得。

## 5. 打开 AT 控制口

串口参数使用 `115200 8N1` 和 UTF-8，命令末尾发送 CR、LF 或 CRLF。
设备启动后默认进入精简中文模式：

```text
AT
AT+TIRTC=?
AT+TIRTC?
```

正常情况下会看到 `OK`、中文帮助和当前状态。主命令 `AT+TIRTC` 以及
`OK`、`ERROR` 保持 ASCII，业务提示、字幕和状态使用中文。

需要自动化解析时再切换到结构化模式：

```text
AT+PROTO=RAW
AT+BUILD?
AT+STATUS?
AT+PROTO=USER
```

精简中文命令和结构化命令是两套面向不同使用者的表面层。第一次体验建议
一直使用 `AT+TIRTC`，这样最容易判断当前该做什么。

## 6. 第一次联网和绑定

### 6.1 配置 Wi-Fi

```text
AT+TIRTC=配网,"<Wi-Fi名称>","<Wi-Fi密码>"
```

命令受理后设备会保存网络配置并自动重启。串口可能重新枚举，请重新打开
AT 控制口，再发送：

```text
AT+TIRTC?
```

### 6.2 获取六位绑定码

Wi-Fi 已联网且设备尚未绑定时，发送：

```text
AT+TIRTC=绑定
```

设备会返回六位绑定码。在已登录的开发者平台绑定页面输入该码。绑定成功后
设备会保存平台身份并自动进入在线流程，不需要把 device key 或 token 写进
AT 命令。

### 6.3 确认已经就绪

再次发送：

```text
AT+TIRTC?
```

看到下面的状态后即可开始业务体验：

```text
+TIRTC:状态,"已就绪","空闲"
```

`已就绪` 表示网络、平台和 TiRTC 启动门禁已经通过；`空闲` 表示当前没有
AI 或普通呼叫会话。

## 7. 体验 AI 对讲

```text
AT+TIRTC=故事
AT+TIRTC=笑话
AT+TIRTC=天气
```

同步返回的 `OK` 只表示命令已经受理。继续等待异步字幕和完成消息：

```text
+TIRTC:字幕,"用户","..."
+TIRTC:字幕,"AI","..."
+TIRTC:AI,"本轮完成"
```

`本轮完成` 表示这一次提问完成，AI 会话仍可继续。需要完全结束 AI 对讲时
发送：

```text
AT+TIRTC=结束AI
```

查看媒体计数：

```text
AT+TIRTC=媒体
```

预录输入正常工作时，上行计数会增长；收到下行媒体时，对应的下行计数也会
增长。这个示例没有扬声器和屏幕，计数增长就是可观察的媒体接收结果。

## 8. 体验双机设备呼叫

两块板都完成联网、绑定并显示“已就绪”后，分别查询设备 ID。下面用最短
路径完成一次 A 呼叫 B：

```text
A: AT+TIRTC=设备
B: AT+TIRTC=设备
A: AT+TIRTC=加好友,"<设备B的设备ID>"
B: AT+TIRTC=待处理
B: AT+TIRTC=同意好友,"<设备A的设备ID>"
A: AT+TIRTC=备注,"<设备B的设备ID>","小李"
A: AT+TIRTC=呼叫,"小李"
B: AT+TIRTC=接听
A: AT+TIRTC=挂断
```

来电方可拒接，主叫方可在接通前取消。双向呼叫、视频参数、按设备 ID 直呼
和完整联系人管理命令见[使用指南](docs/USER_GUIDE_CN.md)；全部精简命令可
随时用 `AT+TIRTC=?` 查看。

## 9. 常见问题

| 现象 | 先这样检查 |
| --- | --- |
| 浏览器找不到串口 | 换一根确认支持数据的 USB 线；关闭占用串口的终端；必要时按住 BOOT、点按 RESET 后重连 |
| 烧录后 AT 没有回应 | 确认连接的是原生 USB Serial/JTAG CDC，串口为 `115200 8N1`，发送行尾包含 CR 或 LF |
| 串口只有内部日志 | 当前打开的是 UART0；切换到原生 USB CDC 发送 AT |
| 配网后端口消失 | 设备正在重启并重新枚举；关闭旧端口，等待几秒后重新选择 |
| 一直没有“已就绪” | 先用 `AT+TIRTC?` 区分 Wi-Fi、绑定、平台和 TiRTC 所在阶段，再检查网络和绑定状态 |
| `OK` 后没有最终结果 | `OK` 只表示受理；继续等待异步中文消息，或发送 `AT+TIRTC?` 恢复当前状态 |
| AI 有字幕但听不到声音 | 本例没有扬声器；用 `AT+TIRTC=媒体` 查看下行音频帧和字节是否增长 |
| 联系人呼叫提示未知或歧义 | 先刷新联系人，并给目标设置唯一备注；不要让多个联系人使用相同或互相包含的备注 |
| 重新烧录后需要再次配网 | Release 完整 BIN 会覆盖 NVS，这是预期行为；重新执行第 6 节即可 |

更深入的状态恢复、自动化和故障判据见[使用指南](docs/USER_GUIDE_CN.md)。

## 10. 文档导航

| 文档 | 什么时候看 |
| --- | --- |
| [使用指南](docs/USER_GUIDE_CN.md) | 需要 RAW 字段、双机完整联调或自动化命令时 |
| [AT 指令](docs/AT_COMMANDS_CN.md) | 编写串口主机、脚本或解析器时 |
| [AI 呼叫设备体验流程](docs/EXPERIENCE_FLOW_CN.md) | 需要逐阶段核对 AI 到设备呼叫交接时 |
| [架构与边界](docs/ARCHITECTURE_CN.md) | 修改组件、状态机、内存或媒体路径前 |
| [上游协议基线](docs/UPSTREAM_CONTRACT_CN.md) | 对接 ThingConnect HTTP/MQTT 或 AI 协议时 |
| [验证清单](docs/VALIDATION_CN.md) | 准备板测、自动化回归或发布证据时 |
| [版本信息](VERSION.md) | 核对示例、SDK 和证据 schema 版本时 |
| [源码来源](SOURCE_PROVENANCE.md) | 核对公开快照和依赖身份时 |
| [示例测试输入](media/README.md) | 替换或检查预录媒体时 |
| [TiRTC SDK 说明](components/tirtc_sdk/README_CN.md) | 更新 SDK 或核对静态库时 |
