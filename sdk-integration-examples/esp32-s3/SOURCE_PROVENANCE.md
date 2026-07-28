# 来源与验证边界

## 来源快照

| 项目 | 值 |
| --- | --- |
| 源工程 | `tirtc_esp32s3_wifi_link_demo` |
| 源分支 | `master` |
| 源 HEAD | `94c5b3348169c8c3813f8be755869fbecdb95957` |
| 源工作树 | 非干净，复制时有 20 项修改、删除或未跟踪文件 |
| 示例版本声明 | `1.1.1` |
| 源文档原 TiRTC SDK 声明 | `2.2.0`，与实际候选二进制不一致 |
| 候选可信 TiRTC SDK 契约 | `2.2.1`，tag `v2.2.1`，BuildInfo commit `3a33bf4ae51b`，tgtrp `v1.4.6` |
| 候选 `libTiRTC.a` SHA-256 | `E039A074E04A0F963C95458EC77DB4B5D6A9693AE7E6463E3C86FB66C39340BE` |

候选使用源工作树当时的当前文件，而不是只导出 HEAD。SDK 升级、应用配置和 TiRTC 接入代码
包含未提交变更，因此本目录是“待审阅快照”，不能把源 HEAD 单独当作完整来源证明。
收口调查确认：候选 `tiRTC.h` 与 SDK Tag `v2.2.1` 的头文件一致，版本宏为 `2.2.1`；静态库
内嵌 BuildInfo 为 `v2.2.1` / `3a33bf4ae51b` / tgtrp `v1.4.6`。旧版本文件记录的是另一份
`2.2.0` 静态库，已按候选实际字节校正。是否保留这份 `2.2.1` 候选作为发布内容，仍需用户确认。

## 公开筛选

已纳入：

- ESP-IDF 工程入口、`main/`、分区表和 `sdkconfig.defaults`。
- TiRTC SDK 公开头文件、版本文件和 ESP32-S3 静态库。
- README、版本、测试和交付内容说明。

已排除：

- `build/`、`firmware/`、`sdk_source/`、`sdkconfig.old`。
- `.git/`、`.vscode/`、`.clangd`。
- 本地 SDK 升级过程文档。

源工作树中的 Wi-Fi、设备 ID、设备 Secret 和 Token 测试值没有复制到公开配置；候选配置已
替换为 `your_*`、`peer_*` 占位符。源工程保持原样。

## 验证边界

本候选已在 ESP-IDF `5.5.4` 中从空构建目录完成编译和链接，生成的验证固件只保存在
Git 本地忽略目录。尚未执行烧录或真机测试；源工程文档中的历史运行记录不能写成当前候选的
真机证据。
