# 第三方组件与 SDK

仓库根目录的 MIT License 适用于探鸽智能在本仓库中提供的原创代码与文档。下列内容保留其
各自的版权、许可和分发条款，不因进入本仓库而改为 MIT：

- Espressif、LVGL、Waveshare、Ingenic 及其他第三方组件和供应商源码。
- `components/tirtc_sdk/` 或 `sdk/` 中的 TiRTC SDK 头文件与目标平台静态库。
- 第三方字体、图像、音视频测试素材和生成资源。

ESP32-P4 Device App `1.3.2` 使用 TiRTC `2.3.0` 定制兼容快照。其公开 API 版本不变，
P4 静态库以 Nano baseline `aaad3da251bac90e0642b51b3279a1f40ca9fa9a` 为基础，并带有从
`fde4f1c58d2dc28f3d3d04e25dd49bc3a399fea6` 回移的 HTTP DNS cache disable 补丁。
该快照的身份和逐文件 SHA-256 以项目 `components/tirtc_sdk/VERSION.md`、
`SHA256SUMS.txt` 和 `SOURCE_PROVENANCE.md` 为准。

项目明确标记为自生成且按仓库根许可分发的测试素材，以项目内 `media/README.md` 的来源、
内容和 SHA-256 声明为准；这不改变编解码器实现本身的许可。

ESP32-S3 日志示例中的五个短语音提示由 Windows 本地 `Microsoft Huihui Desktop` 语音合成，
仅作为固定协议测试输入；文件来源、提示文本和 SHA-256 见
[`media/README.md`](../logging-examples/esp32-s3/media/README.md)。该来源说明不改变 Microsoft
语音组件或输出可能适用的条款，二次分发和商用前应结合使用环境核对相应许可。

具体许可证文件随对应组件保留在项目目录中。使用、修改或再分发前，请同时核对目标项目的
`SOURCE_PROVENANCE.md`、组件目录中的 `LICENSE`/`NOTICE`，以及 SDK 或素材供应方提供的条款。
未随公开仓分发的供应商 SDK、工具链和烧录工具，需要开发者从相应授权渠道单独获取。
