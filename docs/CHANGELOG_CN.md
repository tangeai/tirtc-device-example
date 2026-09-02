# 版本变更记录

## 2026.09.02：ESP32-P4 Device Monitor 1.5.3

- 开发来源固定为 Tag `esp32-p4-device-app-v1.5.3`、commit
  `06583eaf18d9326b9602684a7c509a3badf7dadc`、tree
  `1acc583f3601448e1656f18716924f70125fe0c7`。本次公开更新以 `1.5.1` 为比较基线，包含
  开发版 `1.5.2` 与 `1.5.3` 的变化，不单独补发公开 `1.5.2`。
- RTC 配置入口使用 URL parser 校验显式 HTTPS 和非空 host，拒绝 userinfo、fragment、
  非法端口及未终止字符串；RTC 关闭时允许空地址。校验失败保留原配置且不触发 SDK 重置。
  删除按 TiRTC SDK 版本把 HTTPS 改成 HTTP 的旧兼容分支，TLS 失败保留失败语义。
- TiRTC SDK 使用 `2.3.0` P4 验证重建与补丁版，包含 ICE/TGTRP 有界调度、NACK 容量、码率
  恢复和 TURN 查找栈修正。公开归档经 `--strip-debug` 移除 SDK 行号调试信息，99 个成员
  的分配节内容、运行符号、重定位和归档符号索引保持等价；不是未经修改的官方包。
- P4 静态库为 `4,748,802` bytes，SHA-256 为
  `a7a01ffd496a55364c7e4d665ff3884d078147bba96752a965d97befca12e451`。
- 微信 H264 上行从 `480x320@15fps`、目标 `480kbps` 调整为 `640x480@15fps`、目标
  `800kbps`，主动呼叫使用正式版 `wx_version_type=0`。下行仍请求 `640x480` MJPEG，
  实际帧可以更小，P4 硬解后一次 `cover` 到 `480x320`。
- 设备呼叫保持 `384x256@12fps`、目标 `256kbps`，IPC 保持 `1280x960@20fps`、目标
  `4Mbps`。SDK/TGMP 码率反馈默认开启，旧本地自动弱网调节保持关闭。
- H264 直编码输出复用持久 PSRAM 工作区；异步挂断任务使用后台栈，异步页面动作在 LVGL
  锁内提交。串口回归接口与媒体停滞诊断补充会话和传输观测点。
- Wi-Fi 连接、断开或配置 RPC 错误可触发 Hosted 重建；SDIO 在确认中断前检查寄存器快照和
  包长，并记录 TX throttle。当前依赖没有 `esp_hosted_event.h`，事件与心跳恢复分支被
  编译关闭；这些防护不证明物理故障根因已经消除。
- 已完成 ESP-IDF `5.5.4`、`--no-ccache` 正式干净构建：主工程 `1837/1837`、bootloader
  `133/133`，compiler warning/error/ICE 均为 0；app `6,973,376` bytes，APP 分区剩余
  `563,264` bytes（`7.47%`）。交付 `0x0` 16 MiB 完整镜像、`SHA256SUMS.txt` 和 manifest。
  尚未执行本版本烧录、HTTPS 真机正反例、微信实呼、双向弱网或长稳回归。

## 2026.08.28：ESP32-S3 Device Monitor 1.9.7

- 开发来源固定为 annotated Tag `v1.9.7`、Tag object
  `73f6a3b18ac4ad5e95f813c6eba4f31907961be6`、commit
  `58c2d152a179fa382511f4217fb84858072ffbb0`、tree
  `206b2373a389fab39ece1893f23f1ddbf0e66dfe`；比较基线为 `v1.9.6`。
- 接听、拒绝、挂断和音量/采集增益操作改由应用任务执行；连续本地音量与增益请求合并为
  最新值，UI 不再同步等待网络、RTC、音频硬件或 NVS。
- 接听 worker 使用 generation 防止旧任务复活已结束房间；主叫按 `45` 秒响铃与 `40` 秒
  P2P 建连分阶段计时。接听、拒绝和挂断按钮按下即投递动作。
- 来电铃声改为两组 `659 / 784 / 1047 Hz` 上行三音短句，周期 `3.2` 秒，并带起音和收音
  包络。
- TiRTC SDK 的 ICE UDP 回调限制为每轮最多 `8` 个数据报或 `4 ms`，TGTRP 音频抖动轮询
  每轮最多释放 `4` 个有序项；剩余积压在 `1 ms` 后继续，排序和丢包统计契约保持不变。
- 公开 `libTiRTC.a` 为 `2,127,226` bytes，SHA-256 为
  `f5eca6c3212a34d00ce5706e864f8112adc8e6b20b92bc175435299caf4b082b`。
- 来源侧新增的 `7` 份内部 Markdown 和 `22` 张开发截图没有直接复制到公开仓；公开说明由
  统一仓按面向开发者的结构维护。
- 已从公开代码导入提交完成 ESP-IDF `5.5.4`、`--no-ccache` 正式干净构建：`1767/1767`，
  compiler warning、error、ICE 均为 `0`；app `7,613,216` bytes，SHA-256 为
  `49349c1e1755ae5265e3ddea6f57166ccf60b90cdca7d1a1a004539f4faf43ff`，分区剩余
  `185,568` bytes（`2.38%`）。
- `1.9.7` 的烧录、双机呼叫、铃声听感、弱网和长稳验证尚未完成；正式构建不替代目标板证据。

## 2026.08.28：ESP32-P4 Device Monitor 1.5.1

- 开发来源固定为 annotated Tag `esp32-p4-device-app-v1.5.1`、Tag object
  `4a0fa821b53551432c8341763cf68a61c81285b5`、commit
  `8d26a2bc5267f6bf1db721730a210ce8bc2f7ccc`、tree
  `03e8f4708f69a96df13b8115376351075e5adf78`；比较基线为 `esp32-p4-device-app-v1.5.0`。
- 绑定弹窗中的文字网址和二维码改用独立的 ThingConnect 设备管理门户
  `https://demo-open.tange-ai.com/devices`，不再把设备业务 API 地址展示给用户。
- 设备内部 JSON 服务发现继续使用 `https://ep-open.tangeopen.com/services`，业务 API fallback
  继续使用 `https://srv-open.tangeopen.com`，设备 MQTT fallback 继续使用
  `mqtts://mqtt-open.tangeopen.com:8883`。四类入口职责分开，不能互换。
- TiRTC SDK、H264/ESP-Hosted 组件、媒体参数、分区表和硬件配置均保持 `1.5.0` 基线不变。
- 唯一正式构建使用 ESP-IDF `5.5.4`、GCC `14.2.0_20260121` 和 `--no-ccache`，完成
  `1837/1837`，编译 warning、error、ICE 均为 0；app `6,955,776` bytes、SHA-256
  `1542c19052ae9bdb8804e38916c54f928e405ac06e6ae796faa5d46cd7c7e515`，分区剩余
  `580,864` bytes（`7.71%`）。
- `0x0` 16 MiB 完整镜像 SHA-256 为
  `019eed4e047f9d1a1809c5fa0e441b94d2a509fe83c885e5eafc21adb89c0a7a`；固件只作为
  GitHub Release 资产分发，不进入 Git 历史。本轮没有执行烧录或目标板绑定回归。

## 2026.08.28：ESP32-P4 Device Monitor 1.5.0

- 开发来源固定为 annotated Tag `esp32-p4-device-app-v1.5.0`、Tag object
  `5cb49b6c82a2462c1c5c479040597e45a063591f`、commit
  `97331363a0d3c4e6f89b5b93d39561e74087ba2a`、tree
  `d0abdbe3139bdb1ae7486bb00747d1c58b593445`；比较基线为 `esp32-p4-device-app-v1.4.0`。
- TiRTC SDK 保持 `2.3.0` API，Nano 为 `v2.3.0`，TGWebRTC 源码为 `tag.v1.5.12`；库内
  TGTRP BuildInfo 为 `tagv1.5.11`。P4 静态库大小 `1,827,850` bytes，SHA-256 为
  `6dc4d437ea444761ca21e203fc9babb1799bb1f7fc261d7c523248fde0a96e67`。
- SDK/TGMP 码率控制默认开启，回调只投递绝对目标码率到应用控制任务；旧本地队列压力自动
  降级保持关闭。P4 ICE/TGTRP 继续使用有界接收、公平调度和有界音频抖动处理。
- 摄像头 USERPTR、H264 reference/deblocking、解码、显示和 AEC 使用持久 PSRAM 池；跨 APP
  预热和会话代际清理减少重复媒体会话的大块内存碎片。
- 设备呼叫上行为 `384x256@12fps`、`256kbps` H264，名义 GOP `192` 帧 / `16s`；H264 下行
  使用 `24 x 256KB` 输入槽、4 个解码槽、20 个 RGB 输出槽和最大 16 帧的自适应播放队列。
- 微信设备上行为 `480x320@15fps`、`480kbps` H264；下行继续请求 `640x480` MJPEG，并由 P4
  硬解后一次 `cover` 到 `480x320`。本工程不声明微信手机端原始采集分辨率。
- 音频增益、AEC 双讲近端保护、设备/微信联系人、二维码预览、Wi-Fi 后台恢复、结构化网络指标
  和默认开启的物理串口回归 CLI 一并收口。串口日志可能含设备 ID，外发前应脱敏。
- 唯一正式构建使用 ESP-IDF `5.5.4`、GCC `14.2.0_20260121` 和 `--no-ccache`，完成
  `1837/1837` 且编译 warning、error、ICE 均为 0；app `6,955,728` bytes、SHA-256 为
  `cf57693f03abb8d182a03823cff6764138365e4880e19e113f298292fe0bba26`，分区剩余
  `580,912` bytes（`7.71%`）。
- `0x0` 16 MiB 完整镜像 SHA-256 为
  `6d83ba156aeb7026533e567e3834ed3eb600b102a75ce1b1ded52d0d4358a6ff`；固件只作为
  GitHub Release 资产分发，不进入 Git 历史。
- 开发侧已有双设备呼叫、AI、IPC 重复切换和内存恢复记录；最新持久池修改后，微信外部实呼
  和弱网矩阵没有重新完成。COM7/COM11 上的启明板仍是旧版 `1.3.2`，不属于本版本证据。

## 2026.08.26：ESP32-P4 Device Monitor 1.4.0

- 开发来源固定为 annotated Tag `esp32-p4-device-app-v1.4.0`、Tag object
  `3e4b1b8763fd506555f09794efccf53f41d648b2`、commit
  `d7529030846277dd06fe7332ef61b913d4378d31`、tree
  `dcfc01845f2901115fd11804230f14bc6f924923`。
- TiRTC SDK 更新为 `2.3.0` 官方源码重建版。Nano 源码为 `v2.3.0`，TGWebRTC 源码基线为
  `tag.v1.5.12 / 41c9a25768ffe265c07f17ef78a6439607b19364`；当前库内嵌 TGTRP
  BuildInfo 仍为 `tagv1.5.11`，两项元数据分开记录。P4 静态库 SHA-256 为
  `3f11a4ac6d047eaac1b2e65a82d78cbca41082165b3e3190010e27c727bd577a`。
- P4 ICE UDP 接收加入数据报和耗时预算；NACK scratch 改为连接 heap 并传递真实容量；
  TGTRP 弱网恢复增加有界探测、最低码率保持上限和发送积压门控。
- 设备互呼改为 `384x256@15fps`、`320kbps` 双向 H264。下行使用固定 PSRAM 输入、解码和
  RGB565 显示池，PPA 一次缩放到 `480x320`；控制层 5 秒后隐藏，点击视频恢复。
- 微信 VoIP 保持 `480x320@15fps`、`420kbps` H264 上行；下行请求 `640x480` MJPEG，
  由 P4 JPEG 硬解后居中 `cover` 到 `480x320`。本工程不声明微信手机端原始采集分辨率。
- `esp_h264` 更新到 Espressif `1.3.8` 基线并保留 P4 项目补丁；ESP-Hosted/SDIO、V4L2
  完成帧回收、PSRAM 队列和内存水位路径同步收口。
- AI Chat 隔离旧 generation 命令；设备挂断返回 `40400` 时按幂等结束处理。网络、时间、
  MQTT 和媒体阶段日志继续保留定位第一处异常所需的状态与水位。
- 版本收口前的同一功能代码完成过 ESP-IDF `5.5.4` 构建、COM14/COM30 双机启动、54 次
  双向设备呼叫及弱网矩阵和 12 次 AI 回归，动态画面约 `14.8-14.9fps`。这些记录不是精确
  `1.4.0` Tag 的构建或烧录证明；正式 app、完整镜像和 SHA-256 以本版本 Release manifest
  为准。微信小程序 VoIP 与外部 IPC/H5 对端本轮没有重新完成人工端到端验证。

## 2026.08.26：ESP32-S3 Device Monitor 1.9.6

- 开发来源固定为 annotated Tag `v1.9.6`、Tag object
  `c251806b8904672bac07ea16cca2f3099e2426e4`、commit
  `cdb5d7bec9c955227a1259281686e30377332fb3`、tree
  `2b032dc07ada0a2e15cfb82e320ac69b53272374`。
- 设备呼叫拆分音频准备与 RTC 就绪。正式上线最长等待 30 秒，RTC ready 最长等待 50 秒，
  每 500 ms 推进准备，响铃上限保持 45 秒。
- 发起呼叫前，呼叫请求最多等待 12 秒，让已经运行的房间恢复、联系人刷新或联系人变更任务
  结束；超时仍忙则拒绝本次呼叫。呼叫活跃后不再启动新的房间恢复或联系人刷新，结束后恢复。
- 呼叫页补齐主被叫角色、对端设备、错误和状态消息，清楚显示准备、等待接听、建立连接、
  通话及短暂终态提示。
- 联系人名称改为输入框和屏幕键盘编辑，确认键使用 `OK`；音量加减在按下时立即生效，
  静音仍需完整点击确认。
- RTC 由应用生命周期自动维护；设置页删除 TiRTC 测试、手动启动 RTC 和 sender audio test。
- H5 入口保持 `https://demo-open.tange-ai.com/`，HTTPS/MQTTS 与 TiRTC SDK 契约不变。
- 公开代码提交为 `ccae5fd495109244a679c5ffe1e17f98d6589f52`。正式构建输入与该提交的
  repository tree `a0fde688fb4dbd2b7803fde487046f4de6cccd0d`、项目 tree
  `d5d5a8fb3f818106b77eeccc2599482943260be9` 完全一致。
- 唯一正式构建使用 ESP-IDF `5.5.4`、GCC `14.2.0_20260121`、`--no-ccache`，完成
  `1767/1767`，编译 warning、error、ICE 均为 0；app `7,611,216` bytes，分区剩余
  `187,568` bytes（`2.41%`），容量余量仍是明确风险。
- 构建结果不替代烧录、冷启动呼叫、键盘和音量触摸或长稳验证。Web IPC 和设备互呼仍可能
  听到轻微“沙沙电流声”，根因尚未证实，本版本不宣称已经修复。

## 2026.08.25：ESP32-S3 Device Monitor 1.9.5

- 开发来源固定为 Tag `v1.9.5`、commit
  `45db394cae399967a9c3b882d595cdecb80321be`、tree
  `4a6760708d4fd2bdb973c4bc77d789d45f2bc2be`；公开代码提交为
  `c27914eafab6f700cecf196da48987200cd54d37`。正式构建输入与该提交的 repository tree
  `b0b93ddc0c82ae65130ae6dab17ff6ee6dbfc86a`、项目 tree
  `9fa7f85ce92a0e5fcd27c4a36fabe02609191391` 完全一致。
- 网络测试增加平均时延、相邻 RTT 平均抖动和丢包率三项结构化指标，并贯穿网络层、应用
  快照、UI 和串口诊断。
- 普通设备来电增加本地合成铃声；接听、拒接、取消、挂断和身份重置按状态停止，任务栈和
  PCM 缓冲使用 PSRAM。
- Web IPC、设备互呼和微信 VoIP 的设备上行统一核对为
  `8 kHz / 16 bit / mono / G.711 A-law / 20 ms / 160 bytes`，显式 CALL gate 使用同一
  A-law 契约。
- 设备互呼独立增加约 `1.25%` 的有界播放速率微调；AEC 使用全双工高性能线性模式和 PSRAM
  工作区，AEC 后增加 100 Hz 高通，并采用更保守的通话 AGC 噪声底线。
- TiRTC SDK 以 active-connect `db7290f` 为功能基础，并加入 `13e34c3` 的 HTTPS 服务端认证
  修复。SDK 自有 HTTP 客户端强制验证证书链和 hostname，不回退明文 HTTP。
- 服务发现、业务 HTTP 和 TiRTC 入口只接受 HTTPS，设备 MQTT 只接受 MQTTS；不安全发现
  结果会被拒绝并报错，不覆盖安全兜底。微信主动呼叫保持体验版 `wx_version_type=2`。
- 公开 `libTiRTC.a` 为 `2,125,366` bytes，SHA-256 为
  `83556eeee0c6cae45961899a4c5d1255a5d0d33f8e636104a946ce41ff3e20d7`。`--strip-debug`
  只移除 SDK 内部调试路径，BuildInfo 保持 `v2.3.0-db7290f`。
- 已从与公开代码提交 tree 完全一致的隔离候选执行一次禁用 ccache 的干净构建，完成
  `1767/1767`，app 为
  `7,608,608` bytes、SHA-256 `51a7599942f06556e33ef4820499885d6213ff15fce0f3ed2f11e38e44146503`、分区剩余
  `190,176` bytes（`2.44%`）；完整镜像和 OTA app
  只通过 `esp32-s3-device-monitor-v1.9.5` Release 分发。
- 正式构建确认 app 分区余量为 `190,176` bytes（`2.44%`），属于明确容量风险；`1.9.0`
  的二进制、大小和哈希没有复用。
- 当前 Web IPC 和设备互呼的人耳试听仍可感知轻微“沙沙电流声”。线上格式与 A-law 自检未见
  异常，但底噪根因尚未证实，本版本不宣称该问题已经解决。
- SDK TLS 修复已核对到源码、对象、归档和 APP 链接。目标板上的有效证书成功、错误证书或
  错误主机名失败，以及正式固件烧录、联网、完整业务、音频主观效果和长稳仍按独立证据记录。

## 2026.08.24：ESP32-S3 Device Monitor 1.9.0

- 开发来源固定为 Tag `v1.9.0`、commit
  `a64422b0efdebe6c303370effafd52bbf51593d1`、tree
  `b29d4080a43db0a2b8f2e35f095c5c45f3c1f4c7`。
- TiRTC SDK 升级到官方 `2.3.0 mini`；`libTiRTC.a` 为 `8,081,578` bytes，SHA-256 为
  `43b06d1da421c7d24cc7fdb1385d600ecdffbfd2d3801f7faf0c540fb5cdbaa2`。
- 完善 S3 音频/AEC、媒体抖动缓冲与完整性统计、TiRTC 连接与任务栈策略，以及 Web IPC、
  小钛、微信 VoIP 和设备互呼的生命周期交接。
- 产品能力统一为 RTC 双向音频；摄像头只用于联系人二维码扫描。设备呼叫、微信和小钛动作
  只接受音频类型，不静默降级视频请求。
- 新增默认开启的串口 AT 诊断 CLI，覆盖网络、Socket、RTC、媒体、音频路径、AEC、呼叫和
  内存状态；生产固件不需要时可在 Kconfig 关闭后重新构建。
- 来源工程已完成开发侧构建；统一公开代码快照 `d76e6e3b7f02f112b55ed917daa17f530b3c7a6b`
  也已在全新目录完成 ESP-IDF `5.5.4`、禁用 ccache 的正式干净构建。app 大小
  `7599904` bytes、SHA-256
  `3cdebe0df0946fc7bee65c921f94796c080bc1e03025370b1b76a5cbe560d137`、分区剩余
  `198880` bytes；容量余量较紧。
- 完整镜像和 OTA app 只通过 `esp32-s3-device-monitor-v1.9.0` Release 分发；源码 Git
  历史不保存构建二进制。
- `1.8.1` 的微信主动呼叫体验版 `wx_version_type=2` 配置继续保留，历史 Tag 和 Release
  仍可复核。
- 构建与静态一致性证据和目标板烧录、串口、联网、业务、音频效果、弱网及长稳证据分开记录。

## 2026.08.18：ESP32-S3 Device Monitor 1.8.1

- 设备主动呼叫微信联系人时，默认使用体验版 `wx_version_type=2`。
- 为正式版、开发版和体验版增加具名常量，并在编译期拒绝 `0..2` 之外的取值。
- 本补丁不改变微信来电链路、其他业务代码、板级配置、媒体参数或 TiRTC SDK。
- 已从唯一公开代码提交完成 ESP-IDF `5.5.4` 正式干净构建；完整镜像和 OTA app 只通过
  `esp32-s3-device-monitor-v1.8.1` Release 分发。
- 构建、附件和源码一致性已经核对；目标板烧录、联网、微信响铃/接通、媒体和长稳验证仍按
  独立证据记录。

## 2026.08.13：最小系统例子统一命名

- 对外分类从原有名称统一为“最小系统例子”，目录迁移到 `minimal-system-examples/`。
- S3 更新为 `0.8.0`，公开 Tag 为 `esp32-s3-minimal-system-v0.8.0`；补全 14 张经过
  不可逆脱敏的操作截图，README 现在覆盖完整的 30 张流程图。
- P4 更新为 `0.2.0`，公开 Tag 为 `esp32-p4-minimal-system-v0.2.0`。
- 两个平台的功能代码、TiRTC SDK、预录媒体和板级配置保持不变；分别从最终公开 Tag 重新完成
  一次 4 MB 正式干净构建，并重新生成 manifest 与 SHA256SUMS。
- 原分类的 GitHub Release 和 Tag 在新 Release 下载校验完成后下线，不保留重复公开入口。

## 2026.08.13：ESP32-P4 最小系统例子 0.2.0

- P4 协议体验工程统一以“最小系统例子”身份发布，提供精简中文 AT 指令、结构化脱敏日志和
  ThingConnect 绑定流程。
- 同一份 P4 固件兼容 ESP32-C6 与 ESP32-C61 Wi-Fi 协处理器，使用 ESP-Hosted/SDIO 联网。
- 提供网页远程查看、预录 H264/G711A 媒体、设备互呼与消息、联系人备注和 AI 对讲。
- 采用 4 MB 完整镜像，统一使用 Espressif ESP Tool 从 `0x0` 烧录。
- 开发源没有 Git 历史，公开 Release 用源码快照、公开 commit、逐文件哈希和正式构建记录建立
  可复核身份，不虚构开发 Tag。
- 项目 Tag：`esp32-p4-minimal-system-v0.2.0`；构建资产和校验值以 Release manifest 为准。

## 2026.08.13：ESP32-S3 最小系统例子 0.8.0

- 延续面向无专用音视频开发板用户的完整体验固件，可直接烧录到 4 MB 或更大 Flash。
- 用户交互保持精简中文 AT 指令和中文日志；本次把公开文档收口为一份按步骤操作的体验说明。
- 保持开发者平台绑定、网页远程查看、预录 H264/G711A 循环媒体、双机呼叫和字符串消息。
- 保持联系人备注、AI 故事/笑话/天气对讲，以及 AI 按“小李”等备注呼叫设备。
- 公开快照移除内部 AT 回归脚本；新增截图中的 Wi-Fi、设备 ID、MAC 和联系人身份均已替换为
  示例值，并清除可识别的图片元数据。
- 项目 Tag：`esp32-s3-minimal-system-v0.8.0`；构建资产和校验值以 Release 清单为准。

## 2026.08.11：ESP32-P4 Device Monitor 1.3.2

- 来源 Tag：`esp32-p4-device-app-v1.3.2`，commit `bc1ae8fbd9b64090503128985129a72e024c0551`。
- 公开项目 Tag：`esp32-p4-device-monitor-v1.3.2`。
- 新增 internal-RAM NVS worker，统一串行处理设备身份、音量、AI 头像、RTC 配置和绑定会话的持久化。
- RTC/WHIP 增加并发提交门控、过期回调识别和幂等断连；绑定重置改由 APP 控制队列接管。
- TiRTC SDK 保持 `2.3.0` 定制兼容快照，第三方组件版本不随应用补丁版本改写。
- 正式干净构建的 app 大小为 `6,927,360` 字节，SHA-256 为
  `2df6d9d626a05f19a4fd1f15eb854c54119a32ccd475090f6713f2629afc90e2`。
- `0x0` 完整镜像 `esp32p4-tirtc-device-monitor-full-v1.3.2.bin` 大小为
  `16,777,216` 字节，SHA-256 为 `87bfb67d1ba30d7f79663f63891e29f7f4f4367c80ff0d5cecb1b46f301d40e9`；该资产只通过
  GitHub Release 分发，不进入 Git 历史。

## 2026.08.10：Release 按项目独立版本化

- 八个项目使用“平台 + 项目 + 版本号”的独立 Tag；一个项目更新时，只发布该项目的新版本。
- 该次迁移后，以下六个项目 Tag 保持有效：
  `esp32-s3-minimal-integration-v1.2.0`、
  `esp32-p4-minimal-integration-v1.1.1`、
  `g32s10x-minimal-integration-v0.8.3`、
  `esp32-s3-device-monitor-v1.8.0`、
  `esp32-p4-device-monitor-v1.3.1` 和
  `g32s10x-device-monitor-v0.1.1`。
- 两个 AT ThingConnect 项目后来在 2026.08.13 统一为“最小系统例子”，使用各自的新 Tag 和
  Release；本节不再列出已下线的旧入口。
- 原日期型批次 Release 已下线；公开文档、源码归档、manifest 与下载入口统一指向项目 Tag。
- 固件资产保持原正式构建字节不变。本次只迁移发布身份并重建单项目 manifest 和校验清单，没有重新编译。

## 2026.08.02.1：ESP32-P4 Device App 1.3.1 完整镜像补充

- 新增 `esp32p4-tirtc-device-monitor-full-v1.3.1.bin`，供开发者从 `0x0` 直接烧录体验。
- 完整镜像只进入 GitHub Release，不进入 Git 历史；该次补充后来并入当前 P4 项目 Release。
- P4 源码、SDK 和正式构建输入未变化；补丁更新只收口公开文档、manifest 和发布资产。
- 完整烧录会清除 NVS、Wi-Fi 和绑定信息，烧录后需要重新配置。

## 2026.08.02：ESP32-P4 Device App 1.3.1

### 媒体链路

- 摄像头完成帧增加单调 sequence，消费端会识别并丢弃倒退帧；采集节拍改为相位锁定，
  背压解除后从当前时间恢复，不集中补发过期帧。
- H264 下行把 TinyH264 helper 的双任务调度改为运行时显式配置，并增加通知竞争保护、
  解码阶段和 liveness 记录，以及有限的卡顿隔离与恢复入口。
- 数值型媒体参数集中到 `main/media/media_tuning.h`，下行调度集中到
  `main/services/call_video_renderer_config.h`，避免 Kconfig 与生成配置成为多份事实来源。
- GOP 统一按 `2s` 时长计算：IPC `20fps` 对应 `40` 帧，设备/微信通话 `15fps` 对应 `30` 帧。

### 资源与会话

- 大块媒体负载和后台 worker 明确使用 PSRAM；内部 RAM 留给实时控制、DMA 和同步对象，
  并增加内存水位迁移记录。
- 微信接听改为常驻 PSRAM worker；来电提示阶段不再提前取得 RTC 媒体所有权。
- SDK 仍为 `2.3.0` API，但 P4 静态库更新为带 HTTP DNS cache disable 回移补丁的定制兼容快照，
  避免缓存过期路径出现递归锁风险。

### 发布

- 来源 Tag：`esp32-p4-device-app-v1.3.1`，commit `7391464`。
- 公开项目 Tag：`esp32-p4-device-monitor-v1.3.1`。
- 源码和完整镜像现在由同一个项目 Release 提供。
- 最终公开快照完成静态门禁和干净构建。运行时通话、重复切换与长稳验证单独记录，
  不用构建结果代替真机证据。

## 2026.07.31：新增 AT ThingConnect 协议体验并更新 S3/P4 完整应用

### 信息架构

- 增加独立的 ESP32-S3 AT ThingConnect 协议体验入口，不与最小 TiRTC 调用链混在一起。
- 该项目后来在 2026.08.13 迁入 `minimal-system-examples/esp32-s3/`，并统一使用“最小系统例子”名称。
- 根目录现在只突出最小集成、最小系统例子、完整应用、文档和必要仓库文件。

### ESP32-S3 最小系统例子 0.3.0

- 增加面向普通开发者的中文简洁日志、UTF-8 安全输出和串口 AT facade。
- 增加五组 AI 语音提示、两秒音频与八秒 H264 测试素材，以及 AI prompt 生成入口。
- 保留 ThingConnect 绑定、正式 MQTT、AI 对讲、联系人解析、设备互呼和竞态回归能力。
- 清理可重建且含供应方临时路径的 SDK 符号扫描输出。

### ESP32-S3 Device Monitor 1.8.0

- “小钛”新增联系人状态查询，以及按联系人备注或 ID 发起设备语音/视频呼叫和微信语音呼叫。
- 设备呼叫补齐 `call_type` 校验与生命周期路由；微信联系人兼容多种响应和字段别名。
- 增加联系人备注异步更新、NVS 缓存 schema v2 和九个常用备注预设。
- 新增 43 组文字的 `10/12/16` 三种尺寸资源，共 129 张运行时 PNG。
- TiRTC SDK 保持 `2.2.0`，分区表和依赖锁未变化。

### ESP32-P4 Device App 1.3.0

- 增加 AI 联系人查询、设备呼叫和微信呼叫，并修复嵌套 tool-call 的动作解析优先级。
- 微信 `audio/video` 类型贯穿应用、服务、ThingConnect、会话和媒体链路。
- 订阅成功后 `1s` 无首包时仅补发一次幂等视频订阅；WHIP 超时执行会话级清理。
- P4 IPC 上行保持 `1280x960@20fps`、`4Mbps` H264；设备呼叫和微信设备上行使用
  `480x320@15fps`、`800kbps` 起始码率 H264；微信下行请求 `640x480` MJPEG，并由硬件
  JPEG 解码后显示到 `480x320`。微信客户端原始采集分辨率不由本固件配置。
- TiRTC SDK 保持 `2.3.0`；本项目继续按源码范围交付。

### 发布

- 当次涉及最小系统例子 `0.3.0`、S3 Device Monitor `1.8.0` 和 P4 Device App `1.3.0`；
  该批次对应的公开项目 Tag 分别为 `esp32-s3-minimal-system-v0.8.0`、`esp32-s3-device-monitor-v1.8.0`
  和 `esp32-p4-device-monitor-v1.3.2`。
- ESP32-S3 最小系统例子和 S3 Device Monitor 的正式构建资产只通过 GitHub Releases 分发。
- ESP32-P4 Device App 不生成占位固件；其他平台和最小示例保持上一公开版本。

## 2026.07.30：ESP32-S3 AT ThingConnect 协议体验 0.2.0

### 新增

- 增加与 ESP32-S3 最小示例平级的 AT ThingConnect 协议示例。
- 设备侧配网、绑定、状态、联系人、AI 对讲和普通呼叫全部通过串口 AT 操作。
- 集成 ThingConnect 服务发现、Report、设备 token、正式 MQTT、解绑重绑和 token 刷新。
- 集成 AI token、WHIP、字幕、打断，以及内建 `call_device` 动作到普通设备呼叫的原子交接。
- 增加普通设备呼叫的主叫、被叫、接听、拒绝、取消、挂断和会话恢复。
- 增加预录 G711A/H264 媒体、双串口回归工具、定向竞态测试和脱敏证据格式。
- 增加使用说明、AI 呼叫设备体验流程、架构、AT 协议、上游契约和验证文档。

### 发布

- 正式版本为 `0.2.0`，TiRTC SDK 为 `2.2.1`，ESP-IDF 为 `5.5.4`。
- 该版本当时以 AT ThingConnect 示例身份发布；当前维护版本已归入最小系统例子，公开 Tag 为
  `esp32-s3-minimal-system-v0.8.0`。
- 正式 `0x0` 完整镜像、manifest 和 SHA-256 只通过 GitHub Release 分发。
- 工程不包含微信 VoIP、OTA、SoftAP 网页配网、UI 或板级音视频外设驱动。

## 2026.07.30：六项目统一发布

### 版本

- ESP32-S3 最小 TiRTC 集成示例更新为 `1.2.0`，TiRTC SDK `2.2.1`，来源 Tag `v1.2.0`。
- ESP32-P4 最小 TiRTC 集成示例更新为 `1.1.1`，TiRTC SDK `2.2.1`，来源 Tag `v1.1.1`。
- G32S10X 最小 TiRTC 集成示例更新为 `0.8.3`，TiRTC SDK `2.2.1`，来源 Tag `v0.8.3`。
- ESP32-S3 Device Monitor 更新为 `1.7.6`，TiRTC SDK `2.2.0`，来源 Tag `v1.7.6`。
- ESP32-P4 Device App 更新为 `1.2.3`，TiRTC SDK `2.3.0`，来源 Tag
  `esp32-p4-device-app-v1.2.3`。
- G32S10X Device Monitor 首次纳入完整应用，版本 `0.1.1`，TiRTC SDK `2.2.1`，
  来源 Tag `v0.1.1`。

### 结构

- 保持 `sdk-integration-examples/`、`complete-applications/` 和 `docs/` 三个对外入口。
- 三个平台分别提供最小 TiRTC 集成示例和完整应用，共六个公开项目。
- 统一对外项目表、文档导航、版本来源和固件下载边界。

### 构建与交付

- 六个项目均以开发侧不可移动 Tag 和 commit 作为源码交接基线。
- 已核对 Tag 指向、真实 diff、版本字段、公开文件范围、文档链接、凭据与 SDK 哈希。
- 五个固件项目从统一公开 commit 完成正式干净构建，资产通过
  `release-manifest.json` 和 `SHA256SUMS.txt` 绑定。
- ESP32-P4 Device App `1.2.3` 本次按源码范围交付。
- 固件构建产物只通过
  [GitHub Releases](https://github.com/tangeai/tirtc-device-example/releases) 分发，
  不进入 Git 历史。
- ESP32-S3/P4 统一使用
  [Espressif ESP Tool](https://espressif.github.io/esptool-js/)；G32S10X 使用君正 Cloner。

## 2026.07.28：设备示例统一发布

### 新增

- 建立 `sdk-integration-examples/`，中文定位为“SDK 接入与历史示例”。
- 纳入 ESP32-S3、ESP32-P4、G32S10X 三个平台的最小 TiRTC 集成示例。
- 平台目录直接作为示例根目录，不把当前 Wi-Fi 实现固化为项目身份。
- 为三个示例增加来源工作树状态、筛选范围和验证边界说明。
- 为 G32S10X 增加供应商 SDK 要求和不含本机绝对路径的构建辅助脚本。
- 建立 `complete-applications/`，中文定位为“完整应用”。
- 增加面向外部开发者的版本与证据、固件下载和项目导航文档。

### 移动

- `esp32-s3/trtc-advanced` -> `complete-applications/esp32-s3/device-monitor`
- `esp32-p4/trtc-advanced` -> `complete-applications/esp32-p4/device-monitor`

### 验证

- 五个公开项目均已从当前候选目录完成干净构建。
- 已完成公开目录、仓库卫生、凭据、文档链接和逐文件 SHA-256 manifest 静态检查。

### 发布边界

- 本次源码范围为三个最小 TiRTC 集成示例，以及 ESP32-S3、ESP32-P4 两个 Device Monitor 完整应用。
- G32S10X 供应商 SDK、工具链、WSL 构建树和完整应用继续按外部依赖或内部工程管理。
- 发布协调脚本、Release manifest 生成逻辑和审阅材料保留在本地内部 Skill。
- 五个项目的干净构建结果作为 Release assets 分发，供开发者继续完成目标板集成验证。
- `.bin/.zip/.elf` 构建产物通过 GitHub Releases 分发，不进入 Git 历史。
