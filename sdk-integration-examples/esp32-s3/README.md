# ESP32-S3 最小 TiRTC 集成示例

本工程展示 ESP32-S3 设备接入 TiRTC 的最小完整链路：Wi-Fi、时间同步、SDK
上线、入站/主动连接、订阅控制，以及 H264/PCMA 测试媒体收发。

接入前先从 [设备端 SDK 下载](https://docs.tange.ai/products/tirtc/en/download.html)
获取目标平台包，并以
[设备端接入](https://docs.tange.ai/products/tirtc/en/guides/device-integration.html)、
[连接](https://docs.tange.ai/products/tirtc/en/guides/connection.html)、
[实时音视频](https://docs.tange.ai/products/tirtc/en/guides/real-time-audio-video.html)、
[C API Reference](https://docs.tange.ai/products/tirtc/en/api-reference/c.html)
和正式交付包中的头文件为准。

## 当前版本

- Demo：`1.2.0`，候选构建日期 `2026-07-30`
- 源码：tag `v1.2.0`，commit `44b24a50434ae5e32212a1c014945107076af29c`
- TiRTC SDK：`2.2.1`
- SDK commit：`3a33bf4ae51b3ab9eb246648adb274d0fae32ebf`
- ESP-IDF：`5.5.4`
- 工具链：`xtensa-esp32s3-elf-gcc-14.2.0_20260121`
- 公开 Release：[`esp32-s3-minimal-integration-v1.2.0`](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-s3-minimal-integration-v1.2.0)
- 传输：KCP / noSCTP
- SDK 构建：1 kHz、HTTP-only、PSRAM 线程栈管理优化
- `libTiRTC.a` SHA256：
  `dc9f869194584fd43fe091f887342d170a1422e657ce1fc2a144c90a3ade1f8e`

源码与 SDK 的导入范围、哈希和验证边界见
[来源与验证边界](SOURCE_PROVENANCE.md)。

SDK 组件来自正式交付目录：

```text
ESP32-S3_v2.2.1_20260729\
  esp32s3__freertos__xtensa-esp32s3-elf-gcc-14.2.0_20260121__v2.2.1_tag_3a33bf4ae51b_1khz_http_20260729_105424__kcp\
  packages\esp-idf-component\components\tirtc_sdk
```

组件保持正式包原有布局，静态库位于
`components/tirtc_sdk/lib/esp32s3/libTiRTC.a`，构建契约和符号清单位于
`components/tirtc_sdk/manifest/`。

## 示例能力

- 自动连接 Wi-Fi，并通过 SNTP 同步时间。
- 按官方顺序设置初始化前 option、`TiRtcInit()`、设备密钥、`client_id`，
  再调用 `TiRtcStart(device_id, callbacks)`。
- 同时打印 `TiRtcGetVersion()` 与 `TiRtcGetBuildInfo()`，用于核对实际运行库。
- 显式使用 Wi-Fi、单连接模式和连接参数缓存。
- 支持远端呼入；未连接时按 BOOT 主动连接，已连接时按 BOOT 主动断开。
- 主动连接优先复用 SDK 缓存；缓存过期后使用服务端签发的一次性 token。
- 对端订阅 H264/PCMA 后启动对应流，分别退订时只停止对应流。
- 视频发送缓冲繁忙时丢弃非关键帧，并从文件头尽快恢复关键帧。
- 识别 `TIRTC_EVENT_ACCESS_HIJACKING`，对异常 HTTP 重定向给出显式告警。

## 启动顺序

```text
Wi-Fi -> SNTP
-> TiRtcSetOption(TIRTC_OPT_MAX_SEND_BUFFER)   // 必须在 Init 前
-> TiRtcInit()
-> MAX_CONNECTIONS / NETWORK_TYPE / CONNECT_CACHE
-> DEVICE_SECRET_KEY / CLIENT_ID / 可选 endpoint
-> TiRtcStart(device_id, static_callbacks)
-> 等待 TIRTC_EVENT_SYS_STARTED
-> 接收入站连接或 TiRtcConnect()
```

`TiRtcStart()` 和 `TiRtcConnect()` 返回 `0` 只表示请求已提交，最终结果必须以
异步回调为准。停止时先停止新动作与媒体、断开连接、调用 `TiRtcStop()`，收到
`TIRTC_EVENT_SYS_STOPPED` 后才能 `TiRtcUninit()`。

## 配置

跟踪文件只保留占位值。真实配置写入 Git 忽略的 `main/local_config.h`：

```powershell
Copy-Item main\local_config.example.h main\local_config.h
```

设备上线至少配置：

```c
#define APP_WIFI_SSID "your_wifi_ssid"
#define APP_WIFI_PASSWORD "your_wifi_password"

#define TIRTC_DEVICE_ID "your_device_id"
#define TIRTC_DEVICE_SECRET_KEY "your_device_secret_key"
#define TIRTC_CLIENT_ID "your_stable_printable_client_id"
```

主动连接时再配置：

```c
#define TIRTC_REMOTE_DEVICE_ID "peer_device_id"
#define TIRTC_CONNECT_TOKEN "short_lived_one_time_connect_token"
```

`client_id` 必须是 1 到 64 个可打印 ASCII 字符。同一 `device_id` 首次成功后会
绑定该值，后续必须复用首次值。新设备可使用稳定的序列号、MAC、ICCID、IMEI
或其组合；已有设备不要擅自改变首次绑定值。

`TIRTC_SERVICE_ENDPOINT` 默认留空，使用 SDK 默认入口。只有自部署或测试环境才
覆盖。本次正式 2.2.1 S3 包是 HTTP-only，配置 `https://` 会被示例拒绝。

## Token 与密钥

设备固件不再本地签发连接 token。`TIRTC_CONNECT_TOKEN` 必须是业务服务端或
TiRTC DevTools 签发的短时、一次性 token；它的 `remote_id` scope 必须和
`TIRTC_REMOTE_DEVICE_ID` 一致。

应用级 `AccessKeyId/SecretKeyId` 只能保存在业务服务端，不能放进示例、设备固件
或日志。设备密钥也只能通过受控本地配置或安全存储注入。详细要求见
[密钥保密要求](https://docs.tange.ai/products/tirtc/en/security/secret-management.html)。

连接缓存默认开启。已成功连接过同一 `remote_id` 时，示例先调用
`TiRtcConnect(remote_id, NULL, ...)`；返回 `TIRTC_E_CACHE_EXPIRED` 后才使用新
token。`main/local_config.h` 只是开发联调手段，其中的 device secret 和 token 会
进入固件二进制，不能当作量产安全存储。当前 token 是编译期配置；首次调用
`TiRtcConnect()` 前，
示例会把它的 SHA-256 指纹和 CRC 写入 SPIFFS 双槽标记
`.tirtc_tok_a/.tirtc_tok_b`。设备会拒绝最近两个有效槽中已经记录的 token，
双槽轮换也可在一次写入中断后保留上一份有效记录。这只是开发联调防误用，不是
防篡改账本；一次性语义仍以服务端校验为准。缓存失效时需重新签发、替换配置并
重新编译烧录，或由产品实现运行时 token provider。擦除或重刷 `storage` 分区会
清除本地联调标记，此后仍必须使用新签发的 token。标记写入表示设备已在本地
预留并退役该 token，不等同于 SDK 同步受理或异步连接成功。

DevTools 的最小签发方式见
[官方设备实时音视频示例](https://docs.tange.ai/products/tirtc/en/get-started/view-device-live-av.html)：

```text
tirtc-devtools-cli token issue <remote_id>
```

## 目录

```text
components/tirtc_sdk/             正式 2.2.1 SDK 组件、静态库和 manifest
main/app_main.c                   Wi-Fi、SNTP、TiRTC 主流程
main/local_config.example.h       可提交的本地配置模板
main/local_config.h               本地真实配置，Git 忽略
main/system/                      Wi-Fi、时间同步和 BOOT 键
main/tirtc/tirtc_app.c            生命周期、回调、连接和订阅控制
main/tirtc/tirtc_connect.c        缓存优先的主动连接
main/tirtc/local_media_stream.c   H264/PCMA 测试媒体发送
main/tirtc/test_assets/           打包进 SPIFFS 的测试媒体
```

## 构建

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py reconfigure
idf.py build
```

正式组件要求：

```text
CONFIG_FREERTOS_HZ=1000
CONFIG_FREERTOS_USE_TRACE_FACILITY=n
CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS=n
CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=n
CONFIG_LWIP_TCPIP_TASK_STACK_SIZE=4096
```

项目根 `sdkconfig.defaults` 已显式固定这些配置，并保留 ESP32-S3 PSRAM 配置。

构建后用 Chrome 或 Edge 打开
[Espressif ESP Tool](https://espressif.github.io/esptool-js/)，按
`build/flasher_args.json` 把本次构建的每个 BIN 和地址逐项加入并烧录。不要只写应用
BIN；`storage` 中还包含测试媒体和本地 token 标记。

烧录完成后复位设备，再打开串口监视：

```powershell
idf.py -p COMx monitor
```

把 `COMx` 替换为实际串口。构建通过不等于已烧录、联网或完成真实音视频验证，
完整验证步骤见 [TEST_GUIDE.md](TEST_GUIDE.md)。

## 运行检查点

启动后应至少看到：

```text
TiRTC 版本: 2.2.1
TiRTC BuildInfo: {"tirtc":"v2.2.1","commit":"3a33bf4ae51b",...}
TiRTC 运行策略: max_connections=1 network=Wi-Fi connect_cache=1
TiRTC 启动请求已提交
TiRTC 已上线
```

主动连接时：

```text
主动连接先尝试复用 SDK 连接参数缓存
缓存连接请求已提交
```

缓存未命中时：

```text
连接参数缓存未命中或已过期，改用新的一次性 token
使用一次性 token 发起主动连接
```

收到订阅后分别启动音视频；收到视频或音频退订后，对应发送任务应停止，另一条流
继续运行。视频 stream 为 `11`，音频 stream 为 `10`，符合
[实时音视频指南](https://docs.tange.ai/products/tirtc/en/guides/real-time-audio-video.html)
中的取值约束。

## 回调与句柄

- `TIRTCCALLBACKS` 必须使用静态或等效的长生命周期存储。
- 所有回调都运行在 SDK 内部线程，禁止阻塞、等待，以及执行文件或网络 I/O。
- 回调携带的 payload 只在该次回调期间有效，异步使用前必须复制。
- `TiRtcDisconnect()` 是异步操作，但返回后应用必须立即视 `hconn` 为无效。
- `on_conn_error` 之后应用需要调用 `TiRtcDisconnect()` 释放连接资源。
- 连接错误回调只登记关闭请求；普通关闭任务等待活跃 SDK 调用退出后唯一执行
  `TiRtcDisconnect()`。单连接模式已有连接或正在关闭时，新连接会被拒绝而不会
  强行替换旧句柄。

## 证据边界

源码构建只能证明头文件、静态库、应用代码和 ESP-IDF 配置可以共同编译链接。
完整交付验收还需要板端烧录、串口 `SYS_STARTED`、主动/入站连接、Web 或另一台
设备实际播放、分流退订/重订、连断循环和内存趋势证据。
