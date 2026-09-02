# ESP32-P4 媒体架构

本文档说明摄像头、音频、TiRTC、显示和内存的所有权。目标是让每条媒体链路只有一个生命周期
所有者，运行时不通过跨层补丁争抢硬件或连接句柄。

本文对应应用版本 `1.5.3` 和 TiRTC SDK `2.3.0` P4 验证重建与补丁版。版本来源见
[SOURCE_PROVENANCE.md](../SOURCE_PROVENANCE.md)，项目入口见 [README.md](../README.md)。
环境、构建、烧录和首次启动见 [GETTING_STARTED_CN.md](GETTING_STARTED_CN.md)。
下列参数描述当前源码设计和默认配置。相比公开 `1.5.1`，本版本更新微信 VGA 上行、H264
输出工作区、SDK 传输与栈补丁、Hosted RPC 恢复，并要求 RTC 服务地址保持 HTTPS。
SDK 公开归档只移除调试信息，运行节、重定位和归档成员保持等价；SDK 行号调试需使用原始库。
这些源码与归档结论不构成烧录、联网、媒体运行或长稳证明。

本文中的上行和下行均以 P4 设备为参照：`P4 设备 -> 服务端` 为上行，
`服务端 -> P4 设备` 为下行。微信客户端采集端的原始分辨率不由本工程配置。

## 分层

| 层 | 责任 |
| --- | --- |
| `drivers` | 摄像头、显示、触摸、音频 codec、DMA 和硬件生命周期 |
| `media` | 摄像头 H264 pipeline、像素格式转换、媒体档位和运行指标 |
| `services` | IPC、设备呼叫、微信 VoIP、AI Chat、绑定和 OTA 等业务服务 |
| `protocols/tirtc` | TiRTC SDK、连接句柄、订阅、回调和媒体收发队列 |
| `application` | 业务进入/退出、资源租约和所有权切换 |
| `ui` | 状态展示和用户动作，不持有媒体设备或连接 |

AI Chat 的 `device_action` 可查询联系人状态，并发起设备联系人或微信联系人的音频/视频呼叫。
服务层负责解析动作、联系人别名和备注，应用层异步切换业务并持有生命周期；UI 只展示结果，
不直接创建或释放 RTC 连接。呼叫类型从 AI 请求贯通到对应呼叫服务。

## 内存

P4 使用 capability-based allocation，不把所有 heap 当作可互换内存。

| 内存 | 长期所有者 | 策略 |
| --- | --- | --- |
| Internal RAM | DMA 描述符、实时控制队列、mutex、音频 I/O 控制、flash/NVS task stack | 显式 `MALLOC_CAP_INTERNAL`，媒体分配失败时不回退到这里 |
| DMA-capable internal RAM | ESP-Hosted 描述符、摄像头/H264/JPEG 驱动和 DMA escrow | IDF 预留 `192KB`，运行时 escrow `96KB`，不存放长期大块 SDIO payload |
| PSRAM | SDIO streaming RX、摄像头 USERPTR、H264 reference/deblocking、H264/JPEG payload、RTC TX pool、解码帧、RGB565 帧、AEC、HTTP/MQTT 工作区和后台 task stack | 显式 `MALLOC_CAP_SPIRAM`，持久固定池优先于实时动态扩容 |

启动早期按顺序预热：

1. P4 JPEG decoder 的内部 DMA 描述符。
2. DMA escrow。
3. 摄像头 USERPTR 与 H264 encoder reference/deblocking 持久池。
4. RTC 视频发送池。
5. 视频缩放/旋转工作区和下行显示池。
6. AEC 工作集。

这些资源在服务发现、绑定、MQTT、TiRTC 和 UI 分配长期内存前占位，并由跨 APP 媒体所有权
统一复用。视频大块留在 PSRAM，内部 RAM 只承担硬件必须的描述符和实时控制。业务退出时
清理会话代际、队列和引用，不通过反复释放、重建大块池制造碎片。

直编码 H264 输出工作区也由 camera pipeline 持久持有，按所需容量取得独占使用权，关闭
编码器时归还使用权而不释放大块 PSRAM。工作区被占用或分配失败时保留失败语义，不另建
并发编码所有者。

RTC 视频发送池使用固定 PSRAM slot；音频发送和播放缓冲也使用独立固定池。队列只保存描述符
和 slot index，不保存大 payload。后台网络、媒体和 UI task 使用 PSRAM stack；
flash/NVS、实时音频和小型应用控制 task 保留 internal stack。

ESP-Hosted streaming RX 使用两个 `64KB` cache-aligned PSRAM DMA 缓冲，只有 PSRAM DMA
分配失败时才回退到 `4KB` internal DMA 缓冲。这样把 Wi-Fi burst payload 留在 PSRAM，
同时保护 H264、JPEG 和音频依赖的内部 DMA 连续块。

应用通过统一快照记录 internal、DMA 和 PSRAM 的 free、largest block、minimum free，以及
PSRAM 分配失败次数。运行水位分为：

| 水位 | Internal free / largest | PSRAM free / largest |
| --- | --- | --- |
| warning | 低于 `64KB` / `24KB` | 低于 `4MB` / `2MB` |
| critical | 低于 `32KB` / `8KB` | 低于 `2MB` / `1MB` |

正常状态不周期刷屏；只在 `normal/warning/critical` 转换、恢复或出现新的 PSRAM 分配失败时
记录 `memory waterline`。free 恢复但 largest block 持续下降时，应优先排查碎片化，而不是
简单增加重试。

### NVS internal-RAM worker

`main/platform/platform_nvs_async.c` 是运行时持久化的统一入口。调用方把 namespace、key 和
value 复制到控制内存后，由 internal-RAM task 串行执行 `open -> set/erase -> commit -> close`。
队列中传递请求指针，不让 PSRAM task stack 直接进入 flash/NVS 操作。

- 设备 UUID、音量、AI 头像、RTC 凭证和绑定 pending session 复用同一 worker。
- 需要确认落盘后才能继续的操作使用 `*_and_wait()`；worker 完成 commit 后才唤醒调用方。
- 无需同步等待的 legacy key 清理由同一队列按提交顺序执行。
- 绑定 token reset 回调不直接读写 NVS或启动绑定，而是向 APP control queue 投递
  `DEVICE_REBIND_REQUIRED`，由应用层接管后续生命周期。

这条链路解决的是 NVS 执行上下文和顺序所有权。它不等同于已经完成掉电、队列满、flash
错误或连续重绑定真机验证；这些情况仍需按实际日志检查第一项失败。

## P4 设备视频上行

`main/media/camera_pipeline.c` 持有实时 RTC 摄像头：

- IPC 使用 OV5647 `1280x960` YUV420 和 ESP32-P4 H264 硬编。
- 传感器输出与编码器输入尺寸一致时走 YUV420 direct，不增加 RGB565 中转。
- 热路径为 `camera_driver -> camera_pipeline -> H264 encoder -> tirtc_session`。
- RTC 上行不做本地摄像头预览。
- QR scanner 只在扫码页持有摄像头，离开后释放。
- PSRAM 中的 H264 输入和输出在 DMA 边界使用 `esp_cache_msync`。
- 第一帧必须是完整关键帧，丢失依赖后重新请求 IDR。
- camera driver 记录最后交付的 V4L2 `sequence`，排出旧完成帧并保证上层拿到更新的帧。
- pipeline 只按 RTC 目标节拍取帧。12fps 使用向上取整的 `84ms` 间隔，15fps 使用 `67ms`，
  20fps 使用 `50ms`；
  超过一个周期时按错过的周期数推进原相位，不从当前时刻重新起算。
- GOP 按媒体档位计算：IPC 为 `40` 帧 / `2s`；设备呼叫名义值为 `192` 帧 / `16s`；
  微信上行为 `30` 帧 / `2s`。流开始、订阅恢复、传输恢复和对端请求仍会强制关键帧。

设备间呼叫从 P4 发送 `384x256@12fps`、`256kbps` H264；微信 VoIP 使用独立的
`640x480@15fps`、目标 `800kbps` H264 档位。退出通话后恢复 IPC 正常档位。

## 视频下行

`main/services/call_video_renderer.c` 是下行视频的统一 renderer，但 codec path 分开：

### 设备间呼叫

- 接收 constrained-baseline H264。
- 软件 H264 decoder 输出 YUV420。
- PPA 优先完成缩放、裁剪和 RGB565 转换，软件路径作为回退。
- H264 依赖帧丢失时进入 key-frame resync，不继续显示错误参考帧。
- 压缩输入使用 24 个 `256KB` PSRAM slot；输入溢出后标记延迟恢复，在下一次 IDR 到达时
  清空旧依赖链并切换到新一代解码状态。
- decoded pool 使用 `4` 个 slot，output pool 使用 `20` 个 RGB565 slot；自适应播放队列深度
  上限为 `16`，队列只传递 slot index。
- 当前产品路径关闭 TinyH264 双任务 helper，由单一 decoder owner 在 SMP 上调度，避免第三方
  slice/deblock helper 竞态；双任务参数仅保留给受控实验。
- 解码目标上限为 `384x256`。转换任务通过 PPA 一次缩放到 `480x320`，每帧后主动让出调度窗口，
  避免持续占满 CPU。
- access unit decode 超过 `2s` 时标记 decoder fault 并隔离后续输入，同时记录 caller、helper、
  音频采集和播放 task 状态。只有原 decode 最终返回后才能安全销毁 decoder 并等待新 IDR 重建。

### 微信 VoIP

- P4 设备上报 `down_video_mt=mjpeg`、`screen_width=640`、`screen_height=480`，请求服务端
  提供标准横屏 MJPEG 下行。这组值不代表物理 LCD；本机显示目标固定为 `480x320`。
- 本工程不配置微信手机端采集分辨率，也没有 720p 采集声明；手机端原始视频由微信和
  服务端链路决定。
- 服务端把微信视频转换为独立 MJPEG 帧。
- P4 hardware JPEG decoder 输出 RGB565。
- 解码入口支持不超过 `640x480` 解码预算的服务端实际帧；服务端可根据链路临时下发
  较小档位。
- TiRTC JPEG 帧头不携带旋转元数据。本机按服务端实际下发方向显示，MJPEG 渲染固定使用
  `cw0`；PPA 只执行裁切和缩放，不承担方向修正。
- PPA 基于服务端实际帧执行一次居中 `cover`：大于显示视口的帧对称裁切并等比缩小到
  `480x320`，较小档位等比放大后居中裁切，不做非等比拉伸或第二次显示缩放。
- ThingConnect 协议字段独立上报 `object_fit=contain`；设备上行另行上报摄像头方向提示，
  不改变本机 H264 帧内容。
- 压缩输入复用 16 个 `256KB` PSRAM slot；解码和显示使用固定 PSRAM pool。
- MJPEG 帧彼此独立。队列积压时释放旧帧并解码最新帧，避免延迟持续增长。
- WHIP 连接接受后立即订阅远端视频；订阅调用成功但 `1s` 内仍无首个视频包时，仅补发
  一次幂等订阅，避免信令刚就绪时的单次请求丢失。

微信显示复用固定 RGB565 pool。控制层可见时由 LVGL 合成；控制层 `5s` 后自动隐藏并切换为
整帧 PSRAM direct-LCD DMA，点击画面恢复控制层。

## 微信会话 worker

P4 主动呼叫默认携带正式版 `wx_version_type=0`。该设置由微信服务配置持有，S3 的体验版
`2` 不随本次 P4 发布改变。

微信会话把可能阻塞 SDK 或网络的拒绝、断开等操作放入固定 work worker 串行执行。接听路径
使用独立常驻 worker：大栈只在启动时从 PSRAM 分配一次，每次接听请求用递增序号隔离；请求
被取消或被新请求替代后，旧 worker 结果不能提交到当前会话。业务状态仍由会话 generation
和 application owner 决定，worker 不越过应用生命周期直接切页面或抢媒体设备。

## 音频和 AEC

音频 ownership 由 application policy 统一管理：

- IPC、设备呼叫和微信 VoIP 使用 RTC media owner。
- AI Chat 使用独立 media owner。
- 进入业务时先 prepare 自适应播放缓冲，媒体真正 active 后启用 AEC。
- AEC 优先使用 codec 同步 DAC reference；无法锁定时使用 `80ms` 软件延迟参考。
- 退出业务时停止采集、播放和 AEC 处理，但保留预热工作区供后续会话复用。

播放控制器根据 underflow、积压和抖动调整目标缓冲，不通过长期固定大延迟掩盖弱网。

## TiRTC 发送与码率

`main/protocols/tirtc/tirtc_session.c` 持有连接和发送队列：

- 视频进入预分配 PSRAM TX pool。
- 发送任务优先丢弃过期视频，不让旧帧无界堆积。
- 音频使用独立队列，视频启动期间只允许有界延后。
- 无效句柄、远端关闭和 teardown 在协议层与应用层闭环，UI 不直接释放连接。

### RTC/WHIP 连接生命周期

RTC 配置先使用 URL parser 检查服务地址：要求显式 `https://` 与非空 host，拒绝 userinfo、
fragment、非法端口和未终止字符串。RTC 关闭时允许空地址；拒绝配置不会改写现有状态或
触发 SDK 重置。应用不再按 SDK 版本降级到 HTTP，证书链、主机名和校验结果仍由 SDK 检查。

连接状态由 `tirtc_session` 在 critical section 中统一持有。当前实现把“可以发起新连接”定义为：
网络在线、SDK 已初始化且已启动、没有 prepare/start/stop、没有 active connection、没有
closing connection，也没有进行中的 WHIP attempt。

- 每次 WHIP 提交先取得非零 attempt ID，原子占住空闲窗口；并发提交会在调用 SDK 前返回 busy。
- SDK 同步拒绝、拿不到 SDK API lock 或异步回调完成时释放对应 attempt；网络离线和 SDK 停止
  会清理当前 attempt。
- producer 在投递 accepted event 前先认领连接。event 排队期间生命周期已经变化时，consumer
  只把它记为 stale event，不对同一句柄再次 disconnect。
- 连接接受结果区分正常拒绝和 `STALE_CLOSING`。已经进入 closing 的句柄命中过期回调时直接
  忽略，避免 SDK connection double-destroy。
- `tirtc_session_disconnect_connection(conn)` 先原子脱离 active owner，再投递关闭请求；同一
  句柄已经在关闭时重复调用保持幂等。
- AI Chat 在获取 Token 前调用 RTC ready 检查，并在等待期间持续核对 generation。用户退出或
  新任务替代旧任务后，旧启动流程不能继续提交连接；超时清理使用具体连接句柄。

这些规则保护连接所有权，但不把 SDK 永不回调等外部故障伪装成成功。验证时应保留 attempt、
active、closing、generation 和 disconnect 时间线，确认每轮会话关闭后下一轮才开始。

本地上行 liveness 按 `enq -> deq -> SDK API -> send OK` 记录年龄、队列、free slot、capture
状态和 SDK buffer 水位。远端下行把停滞分为：

- `transport`：订阅后没有包，或远端包停止到达；可以执行有界订阅修复。
- `renderer`：SDK callback 已收到包，但 renderer 提交没有推进；可以请求关键帧并检查解码链。

renderer 自身再区分 `input` 和 `decode` stall。周期统计分别记录 received、queued、decoded、
converted、presented fps，输入/显示队列深度、access unit/转换阶段耗时、queue age、PTS 回退
和画面间隔。这样能定位第一处停止推进的阶段，不用根据“最后看到黑屏”猜责任层。

TiRTC `2.3.0` 的 TGMP SDK 回调 `on_update_bitrate()` 给出绝对目标码率；协议层内部 observer
名为 `on_video_bitrate_required`。回调只投递事件，应用控制任务更新 media governor 和硬件
编码器。设备呼叫 compact 档位按 `96-256kbps` 有界调整；连接先以正常 `256kbps` 档启动，
TGMP 注册起点为 `224kbps`，收到传输反馈后才调整。IPC 和微信按各自基准码率与画面规模计算。

SDK/TGMP 控制器默认开启；拥塞降码率立即生效，恢复升码率需经过稳定等待并分级上调。
旧的本地队列压力自动降级默认关闭，避免两个控制器竞争。

## Hosted 与 SDIO 恢复边界

网络 owner 在 Wi-Fi 连接、断开或配置 RPC 返回错误时请求 Hosted 重建，记录触发原因、
重建步骤和失败返回值。Hosted/STA netif 重建后仍要等待 got-IP，再由上层分别恢复 MQTT、
TiRTC 和业务连接；重建完成日志不代表业务已经恢复。

当前依赖没有 `esp_hosted_event.h`，`WIFI_HOSTED_EVENT_RECOVERY_SUPPORTED` 为 `0`。
传输事件、C6 心跳与心跳超时恢复分支不参与编译，不能把源码中保留的条件分支当作本次
固件能力。

SDIO 读取全 `0xff` 寄存器快照时最多重试 3 次，间隔 `200us`；读数和包长通过检查后才
确认中断，清中断失败也保留错误。TX throttle 起止、持续时间与丢包计数用于区分发送节流
和媒体停顿。这些是防护与恢复机制；异常读数的物理根因尚未证实，仍需供电、C6/SDIO 和
故障注入证据。

## 媒体参数事实源

运行时媒体策略集中在：

- `main/media/media_tuning.h`：摄像头尺寸、fps、码率、QP、分档 GOP、payload 和 TGMP 参数。
- `main/services/call_video_renderer_config.h`：下行池、任务优先级、TinyH264 双任务、节拍和
  stall 阈值。
- `main/protocols/tirtc/tirtc_session_options.h`：TiRTC 发送池、水位和 liveness 时间。

`main/Kconfig.projbuild` 继续管理构建组成、硬件特性和诊断开关；生成的 `sdkconfig` 不能成为
运行时媒体数值的唯一事实源。调整数值后要重新构建，并用相同触发条件验证上、下游契约。

## 默认参数

| 项目 | 默认值 |
| --- | --- |
| P4 设备 -> 服务端（IPC） | `1280x960@20fps`, `4Mbps`，GOP `40` 帧 / `2s` |
| P4 设备 -> 服务端（设备呼叫） | `384x256@12fps`, `256kbps`，TGMP `96-256kbps`，名义 GOP `192` 帧 / `16s` |
| P4 设备 -> 服务端（微信 VoIP） | `640x480@15fps`，目标 `800kbps`，GOP `30` 帧 / `2s` |
| 服务端 -> P4 设备（微信 VoIP） | 请求 `640x480` MJPEG，实际帧可以更小，`cover` 到 `480x320` |
| H264 downlink input | `24 x 256KB` PSRAM slot |
| H264 decoded/output | decoded `4` 个、output `20` 个 RGB565 slot；playout 深度上限 `16` |
| H264 output buffer | `1MB` |
| Max delta payload | `256KB` |
| Startup max delta payload | 首 `2500ms` 为 `128KB` |
| TiRTC SDK/TGMP bitrate adaptation | 开启 |
| Legacy local auto adaptation | 关闭 |
| Wait subscribe before capture | 关闭 |
| Direct LCD with auto-hidden controls | 开启 |

码率均为编码目标，不是实测吞吐保证。微信 MJPEG 下行没有固定帧率或码率声明。

## 失败边界

- 丢弃二维码预览帧不能停止 RTC。
- 丢弃视频帧或显示帧不能关闭 TiRTC 连接。
- JPEG/H264 单帧解码失败只能丢帧并保留下一次恢复机会。
- 媒体队列必须有界；业务退出后队列、帧 slot 和连接状态归零。
- 日志只保留首帧、状态转换、周期汇总和可执行错误，不按帧刷屏。
- TinyH264 decode 超时后的隔离不等于强制回收。第三方调用永久不返回时，当前实现不能在未知
  内部锁状态下安全删除 task；这项边界必须通过故障注入和长稳验证继续收口。

## 建议验证

本次 `1.5.3` 已完成 ESP-IDF `5.5.4` 正式干净构建，app 为 `6,973,376` bytes，APP 分区
剩余 `563,264` bytes（`7.47%`）。16 MiB 完整镜像只作为 GitHub Release 资产，不进入 Git 历史；
实际烧录分段和哈希以本次 manifest 为准。目标板验证至少包括：

1. 检查横屏显示和触摸坐标。
2. 检查绑定、正式 MQTT 和 TiRTC 上线。
3. 连续完成至少两次重置绑定、重新绑定和重启，核对 NVS commit 与 APP control event 顺序。
4. 快速切换 AI Chat、设备呼叫和微信呼叫，确认同一时刻只有一个 WHIP attempt，过期回调
   不会再次销毁 closing connection，重复 disconnect 保持幂等。
5. 分别检查 IPC、设备呼叫、微信 VoIP 和 AI Chat。
6. 对微信正式版 VoIP 分别确认 P4 设备发送的 `640x480` H264 和服务端下发的 MJPEG 均有
   首帧证据，并记录服务端实际下发分辨率。
7. 保持每个主要场景至少 5 分钟，观察 fps、bitrate、queue、DMA largest block、持久 PSRAM
   pool 和 AEC；单独确认设备呼叫 `384x256@12fps` 弱网恢复。
8. 每个场景连续进入和退出至少 10 次，确认无残留资源和连接句柄。
9. 对 H264 下行做连续呼叫和故障注入，确认 persistent reference/deblocking pool 没有跨会话
   残留，记录 decode 超过 `2s` 时原调用是否返回、decoder 是否能从新 IDR 重建。
10. 用静态画面与持续高运动画面做对照，覆盖同一设备入站/出站弱网，记录编码耗时、帧大小、
    码率、发送队列和内存水位；不能用静态场景证明高运动压力下的余量。
11. 分别验证有效 HTTPS 成功、错误证书与错误主机名失败，以及无效 URL 被入口拒绝。
12. 触发 Wi-Fi RPC 错误与 SDIO 异常，核对首个错误、重建步骤、got-IP 和业务恢复，不把重试
    后暂时恢复当作物理故障根因修复。
