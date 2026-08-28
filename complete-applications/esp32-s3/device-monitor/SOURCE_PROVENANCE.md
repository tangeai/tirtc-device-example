# 来源与验证边界

## 来源快照

| 项目 | 值 |
| --- | --- |
| 源工程 | `esp32s3_tirtc_device_monitor_demo` |
| 源分支 | `main` |
| 源标签 | `v1.9.7`，annotated Tag |
| 源 Tag object | `73f6a3b18ac4ad5e95f813c6eba4f31907961be6` |
| 源提交 | `58c2d152a179fa382511f4217fb84858072ffbb0` |
| 源 tree | `206b2373a389fab39ece1893f23f1ddbf0e66dfe` |
| 比较基线 | `v1.9.6` / `cdb5d7bec9c955227a1259281686e30377332fb3` |
| 公开代码导入提交 | `a3c6d4c5bdecb215f8c8ebec554424de77ed193b` |
| 公开 repository tree | `7508d3f00fb436a0d9bfd716fc1fc152f2d0cf8a` |
| 公开项目 tree | `ac13d559aae8f74e92afbf585fe79c5da02aa324` |
| 公开项目 Tag | `esp32-s3-device-monitor-v1.9.7` |
| 应用版本 | `1.9.7` |
| 发布日期 | `2026-08-28` |
| ESP-IDF | `5.5.4` |
| TiRTC SDK | `2.3.0 mini` |
| TiRTC SDK BuildInfo | `v2.3.0-db7290f` |
| SDK active-connect 修复 | `db7290f2404b15f2ab8567dd221853ca2ec054ab` |
| SDK HTTPS 认证修复 | `13e34c3e3e3dc6776be4713b5c1e3c17bd282766` |
| TiRTC 静态库 | `2,127,226` bytes |
| TiRTC 静态库 SHA-256 | `f5eca6c3212a34d00ce5706e864f8112adc8e6b20b92bc175435299caf4b082b` |

本目录按 `v1.9.7` 指向的不可移动源码快照整理。设备使用
`https://ep-open.tangeopen.com/services` 做服务发现，只接受 `HTTPS` 和 `MQTTS` 服务地址；
发现结果不满足安全协议时会被拒绝并报错，不覆盖编译期安全兜底。设备主动呼叫微信联系人继续使用
体验版 `wx_version_type=2`。

TiRTC SDK 以 `2.3.0 mini` 为基础，BuildInfo 保留 `v2.3.0-db7290f`。SDK 自有 HTTP 传输另含
`13e34c3` 的 HTTPS 服务端认证修复：强制 `MBEDTLS_SSL_VERIFY_REQUIRED`，挂接 ESP-IDF
证书包并校验证书链和 hostname。公开归档随后执行 `--strip-debug`，只移除 SDK 内部调试信息；
功能代码、只读数据、成员顺序和全局符号契约保持一致。

来源源码经过显式清单筛选后形成公开代码导入提交。代码身份已经绑定到 commit 与 tree；
正式构建输入直接从该提交的项目 tree 导出，共 `549` 个文件；源码归档 SHA-256 为
`9eeed47fe7fe0529dfaf2fad2b9d447caed9d986162936e544e3ee11f026c213`。最终公开 Tag、逐文件
清单和构建输入继续写入 `release-manifest.json`。源码归档哈希不是固件哈希，也不代替正式构建。

## 1.9.7 变更范围

源工程相对 `v1.9.6` 共变更 `40` 个文件，新增 `1,441` 行、删除 `117` 行：

- 接听、拒绝、挂断和音量/采集增益操作从 LVGL 回调移到应用任务。音量与增益的连续操作
  合并为最新值，避免控制队列被过期中间值占满。
- Device Call 使用 generation 约束异步接听 worker。worker 在提交连接前、提交过程中和等待
  连接时都复核 generation，已经结束的旧房间不能被迟到任务重新拉起。
- 主叫的响铃等待保持 `45` 秒；状态进入 `CONNECTING` 后重新开始 `40` 秒 P2P 建连计时。
  响铃超时走取消语义，P2P 超时走挂断和 `p2p_error` 语义。
- 普通设备来电铃声改为两组 `659 / 784 / 1047 Hz` 上行三音短句，周期 `3.2` 秒，并加入
  起音和收音包络。接听、拒绝、取消、挂断和身份重置仍会停止铃声。
- 接听、拒绝和挂断按钮在 `LV_EVENT_PRESSED` 时投递动作，让触摸反馈更及时；业务完成状态
  仍由应用快照和协议结果驱动。
- TiRTC SDK 增加有界公平调度：一次 ICE UDP 回调最多处理 `8` 个数据报或 `4 ms`；一次
  TGTRP 音频抖动轮询最多释放 `4` 个有序项，有剩余积压时延后 `1 ms` 继续。排序和丢包统计
  契约不变。
- TiRTC 静态库更新为 `2,127,226` bytes，SHA-256 为
  `f5eca6c3212a34d00ce5706e864f8112adc8e6b20b92bc175435299caf4b082b`。应用版本提升到
  `1.9.7`；ESP-IDF、Flash、分区表和安全传输契约保持不变。

## 公开筛选

统一仓保留应用源码、构建所需组件、`sdkconfig.defaults`、分区表、公开开发者文档和必要辅助
脚本。以下内容不会进入公开源码快照：

- 来源仓 Git 元数据、本地 `AGENTS.md`、本地 Skill、IDE 配置和开发机完整 `sdkconfig`。
- `build/`、`build-current/`、`managed_components/`、日志、NVS 转储、ELF、MAP 和固件镜像。
- 真实 Wi-Fi、设备身份、密钥、Token、串口记录和个人绝对路径。
- 内部交接、一次性审阅材料、旧版本构建目录和与本版本无关的调试资料。

来源仓中与该 Tag 无关的内部 Skill、本地架构草稿和开发文档未进入公开代码提交。来源 Tag
新增的四章通用开发者指南、`22` 张信息图及 `2` 张界面参考图已独立整理到仓库顶层 `docs/`；其余 `3` 份写作规范
和专项架构材料继续留在开发侧。SDK 归档卫生继续生效：公开库 SHA-256 为
`f5eca6c3212a34d00ce5706e864f8112adc8e6b20b92bc175435299caf4b082b`，BuildInfo 仍为
`v2.3.0-db7290f`。`strip-debug` 的代价是公开库不再提供 SDK 内部源码行号级调试信息，
不影响对外 API、链接符号或运行代码。

## 唯一正式构建

来源侧构建用于开发自检，不复用为公开资产。正式固件已经从隔离候选的全新检出构建一次。
代码导入提交为 `a3c6d4c5bdecb215f8c8ebec554424de77ed193b`，其 repository tree 为
`7508d3f00fb436a0d9bfd716fc1fc152f2d0cf8a`、项目 tree 为
`ac13d559aae8f74e92afbf585fe79c5da02aa324`；构建输入与这个项目 tree 一致：

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py -B <fresh-build-dir> --no-ccache `
  -D SDKCONFIG=<fresh-build-dir>/sdkconfig `
  -D SDKCONFIG_DEFAULTS=<project-dir>/sdkconfig.defaults `
  reconfigure build
```

构建目录和生成的 `sdkconfig` 都在公开源码目录之外或 Git 忽略范围内。不得读取来源工程的
`sdkconfig`，不得复用 `1.9.6` 的 `build/`、OTA app、完整镜像或哈希。正式构建记录：

| 证据 | 正式值 |
| --- | --- |
| 公开代码导入提交 | `a3c6d4c5bdecb215f8c8ebec554424de77ed193b` |
| 构建步骤 | `1767/1767` |
| compiler warning / error / ICE | `0 / 0 / 0` |
| app 大小 | `7,613,216` bytes（`0x742b20`） |
| app SHA-256 | `49349c1e1755ae5265e3ddea6f57166ccf60b90cdca7d1a1a004539f4faf43ff` |
| app 分区 | `7798784` bytes |
| app 分区剩余 | `185,568` bytes（`2.38%`） |
| 16 MB 完整镜像大小 | `16777216` bytes |
| 16 MB 完整镜像 SHA-256 | `48a92849ef0149a4b099e981c4e109fcfa81afcbf99d379da1f7de61ad996bd1` |
| 正式构建输入源码归档 SHA-256 | `9eeed47fe7fe0529dfaf2fad2b9d447caed9d986162936e544e3ee11f026c213` |
| build log SHA-256 | `54f21240ef82ec657966a08b187fda0ea94f13242bff283ffe0f35b9711b9043` |

表中数据来自同一次 `1767/1767` 正式干净构建，compiler warning、error 和 ICE 均为 `0`。
app 分区余量为 `185,568` bytes（`2.38%`），这是明确容量风险；本次没有复用 `1.9.6` 的
数值，也没有把开发侧 app 哈希写成公开资产哈希。

## Release 资产绑定

正式构建只生成一套发布输出：

1. `esp32s3-tirtc-device-monitor-ota-v1.9.7.bin`：正式构建产生的 app 镜像原件。
2. `esp32s3-tirtc-device-monitor-full-v1.9.7.bin`：严格按同一次
   `flasher_args.json` 的实际地址合并，文件长度固定覆盖 16 MB Flash。
3. `release-manifest.json`：记录公开 Tag/commit、开发来源、项目路径、逐文件源码清单与
   SHA-256、SDK 身份、构建环境、完整命令、分区和两个固件附件的大小与 SHA-256。
4. `SHA256SUMS.txt`：至少列出 full、OTA app 和 manifest 三个文件的 SHA-256。

完整镜像生成后必须逐段与 bootloader、partition table、OTA data 和 app 原件比对；所有未
使用区域必须保持 `0xFF`。同时核对镜像描述信息中的版本为 `1.9.7`。只有这些检查通过后，
才能把 full、OTA app、manifest 和 SHA256SUMS 上传到同一个项目 Release；构建二进制不进入
普通 Git 历史。

## 运行证据边界

旧版本的相同音频链路做过双板媒体包核对和 G.711 A-law 编解码自检。这些历史证据支持线
格式与 Codec 路径核对。`1.9.7` 已完成正式干净构建，但新增的 UI 动作队列、generation
防护、分阶段超时、移动铃声和 SDK 公平调度没有目标板回归，因此不能写成该 Release 的真机证明。

HTTPS 服务端认证已核对到 SDK 源码、`httpclt.o`、静态归档和 APP 链接：auth mode 为
`MBEDTLS_SSL_VERIFY_REQUIRED`，证书包、hostname 和 verify result 依赖均保留。当前没有目标板
上的“有效证书成功 + 错误证书或错误主机名失败”双向握手证据，也没有对绑定、MQTT、小钛、
设备呼叫、微信 VoIP 和 Web IPC 做本版本真机回归，因此文档不把静态检查写成运行时证明。
目前也没有 `1.9.7` 的烧录、串口启动、Wi-Fi、响铃听感、呼叫闭环、弱网或长稳证据。

Web IPC 和设备互呼的当前人耳试听仍可感知轻微“沙沙电流声”。线上格式和 Codec 自检没有
发现异常，说明排查范围已经缩小；它们不能证明底噪来源，根因仍未闭环。本版本明确保留这个
已知现象，不宣称已经修复。

源码身份、构建、附件、烧录、联网和业务运行分别留证。静态文件哈希可以证明公开内容彼此
一致；正式构建和镜像分段比对可以把附件绑定到公开代码快照。这些证据仍不替代目标板烧录、
Wi-Fi、服务发现、ThingConnect、Web IPC、小钛、微信 VoIP、设备互呼、OTA、弱网和长稳验证。
