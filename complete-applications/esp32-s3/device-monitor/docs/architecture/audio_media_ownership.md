# 音频和媒体所有权

`1.9.5` 的 RTC 产品能力是双向音频。Web IPC、小钛、微信 VoIP 和设备互呼共享同一套物理
麦克风、扬声器和 TiRTC 运行时，但每条业务拥有自己的会话状态、流号、队列和退出路径。
摄像头不属于 RTC 媒体资源，只由二维码扫描流程短时申请。

## 四条音频链路

| 应用 | 音频 owner | 采集入口 | 发送方式 | 关键边界 |
| --- | --- | --- | --- | --- |
| Web IPC | TiRTC built-in media | 主麦克风 callback | local audio stream | 进入查看页必须恢复 built-in owner，不继承 AI/VoIP 外部音频状态 |
| 小钛 | AI Chat 服务 | 麦克风 observer | WHIP command/audio frame | `start_session` 成功后才上行；退出按 generation 取消旧回调 |
| 微信 VoIP | WeChat VoIP 服务 | 麦克风 observer | external audio | 入会后标记 external audio；挂断、失败和超时都要解除 |
| 设备互呼 | Device Call + TiRTC session | 主麦克风 callback | local audio stream | 只支持 `call_type=audio`；显式 CALL gate 使用 A-law；房间和 `0x2000` 确认后启动媒体 |

`APP_PRODUCT_RTC_VIDEO_ENABLED=0` 同时约束本地视频发送、远端视频订阅、关键帧请求和相关
任务创建。任何业务都不能绕过产品能力在自己的服务层重新开启视频。

## AEC 与播放路径

通话类业务的采集和播放按下面的责任链处理：

```text
麦克风采集 -> 音频格式整理 -> AEC/上行处理 -> TiRTC 发送
TiRTC 下行 -> 格式校验/解码 -> 抖动缓冲 -> 播放队列 -> 扬声器
                                      |
                                      +-> 远端参考送入 AEC
```

- AEC 使用本机麦克风作为近端语音、扬声器播放内容作为远端参考。
- 下行自适应抖动缓冲负责吸收轻微乱序和到达间隔变化；缺帧补偿不能伪造成真实收到的帧。
- 媒体完整性统计分别记录 checksum failure、missing、duplicate 和 reordered，避免只用“有声”
  或“无声”猜测网络与播放问题。
- 大块抖动队列和非 DMA 音频存储优先放入 PSRAM；DMA、同步对象和实时控制保留 internal RAM。

### 设备上行线格式

| 业务 | 线上格式 | 帧长 |
| --- | --- | --- |
| Web IPC | `8 kHz / 16 bit / mono / G.711 A-law` | `20 ms / 160 bytes` |
| 设备互呼 | `8 kHz / 16 bit / mono / G.711 A-law` | `20 ms / 160 bytes` |
| 微信 VoIP | `8 kHz / 16 bit / mono / G.711 A-law` | `20 ms / 160 bytes` |

`SUBSCRIBED` 是 built-in microphone 路径，`CALL` 是显式采集发送路径。二者在应用选择 A-law
后必须走同一个编码器和媒体类型，不能因为业务入口不同而让其中一条退回 PCM。

### Device Call 专用播放配置

设备互呼使用独立的 `MEDIA_SINK_AUDIO_PROFILE_DEVICE_CALL`：

- 起播和目标水位由 PSRAM PCM 环形缓冲承载；轻微乱序、回调成批到达和时间戳缺口分别
  统计，缺失段只做有界静音补偿，不重复整段语音制造音调噪声。
- 水位低于目标区间时，每个 20 ms 输出周期少消费 4 个 16 kHz PCM 帧；水位高于目标区间
  时多消费 4 帧，约为 `1.25%`。进入与退出使用滞回，干净网络回到 `1.0x`。
- 扬声器仍收到固定 20 ms 数据。速率微调只改变从环形缓冲取出的源帧数，不改变 TiRTC 包、
  时间戳或其他业务的播放时钟。
- AEC 使用全双工高性能线性模式，工作区申请在 PSRAM。AEC 输出后增加 100 Hz 一阶高通，
  通话 AGC 的静态噪声底线提高到已测板端残留之上；噪声门保持关闭。

这些策略用于把缓冲水位拉回稳定区间，并降低对静态残留的继续放大。它们不能证明网络没有
丢包，也不能单独证明主观音质已经达到目标。

### 来电铃声所有权

普通设备 `call_incoming` 到达后，由 Device Call 服务启动本地合成铃声。铃声任务栈和 PCM
缓冲使用 PSRAM；接听、拒接、对端取消、用户挂断或身份重置时停止。铃声不属于远端媒体流，
不能进入 AEC/通话 profile 后继续播放，也不能跨会话留下后台任务。

## 生命周期门禁

1. 进入应用前，由 `application/app.c` 按资源表申请 RTC 和音频，再启动业务服务。
2. 每个业务入口显式设置自己的 media profile、stream 和 owner，不依赖上一个页面的残留状态。
3. 退出时先停止业务生产者和回调，再清空自己的队列、取消订阅、断开连接，最后释放音频。
4. 连接、回调和后台任务使用 generation 或当前连接检查，旧会话不能把新页面拉回旧状态。
5. 微信 VoIP 与 Device Call 的共享命令按会话 owner 分流；一个业务不能消费另一个业务的
   `0x2000` 或断开事件。
6. 视频类型必须明确拒绝，不能为了“呼叫成功”偷偷改成音频。

修改后至少覆盖这些切换：

- Web IPC -> 主页 -> 小钛 -> 主页 -> Web IPC。
- Web IPC -> 主页 -> 微信 VoIP -> 主页 -> Web IPC。
- 小钛 -> 主页 -> 设备互呼 -> 主页 -> 小钛。
- 微信 VoIP -> 主页 -> 设备互呼 -> 主页 -> 微信 VoIP。
- 每条链路连续进入、退出 5 次，并观察 heap、队列和连接句柄是否回到稳定水位。

## 诊断顺序

遇到无声、卡顿或回声时，先找第一处异常：

| 阶段 | 证据 | 说明 |
| --- | --- | --- |
| 物理采集 | 麦克风 level、capture frame 计数 | 确认 Codec/I2S 真的在产出 PCM |
| owner | built-in/external audio 状态和当前应用 | 确认媒体归属没有继承错 |
| 上行 | local audio first packet / tx 计数 | 确认帧已进入 TiRTC |
| 下行 | remote packet、格式和完整性统计 | 区分没收到、格式不符、丢失或乱序 |
| 缓冲 | jitter depth、trim、queue drop、play drop | 区分网络抖动和本地背压 |
| 播放 | speaker level、播放 callback、AEC reference | 确认扬声器和远端参考同步 |
| 来电铃声 | `call_ringtone` start/stop、CALL_FLOW 状态 | 确认铃声只存在于待接阶段并在终态停止 |

串口可先使用：

```text
AT+MEDIA?
AT+AUDIOPATH?
AT+AUDIOCHECK?
AT+AECDUMP?
AT+RTCLOG?
AT+RTCLINK?
```

CLI 默认开启，但它只展示真实状态，不会绕过状态机。量产关闭 CLI 后，上述运行日志和统计仍
应保留足够的错误、阶段和计数，不能用“减少日志”掩盖失败。

## 代码锚点

- `main/product_capabilities.h`：RTC 视频、IPC 音频和二维码摄像头的产品能力事实源。
- `main/application/app.c`：应用资源表、会话仲裁和页面切换。
- `main/protocols/tirtc/tirtc_session.c`：media profile、built-in/external audio、发送与订阅。
- `main/services/rtc_media_bridge.c`：远端音频格式、解码和本地播放桥接。
- `main/services/media_sink.c`：抖动缓冲、完整性统计和播放队列。
- `main/services/device_call/device_call_ringtone.c`：普通设备来电铃声与停止边界。
- `main/drivers/audio/audio_echo_cancel.c`：AEC 处理和诊断。
- `main/debug/serial_net_cli/serial_net_cli.c`：只读状态和受控诊断命令。

## 验证边界

静态检查可以证明 owner、产品能力和函数调用关系一致；构建可以证明代码能够编译链接。真实
回声抑制、弱网听感、双向通话、重复切换和长稳必须在目标板上分别验证，并保留对应日志和
主观听感记录。

当前 Web IPC 和设备互呼的人耳试听仍可感知轻微“沙沙电流声”。线上格式核对与 20,000 次
A-law 编解码自检没有发现异常，说明明显的格式不一致和 Codec 自检不稳定不是现有首要证据；
底噪根因仍未证实，`1.9.5` 不宣称已经修复。
