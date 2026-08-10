# 源码来源

## 当前发布身份

- 项目：TiRTC ESP32-S3 AT ThingConnect 日志示例
- 版本：`0.3.0`
- 公开路径：`logging-examples/esp32-s3`
- 开发源快照：93 个文件
- 开发源快照 SHA-256：
  `8d0dc2cab067952e5f40e7b60d7da60012a2c96830cd79c885ed76dc791a37db`

开发交接来自独立源码树，不带可用于对外引用的 Git commit 或 Tag。上面的
93 文件哈希用于固定进入公开整理前的源快照；公开文档和版本字段收口会改变
文件清单，因此最终公开 commit、Tag 和 Release manifest 才是 `0.3.0` 的
发布身份。Release manifest 应记录完整 commit，以及每个公开源码文件和
发布资产的 SHA-256。

## 公开发布身份

- 公开项目 Tag：`esp32-s3-logging-v0.3.0`
- Release：<https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-s3-logging-v0.3.0>

`0.3.0` 已归入日志示例分类。公开 Tag、源码归档、完整镜像、manifest 和校验清单使用同一
项目版本；开发源快照仍按上节哈希追溯，不为统一格式虚构上游 Git commit。

## 纳入范围

- ESP-IDF 工程文件、`main/` 和应用组件。
- TiRTC SDK 头文件、ESP32-S3 静态库、版本和构建清单。
- AT、架构、验证、使用和体验文档。
- G711A/H264 示例测试输入及其媒体配置。
- AT 回归、场景、采集和静态校验工具。

以下内容不进入公开源码：开发 `build*` 目录、生成的 `sdkconfig`、串口证据、
Python 缓存、编辑器状态、日志、本地发布暂存，以及 Wi-Fi 凭据、已绑定设备
身份、密钥和短期 token。

## 依赖身份

- ESP-IDF：`v5.5.4`
- TiRTC SDK：`v2.2.1`
- TiRTC SDK commit：`3a33bf4ae51b3ab9eb246648adb274d0fae32ebf`
- TiRTC 静态库 SHA-256：
  `dc9f869194584fd43fe091f887342d170a1422e657ce1fc2a144c90a3ade1f8e`

构建、目标板烧录、平台绑定、AI、设备呼叫、竞态和真实媒体验证属于独立
证据层。Release manifest 和 Release notes 应分别记录本版本实际完成的
证据，不用静态文件检查替代运行验证。
