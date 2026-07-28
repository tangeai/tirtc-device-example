# 来源与验证边界

## 来源快照

| 项目 | 值 |
| --- | --- |
| 源工程 | `tirtc_g32s10x_wifi_link_demo` |
| 源分支 | `codex/g32s10x-wifi-link-adaptation` |
| 源 HEAD | `697f21aec1ab956fdf38b2d8c28c127e818eb73c` |
| 源工作树 | 非干净，复制时有 18 项修改或未跟踪内容 |
| 示例版本声明 | `0.8.1` |
| TiRTC SDK 版本声明 | `2.2.0` |
| 候选 `libTiRTC.a` SHA-256 | `FFE521FCA1B882F32FF217682118D862C5740D68191CB88A5AFFA55ECD4ECF9A` |

候选只复制源仓中定义为对外交付单元的 `tirtc_g32s10x_wifi_link_demo/`。媒体素材、配置说明、
代码和测试中包含未提交更新，因此这是待审阅的源工作树快照。

## 公开筛选

已纳入：

- 最小 TiRTC 生命周期、ATBM WiFi、lwIP/NTP、MJPEG/PCMA 示例源码。
- G32S10X 平台覆盖、构建接线、defconfig 和 TiRTC G32 静态库。
- 配置、架构、移植、素材许可证和主机解析测试。
- 只使用环境变量的最小 SDK 构建辅助脚本。

已排除：

- WSL 中展开的君正 SDK 与构建输出。
- Windows `building/`、Cloner、供应商工具链和本地配置。
- 源仓外层含本机绝对路径的编排脚本。
- `.git/`、`.playwright-cli/` 和个人工作区文件。

## 验证边界

本候选已在 WSL 中以 `im_sdk_v0.4.0` 的独立干净副本和
`riscv32-ingenic-g32s10-elf-tools-r1.0.5` 完成编译和链接。构建使用空凭据占位配置，
没有烧录或真机验证。供应商 SDK 仍需由开发者按授权渠道单独取得。
