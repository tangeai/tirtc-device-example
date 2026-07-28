# 版本与证据清单

本文件把“代码来自哪里”“使用什么 SDK”“是否构建过”“是否在真机验证过”分开记录。
历史证据不会自动继承为目录整理后的新证据。

## 发布候选基线

| 项目 | 发布仓路径 | 版本 | TiRTC SDK | 来源基线 | 来源状态 |
| --- | --- | --- | --- | --- | --- |
| ESP32-S3 最小 TiRTC 集成示例 | `sdk-integration-examples/esp32-s3` | `1.1.1` | `2.2.1` | `94c5b3348169c8c3813f8be755869fbecdb95957` | 源工作树非干净，20 项状态；候选已脱敏并按二进制 BuildInfo 校正版本 |
| ESP32-P4 最小 TiRTC 集成示例 | `sdk-integration-examples/esp32-p4` | `1.0.2` | `0.1.4` | `8651471f8bbde59659922986d97f8f5735597c1e` | 源工作树非干净，SDK 头文件与静态库有未提交修改 |
| G32S10X 最小 TiRTC 集成示例 | `sdk-integration-examples/g32s10x` | `0.8.1` | `2.2.0` | `697f21aec1ab956fdf38b2d8c28c127e818eb73c` | 源工作树非干净，18 项状态；只复制公开交付单元 |
| ESP32-S3 Device Monitor | `complete-applications/esp32-s3/device-monitor` | `0.7.5` | `2.2.0`，BuildInfo commit `1df9e045a9dc` | 发布仓 commit `31eb69399ffe8732ff48a4ca0f4f322934a376a7` | 从原路径 `esp32-s3/trtc-advanced` 移入新分类 |
| ESP32-P4 Device Monitor | `complete-applications/esp32-p4/device-monitor` | `1.1.0` | `2.2.0` | 发布仓 commit `31eb69399ffe8732ff48a4ca0f4f322934a376a7` | 从原路径 `esp32-p4/trtc-advanced` 移入新分类 |

最小 TiRTC 集成示例使用复制时的源工作树当前文件，不是只导出 HEAD。每个示例的
`SOURCE_PROVENANCE.md` 记录了具体筛选范围和边界。

## 证据分层

| 项目 | 静态契约 | 构建证据 | 真机证据 | 本次整理后状态 |
| --- | --- | --- | --- | --- |
| ESP32-S3 最小 TiRTC 集成示例 | 头文件与 Tag `v2.2.1` 一致；静态库 BuildInfo 为 `v2.2.1` / `3a33bf4ae51b` | ESP-IDF `5.5.4` 干净构建通过 | 源工程有历史说明 | 供目标板集成验证 |
| ESP32-P4 最小 TiRTC 集成示例 | 静态库与归档正式包 SHA-256 一致；头文件只有 `stddef.h` 兼容补丁 | ESP-IDF `5.5.4` 干净构建通过 | 源工程有历史说明 | 供目标板集成验证 |
| G32S10X 最小 TiRTC 集成示例 | 版本、G32 静态库与供应商 SDK 要求已记录 | `im_sdk_v0.4.0` 隔离干净副本构建通过 | 以本次构建证据为基线 | 供目标板集成验证 |
| ESP32-S3 Device Monitor | SDK 版本、静态库 SHA-256、ESP-IDF 版本已记录 | ESP-IDF `5.5.4` 干净构建通过 | 整理前版本有历史发布与设备体验资料 | 供目标板继续验证 |
| ESP32-P4 Device Monitor | SDK 版本、静态库 SHA-256、ESP-IDF 版本已记录 | ESP-IDF `5.5.4` 干净构建通过 | 当前版本以本次构建证据为基线 | 供目标板继续验证 |

静态契约说明头文件、静态库、版本文件和目标平台的组合；干净构建进一步证明源码、ABI 和
链接闭环。目标板功能验收以对应开发板、固件哈希、串口日志或测试记录为准。

## G32S10X 完整应用边界

G32S10X 最小 TiRTC 集成示例已经纳入。G32S10X 完整应用仍依赖内部来源、供应商 SDK、
WSL 构建树和补丁收口，本次不纳入公开源码。

完整应用公开前至少需要：

- 明确供应商 SDK、工具链、Cloner 和示例代码的公开许可证边界。
- 把发布源码收口为不依赖本机绝对路径的自包含目录。
- 锁定 TiRTC SDK 版本、静态库哈希和目标 G32S10X ABI。
- 在干净 WSL 环境复现构建。
- 分开记录最小示例与完整应用的真机验证矩阵。
