# 源码来源

## 当前发布身份

- 项目：TiRTC ESP32-S3 最小系统例子
- 版本：`0.8.0`
- 公开路径：`minimal-system-examples/esp32-s3`
- 公开 Tag：`esp32-s3-minimal-system-v0.8.0`

## 开发来源

开发来源是独立源码目录，没有可公开引用的 Git commit 或 Tag。公开发布不虚构开发提交；
正式身份由公开 Tag、项目树和 Release manifest 共同确定。

- 核心快照：根配置文件及 `components/`、`main/`、`media/`，共 `88` 个文件
- 核心快照 SHA-256：
  `8cdcb658f6e7eddcc72c2b4f3b0259a5e2175276d4b766d5ca237e51018b5956`
- 公开核心：从上述 `88` 个输入文件中排除 `2` 份可重建的 SDK 符号扫描输出，共 `86` 个文件
- 操作截图：README 引用 `30` 张；新增的 `14` 张在公开前完成不可逆像素替换并清除可识别元数据

公开整理纳入运行源码、SDK、媒体输入、必要配置和一份完整体验文档。开发构建目录、生成的
`sdkconfig`、内部 AT 回归工具、串口原始记录、真实 Wi-Fi 与设备身份均不进入公开源码。

本版本相对上一公开版本没有功能代码、SDK、媒体或板级配置变化；只统一对外项目身份，并补全
经过脱敏的操作截图。开发阶段近期的三次自动化回归记录均未通过，根因尚未证实，因此截图只用于
说明操作步骤，不作为最终快照的真机流程通过证据。

## 依赖身份

- ESP-IDF：`v5.5.4`
- TiRTC SDK：`v2.2.1`
- TiRTC SDK commit：`3a33bf4ae51b3ab9eb246648adb274d0fae32ebf`
- TiRTC 静态库 SHA-256：
  `dc9f869194584fd43fe091f887342d170a1422e657ce1fc2a144c90a3ade1f8e`

BIN、ELF 和构建目录不提交到 Git。正式完整镜像只通过对应 GitHub Release 分发，并由 manifest
绑定到公开 commit、构建命令、SDK、配置和 SHA-256。
