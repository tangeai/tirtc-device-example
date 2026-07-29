# 来源与验证边界

## 来源快照

| 项目 | 值 |
| --- | --- |
| 源工程 | `tirtc_g32s10x_wifi_link_demo` |
| 源分支 | `codex/g32s10x-wifi-link-adaptation` |
| 源标签 | `v0.8.3` |
| 源提交 | `b2abc223db64a36edbbcc92b9a9d7e5313dd3f72` |
| 源工作树 | 标签指向上述提交；同步时源工作树干净 |
| 示例版本 | `0.8.3` |
| TiRTC SDK 版本 | `2.2.1` |
| SDK 交付状态 | `candidate` |
| 候选 `libTiRTC.a` SHA-256 | `33e889d70d4459587faf57f611e9648d52accb2bc02d0ec75205a52e9ca27fba` |

候选内容来自 `v0.8.3` 的不可变源码快照。公开目录保持“最小 TiRTC 集成示例”的项目身份，
不把当前 WiFi Link 实现固化为额外目录层级。

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

## 静态清单

`sdk/manifest/` 保存 SDK 版本、来源提交、工具链契约、静态库大小与哈希，以及下层库
嵌入关系。该清单和 `sdk/VERSION.md` 共同约束候选中的 TiRTC SDK 快照；开发者应以
`STATIC_LIBRARY_SHA256` 核对 `sdk/lib/g32/libTiRTC.a`。

## 验证边界

来源版本的构建记录使用 `im_sdk_v0.4.0` 与
`riscv32-ingenic-g32s10-elf-tools-r1.0.5`。本次公开候选同步只核对源码、版本和
静态库哈希；供应商 SDK 与工具链仍需由开发者按授权渠道单独取得。
