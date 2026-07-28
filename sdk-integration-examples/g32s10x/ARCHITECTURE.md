# Architecture

## 依赖方向

```text
integration/application.c
        |
        v
  tirtc_link.c                 应用编排、TiRTC 生命周期、连接唯一所有者
   |       |       |
   |       |       +----> tirtc_test_log.c      测试日志和错误解释
   |       +------------> tirtc_sample_media.c  订阅和发送节拍
   |                         |
   |                         +-> tirtc_sample_avi.c  纯 AVI 解析和游标
   +--------------------> tirtc_link_platform.c WiFi、NTP、MAC、墙上时钟
                                  |
                                  v
                          G32S10X / ATBM / lwIP
```

依赖只能向下。平台层不知道 TiRTC 连接，AVI 解析层不知道 RTOS 和 TiRTC，诊断层不拥有状态，
媒体层不启动或停止 SDK。

## 模块职责

| 模块 | 唯一职责 | 明确不做 |
| --- | --- | --- |
| `tirtc_link.c` | 前置条件状态机、SDK 生命周期、连接句柄、回调事件队列 | WiFi 驱动、AVI 解析、媒体硬件 |
| `tirtc_link_platform.c` | ATBM WiFi 重试、DNS/NTP、软件墙上时钟、MAC client ID | TiRTC API、产品业务 |
| `tirtc_sample_media.c` | 单连接订阅状态、发送节拍、背压和发送统计 | 解析 RIFF 边界、申请整片缓存 |
| `tirtc_sample_avi.c` | 校验只读 AVI、返回 JPEG 帧和定长 PCMA 包 | RTOS、线程、网络、TiRTC |
| `tirtc_test_log.c` | 将错误码翻译为测试结果和处理建议 | 改变状态或执行恢复 |
| `tirtc_sample_asset.S` | 把 AVI 映射到固件只读段 | 运行时复制和解析 |

`include/tirtc_link_config.h` 是用户配置面，`include/tirtc_link_defaults.h` 负责默认值和编译期
范围检查，`port/g32/vendor_overrides/` 只保存 G32 网络/TLS 兼容修正。

## 状态与所有权

`tirtc_link` worker 是 SDK 生命周期和 `g_connection` 的唯一所有者：

```text
idle -> wait-wifi-config -> wait-network -> sync-time
     -> wait-tirtc-config -> starting -> listening
     -> connecting/connected -> listening
```

- SDK 回调只复制固定大小数据、修改锁保护状态并投递事件。
- 命令、订阅、取消订阅和关键帧日志进入 8 项有界队列，SDK 回调线程不做串口 I/O。
- 连接错误和主动断开都进入固定深度的断开队列。
- `g_link_lock` 保护回调和 worker 共享的状态、句柄与队列。
- `g_sdk_api_lock` 串行化媒体发送、发送缓存查询和断开，避免释放句柄时仍在发送。
- `tirtc_sample` worker 只持有当前连接快照；切换连接会递增 generation 并重置游标。

## 内存和实时性

- AVI 位于只读段，运行时不复制整片，也不申请视频帧堆内存。
- 视频直接发送只读 JPEG 地址；音频栈上只保留一个 PCMA 包，默认 160 字节。
- 所有队列都有固定上限，不存在无界消息或媒体堆积。
- 视频缓冲达到默认 80% 时先丢帧；音频阈值为 95%。
- 控制任务默认栈 12 KiB，媒体任务默认栈 6 KiB，均可在配置文件中调整并受编译门禁保护。

## 失败与恢复

- WiFi、NTP 和 TiRTC 启动都有独立重试周期。
- `TiRtcStart()` 返回 0 只代表请求提交，只有 `TIRTC_EVENT_SYS_STARTED` 才表示设备在线。
- 入站和主动连接统一进入同一个连接结果处理函数。
- 发送失败只记录有界统计；`TIRTC_E_BUSY` 通过丢帧恢复，不申请额外缓存。
- 错误日志统一输出测试项、错误码、中文含义和检查建议。
