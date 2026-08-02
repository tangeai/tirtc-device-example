# ESP32-P4 媒体架构

本文档说明摄像头、音频、TiRTC、显示和内存的所有权。目标是让每条媒体链路只有一个生命周期
所有者，运行时不通过跨层补丁争抢硬件或连接句柄。

本文对应应用版本 `1.3.1` 和 TiRTC SDK `2.3.0` 定制兼容快照。版本来源见
[SOURCE_PROVENANCE.md](../SOURCE_PROVENANCE.md)，项目入口见 [README.md](../README.md)。
环境、构建、烧录和首次启动见 [GETTING_STARTED_CN.md](GETTING_STARTED_CN.md)。
下列参数描述当前源码设计和默认配置。公开候选已完成 ESP-IDF `5.5.4` 干净构建；构建通过
不单独构成烧录、联网、媒体运行或长稳证明。

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
| PSRAM | SDIO streaming RX、H264/JPEG payload、RTC TX pool、解码帧、RGB565 帧、HTTP/MQTT 工作区和后台 task stack | 显式 `MALLOC_CAP_SPIRAM`，固定池优先于实时动态扩容 |

启动早期按顺序预热：

1. P4 JPEG decoder 的内部 DMA 描述符。
2. DMA escrow。
3. H264 encoder。
4. RTC 视频发送池。
5. 视频缩放/旋转工作区和下行显示池。
6. AEC 工作集。

这些资源在服务发现、绑定、MQTT、TiRTC 和 UI 分配长期内存前占位。视频大块留在 PSRAM，
内部 RAM 只承担硬件必须的描述符和实时控制。

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
- pipeline 只按 RTC 目标节拍取帧。15fps 使用向上取整的 `67ms` 间隔，20fps 使用 `50ms`；
  超过一个周期时按错过的周期数推进原相位，不从当前时刻重新起算。
- GOP 由 `APP_MEDIA_H264_GOP_DURATION_MS=2000` 和当前 fps 计算，IPC 为 `40` 帧，通话为
  `30` 帧。这样两种档位都保持 2 秒 IDR 周期。

设备间呼叫和微信 VoIP 从 P4 设备向服务端发送 `480x320@15fps` H264，起始码率
`800kbps`；退出通话后恢复 IPC 正常档位。

## 视频下行

`main/services/call_video_renderer.c` 是下行视频的统一 renderer，但 codec path 分开：

### 设备间呼叫

- 接收 constrained-baseline H264。
- 软件 H264 decoder 输出 YUV420。
- PPA 优先完成缩放、裁剪和 RGB565 转换，软件路径作为回退。
- H264 依赖帧丢失时进入 key-frame resync，不继续显示错误参考帧。
- 压缩输入使用 16 个 `256KB` PSRAM slot；输入溢出后标记延迟恢复，在下一次 IDR 到达时
  清空旧依赖链并切换到新一代解码状态。
- decoded pool 和 output pool 各使用 `4` 个 RGB565 slot；队列传递 slot index，不复制大帧。
- 解码任务优先保持 H264 参考链连续；转换任务每处理一帧主动让出调度窗口，避免持续占满 CPU。
- TinyH264 helper 固定在 CPU1、优先级 `17`；decoder caller 优先级 `16`。helper 高于同步等待它的
  caller，同时低于实时音频采集，避免同步等待方把 helper 饿死。
- 双任务同步保护识别 TinyH264 的两个阶段通知。前一通知仍未被 helper 消费时，保护层等待后
  送达原阶段值，不使用覆盖通知，也不把通知失败当作正常推进。
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

统一输出由 `4` 个 RGB565 slot 组成。控制层可见时由 LVGL 合成；控制层自动隐藏后切换为
整帧 PSRAM direct-LCD DMA，点击画面恢复控制层。

## 微信会话 worker

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

本地上行 liveness 按 `enq -> deq -> SDK API -> send OK` 记录年龄、队列、free slot、capture
状态和 SDK buffer 水位。远端下行把停滞分为：

- `transport`：订阅后没有包，或远端包停止到达；可以执行有界订阅修复。
- `renderer`：SDK callback 已收到包，但 renderer 提交没有推进；可以请求关键帧并检查解码链。

renderer 自身再区分 `input` 和 `decode` stall。周期统计分别记录 received、queued、decoded、
converted、presented fps，输入/显示队列深度、access unit/转换阶段耗时、queue age、PTS 回退
和画面间隔。这样能定位第一处停止推进的阶段，不用根据“最后看到黑屏”猜责任层。

TiRTC `2.3.0` 的 TGMP 控制器通过 `on_video_bitrate_required()` 给出绝对目标码率。回调只投递
事件，应用控制任务更新 media governor 和硬件编码器。双向通话档位为 `200-800kbps`；
IPC 等其他档位按当前基准码率和画面规模计算范围。

该控制器保留为可选策略，默认关闭；启用后拥塞降码率立即生效，恢复升码率需经过稳定等待
并分级上调。旧的本地队列压力自动降级也默认关闭，避免两个控制器竞争。

## 媒体参数事实源

运行时媒体策略集中在：

- `main/media/media_tuning.h`：摄像头尺寸、fps、码率、QP、2 秒 GOP、payload 和 TGMP 参数。
- `main/services/call_video_renderer_config.h`：下行池、任务优先级、TinyH264 双任务、节拍和
  stall 阈值。
- `main/protocols/tirtc/tirtc_session_options.h`：TiRTC 发送池、水位和 liveness 时间。

`main/Kconfig.projbuild` 继续管理构建组成、硬件特性和诊断开关；生成的 `sdkconfig` 不能成为
运行时媒体数值的唯一事实源。调整数值后要重新构建，并用相同触发条件验证上、下游契约。

## 默认参数

| 项目 | 默认值 |
| --- | --- |
| P4 设备 -> 服务端（IPC） | `1280x960@20fps`, `4Mbps`，GOP `40` 帧 / `2s` |
| P4 设备 -> 服务端（双向通话/微信 VoIP） | `480x320@15fps`, `800kbps` 起始，TGMP `200-800kbps`，GOP `30` 帧 / `2s` |
| 服务端 -> P4 设备（微信 VoIP） | 请求 `640x480` MJPEG，显示到 `480x320` |
| H264 downlink input | `16 x 256KB` PSRAM slot |
| H264 decoded/output | 各 `4` 个 RGB565 slot |
| H264 output buffer | `1MB` |
| Max delta payload | `256KB` |
| Startup max delta payload | 首 `2500ms` 为 `128KB` |
| TiRTC SDK bitrate adaptation | 关闭 |
| Legacy local auto adaptation | 关闭 |
| Wait subscribe before capture | 关闭 |
| Direct LCD with auto-hidden controls | 开启 |

## 失败边界

- 丢弃二维码预览帧不能停止 RTC。
- 丢弃视频帧或显示帧不能关闭 TiRTC 连接。
- JPEG/H264 单帧解码失败只能丢帧并保留下一次恢复机会。
- 媒体队列必须有界；业务退出后队列、帧 slot 和连接状态归零。
- 日志只保留首帧、状态转换、周期汇总和可执行错误，不按帧刷屏。
- TinyH264 decode 超时后的隔离不等于强制回收。第三方调用永久不返回时，当前实现不能在未知
  内部锁状态下安全删除 task；这项边界必须通过故障注入和长稳验证继续收口。

## 建议验证

公开候选已经完成 ESP-IDF `5.5.4` 干净构建，应用镜像为 `6,924,512` bytes，SHA-256 为
`EBD5FE3B930BA000FDBE7094F287AD66CBB745D56F8D167ED4890895A691DFA5`。由该构建生成的
`16MB` 完整镜像只上传 GitHub Release，不进入 Git；它不改变以下目标板验证要求：

1. 检查横屏显示和触摸坐标。
2. 检查绑定、正式 MQTT 和 TiRTC 上线。
3. 分别检查 IPC、设备呼叫、微信 VoIP 和 AI Chat。
4. 对微信 VoIP 分别确认 P4 设备发送的 `480x320` H264 和服务端下发的 MJPEG 均有首帧证据，
   并记录服务端实际下发分辨率。
5. 保持每个主要场景至少 5 分钟，观察 fps、bitrate、queue、DMA largest block、PSRAM pool
   和 AEC。
6. 每个场景连续进入和退出至少 10 次，确认无残留资源和连接句柄。
7. 对 H264 下行做连续呼叫和故障注入，确认 sync guard 后链路继续推进，并记录 decode 超过
   `2s` 时原调用是否返回、decoder 是否能从新 IDR 重建。
