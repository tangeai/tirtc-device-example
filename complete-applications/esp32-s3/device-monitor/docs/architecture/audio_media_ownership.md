# 音频和媒体所有权

`1.9.0` 的 RTC 产品能力是双向音频。Web IPC、小钛、微信 VoIP 和设备互呼共享同一套物理
麦克风、扬声器和 TiRTC 运行时，但每条业务拥有自己的会话状态、流号、队列和退出路径。
摄像头不属于 RTC 媒体资源，只由二维码扫描流程短时申请。

## 四条音频链路

| 应用 | 音频 owner | 采集入口 | 发送方式 | 关键边界 |
| --- | --- | --- | --- | --- |
| Web IPC | TiRTC built-in media | 主麦克风 callback | local audio stream | 进入查看页必须恢复 built-in owner，不继承 AI/VoIP 外部音频状态 |
| 小钛 | AI Chat 服务 | 麦克风 observer | WHIP command/audio frame | `start_session` 成功后才上行；退出按 generation 取消旧回调 |
| 微信 VoIP | WeChat VoIP 服务 | 麦克风 observer | external audio | 入会后标记 external audio；挂断、失败和超时都要解除 |
| 设备互呼 | Device Call + TiRTC session | 主麦克风 callback | local audio stream | 只支持 `call_type=audio`；房间和 `0x2000` 确认后启动媒体 |

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
- `main/drivers/audio/audio_echo_cancel.c`：AEC 处理和诊断。
- `main/debug/serial_net_cli/serial_net_cli.c`：只读状态和受控诊断命令。

## 验证边界

静态检查可以证明 owner、产品能力和函数调用关系一致；构建可以证明代码能够编译链接。真实
回声抑制、弱网听感、双向通话、重复切换和长稳必须在目标板上分别验证，并保留对应日志和
主观听感记录。
