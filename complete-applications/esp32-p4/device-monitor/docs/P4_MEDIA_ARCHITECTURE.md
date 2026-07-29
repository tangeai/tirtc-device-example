# ESP32-P4 媒体架构

本文档说明摄像头、音频、TiRTC、显示和内存的所有权。目标是让每条媒体链路只有一个生命周期
所有者，运行时不通过跨层补丁争抢硬件或连接句柄。

本文对应应用版本 `1.2.3` 和 TiRTC SDK `2.3.0`。版本来源见
[SOURCE_PROVENANCE.md](../SOURCE_PROVENANCE.md)，项目入口见 [README.md](../README.md)。
下列参数描述当前源码设计和默认配置，不单独构成目标板运行证明。

## 分层

| 层 | 责任 |
| --- | --- |
| `drivers` | 摄像头、显示、触摸、音频 codec、DMA 和硬件生命周期 |
| `media` | 摄像头 H264 pipeline、像素格式转换、媒体档位和运行指标 |
| `services` | IPC、设备呼叫、微信 VoIP、AI Chat、绑定和 OTA 等业务服务 |
| `protocols/tirtc` | TiRTC SDK、连接句柄、订阅、回调和媒体收发队列 |
| `application` | 业务进入/退出、资源租约和所有权切换 |
| `ui` | 状态展示和用户动作，不持有媒体设备或连接 |

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

## 摄像头上行

`main/media/camera_pipeline.c` 持有实时 RTC 摄像头：

- IPC 使用 OV5647 `1280x960` YUV420 和 ESP32-P4 H264 硬编。
- 传感器输出与编码器输入尺寸一致时走 YUV420 direct，不增加 RGB565 中转。
- 热路径为 `camera_driver -> camera_pipeline -> H264 encoder -> tirtc_session`。
- RTC 上行不做本地摄像头预览。
- QR scanner 只在扫码页持有摄像头，离开后释放。
- PSRAM 中的 H264 输入和输出在 DMA 边界使用 `esp_cache_msync`。
- 第一帧必须是完整关键帧，丢失依赖后重新请求 IDR。

设备间呼叫和微信 VoIP 使用 `480x320@15fps`、`800kbps` 起始的独立上行档位；退出通话后恢复 IPC 正常档位。

## 视频下行

`main/services/call_video_renderer.c` 是下行视频的统一 renderer，但 codec path 分开：

### 设备间呼叫

- 接收 constrained-baseline H264。
- 软件 H264 decoder 输出 YUV420。
- PPA 优先完成缩放、裁剪和 RGB565 转换，软件路径作为回退。
- H264 依赖帧丢失时进入 key-frame resync，不继续显示错误参考帧。
- 压缩输入使用 16 个 `256KB` PSRAM slot；输入溢出后标记延迟恢复，在下一次 IDR 到达时
  清空旧依赖链并切换到新一代解码状态。
- 解码任务优先保持 H264 参考链连续；转换任务每处理一帧主动让出一个 tick，避免持续占满调度窗口。

### 微信 VoIP

- 设备能力上报 `down_video_mt=mjpeg`、`screen_width=480`、`screen_height=320`。
- 服务端把微信视频转换为独立 MJPEG 帧。
- P4 hardware JPEG decoder 输出 RGB565。
- PPA 优先完成旋转、居中 `cover` 裁剪和 `480x320` 输出，软件转换作为回退。
  ThingConnect 协议字段独立上报 `object_fit=contain`。
- 竖向 JPEG 默认顺时针旋转 90 度，横向 JPEG保持原方向。
- 压缩输入复用 16 个 `256KB` PSRAM slot；解码和显示使用固定 PSRAM pool。
- MJPEG 帧彼此独立。队列积压时释放旧帧并解码最新帧，避免延迟持续增长。

统一输出由三个 RGB565 slot 组成。控制层可见时由 LVGL 合成；控制层自动隐藏后切换为整帧 PSRAM direct-LCD DMA，点击画面恢复控制层。

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

TiRTC `2.3.0` 的 TGMP 控制器通过 `on_video_bitrate_required()` 给出绝对目标码率。回调只投递
事件，应用控制任务更新 media governor 和硬件编码器。双向通话档位为 `200-800kbps`；
IPC 等其他档位按当前基准码率和画面规模计算范围。

该控制器保留为可选策略，默认关闭；启用后拥塞降码率立即生效，恢复升码率需经过稳定等待
并分级上调。旧的本地队列压力自动降级也默认关闭，避免两个控制器竞争。

## 默认参数

| 项目 | 默认值 |
| --- | --- |
| IPC | `1280x960@20fps`, `4Mbps` |
| 双向通话/微信上行 | `480x320@15fps`, `800kbps` 起始，TGMP `200-800kbps` |
| H264 GOP | `30` |
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

## 建议验证

以下项目用于开发者完成构建后的目标板验证：

1. 检查横屏显示和触摸坐标。
2. 检查绑定、正式 MQTT 和 TiRTC 上线。
3. 分别检查 IPC、设备呼叫、微信 VoIP 和 AI Chat。
4. 对微信 VoIP 确认 H264 上行与 MJPEG 下行均有首帧证据。
5. 保持每个主要场景至少 5 分钟，观察 fps、bitrate、queue、DMA largest block、PSRAM pool
   和 AEC。
6. 每个场景连续进入和退出至少 10 次，确认无残留资源和连接句柄。
