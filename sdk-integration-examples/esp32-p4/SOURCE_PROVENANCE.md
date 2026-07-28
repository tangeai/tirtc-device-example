# 来源与验证边界

## 来源快照

| 项目 | 值 |
| --- | --- |
| 源工程 | `tirtc_esp32p4_wifi_link_demo` |
| 源分支 | `master` |
| 源 HEAD | `8651471f8bbde59659922986d97f8f5735597c1e` |
| 源工作树 | 非干净，复制时有 2 项未提交修改 |
| 示例版本声明 | `1.0.2` |
| TiRTC SDK 版本声明 | `0.1.4` |
| 候选 `libTiRTC.a` SHA-256 | `DA0A165C062CBB3F2E176AEA2F517E8ECD5FF116F10FB5E9F8C4DB729C1AFE88` |

未提交修改位于 TiRTC 头文件和 ESP32-P4 静态库。候选使用源工作树当时的当前文件，因此
不能只按源 HEAD 推断候选内容。

收口调查确认候选静态库与 2026-07-01 归档的 ESP32-P4 `0.1.4` 正式交付包逐字节一致：
SHA-256 为 `DA0A165C062CBB3F2E176AEA2F517E8ECD5FF116F10FB5E9F8C4DB729C1AFE88`。
`basedef.h` 也与该包一致；`tiRTC.h` 唯一差异是增加标准 `#include <stddef.h>`，用于声明公开
接口使用的 `size_t`。因此可信契约是 `0.1.4`、source commit `e2c7020`、ESP-IDF `5.5.4`、
1000 Hz、单一 KCP 静态库。

## 公开筛选

已纳入：

- ESP-IDF 工程入口、`main/`、分区表、`sdkconfig`、`sdkconfig.defaults`。
- `dependencies.lock`。
- TiRTC SDK 公开头文件、版本文件和 ESP32-P4 静态库。
- README、版本、测试和交付内容说明。

已排除：

- `build/`、`firmware/`、`managed_components/`。
- `.git/`、`.agents/`、`.codex/`、`.vscode/`、`.clangd`。

候选配置只保留公开占位值。源工程保持原样。

## 验证边界

本候选已在 ESP-IDF `5.5.4` 中从空构建目录完成编译和链接，生成的验证固件只保存在
Git 本地忽略目录。静态库与归档正式包的契约已经核验；尚未执行烧录或真机测试。
