# 版本变更记录

## 2026.08.13：ESP32-P4 日志示例 0.1.0

- 首次公开 P4 日志示例，提供精简中文 AT 指令、结构化脱敏日志和 ThingConnect 绑定流程。
- 同一份 P4 固件兼容 ESP32-C6 与 ESP32-C61 Wi-Fi 协处理器，使用 ESP-Hosted/SDIO 联网。
- 提供网页远程查看、预录 H264/G711A 媒体、设备互呼与消息、联系人备注和 AI 对讲。
- 采用 4 MB 完整镜像，统一使用 Espressif ESP Tool 从 `0x0` 烧录。
- 开发源没有 Git 历史，公开 Release 用源码快照、公开 commit、逐文件哈希和正式构建记录建立
  可复核身份，不虚构开发 Tag。
- 项目 Tag：`esp32-p4-logging-v0.1.0`；构建资产和校验值以 Release manifest 为准。

## 2026.08.13：ESP32-S3 日志示例 0.7.0

- 面向无专用音视频开发板的用户，提供可直接烧录到 4 MB 或更大 Flash 的完整体验固件。
- 用户交互收口为精简中文 AT 指令和中文日志，公开文档只保留一份按步骤操作的体验说明。
- 增加开发者平台绑定、网页远程查看、预录 H264/G711A 循环媒体、双机呼叫和字符串消息。
- 增加联系人备注、AI 故事/笑话/天气对讲，以及 AI 按“小李”等备注呼叫设备。
- 公开快照移除内部 AT 回归脚本，并排除包含 Wi-Fi、设备 ID、MAC 或绑定身份的截图。
- 项目 Tag：`esp32-s3-logging-v0.7.0`；构建资产和校验值以 Release 清单为准。

## 2026.08.11：ESP32-P4 Device Monitor 1.3.2

- 来源 Tag：`esp32-p4-device-app-v1.3.2`，commit `bc1ae8fbd9b64090503128985129a72e024c0551`。
- 公开项目 Tag：`esp32-p4-device-monitor-v1.3.2`。
- 新增 internal-RAM NVS worker，统一串行处理设备身份、音量、AI 头像、RTC 配置和绑定会话的持久化。
- RTC/WHIP 增加并发提交门控、过期回调识别和幂等断连；绑定重置改由 APP 控制队列接管。
- TiRTC SDK 保持 `2.3.0` 定制兼容快照，第三方组件版本不随应用补丁版本改写。
- 正式干净构建的 app 大小为 `6,927,360` 字节，SHA-256 为
  `2df6d9d626a05f19a4fd1f15eb854c54119a32ccd475090f6713f2629afc90e2`。
- `0x0` 完整镜像 `esp32p4-tirtc-device-monitor-full-v1.3.2.bin` 大小为
  `16,777,216` 字节，SHA-256 为 `87bfb67d1ba30d7f79663f63891e29f7f4f4367c80ff0d5cecb1b46f301d40e9`；该资产只通过
  GitHub Release 分发，不进入 Git 历史。

## 2026.08.10：Release 按项目独立版本化

- 七个项目改为“平台 + 项目 + 版本号”的独立 Tag；一个项目更新时，只发布该项目的新版本。
- 该次迁移后的公开 Tag 为：
  `esp32-s3-minimal-integration-v1.2.0`、
  `esp32-p4-minimal-integration-v1.1.1`、
  `g32s10x-minimal-integration-v0.8.3`、
  `esp32-s3-logging-v0.3.0`、
  `esp32-s3-device-monitor-v1.8.0`、
  `esp32-p4-device-monitor-v1.3.1` 和
  `g32s10x-device-monitor-v0.1.1`。
- 原日期型批次和旧分类 Release 已下线；公开文档、源码归档、manifest 与下载入口统一指向项目 Tag。
- 固件资产保持原正式构建字节不变。本次只迁移发布身份并重建单项目 manifest 和校验清单，没有重新编译。

## 2026.08.02.1：ESP32-P4 Device App 1.3.1 完整镜像补充

- 新增 `esp32p4-tirtc-device-monitor-full-v1.3.1.bin`，供开发者从 `0x0` 直接烧录体验。
- 完整镜像只进入 GitHub Release，不进入 Git 历史；该次补充后来并入当前 P4 项目 Release。
- P4 源码、SDK 和正式构建输入未变化；补丁更新只收口公开文档、manifest 和发布资产。
- 完整烧录会清除 NVS、Wi-Fi 和绑定信息，烧录后需要重新配置。

## 2026.08.02：ESP32-P4 Device App 1.3.1

### 媒体链路

- 摄像头完成帧增加单调 sequence，消费端会识别并丢弃倒退帧；采集节拍改为相位锁定，
  背压解除后从当前时间恢复，不集中补发过期帧。
- H264 下行把 TinyH264 helper 的双任务调度改为运行时显式配置，并增加通知竞争保护、
  解码阶段和 liveness 记录，以及有限的卡顿隔离与恢复入口。
- 数值型媒体参数集中到 `main/media/media_tuning.h`，下行调度集中到
  `main/services/call_video_renderer_config.h`，避免 Kconfig 与生成配置成为多份事实来源。
- GOP 统一按 `2s` 时长计算：IPC `20fps` 对应 `40` 帧，设备/微信通话 `15fps` 对应 `30` 帧。

### 资源与会话

- 大块媒体负载和后台 worker 明确使用 PSRAM；内部 RAM 留给实时控制、DMA 和同步对象，
  并增加内存水位迁移记录。
- 微信接听改为常驻 PSRAM worker；来电提示阶段不再提前取得 RTC 媒体所有权。
- SDK 仍为 `2.3.0` API，但 P4 静态库更新为带 HTTP DNS cache disable 回移补丁的定制兼容快照，
  避免缓存过期路径出现递归锁风险。

### 发布

- 来源 Tag：`esp32-p4-device-app-v1.3.1`，commit `7391464`。
- 公开项目 Tag：`esp32-p4-device-monitor-v1.3.1`。
- 源码和完整镜像现在由同一个项目 Release 提供。
- 最终公开快照完成静态门禁和干净构建。运行时通话、重复切换与长稳验证单独记录，
  不用构建结果代替真机证据。

## 2026.07.31：新增日志示例并更新 S3/P4 完整应用

### 信息架构

- 新增 `logging-examples/`，中文定位为“日志示例”。
- 将原 ESP32-S3 AT ThingConnect 示例从最小集成分类迁移到
  `logging-examples/esp32-s3/`，不保留重复源码。
- 根目录继续只突出最小集成、日志示例、完整应用、文档和必要仓库文件。

### ESP32-S3 日志示例 0.3.0

- 增加面向普通开发者的中文简洁日志、UTF-8 安全输出和串口 AT facade。
- 增加五组 AI 语音提示、两秒音频与八秒 H264 测试素材，以及 AI prompt 生成入口。
- 保留 ThingConnect 绑定、正式 MQTT、AI 对讲、联系人解析、设备互呼和竞态回归能力。
- 清理可重建且含供应方临时路径的 SDK 符号扫描输出。

### ESP32-S3 Device Monitor 1.8.0

- “小钛”新增联系人状态查询，以及按联系人备注或 ID 发起设备语音/视频呼叫和微信语音呼叫。
- 设备呼叫补齐 `call_type` 校验与生命周期路由；微信联系人兼容多种响应和字段别名。
- 增加联系人备注异步更新、NVS 缓存 schema v2 和九个常用备注预设。
- 新增 43 组文字的 `10/12/16` 三种尺寸资源，共 129 张运行时 PNG。
- TiRTC SDK 保持 `2.2.0`，分区表和依赖锁未变化。

### ESP32-P4 Device App 1.3.0

- 增加 AI 联系人查询、设备呼叫和微信呼叫，并修复嵌套 tool-call 的动作解析优先级。
- 微信 `audio/video` 类型贯穿应用、服务、ThingConnect、会话和媒体链路。
- 订阅成功后 `1s` 无首包时仅补发一次幂等视频订阅；WHIP 超时执行会话级清理。
- P4 IPC 上行保持 `1280x960@20fps`、`4Mbps` H264；设备呼叫和微信设备上行使用
  `480x320@15fps`、`800kbps` 起始码率 H264；微信下行请求 `640x480` MJPEG，并由硬件
  JPEG 解码后显示到 `480x320`。微信客户端原始采集分辨率不由本固件配置。
- TiRTC SDK 保持 `2.3.0`；本项目继续按源码范围交付。

### 发布

- 当次涉及日志示例 `0.3.0`、S3 Device Monitor `1.8.0` 和 P4 Device App `1.3.0`；
  当前公开项目 Tag 分别为 `esp32-s3-logging-v0.3.0`、`esp32-s3-device-monitor-v1.8.0`
  和 `esp32-p4-device-monitor-v1.3.1`。
- ESP32-S3 日志示例和 S3 Device Monitor 的正式构建资产只通过 GitHub Releases 分发。
- ESP32-P4 Device App 不生成占位固件；其他平台和最小示例保持上一公开版本。

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
- 该版本当时以 AT ThingConnect 示例身份发布；当前维护版本已归入日志示例，公开 Tag 为
  `esp32-s3-logging-v0.3.0`。
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
