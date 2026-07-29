# Porting

整个 `tirtc_g32s10x_wifi_link_demo/` 是唯一对外交付单元。不要从文件夹中单独摘取
`tirtc_link.c`，否则会丢失平台适配、静态库、构建接线和配置门禁。

## 接入君正 SDK

1. 复制本文件夹到 SDK 的 `application/tirtc_g32s10x_wifi_link_demo/`。
2. 按相对路径覆盖 `port/g32/vendor_overrides/`；这里同时包含网络/TLS 兼容和君正基础运行时
   告警修复，不能只复制其中的 WiFi 文件。
3. 用 `integration/application/application.c` 接入应用入口。
4. 合并 `integration/package/application/` 下的 `Config.in`、`application.mk` 和包目录。
5. 复制 `integration/configs/g32s10x_tirtc_wifi_link_demo_defconfig` 到 SDK `configs/`。
6. 从配置模板建立本机配置，填写 WiFi 和 TiRTC 凭据。
7. 执行 defconfig、语法检查和完整构建。

TiRTC 头文件按本交付包实际目录包含：

```c
#include "TiRTC/tiRTC.h"
```

只链接 `sdk/lib/g32/libTiRTC.a`。该归档已经包含配套 noSCTP WebRTC 对象，不能再重复链接
`libwebrtc_nosctp.a`。版本、SHA256 和初始化顺序见 [SDK_INTEGRATION.md](SDK_INTEGRATION.md)。

```bash
make g32s10x_tirtc_wifi_link_demo_defconfig
make -j4
```

生成的 `.config.in` 必须保持摄像头、PCM、JPEG、LVGL、Opus、文件系统和 OTA 关闭；ATBM、
lwIP、net manage、MbedTLS、DTRNG 和本例程应用必须开启。
应用源码仍须使用 `-Wformat=2 -Werror` 检查。供应商旧代码的格式兼容只处理 r1.0.5 中
`uint32_t` 与 `printf` 规格的 ABI 差异，完整构建日志不得残留 `warning:`。

## 替换到其他 RTOS 或 MCU

保留以下通用模块：

- `tirtc_link.c`：TiRTC 生命周期和连接状态机。
- `tirtc_sample_media.c`：订阅驱动的演示媒体发送。
- `tirtc_sample_avi.c`：无平台依赖的 AVI 解析器。
- `tirtc_test_log.c`：错误解释，可替换底层输出函数。

需要重新实现 `tirtc_link_platform.h` 中的接口，并提供 TiRTC SDK 所需的网络、随机数、线程、
时间和 TLS 适配。`tirtc_link_platform_init()` 必须可重复调用，`poll()` 不得长时间阻塞，
`sync_time()` 必须有超时，client ID 必须稳定且每台设备唯一。

## 配置与凭据

完整参数见 [CONFIGURATION.md](CONFIGURATION.md)。生产项目应从安全存储读取设备密钥和主动连接
Token；本示例使用编译期宏只是为了形成最小可移植例程。任何日志都不得输出密码、Secret 或
Token 正文。

## 验收边界

构建通过只证明源码、ABI、链接和镜像闭环。烧录后仍需按顺序验证：

1. WiFi 联网和 NTP 校时通过。
2. 收到 `TiRTC上线 PASS`，而不只是 `TiRtcStart()` 返回 0。
3. 网页连接后收到音视频订阅。
4. 视频首帧和音频首包发送成功，计数持续增长。
5. 取消订阅停止对应发送，断开后回到等待连接。
6. 再次连接仍可输出，长连接无持续缓冲增长。

该例程只生成并烧录 RTOS 镜像，不生成也不要求更新 `fs`、`data` 分区。
