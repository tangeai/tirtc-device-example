# G32S10X 最小 TiRTC 集成示例

这是一个可整体移植、面向普通开发者的君正 G32S10X TiRTC 最小例程。当前网络实现使用
ATBM WiFi，但项目身份保持为“最小 TiRTC 集成示例”：

```text
FreeRTOS -> ATBM WiFi -> lwIP/DNS/NTP -> TiRTC -> MJPEG/PCMA sample
```

| 项目 | 当前值 |
| --- | --- |
| 示例版本 | `0.8.3` |
| TiRTC SDK | `2.2.1`，manifest 状态 `candidate` |
| 君正 SDK | `im_sdk_v0.4.0` |
| 工具链 | `riscv32-ingenic-g32s10-elf-tools-r1.0.5` |
| 公开 Release | [`g32s10x-minimal-integration-v0.8.3`](https://github.com/tangeai/tirtc-device-example/releases/tag/g32s10x-minimal-integration-v0.8.3) |

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
3. [SDK_INTEGRATION.md](SDK_INTEGRATION.md)：TiRTC 2.2.1 版本、官方 API 映射和升级点。
4. [PORTING.md](PORTING.md)：接入君正 SDK 或替换到其他平台的方法。
5. `include/tirtc_link.h`：对外控制 API。
6. `src/tirtc_link.c`：从 WiFi 就绪到 TiRTC 连接的主流程。

## 目录结构

```text
assets/       演示 AVI、预览图和授权说明
include/      对外 API、用户配置、默认值和平台契约
src/          会话编排、平台适配、诊断、AVI 解析和媒体发送
sdk/          TiRTC 2.2.1 公共头文件、G32 静态库与构建 manifest
port/g32/     君正网络、TLS 和基础运行时的必要兼容/告警修复覆盖
integration/  application、package、defconfig 接线
tools/        演示素材转码工具
tests/        无硬件依赖的 AVI 解析和时间戳测试
```

`tirtc_sample_avi.c` 是无 RTOS 依赖的纯解析层；`tirtc_sample_media.c` 只负责发送节拍和订阅；
`tirtc_link.c` 是 TiRTC 生命周期与连接句柄的唯一所有者。SDK 回调不打印串口，而是把控制事件
送入固定大小队列交给 worker 输出。

SDK 版本固定为 `2.2.1`。启动日志会输出 `TiRtcGetVersion()` 和
`TiRtcGetBuildInfo()`；构建门禁会同时核对头文件版本、静态库 SHA256 和交付 manifest。

## 配置与构建

真实凭据只在仓库外、不受 Git 管理的私有构建副本中填写。把本项目复制到该副本后，修改
`include/tirtc_link_config.h` 中的 WiFi、设备 ID 和设备 Secret，再准备一份干净展开的
君正 SDK 和 G32S10X 工具链并执行：

```bash
G32_SDK_ROOT=/path/to/im_sdk/opensource/freertos \
G32_TOOLCHAIN_BIN=/path/to/g32-toolchain/bin \
bash scripts/build-in-sdk.sh
```

脚本把示例和必要的平台覆盖复制到指定 SDK，应用
`g32s10x_tirtc_wifi_link_demo_defconfig` 并执行完整构建。它拒绝覆盖已存在的同名应用，
因此每个发布版本应使用干净 SDK 树。固件输出位于 SDK 根目录的 `rtos-with-spl.bin`。

真实配置、构建目录和固件只保存在开发者本地，不应提交到 Git 或对外分享。
替换素材时执行 `bash tools/transcode_sample_media.sh <source-video>`。

当前 G32S10X 固件分区为 `8 MiB`。发布流程会检查固件大小和 SHA-256；素材位于固件只读区，
不会在启动时整段复制到堆。

## 使用 Release 固件

不改源码时，从
[`g32s10x-minimal-integration-v0.8.3` Release](https://github.com/tangeai/tirtc-device-example/releases/tag/g32s10x-minimal-integration-v0.8.3)
下载 `g32s10x-tirtc-minimal-rtos-with-spl-v0.8.3.bin` 和 `SHA256SUMS.txt`。核对哈希后，在君正
Cloner 中把该文件映射到目标板主固件 policy；分区布局和 policy 以板卡供应方配置为准，不能
照抄其他板卡地址。通用下载与校验说明见[固件下载与校验](../../docs/RELEASES_CN.md)。

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
