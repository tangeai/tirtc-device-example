---
name: tirtc-device-repo-management
description: 管理 TiRTC 设备示例本地仓、公开仓 tirtc-device-example 目录规划、发布前仓库收口和架构梳理入口。Use when the user asks to manage the local repo, prepare the future GitHub repo, align repository layout, inspect branches/remotes/status, plan migration into esp32-s3/trtc-advanced, or start an architecture review before release.
---

# TiRTC 设备示例仓库管理

## 核心原则

先保护当前可运行工程，再规划公开仓结构。不要为了目录漂亮而提前移动源码、改远端、推分支或打 tag，除非用户明确要求。

当前工程默认位置：

```text
C:\Users\61852\Desktop\Device_OTA_S3_Release\esp32s3_tirtc_device_monitor_demo
```

统一公开仓：

```text
https://github.com/tangeai/tirtc-device-example
```

当前 ESP32-S3 设备监控示例归属：

```text
esp32-s3/trtc-advanced/
```

详细公开仓目录规划见 `docs/release/PUBLIC_REPO_LAYOUT_CN.md`。需要迁移或重排目录时，先读 `references/repo-governance.md`。

## 工作流

### 1. 先做仓库快照

每次处理仓库管理、发布收口或架构梳理前，先执行只读检查：

```powershell
git status --short --branch
git log --oneline --decorate -8
git remote -v
```

如果工作区是脏的，先把改动分成三类：

- 已验证功能改动
- 发布/文档/资产改动
- 临时调试、探针、日志、实验文件

不要在没有分类前提交、推送、rebase、reset 或删除文件。

### 2. 明确本地仓和公开仓边界

本地仓当前仍按 ESP-IDF 工程原路径工作，优先保证能编译、能打包、能烧录验证。

公开仓结构是发布形态。同步源码前必须先确认：

- 当前路径完整构建通过
- 版本号、文档和 GitHub Release 说明已收口
- 敏感信息已经排除
- 新目录下相对路径、脚本、文档链接、CMake 路径能重新跑通

### 3. 架构梳理入口

用户说“梳理架构”“整体结构”“分层”“本地仓管理”时，先产出架构地图，不急着改代码。

架构地图至少覆盖：

- `main/application`：应用生命周期、页面进入退出、资源申请释放
- `main/drivers`：屏幕、触摸、音频、摄像头等硬件驱动
- `main/connectivity`：Wi-Fi、网络状态、时间同步
- `main/protocols`：TiRTC 抽象层和 SDK 边界
- `main/services`：AI Chat、微信 VoIP、OTA、绑定、媒体服务
- `main/ui`：LVGL UI、页面事件和显示资产
- `components/tirtc_sdk`：SDK 版本、库文件和接口约束
- GitHub Releases：完整镜像、OTA app、维护包、校验文件和发布说明
- `docs`：使用、烧录、发布、架构说明

输出时说明每层的职责、上游依赖、下游调用、能否独立替换、退出/释放边界和发布风险。

### 4. 发布前仓库收口

发布前必须检查：

- 工程版本号是否和目标版本一致
- docs 和 GitHub Release 链接是否只指向当前发布版本
- 暂存区是否包含旧调试代码或过期资产
- `docs/debug` 是否应该进入正式仓
- 远端地址是否符合本次发布目标
- 是否存在不该公开的设备 ID、密钥、token、个人联系人或私有服务器凭证
- `git ls-files "*.bin" "*.zip"` 是否为空

只在完成这些检查后，才继续编译、打包、提交或推送。

## 禁止事项

- 不要在用户只要求“先看一下”“先规划”时推送远端。
- 不要在发布收口前移动整个 ESP-IDF 工程目录。
- 不要把旧版本 bin/zip 混入当前发布版本。
- 不要把测试探针、heap probe、临时日志说明当成正式文档提交。
- 不要把真实设备凭证或个人业务配置写入公开仓。
