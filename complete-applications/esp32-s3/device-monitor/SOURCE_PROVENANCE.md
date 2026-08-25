# 来源与验证边界

## 来源快照

| 项目 | 值 |
| --- | --- |
| 源工程 | `esp32s3_tirtc_device_monitor_demo` |
| 源分支 | `main` |
| 源标签 | `v1.9.5`，annotated Tag |
| 源 Tag object | `318f99c88fa1f8a8a767b0686cbb8f0182b713f4` |
| 源提交 | `45db394cae399967a9c3b882d595cdecb80321be` |
| 源 tree | `4a6760708d4fd2bdb973c4bc77d789d45f2bc2be` |
| 比较基线 | `v1.9.0` / `a64422b0efdebe6c303370effafd52bbf51593d1` |
| 公开代码提交 | `c27914eafab6f700cecf196da48987200cd54d37` |
| 公开 repository tree | `b0b93ddc0c82ae65130ae6dab17ff6ee6dbfc86a` |
| 公开项目 tree | `9fa7f85ce92a0e5fcd27c4a36fabe02609191391` |
| 公开项目 Tag | `esp32-s3-device-monitor-v1.9.5` |
| 应用版本 | `1.9.5` |
| 发布日期 | `2026-08-25` |
| ESP-IDF | `5.5.4` |
| TiRTC SDK | `2.3.0 mini` |
| TiRTC SDK BuildInfo | `v2.3.0-db7290f` |
| SDK active-connect 修复 | `db7290f2404b15f2ab8567dd221853ca2ec054ab` |
| SDK HTTPS 认证修复 | `13e34c3e3e3dc6776be4713b5c1e3c17bd282766` |
| TiRTC 静态库 | `2,125,366` bytes |
| TiRTC 静态库 SHA-256 | `83556eeee0c6cae45961899a4c5d1255a5d0d33f8e636104a946ce41ff3e20d7` |

本目录按 `v1.9.5` 指向的不可移动源码快照整理。设备使用
`https://ep-open.tangeopen.com/services` 做服务发现，只接受 `HTTPS` 和 `MQTTS` 服务地址；
发现结果不满足安全协议时会被拒绝并报错，不覆盖编译期安全兜底。设备主动呼叫微信联系人继续使用
体验版 `wx_version_type=2`。

TiRTC SDK 以 `2.3.0 mini` 为基础，BuildInfo 保留 `v2.3.0-db7290f`。SDK 自有 HTTP 传输另含
`13e34c3` 的 HTTPS 服务端认证修复：强制 `MBEDTLS_SSL_VERIFY_REQUIRED`，挂接 ESP-IDF
证书包并校验证书链和 hostname。公开归档随后执行 `--strip-debug`，只移除 SDK 内部调试信息；
功能代码、只读数据、成员顺序和全局符号契约保持一致。

来源源码经过显式清单筛选后形成公开代码提交。筛选使用的 `532` 个源文件对象清单按路径排序，
SHA-256 为 `4f62119b14935198128abb809142beb3bf91367339ae9f052acecb1d93499b00`，用于证明导入范围可
复核；它不是固件哈希，也不代替正式构建。

## 1.9.5 变更范围

源工程相对 `v1.9.0` 共变更 `37` 个文件，新增 `1588` 行、删除 `337` 行：

- 网络质量测试从 4 次基础 Ping 调整为 20 次探测，并把平均 RTT、相邻 RTT 绝对变化的
  平均值和丢包率作为结构化字段送到应用快照、UI 和串口诊断。
- 普通设备来电增加本地合成铃声。接听、拒接、取消、挂断和身份重置都会按当前状态停止；
  铃声任务栈与 PCM 缓冲使用 PSRAM。
- 设备呼叫接听和挂断状态继续收口，新引入路径中的内存不足保留真实失败语义。
- 设备互呼新增有界自适应播放速率：缓冲水位偏离目标时约以 `1.25%` 慢放或快放，策略只
  作用于 Device Call profile。
- 设备互呼 AEC 改为全双工高性能线性模式，工作区使用 PSRAM；AEC 后增加 100 Hz 高通，
  通话 AGC 使用更保守的静态噪声底线。
- Web IPC、设备互呼和微信 VoIP 的设备上行统一核对为
  `8 kHz / 16 bit / mono / G.711 A-law / 20 ms / 160 bytes`，并补齐显式 CALL gate 的
  A-law 编码路径。
- G.711 A-law 连续流编码保留跨包 FIR 状态，并在连接代际变化时重置，TiRTC 与微信 VoIP
  共用这一行为。
- 服务发现增加有限退避；首次绑定码只在临时 MQTT 已订阅后展示，刷新与 NVS pending
  session 清理按 generation 收口，联系人和来电拒绝路径也补齐失败语义。
- 默认业务入口切换到 HTTPS/MQTTS；服务注册表、通用 HTTP、临时绑定 MQTT、正式设备 MQTT
  和 TiRTC 配置入口分别执行 fail-closed 检查，禁止敏感业务降级到明文协议。
- TiRTC SDK 的自有 HTTPS 客户端在责任层补齐证书链和主机名校验；公开静态库清除了个人构建
  路径和行号级调试信息。Flash、分区表和 ESP-IDF 保持不变，应用版本提升到 `1.9.5`。

## 公开筛选

统一仓保留应用源码、构建所需组件、`sdkconfig.defaults`、分区表、公开开发者文档和必要辅助
脚本。以下内容不会进入公开源码快照：

- 来源仓 Git 元数据、本地 `AGENTS.md`、本地 Skill、IDE 配置和开发机完整 `sdkconfig`。
- `build/`、`build-current/`、`managed_components/`、日志、NVS 转储、ELF、MAP 和固件镜像。
- 真实 Wi-Fi、设备身份、密钥、Token、串口记录和个人绝对路径。
- 内部交接、一次性审阅材料、旧版本构建目录和与本版本无关的调试资料。

来源仓中与该 Tag 无关的内部 Skill、本地架构草稿和开发文档未进入公开代码提交。SDK 归档
卫生处理是 `v1.9.5` 的一部分：公开库 SHA-256 为
`83556eeee0c6cae45961899a4c5d1255a5d0d33f8e636104a946ce41ff3e20d7`，BuildInfo 仍为
`v2.3.0-db7290f`。`strip-debug` 的代价是公开库不再提供 SDK 内部源码行号级调试信息，
不影响对外 API、链接符号或运行代码。

## 唯一正式构建

来源侧构建用于开发自检，不复用为公开资产。正式固件从隔离候选的全新检出构建一次；该候选
与公开代码提交 `c27914eafab6f700cecf196da48987200cd54d37` 的 repository tree
`b0b93ddc0c82ae65130ae6dab17ff6ee6dbfc86a`、项目 tree
`9fa7f85ce92a0e5fcd27c4a36fabe02609191391` 完全一致，因此构建输入可绑定到这份公开源码：

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py -B <fresh-build-dir> --no-ccache `
  -D SDKCONFIG=<fresh-build-dir>/sdkconfig `
  -D SDKCONFIG_DEFAULTS=<project-dir>/sdkconfig.defaults `
  reconfigure build
```

构建目录和生成的 `sdkconfig` 都在公开源码目录之外或 Git 忽略范围内。不得读取来源工程的
`sdkconfig`，不得复用 `1.9.0` 的 `build/`、OTA app、完整镜像或哈希。本次正式构建记录：

| 证据 | 正式值 |
| --- | --- |
| 构建步骤 | `1767/1767` |
| app 大小 | `7,608,608` bytes |
| app SHA-256 | `51a7599942f06556e33ef4820499885d6213ff15fce0f3ed2f11e38e44146503` |
| app 分区 | `7798784` bytes |
| app 分区剩余 | `190,176` bytes（`2.44%`） |
| 16 MB 完整镜像大小 | `16777216` bytes |
| 16 MB 完整镜像 SHA-256 | `fae989a721e076eed7ba8d2d31cdec0c040acf133124199989f381e7854a162e` |
| 正式构建输入源码归档 SHA-256 | `dbca97612cc519dc86caa999ee17a2ac689ef90a4a256fb4eea7efa42a3dee9c` |

表中数据来自同一次 `1767/1767` 正式干净构建。app 分区余量为 `190,176` bytes（`2.44%`），
这是明确容量风险；本次没有复用 `1.9.0` 的数值，也没有把开发侧 app 哈希写成公开资产哈希。

## Release 资产绑定

正式构建只生成一套发布输出：

1. `esp32s3-tirtc-device-monitor-ota-v1.9.5.bin`：正式构建产生的 app 镜像原件。
2. `esp32s3-tirtc-device-monitor-full-v1.9.5.bin`：严格按同一次
   `flasher_args.json` 的实际地址合并，文件长度固定覆盖 16 MB Flash。
3. `release-manifest.json`：记录公开 Tag/commit、开发来源、项目路径、逐文件源码清单与
   SHA-256、SDK 身份、构建环境、完整命令、分区和两个固件附件的大小与 SHA-256。
4. `SHA256SUMS.txt`：至少列出 full、OTA app 和 manifest 三个文件的 SHA-256。

完整镜像生成后必须逐段与 bootloader、partition table、OTA data 和 app 原件比对；所有未
使用区域必须保持 `0xFF`。同时核对镜像描述信息中的版本为 `1.9.5`。只有这些检查通过后，
才能把 full、OTA app、manifest 和 SHA256SUMS 上传到同一个项目 Release；构建二进制不进入
普通 Git 历史。

## 运行证据边界

相同功能代码在版本号收口前完成过双板设备互呼开发验证：约 14 秒、每端约 690 个媒体包，
`media=2`、`flags=0`、`160 bytes / 20 ms`，发送失败、丢弃和下溢计数为 0。G.711 A-law
完成 20,000 次编解码自检，结果为 `checksum=415d3715`、`heap_ok=1`。这些证据支持线格式和
Codec 路径核对，但 `1.9.5` 版本号收口后的正式固件没有重新烧录，因此不能写成最终 Release
资产的目标板证明。

HTTPS 服务端认证已核对到 SDK 源码、`httpclt.o`、静态归档和 APP 链接：auth mode 为
`MBEDTLS_SSL_VERIFY_REQUIRED`，证书包、hostname 和 verify result 依赖均保留。当前没有目标板
上的“有效证书成功 + 错误证书或错误主机名失败”双向握手证据，也没有对绑定、MQTT、小钛、
设备呼叫、微信 VoIP 和 Web IPC 做本版本真机回归，因此文档不把静态检查写成运行时证明。

Web IPC 和设备互呼的当前人耳试听仍可感知轻微“沙沙电流声”。线上格式和 Codec 自检没有
发现异常，说明排查范围已经缩小；它们不能证明底噪来源，根因仍未闭环。本版本明确保留这个
已知现象，不宣称已经修复。

源码身份、构建、附件、烧录、联网和业务运行分别留证。静态文件哈希可以证明公开内容彼此
一致；正式构建和镜像分段比对可以把附件绑定到公开代码快照。这些证据仍不替代目标板烧录、
Wi-Fi、服务发现、ThingConnect、Web IPC、小钛、微信 VoIP、设备互呼、OTA、弱网和长稳验证。
