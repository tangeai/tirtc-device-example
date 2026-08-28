# 固件分层与责任边界

本工程把硬件、网络、协议、业务、应用和 UI 分开。排查问题时先找到第一处异常，再到真正
拥有该状态的层处理，不在页面或上层状态机里堆叠临时判断。

## 依赖方向

| 层 | 目录 | 责任 |
| --- | --- | --- |
| UI | `main/ui/` | 渲染页面、展示应用快照、投递用户动作；不初始化硬件，不解析协议原文 |
| 应用 | `main/application/` | 应用生命周期、结构化快照、会话仲裁、资源表和产品策略 |
| 服务 | `main/services/` | 小钛、微信 VoIP、设备呼叫、OTA、媒体播放、二维码扫描等完整业务流程 |
| 调试 | `main/debug/serial_net_cli/` | 只读查询网络、RTC、音频、AEC、呼叫和内存状态；不接管业务 |
| 协议 | `main/protocols/` | HTTP、MQTT、RTC/TiRTC 的传输、鉴权和会话适配 |
| 连接 | `main/connectivity/` | Wi-Fi、IP、扫描、网络状态和网络质量探测 |
| 驱动 | `main/drivers/` | 显示、触摸、麦克风、扬声器、摄像头和 PCM 音频后端 |
| 硬件 | `main/hardware/` | 板卡引脚、共享外设和硬件资源分配 |

依赖只向下。驱动、连接和协议层不能反向包含 `application/` 或 `ui/`，UI 也不能绕过应用层
直接启动 RTC、音频或后台业务任务。

## 驱动边界

- 显示和触摸由 `drivers/display/display_driver.*` 初始化，UI 只接收 LVGL 句柄。
- 麦克风和扬声器由 `drivers/audio/audio_device.*` 暴露；底层 `audio.*` 负责 Codec/I2S PCM。
- AEC 位于 `drivers/audio/audio_echo_cancel.*`。设备互呼 profile 使用全双工
  高性能线性模式，工作区从 PSRAM 申请；Web IPC、微信和小钛不能隐式继承这套参数。
- 摄像头由 `drivers/camera/camera_driver.*` 暴露，只服务二维码扫描。
  `main/product_capabilities.h` 阻止 RTC 业务把它当作视频源。
- LEDC 等共享资源统一在 `hardware/hardware_board_config.h` 分配。

## 网络数据边界

应用只使用 `network.*`，Wi-Fi 驱动细节留在 `connectivity/wifi.*`。网络质量探测由
`connectivity/network.c` 产生结构化的平均 RTT、抖动和丢包率；`app_snapshot.c` 把字段复制
到应用快照，`app_ui.c` 再交给显示层。UI 只负责格式化和阈值提示，不从 `summary` 文本反向
解析数据。

网络测试中的抖动是相邻成功 RTT 变化的平均值；音频 `media_sink` 的 jitter depth 是远端
音频缓冲状态。二者名称相近但责任不同，日志和文档不能混为一个指标。

协议层拥有传输安全门禁：服务发现只接受 HTTPS/MQTTS，通用 HTTP 和 TiRTC SDK HTTPS
分别校验证书链与 hostname，临时绑定和正式设备 MQTT 在创建客户端前拒绝非 MQTTS 地址。
业务层只接收成功结果，不能在失败后改用 HTTP/MQTT；UI 只展示失败，不提供“忽略证书”入口。

## 应用生命周期与资源表

`application/app.c` 持有应用资源表。打开应用时先申请声明的资源，再启动对应服务；返回主页
时先停止业务生产者和回调，再按反向顺序释放二维码摄像头、音频、RTC 和网络资源。应用互相
切换时也先回到主页的零前台资源状态，避免旧页面的硬件、队列和 TiRTC 连接泄漏到新页面。

启动阶段只保留全局 UI、OTA、回调和策略状态。主页可见不代表 Wi-Fi、TiRTC runtime、音频
设备或摄像头必须常驻。

当前产品启用 RTC 音频并禁用 RTC 视频。Web IPC、小钛、微信 VoIP 和设备呼叫都必须在应用
和协议边界遵守该能力；视频请求明确拒绝，不静默改成音频。

UI 的接听、拒绝、挂断、扬声器音量和采集增益回调只投递结构化动作，不同步执行 HTTP、
TiRTC、音频硬件或 NVS 操作。应用控制任务串行执行呼叫动作；本地音量与增益允许合并连续
请求，但必须保留最新值。这样既不阻塞 LVGL，也不会为每次快速点击堆积一条过期硬件操作。

设备呼叫的冷启动责任链也遵守分层：UI 只发起请求并显示应用快照；应用层准备音频并负责
失败时释放资源；Device Call 服务等待正式上线和 TiRTC ready。正式在线最长等待 `30` 秒，
RTC ready 最长等待 `50` 秒并每 `500 ms` 推进准备。发起呼叫前，呼叫请求最多等待 `12` 秒，
让已经运行的房间恢复、联系人刷新或联系人变更任务结束；超时仍忙则拒绝本次呼叫。呼叫活跃
后不再启动新的房间恢复或联系人刷新，结束后恢复。UI 不通过手动启动 RTC 或清状态绕过这条链路。

每次设备呼叫由 Device Call 服务分配独立 generation。异步接听 worker 在提交连接前、提交
过程中和等待连接时都检查 generation；一旦拒绝、挂断、取消或远端结束推进了 generation，
旧 worker 必须退出，不能复活旧房间。主叫的 `45` 秒响铃等待与进入 `CONNECTING` 后的
`40` 秒 P2P 建连等待也是两个独立状态阶段，分别使用取消和挂断语义。

## RTC 与音频边界

应用和调试代码使用 `rtc_transport.*`。TiRTC connection handle、command word、WHIP 请求和
服务请求留在 `protocols/tirtc/` 及真正需要这些契约的业务服务中。

Web IPC、设备呼叫和微信 VoIP 的设备上行统一使用
`8 kHz / 16 bit / mono / G.711 A-law / 20 ms / 160 bytes`。协议层负责把 built-in
microphone 与显式 CALL gate 映射到同一线上格式；UI 和业务层不参与编码。

远端音频由 `services/media_sink.c` 按当前业务 profile 处理。有界 `1.25%` 播放
速率微调只属于 Device Call；Web IPC、微信和小钛保持各自缓冲策略。页面不能为了让声音暂时
连续而自行调队列、补帧或清状态。

TiRTC SDK 的网络回调也遵守有界公平性：一次 ICE UDP 回调最多处理 `8` 个数据报或 `4 ms`，
一次 TGTRP 音频抖动轮询最多释放 `4` 个有序项；仅在仍有积压时安排 `1 ms` 后继续。该预算
属于协议层调度契约，应用和 UI 不通过延时或清队列进行二次“修复”。

## 来电铃声边界

普通设备来电铃声由 `services/device_call/device_call_ringtone.*` 持有。它在
`call_incoming` 接受并保存后启动，在接听、拒接、取消、挂断或身份重置时停止。任务栈和 PCM
缓冲使用 PSRAM，扬声器仍由音频驱动提供。铃声是 `659 / 784 / 1047 Hz` 的两组三音上行
短句，周期 `3.2` 秒，并带起音/收音包络。UI 只投递接听/拒接动作，不直接创建或删除铃声任务。

## 调试边界

串口 CLI 只调用公开状态和诊断接口。它可以显示网络质量、RTC、音频路径、AEC 和呼叫状态，
但不能访问私有协议状态、绕过鉴权、伪造在线或把失败改成成功。

Web IPC 和设备互呼当前仍可能听到轻微“沙沙电流声”。线上格式和 A-law 自检未见异常，但
底噪根因尚未证实。这个现象需要从驱动采集、AEC、上行、远端到达、缓冲和播放逐层找第一处
异常，不能在 UI 或业务状态机里增加静音、重启或吞错来制造“已经修好”。

TLS 源码、对象、静态库和 APP 链接检查证明认证逻辑进入发布代码，但不等于目标板握手验证。
有效证书成功和错误证书/hostname 失败应在可控环境分别留证。

`1.9.7` 已完成正式干净构建，但没有烧录或真机回归证据。分层关系、generation 检查和调度
预算的静态核对及构建通过，都不能替代触摸响应、双机呼叫、铃声听感、弱网与长稳验证。
