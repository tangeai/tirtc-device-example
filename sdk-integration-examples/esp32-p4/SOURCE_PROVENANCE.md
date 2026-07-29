# 来源与验证边界

## 版本身份

| 项目 | 值 |
| --- | --- |
| 内部开发仓 | `tirtc_esp32p4_wifi_link_demo` |
| 来源 tag | `v1.1.1` |
| 来源 commit | `0f36ddbb053d09c52efcd66cb7e4eb8cd113ee32` |
| 示例版本 | `1.1.1` |
| TiRTC SDK | `2.2.1` |
| SDK source commit | `3a33bf4ae51b3ab9eb246648adb274d0fae32ebf` |
| SDK 静态库 | `components/tirtc_sdk/lib/esp32p4/libTiRTC.a` |
| SDK 静态库 SHA-256 | `a8eadc99e97e9d6fcc7d871963d3456484ed3625469804a7ff6718218b117d65` |

检查来源时，内部开发仓位于上述 tag 对应 commit。工作树另有未跟踪文件
`IPC_COORDINATION.md`；该文件不属于 `v1.1.1`，也未导入本候选目录。

## 导入范围

候选目录保留公开接入所需的源码、配置、测试媒体、SDK 头文件、静态库、SDK
manifest 和 ESP-IDF 依赖锁定信息。构建目录、固件产物、工具链、ESP-IDF
展开树、真实凭据、本机配置和 Git 元数据不属于公开源码。

SDK manifest 中的临时 ESP-IDF 与构建根目录已替换为
`<ESP_IDF_ROOT>`、`<SDK_BUILD_ROOT>` 等稳定占位符；该处理只规范化路径，不修改
符号记录、SDK 头文件、静态库或其哈希。

## 验证边界

本次发布前准备已静态核对示例 tag/commit、SDK 版本、SDK source commit、库路径
和 SHA-256，并检查公开路径中不保留临时构建目录。本次未执行编译、烧录或真机
功能验证；这些证据应在正式发布前的独立验证阶段生成并记录。
