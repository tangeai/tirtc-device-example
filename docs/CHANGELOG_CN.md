# 版本变更记录

## 0.2.0：ESP32-S3 AT ThingConnect 示例首次发布

### 新增

- 增加与 ESP32-S3 最小示例平级的 AT ThingConnect 协议示例。
- 设备侧配网、绑定、状态、联系人、AI 对讲和普通呼叫全部通过串口 AT 操作。
- 集成 ThingConnect 服务发现、Report、设备 token、正式 MQTT、解绑重绑和 token 刷新。
- 集成 AI token、WHIP、字幕、打断，以及内建 `call_device` 动作到普通设备呼叫的原子交接。
- 增加普通设备呼叫的主叫、被叫、接听、拒绝、取消、挂断和会话恢复。
- 增加预录 G711A/H264 媒体、双串口回归工具、定向竞态测试和脱敏证据格式。
- 增加使用说明、AI 呼叫设备体验流程、架构、AT 协议、上游契约和验证文档。

### 发布

- 正式版本为 `0.2.0`，TiRTC SDK 为 `2.2.1`，ESP-IDF 为 `5.5.4`。
- 使用独立公开 Tag `esp32-s3-at-thingconnect-v0.2.0`，不移动既有统一日期 Tag。
- 正式 `0x0` 完整镜像、manifest 和 SHA-256 只通过 GitHub Release 分发。
- 工程不包含微信 VoIP、OTA、SoftAP 网页配网、UI 或板级音视频外设驱动。

## 2026.07.30：六项目统一发布

### 版本

- ESP32-S3 最小 TiRTC 集成示例更新为 `1.2.0`，TiRTC SDK `2.2.1`，来源 Tag `v1.2.0`。
- ESP32-P4 最小 TiRTC 集成示例更新为 `1.1.1`，TiRTC SDK `2.2.1`，来源 Tag `v1.1.1`。
- G32S10X 最小 TiRTC 集成示例更新为 `0.8.3`，TiRTC SDK `2.2.1`，来源 Tag `v0.8.3`。
- ESP32-S3 Device Monitor 更新为 `1.7.6`，TiRTC SDK `2.2.0`，来源 Tag `v1.7.6`。
- ESP32-P4 Device App 更新为 `1.2.3`，TiRTC SDK `2.3.0`，来源 Tag
  `esp32-p4-device-app-v1.2.3`。
- G32S10X Device Monitor 首次纳入完整应用，版本 `0.1.1`，TiRTC SDK `2.2.1`，
  来源 Tag `v0.1.1`。

### 结构

- 保持 `sdk-integration-examples/`、`complete-applications/` 和 `docs/` 三个对外入口。
- 三个平台分别提供最小 TiRTC 集成示例和完整应用，共六个公开项目。
- 统一对外项目表、文档导航、版本来源和固件下载边界。

### 构建与交付

- 六个项目均以开发侧不可移动 Tag 和 commit 作为源码交接基线。
- 已核对 Tag 指向、真实 diff、版本字段、公开文件范围、文档链接、凭据与 SDK 哈希。
- 五个固件项目从统一公开 commit 完成正式干净构建，资产通过
  `release-manifest.json` 和 `SHA256SUMS.txt` 绑定。
- ESP32-P4 Device App `1.2.3` 本次按源码范围交付。
- 固件构建产物只通过
  [GitHub Releases](https://github.com/tangeai/tirtc-device-example/releases) 分发，
  不进入 Git 历史。
- ESP32-S3/P4 统一使用
  [Espressif ESP Tool](https://espressif.github.io/esptool-js/)；G32S10X 使用君正 Cloner。

## 2026.07.28：设备示例统一发布

### 新增

- 建立 `sdk-integration-examples/`，中文定位为“SDK 接入与历史示例”。
- 纳入 ESP32-S3、ESP32-P4、G32S10X 三个平台的最小 TiRTC 集成示例。
- 平台目录直接作为示例根目录，不把当前 Wi-Fi 实现固化为项目身份。
- 为三个示例增加来源工作树状态、筛选范围和验证边界说明。
- 为 G32S10X 增加供应商 SDK 要求和不含本机绝对路径的构建辅助脚本。
- 建立 `complete-applications/`，中文定位为“完整应用”。
- 增加面向外部开发者的版本与证据、固件下载和项目导航文档。

### 移动

- `esp32-s3/trtc-advanced` -> `complete-applications/esp32-s3/device-monitor`
- `esp32-p4/trtc-advanced` -> `complete-applications/esp32-p4/device-monitor`

### 验证

- 五个公开项目均已从当前候选目录完成干净构建。
- 已完成公开目录、仓库卫生、凭据、文档链接和逐文件 SHA-256 manifest 静态检查。

### 发布边界

- 本次源码范围为三个最小 TiRTC 集成示例，以及 ESP32-S3、ESP32-P4 两个 Device Monitor 完整应用。
- G32S10X 供应商 SDK、工具链、WSL 构建树和完整应用继续按外部依赖或内部工程管理。
- 发布协调脚本、Release manifest 生成逻辑和审阅材料保留在本地内部 Skill。
- 五个项目的干净构建结果作为 Release assets 分发，供开发者继续完成目标板集成验证。
- `.bin/.zip/.elf` 构建产物通过 GitHub Releases 分发，不进入 Git 历史。
