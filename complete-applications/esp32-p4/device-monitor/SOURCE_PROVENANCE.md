# 源码来源与公开边界

本文记录 ESP32-P4 Device Monitor `1.5.0` 的开发来源、公开筛选和验证边界。开发者可以据此
确认代码从哪里来，也能区分源码一致性、构建证据和目标板运行证据。

## 来源快照

| 项目 | 内容 |
| --- | --- |
| 应用版本 | `1.5.0` |
| 来源 Tag | `esp32-p4-device-app-v1.5.0` |
| Tag object | `5cb49b6c82a2462c1c5c479040597e45a063591f` |
| 来源 commit | `97331363a0d3c4e6f89b5b93d39561e74087ba2a` |
| 来源 tree | `d0abdbe3139bdb1ae7486bb00747d1c58b593445` |
| 比较基线 | `esp32-p4-device-app-v1.4.0` / `d7529030846277dd06fe7332ef61b913d4378d31` |
| 公开项目 Tag | `esp32-p4-device-monitor-v1.5.0` |
| 公开路径 | `complete-applications/esp32-p4/device-monitor/` |
| 目标板 | Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 |
| ESP-IDF | `5.5.4` |

来源 `1.4.0..1.5.0` 共 2 个提交，最终由 release commit 收口；范围包括 `68` 个文件、
`6,095` 行新增和 `1,191` 行删除。主要变化包括 TGMP 码率反馈、跨 APP 媒体资源所有权、持久 PSRAM 摄像头/H264 池、
音频与 AEC、设备与微信呼叫交互、二维码、Wi-Fi 恢复、网络指标和串口回归接口。

来源树共有 `1,208` 个文件、`102,917,377` bytes。按路径排序并使用
`sha256  /path\n` 规范化后的清单 SHA-256 为
`4ec9947b779641a5df5e8294367108e6438a834f739f1846c1f8a81d882dac38`。排除下述 3 项后，
保留来源为 `1,205` 个文件、`97,904,212` bytes，规范化清单 SHA-256 为
`e4713a77c95f286ef32b21bb8f0283490ced4ab90083754bf10a1cf891d43fa3`。

## 公开筛选

来源 Tag 有 `1,208` 个文件。统一仓：

- 保留 `1,205` 个来源路径。
- 其中 `1,191` 个路径与来源 Tag 的 Git blob 字节完全一致。
- 14 个路径按公开仓职责维护：项目 README、VERSION、媒体架构、SDK 校验清单、9 份
  ESP-Hosted 文档，以及 1 份 ESP-LVGL 测试 README。
- 额外维护 `SOURCE_PROVENANCE.md` 和 `docs/GETTING_STARTED_CN.md` 两份对外文档。

未纳入的三个来源文件：

- `.clangd`：开发机编辑器索引配置。
- `sdkconfig`：生成配置；可复用默认值由 `sdkconfig.defaults` 提供。
- `components/tirtc_sdk/lib/esp32s3/libTiRTC.a`：P4 工程不链接的 S3 静态库，且归档包含供应方
  构建路径；公开 P4 包只保留目标平台库。

ESP-Hosted 的 4 份协议文档保留统一仓已修正的相对链接；两份上游示例 README 把
供应方 Linux 构建日志路径改为 `<esp-hosted-source>/...`。这些变换只处理文档可读性和
公开卫生；组件 README、设计说明和 P4 功能评估板说明另修正了失效的目录锚点，不改变组件
源码。ESP-LVGL 测试 README 只修正两个大小写错误的目录锚点。项目 README、VERSION 和
媒体架构则按统一仓风格重写，并以
`1.5.0` 代码常量为事实源。

以下内容始终排除：

- 开发仓 Git 元数据、内部协作资料和本机 IDE 配置。
- `build*/`、`managed_components/`、`dependencies.lock`、日志和缓存。
- `firmware/`、`release_assets/`、BIN、ELF、MAP 和压缩包。
- 真实 Wi-Fi、设备密钥、access key、token 和个人配置。

完整镜像只作为 GitHub Release 资产分发，不进入 Git 历史。

## TiRTC SDK 字节契约

| 项目 | 内容 |
| --- | --- |
| API 版本 | `2.3.0` |
| Nano source | `v2.3.0` / `1baf7c95f3ca715c9367b9c998417f647934dc35` |
| TGWebRTC source | `tag.v1.5.12` / `41c9a25768ffe265c07f17ef78a6439607b19364` |
| 嵌入 TGTRP BuildInfo | `tagv1.5.11` |
| P4 `libTiRTC.a` 大小 | `1,827,850` bytes |
| P4 library MD5 | `7e5fe37e9530bcaffce015583ae8cfb0` |
| P4 library SHA-256 | `6dc4d437ea444761ca21e203fc9babb1799bb1f7fc261d7c523248fde0a96e67` |

来源 SDK 7 项校验和已逐项核对；公开 P4 包删除 S3 库条目后保留 6 项。当前 P4 归档没有命中 Linux、Windows 或 WSL 的
个人用户目录，也没有命中真实凭据。TGWebRTC 源 Tag 与库内 TGTRP BuildInfo 字符串不同，因此公开资料
同时记录两者；仅写“TGTRP 1.5.12”会掩盖实际运行时元数据。

P4 重建保留以下项目修复：ICE callback 有界接收、RTC 调度让步、NACK scratch heap 化、
真实容量传递和有界弱网恢复。完整身份由源码 commit、SDK 文件清单和静态库哈希共同确定。

## H264 组件边界

H264 组件以 Espressif `1.3.8`、commit `8e86030d` 为基线。三个新增文件与官方 commit
逐字节一致；组件另有 4 个 P4 编码源文件补丁和本地 manifest 差异，覆盖输出溢出保护、
persistent reference pool 和码率控制。公开仓保留这些代码，不把组件误写成“完整上游原件”。
组件自己的许可证文件继续随源码保留。

## 构建与运行证据

统一公开候选已完成唯一一次 ESP-IDF `5.5.4`、`--no-ccache` 干净构建，共完成
`1837/1837` 个步骤，编译 warning、error 和 ICE 均为 0。应用镜像为 `6,955,728` bytes，
SHA-256 为 `cf57693f03abb8d182a03823cff6764138365e4880e19e113f298292fe0bba26`；最小 APP 分区
剩余 `580,912` bytes（`7.71%`）。`0x0` 完整镜像为 `16,777,216` bytes，SHA-256 为
`6d83ba156aeb7026533e567e3834ed3eb600b102a75ce1b1ded52d0d4358a6ff`。

构建完成后只修正了公开 Markdown 中的来源提交数量、构建证据和固件校验值，项目代码、配置、
SDK、分区表和依赖输入均未改变。正式 manifest 记录构建起点、最终公开 commit/tree、
文档差异、解析后的依赖、生成配置、完整镜像分段和所有 SHA-256。

开发侧已有双设备呼叫、AI、IPC 重复切换和内存恢复记录，但最新持久池修改后没有重新完成
微信外部实呼和弱网矩阵。这些记录不是精确 `1.5.0` Tag 的不可变运行快照，也不能替代：

- 从正式 Release 镜像烧录 P4，并确认 C6 ESP-Hosted/SDIO、Wi-Fi、绑定和平台在线。
- IPC、设备互呼、微信 VoIP、AI Chat、AEC 和长时间运行。
- 微信小程序 VoIP 与外部 IPC/H5 对端本轮的端到端人工回归。
- 每次退出后摄像头、音频、解码池、显示和 TiRTC 连接均可被下一会话重新获取。

静态校验能证明公开源码、版本记录和 SDK 文件相互一致；干净构建能证明候选可编译；烧录、
联网、真实媒体和长稳仍需目标板分别验证。
