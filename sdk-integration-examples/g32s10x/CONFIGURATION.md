# Configuration

仓库中的 `include/tirtc_link_config.h` 是无凭据模板。需要真实配置时，先把项目复制到仓库外、
不受 Git 管理的私有构建目录，再修改副本中的该文件。不要把 WiFi 密码、设备密钥或一次性
Token 写回准备提交、压缩或分享的源码工作树；私有构建输出也按敏感文件管理。

`include/tirtc_link_defaults.h` 为旧配置文件补齐新增选项，并在编译期检查范围。产品工程可以
只声明需要覆盖的宏，其余参数继续使用默认值。

## 必填参数

| 参数 | 含义 | 说明 |
| --- | --- | --- |
| `TIRTC_LINK_WIFI_SSID` | WiFi 名称 | 空字符串会停在“等待 WiFi 配置” |
| `TIRTC_LINK_WIFI_PASSWORD` | WiFi 密码 | 不会输出到串口 |
| `TIRTC_LINK_SERVICE_ENDPOINT` | TiRTC 服务入口覆盖 | 默认留空并使用 SDK 内置入口；仅自部署或指定联调环境时填写 |
| `TIRTC_LINK_DEVICE_ID` | 设备 ID | 不得与其他在线设备混用 |
| `TIRTC_LINK_DEVICE_SECRET` | 设备密钥 | 不得写入日志或提交 Git |

## 连接与媒体

| 参数 | 默认值 | 可调范围与影响 |
| --- | ---: | --- |
| `TIRTC_LINK_AUTO_CONNECT` | `0` | `0` 只接受网页呼入；`1` 启动后主动连接 |
| `TIRTC_LINK_ENABLE_CONNECT_CACHE` | `1` | `0/1`；缓存主动连接参数，缓存有效时可用空 Token 重连 |
| `TIRTC_LINK_REMOTE_DEVICE_ID` | 空 | 仅主动连接使用 |
| `TIRTC_LINK_REMOTE_TOKEN` | 空 | 首次主动连接使用一次性 Token；缓存未命中或过期时必须重新获取 |
| `TIRTC_LINK_ENABLE_SAMPLE_MEDIA` | `1` | `0/1`；关闭后只验证连接，不发送演示流 |
| `TIRTC_LINK_SAMPLE_AUTO_PUBLISH` | `1` | `1` 在订阅命令到达前也可发流；严格订阅驱动时设 `0` |
| `TIRTC_LINK_SAMPLE_VIDEO_STREAM_ID` | `11` | `0..15`，不能与音频流相同 |
| `TIRTC_LINK_SAMPLE_AUDIO_STREAM_ID` | `10` | `0..15`，不能与视频流相同 |
| `TIRTC_LINK_SAMPLE_START_DELAY_MS` | `700` | 连接成功到首包发送的等待时间 |
| `TIRTC_LINK_SAMPLE_AUDIO_PACKET_MS` | `20` | `1..100`；PCMA 每包时长，网页端建议保持 `20` |

演示素材固定为 `1280x720 / 15 fps / MJPEG + 8 kHz mono PCMA`。这些值属于素材协议，
不能只改宏。替换素材时使用：

```bash
bash tools/transcode_sample_media.sh <source-video>
```

网页端可以继续调用 `requestKeyFrame(stream=11)`，但 MJPEG 的每一帧都是独立 JPEG，
固件会无副作用地忽略该请求，不重置播放游标、不唤醒媒体线程，也不改变发送节拍。

`TIRTC_LINK_ENABLE_CONNECT_CACHE=1` 只影响设备主动连接其他端。缓存按 `remote_id` 保存，TTL
由服务端返回；收到 `-40011` 时说明缓存未命中或已过期，不能重复使用旧 Token。

## 内存与背压

| 参数 | 默认值 | 调节建议 |
| --- | ---: | --- |
| `TIRTC_LINK_MAX_SEND_BUFFER` | `128 KiB` | SDK 发送缓存上限；增大前先确认设备剩余堆内存 |
| `TIRTC_LINK_SAMPLE_VIDEO_BUFFER_PERCENT` | `80` | 达到阈值后丢视频帧，避免控制面被慢网拖死 |
| `TIRTC_LINK_SAMPLE_AUDIO_BUFFER_PERCENT` | `95` | 音频优先级更高，因此阈值高于视频 |
| `TIRTC_LINK_WORKER_STACK_BYTES` | `12288` | 控制任务栈；缩小后必须重新做长连接测试 |
| `TIRTC_LINK_SAMPLE_WORKER_STACK_BYTES` | `6144` | 媒体任务栈；不得低于编译门禁值 |

优先通过降低素材码率解决持续背压，不要先无限增大发送缓存。`-40006` 表示缓冲区满或网络忙，
健康日志中的 `dropped` 和 `buffer` 用于判断调整方向。

构建阶段还会检查 `rtos-with-spl.bin` 不超过 8 MiB 固件分区的 `80%`。素材文件门禁为
`5,130,000` 字节，最终仍以完整固件门禁为准。该限制属于 Flash 容量，不等于运行时堆占用；
演示 AVI 由链接器映射到只读区并逐帧读取。

## 重试、超时与日志

| 参数 | 默认值 | 说明 |
| --- | ---: | --- |
| `TIRTC_LINK_WIFI_JOIN_DELAY_MS` | `2000` | 启动后首次连接 WiFi 的延时 |
| `TIRTC_LINK_WIFI_RETRY_MS` | `10000` | WiFi 断线重试周期 |
| `TIRTC_LINK_NTP_TIMEOUT_MS` | `5000` | 单次 NTP 接收超时 |
| `TIRTC_LINK_NTP_RETRY_MS` | `30000` | 校时失败重试周期 |
| `TIRTC_LINK_START_RETRY_MS` | `10000` | TiRTC 启动失败重试周期 |
| `TIRTC_LINK_WORKER_POLL_MS` | `250` | 控制任务最大轮询延时；过小会增加唤醒次数 |
| `TIRTC_LINK_SAMPLE_WORKER_POLL_MS` | `10` | 媒体发送节拍检查周期 |
| `TIRTC_LINK_SDK_LOG_LEVEL` | `2` | `1..5`；默认只保留 SDK error/warn |
| `TIRTC_LINK_STATUS_LOG_INTERVAL_MS` | `30000` | 健康日志周期；设 `0` 关闭周期日志 |

每次修改配置都必须重新编译。流 ID、任务栈、缓存和媒体包时长属于固件编译期参数，不能在设备
运行中动态改变。
