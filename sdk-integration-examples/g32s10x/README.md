# G32S10X 最小 TiRTC 集成示例

这是一个可整体移植、可供客户阅读的君正 G32S10X TiRTC 最小例程：

```text
FreeRTOS -> ATBM WiFi -> lwIP/DNS/NTP -> TiRTC -> MJPEG/PCMA sample
```

## 功能范围

包含 WiFi STA 自动连接和重试、一次 NTP 校时、TiRTC 上线和连接、订阅控制，以及固件只读区
内嵌的 9.4 秒 VOA 播报员人声音画同步片段。素材规格为
`1280x720 / 15 fps / MJPEG + 8 kHz mono PCMA`，
音频默认 160 字节、20 ms 一包。设备只向网页输出，不接收或播放网页音视频。

不包含 UI、绑定、OTA、AI、微信、文件系统业务、摄像头、麦克风、扬声器和运行时编解码。
媒体外设、JPEG/Opus/PCM 驱动均未启用。

## 从哪里开始读

1. [ARCHITECTURE.md](ARCHITECTURE.md)：依赖方向、模块职责、线程和连接所有权。
2. [CONFIGURATION.md](CONFIGURATION.md)：所有用户可调参数、范围和调整建议。
3. [PORTING.md](PORTING.md)：接入君正 SDK 或替换到其他平台的方法。
4. `include/tirtc_link.h`：对外控制 API。
5. `src/tirtc_link.c`：从 WiFi 就绪到 TiRTC 连接的主流程。

供应商 SDK 与工具链要求见 [君正 SDK 要求](SDK_REQUIREMENTS.md)，候选快照的来源状态和验证
边界见 [来源与验证边界](SOURCE_PROVENANCE.md)。

## 目录结构

```text
assets/       演示 AVI、预览图和授权说明
include/      对外 API、用户配置、默认值和平台契约
src/          会话编排、平台适配、诊断、AVI 解析和媒体发送
sdk/          TiRTC 公共头文件与 G32 静态库
port/g32/     君正网络、TLS 和基础运行时的必要兼容/告警修复覆盖
integration/  application、package、defconfig 接线
tools/        演示素材转码工具
tests/        无硬件依赖的 AVI 解析和时间戳测试
```

`tirtc_sample_avi.c` 是无 RTOS 依赖的纯解析层；`tirtc_sample_media.c` 只负责发送节拍和订阅；
`tirtc_link.c` 是 TiRTC 生命周期与连接句柄的唯一所有者。SDK 回调不打印串口，而是把控制事件
送入固定大小队列交给 worker 输出。

## 配置与构建

填写 `include/tirtc_link_config.h` 中的 WiFi、设备 ID 和设备 Secret。将本目录集成到君正
SDK 后执行：

```bash
make g32s10x_tirtc_wifi_link_demo_defconfig
make -j4
```

也可以按 [君正 SDK 要求](SDK_REQUIREMENTS.md) 使用 `scripts/build-in-sdk.sh` 把示例复制到
干净 SDK，并执行同一 defconfig 和构建命令。脚本只从环境变量读取 SDK 与工具链路径。
替换素材时执行 `bash tools/transcode_sample_media.sh <source-video>`。

当前 G32S10X 固件分区为 `8 MiB`。生成 `rtos-with-spl.bin` 后应检查镜像大小并保留升级余量；
素材位于固件只读区，不会在启动时整段复制到堆。

## 真机验收

正常链路应依次出现：

```text
[TEST][PASS] WiFi联网 | 网络已就绪
[TEST][PASS] 时间校准 | ...
[TEST][PASS] TiRTC上线 | 设备在线，等待网页连接
[TEST][PASS] 对端连接 | ...
[TEST][PASS] 视频订阅 | stream=11 ...
[TEST][PASS] 音频订阅 | stream=10 ...
[TEST][PASS] 视频输出 | 首帧已发送 ...
[TEST][PASS] 音频输出 | 首包已发送 ...
```

默认每 30 秒输出一条健康摘要。失败行会同时说明错误码、中文含义和处理建议。构建成功不能
替代真机上线、网页播放和断开重连验证。
