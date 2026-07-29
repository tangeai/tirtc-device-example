# 来源与验证边界

## 版本身份

| 项目 | 值 |
| --- | --- |
| 内部开发仓 | `tirtc_esp32s3_wifi_link_demo` |
| 来源 tag | `v1.2.0` |
| 来源 commit | `44b24a50434ae5e32212a1c014945107076af29c` |
| 示例版本 | `1.2.0` |
| TiRTC SDK | `2.2.1` |
| SDK source commit | `3a33bf4ae51b3ab9eb246648adb274d0fae32ebf` |
| SDK 静态库 | `components/tirtc_sdk/lib/esp32s3/libTiRTC.a` |
| SDK 静态库 SHA-256 | `dc9f869194584fd43fe091f887342d170a1422e657ce1fc2a144c90a3ade1f8e` |

检查来源时，内部开发仓位于上述 tag 对应 commit，工作树无未提交改动。

## 导入范围

候选目录保留公开接入所需的源码、配置模板、测试媒体、SDK 头文件、静态库和
SDK manifest。构建目录、固件产物、工具链、ESP-IDF 展开树、真实凭据、本机
配置和 Git 元数据不属于公开源码。

SDK manifest 中的临时构建根目录已替换为
`<SDK_BUILD_ROOT>` 等稳定占位符；该处理只规范化路径，不修改符号记录、SDK
头文件、静态库或其哈希。

## 验证边界

本次发布前准备已静态核对示例 tag/commit、SDK 版本、SDK source commit、库路径
和 SHA-256，并检查公开路径中不保留临时构建目录。本次未执行编译、烧录或真机
功能验证；这些证据应在正式发布前的独立验证阶段生成并记录。
