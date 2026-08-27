# 第三方组件与 SDK

仓库根目录的 MIT License 适用于探鸽智能在本仓库中提供的原创代码与文档。下列内容保留其
各自的版权、许可和分发条款，不因进入本仓库而改为 MIT：

- Espressif、LVGL、Waveshare、Ingenic 及其他第三方组件和供应商源码。
- `components/tirtc_sdk/` 或 `sdk/` 中的 TiRTC SDK 头文件与目标平台静态库。
- 第三方字体、图像、音视频测试素材和生成资源。

ESP32-S3 Device Monitor `1.9.6` 继续使用 TiRTC SDK `2.3.0 mini` 官方基础包及项目修复快照。SDK BuildInfo 为
`v2.3.0-db7290f`，静态库大小为 `2,125,366` bytes，SHA-256 为
`83556eeee0c6cae45961899a4c5d1255a5d0d33f8e636104a946ce41ff3e20d7`。该快照以
active-connect `db7290f2404b15f2ab8567dd221853ca2ec054ab` 为功能基础，并包含
`13e34c3e3e3dc6776be4713b5c1e3c17bd282766` 的 HTTPS 服务端认证修复。公开归档使用
`--strip-debug` 移除 SDK 内部源码路径和行号级调试信息，BuildInfo、成员、全局符号以及运行
代码和只读数据契约保持一致。该项目同时使用 TGTRP BuildInfo `tagv1.5.11-41c9a257` 和包元数据中的 TGWebRTC ref
`tag.v1.5.12 / 41c9a25768ffe265c07f17ef78a6439607b19364`。完整身份以项目
`components/tirtc_sdk/VERSION.md`、`VERSION.md`、`SOURCE_PROVENANCE.md` 和 Release
manifest 为准；SDK 头文件和静态库保留供应方条款，不因本仓 MIT License 改为 MIT。

ESP32-P4 Device Monitor `1.5.1` 继续使用 TiRTC `2.3.0` 官方源码重建版，SDK 文件与 `1.5.0`
保持一致。Nano 源码为
`v2.3.0 / 1baf7c95f3ca715c9367b9c998417f647934dc35`，TGWebRTC 源码基线为
`tag.v1.5.12 / 41c9a25768ffe265c07f17ef78a6439607b19364`；静态库内嵌 TGTRP
BuildInfo 为 `tagv1.5.11`，两项元数据分开保留。P4 `libTiRTC.a` 大小为
`1,827,850` bytes，SHA-256 为
`6dc4d437ea444761ca21e203fc9babb1799bb1f7fc261d7c523248fde0a96e67`。P4 公开包 SDK 6 项文件身份
以项目 `components/tirtc_sdk/VERSION.md`、`SHA256SUMS.txt` 和 `SOURCE_PROVENANCE.md`
为准；来源包内未被 P4 链接且含供应方构建路径的 S3 静态库不进入 P4 公开目录。

该项目中的 `components/espressif__esp_h264/` 以 Espressif `esp_h264 1.3.8` 为基线，并
包含 P4 项目所需的输出边界、参考帧池和码率控制补丁。组件不是整目录逐字节等同上游；
Espressif 原始许可证文件继续随组件保留。

P4 Device Monitor、S3 Device Monitor 和 S3/P4 最小集成示例复用同一份 TiRTC H264
协议测试素材 `send_video.h264`。文件大小为 `298,486` bytes，SHA-256 为
`06fe8e1cd3376ce1580b92d17a31ce6c89372d3d1195d258941a568199d04eea`；它只用于固定码流
联调，不代表摄像头实采能力。该素材与 TiRTC SDK 均不适用仓库根 MIT License；使用和再分发
应遵循 TiRTC SDK/测试素材供应方条款。

ESP32-S3 最小系统例子的 `audio.g711a` 和 `video.h264` 测试输入来自
[`tirtc-server-example/thing-connect/device-sim/assets`](https://github.com/tangeai/tirtc-server-example/tree/main/thing-connect/device-sim/assets)。
五个 `ai_*.g711a` 短语音提示由 Windows 本地 `Microsoft Huihui Desktop` 语音合成，
仅用于固定协议测试。这些来源说明不改变编解码器、语音组件或输出可能适用的条款；
二次分发和商用前应结合使用环境核对相应许可。

ESP32-P4 最小系统例子使用 TiRTC `2.3.0` 定制兼容快照、ESP-Hosted `2.12.11` 和
ESP-WiFi-Remote `1.6.3`。P4 固件复用同一组协议测试媒体，并保留 C6/C61 同包兼容所需的
ESP-Hosted 源码与 Apache-2.0 许可证；各组件具体身份以项目 `VERSION.md`、
`SOURCE_PROVENANCE.md` 和 Release manifest 为准。

已有许可证文件会随对应组件保留在项目目录中；TiRTC SDK 目录没有单独附带开源许可证。
使用、修改或再分发前，请同时核对目标项目的 `SOURCE_PROVENANCE.md`、组件目录中的
`LICENSE`/`NOTICE`，以及 SDK 或素材供应方提供的条款。
未随公开仓分发的供应商 SDK、工具链和烧录工具，需要开发者从相应授权渠道单独获取。
