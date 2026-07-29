# TiRTC SDK Integration

本例程固定使用 G32S10X TiRTC C SDK `2.2.1`：

- 静态库：`sdk/lib/g32/libTiRTC.a`
- 静态库 SHA256：`33e889d70d4459587faf57f611e9648d52accb2bc02d0ec75205a52e9ca27fba`
- 构建信息：`sdk/manifest/build-info.json`
- 源码基线：`remote-v2.2.1`，commit `3a33bf4ae51b`
- 传输：KCP / noSCTP；底层对象已经合入 `libTiRTC.a`，不要再重复链接
  `libwebrtc_nosctp.a`

交付包自身把该 G32 版本标记为 `candidate`，因为上游 `remote-v2.2.1` 尚未原生发布
G32S10X target。例程的静态检查和完整固件链接可以证明集成闭环；烧录、联网和网页播放仍需
单独验证。

## 与官方文档的对应关系

- [SDK 接入与初始化](https://docs.tange.ai/products/tirtc/guides/sdk-integration/c.html)
- [建立与设备端的连接](https://docs.tange.ai/products/tirtc/guides/connection.html)
- [音视频流传输与播放](https://docs.tange.ai/products/tirtc/guides/real-time-audio-video.html)
- [C API 说明](https://docs.tange.ai/products/tirtc/api-reference/c.html)
- [密钥保密要求](https://docs.tange.ai/products/tirtc/security/secret-management.html)

通用 Linux 包在文档中使用 `#include <tirtc/tiRTC.h>`；本 G32 交付包的实际目录名为
`include/TiRTC/`，所以本例程按包内布局使用：

```c
#include "TiRTC/tiRTC.h"
```

## 初始化顺序

`tirtc_link.c` 按下面的固定顺序调用 SDK：

1. 在 `TiRtcInit()` 前设置 `TIRTC_OPT_MAX_SEND_BUFFER`。
2. 调用 `TiRtcInit()`。
3. 仅当 `TIRTC_LINK_SERVICE_ENDPOINT` 非空时覆盖服务入口；空字符串使用 SDK 默认入口。
4. 设置 `TIRTC_OPT_DEVICE_SECRET_KEY`、`TIRTC_OPT_CLIENT_ID`、
   `TIRTC_OPT_MAX_CONNECTIONS`、`TIRTC_OPT_NETWORK_TYPE` 和
   `TIRTC_OPT_CONNECT_CACHE`。
5. 调用 `TiRtcStart(device_id, &callbacks)`。
6. 只有收到 `TIRTC_EVENT_SYS_STARTED` 才把设备标记为在线。

设备 Secret、WiFi 密码和一次性 Token 不进入 Git，也不打印到日志。启动日志只输出
`TiRtcGetVersion()` 与 `TiRtcGetBuildInfo()`，便于准确定位 SDK 版本和底层版本。

## 2.2.1 适配点

`2.2.1` 与本例程原有 `2.2.0` 公共函数和回调结构兼容，应用侧无需改写媒体协议。升级包主要
补齐 G32 平台内部可靠性，包括启动 JSON 整数序列化、解析/时间适配、连接内存申请检查和
DataChannel label 生命周期加固。

本例程同时显式接入两个公开能力：

- `TIRTC_OPT_CONNECT_CACHE`：由 `TIRTC_LINK_ENABLE_CONNECT_CACHE` 控制，默认开启。主动连接
  成功后可缓存同一 `remote_id` 的连接参数；缓存有效时 Token 可以传 `NULL`，返回
  `TIRTC_E_CACHE_EXPIRED` 时必须重新获取一次性 Token。
- `TiRtcGetBuildInfo()`：启动时打印 JSON 构建描述，构建门禁也会核对头文件版本、静态库
  SHA256 和 manifest。

## 回调与媒体

SDK 回调运行在内部线程。回调只修改受锁保护的固定大小状态或投递有界事件，串口输出和真正送流
都在例程 worker 中执行。

- 音频流固定为 `stream_id = 10`，格式为 8 kHz 单声道 PCMA。
- 视频流固定为 `stream_id = 11`，格式为 MJPEG。
- 订阅回调返回 `0` 后开始送流，取消订阅后停止对应媒体。
- 每个 MJPEG 帧都可独立解码，因此关键帧请求回调保持无副作用，不重置播放游标。

当前例程把最大连接数设为 `1`，连接句柄由 `tirtc_link` worker 唯一持有。若产品需要多客户端，
应按官方文档为每个 `hconn` 建立独立上下文，不要直接复用这里的单连接状态。
