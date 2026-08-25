# 来源与验证边界

## 来源快照

| 项目 | 值 |
| --- | --- |
| 源工程 | `esp32s3_tirtc_device_monitor_demo` |
| 源分支 | `main` |
| 源标签 | `v1.9.6`，annotated Tag |
| 源 Tag object | `c251806b8904672bac07ea16cca2f3099e2426e4` |
| 源提交 | `cdb5d7bec9c955227a1259281686e30377332fb3` |
| 源 tree | `2b032dc07ada0a2e15cfb82e320ac69b53272374` |
| 比较基线 | `v1.9.5` / `45db394cae399967a9c3b882d595cdecb80321be` |
| 公开代码提交 | `ccae5fd495109244a679c5ffe1e17f98d6589f52` |
| 公开 repository tree | `a0fde688fb4dbd2b7803fde487046f4de6cccd0d` |
| 公开项目 tree | `d5d5a8fb3f818106b77eeccc2599482943260be9` |
| 公开项目 Tag | `esp32-s3-device-monitor-v1.9.6` |
| 应用版本 | `1.9.6` |
| 发布日期 | `2026-08-26` |
| ESP-IDF | `5.5.4` |
| TiRTC SDK | `2.3.0 mini` |
| TiRTC SDK BuildInfo | `v2.3.0-db7290f` |
| SDK active-connect 修复 | `db7290f2404b15f2ab8567dd221853ca2ec054ab` |
| SDK HTTPS 认证修复 | `13e34c3e3e3dc6776be4713b5c1e3c17bd282766` |
| TiRTC 静态库 | `2,125,366` bytes |
| TiRTC 静态库 SHA-256 | `83556eeee0c6cae45961899a4c5d1255a5d0d33f8e636104a946ce41ff3e20d7` |

本目录按 `v1.9.6` 指向的不可移动源码快照整理。设备使用
`https://ep-open.tangeopen.com/services` 做服务发现，只接受 `HTTPS` 和 `MQTTS` 服务地址；
发现结果不满足安全协议时会被拒绝并报错，不覆盖编译期安全兜底。设备主动呼叫微信联系人继续使用
体验版 `wx_version_type=2`。

TiRTC SDK 以 `2.3.0 mini` 为基础，BuildInfo 保留 `v2.3.0-db7290f`。SDK 自有 HTTP 传输另含
`13e34c3` 的 HTTPS 服务端认证修复：强制 `MBEDTLS_SSL_VERIFY_REQUIRED`，挂接 ESP-IDF
证书包并校验证书链和 hostname。公开归档随后执行 `--strip-debug`，只移除 SDK 内部调试信息；
功能代码、只读数据、成员顺序和全局符号契约保持一致。

来源源码经过显式清单筛选后形成公开代码提交。本次正式构建输入从公开代码提交的项目 tree
直接导出，共 `549` 个文件；归档 SHA-256 为
`1f2980e339c10555298e95355cd8a70c49d248bee6d27bd2d973d8e042e01992`。逐文件清单继续由
`release-manifest.json` 记录；源码归档哈希不是固件哈希，也不代替正式构建。

## 1.9.6 变更范围

源工程相对 `v1.9.5` 共变更 `17` 个文件，新增 `765` 行、删除 `508` 行：

- 设备呼叫把音频资源准备与 RTC 就绪等待拆开。正式上线最长等待 `30` 秒，RTC 就绪最长等待
  `50` 秒，准备动作每 `500 ms` 推进一次；呼叫响铃上限保持 `45` 秒。
- 发起呼叫前，呼叫请求最多等待 `12` 秒，让已经运行的房间恢复、联系人刷新或联系人变更任务
  结束；超时仍忙则明确拒绝本次呼叫。呼叫活跃后不再启动新的房间恢复或联系人刷新，结束后恢复，
  减少 HTTPS 与 RTC 同时争用网络和内存。
- 应用快照和呼叫页补齐主叫/被叫角色、对端设备、`last_error` 和状态消息，展示准备、等待
  接听、建立连接、通话中，以及取消、拒接、挂断、超时、断开和失败终态。
- 设备联系人和微信联系人名称改为输入框与屏幕键盘编辑，确认键使用 `OK`，不再依赖固定
  备注列表。
- 设备查看、设备呼叫、微信 VoIP 和小钛设置页的音量 `+/-` 在按下时立即生效，触摸区域扩大；
  静音仍需完整点击确认。
- 删除 TiRTC 测试页、手动启动 RTC 和 sender audio test 入口；RTC 继续由应用生命周期维护。
- H5 入口保持 `https://demo-open.tange-ai.com/`。HTTPS/MQTTS、SDK、Flash、分区表和 ESP-IDF
  保持不变，应用版本提升到 `1.9.6`。

## 公开筛选

统一仓保留应用源码、构建所需组件、`sdkconfig.defaults`、分区表、公开开发者文档和必要辅助
脚本。以下内容不会进入公开源码快照：

- 来源仓 Git 元数据、本地 `AGENTS.md`、本地 Skill、IDE 配置和开发机完整 `sdkconfig`。
- `build/`、`build-current/`、`managed_components/`、日志、NVS 转储、ELF、MAP 和固件镜像。
- 真实 Wi-Fi、设备身份、密钥、Token、串口记录和个人绝对路径。
- 内部交接、一次性审阅材料、旧版本构建目录和与本版本无关的调试资料。

来源仓中与该 Tag 无关的内部 Skill、本地架构草稿和开发文档未进入公开代码提交。来源 Tag
内的 README、三份体验文档和本地打包脚本也没有直接覆盖公开文件；公开文档在 `1.9.5`
基线上按本仓风格增量更新。SDK 归档卫生沿用 `v1.9.5` 的结果：公开库 SHA-256 为
`83556eeee0c6cae45961899a4c5d1255a5d0d33f8e636104a946ce41ff3e20d7`，BuildInfo 仍为
`v2.3.0-db7290f`。`strip-debug` 的代价是公开库不再提供 SDK 内部源码行号级调试信息，
不影响对外 API、链接符号或运行代码。

## 唯一正式构建

来源侧构建用于开发自检，不复用为公开资产。正式固件从隔离候选的全新检出构建一次；该候选
与公开代码提交 `ccae5fd495109244a679c5ffe1e17f98d6589f52` 的 repository tree
`a0fde688fb4dbd2b7803fde487046f4de6cccd0d`、项目 tree
`d5d5a8fb3f818106b77eeccc2599482943260be9` 完全一致，因此构建输入可绑定到这份公开源码：

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py -B <fresh-build-dir> --no-ccache `
  -D SDKCONFIG=<fresh-build-dir>/sdkconfig `
  -D SDKCONFIG_DEFAULTS=<project-dir>/sdkconfig.defaults `
  reconfigure build
```

构建目录和生成的 `sdkconfig` 都在公开源码目录之外或 Git 忽略范围内。不得读取来源工程的
`sdkconfig`，不得复用 `1.9.5` 的 `build/`、OTA app、完整镜像或哈希。本次正式构建记录：

| 证据 | 正式值 |
| --- | --- |
| 构建步骤 | `1767/1767` |
| app 大小 | `7,611,216` bytes |
| app SHA-256 | `fa87ae46ffa93bd01da6deffade8cf23b22022f19040815b3b305ce7a56f0eec` |
| app 分区 | `7798784` bytes |
| app 分区剩余 | `187,568` bytes（`2.41%`） |
| 16 MB 完整镜像大小 | `16777216` bytes |
| 16 MB 完整镜像 SHA-256 | `0fa03360dc65281903af0854a3624f4de208ac0f83956047416916442eea0bdc` |
| 正式构建输入源码归档 SHA-256 | `1f2980e339c10555298e95355cd8a70c49d248bee6d27bd2d973d8e042e01992` |

表中数据来自同一次 `1767/1767` 正式干净构建，编译 warning、error 和 ICE 均为 `0`。app
分区余量为 `187,568` bytes（`2.41%`），这是明确容量风险；本次没有复用 `1.9.5` 的数值，
也没有把开发侧 app 哈希写成公开资产哈希。

## Release 资产绑定

正式构建只生成一套发布输出：

1. `esp32s3-tirtc-device-monitor-ota-v1.9.6.bin`：正式构建产生的 app 镜像原件。
2. `esp32s3-tirtc-device-monitor-full-v1.9.6.bin`：严格按同一次
   `flasher_args.json` 的实际地址合并，文件长度固定覆盖 16 MB Flash。
3. `release-manifest.json`：记录公开 Tag/commit、开发来源、项目路径、逐文件源码清单与
   SHA-256、SDK 身份、构建环境、完整命令、分区和两个固件附件的大小与 SHA-256。
4. `SHA256SUMS.txt`：至少列出 full、OTA app 和 manifest 三个文件的 SHA-256。

完整镜像生成后必须逐段与 bootloader、partition table、OTA data 和 app 原件比对；所有未
使用区域必须保持 `0xFF`。同时核对镜像描述信息中的版本为 `1.9.6`。只有这些检查通过后，
才能把 full、OTA app、manifest 和 SHA256SUMS 上传到同一个项目 Release；构建二进制不进入
普通 Git 历史。

## 运行证据边界

`1.9.5` 相同音频链路做过双板媒体包核对和 G.711 A-law 编解码自检。这些历史证据支持线
格式与 Codec 路径核对，但 `1.9.6` 新增的冷启动呼叫、呼叫状态、联系人键盘和音量按下响应
没有在本次正式固件上重新烧录验证，因此不能写成 `1.9.6` Release 资产的目标板证明。

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
