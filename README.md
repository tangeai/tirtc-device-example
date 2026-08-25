# TiRTC 设备端示例

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![ESP32-S3](https://img.shields.io/badge/Platform-ESP32--S3-000000?logo=espressif)](sdk-integration-examples/esp32-s3)
[![ESP32-P4](https://img.shields.io/badge/Platform-ESP32--P4-E7352C?logo=espressif)](sdk-integration-examples/esp32-p4)
[![G32S10X](https://img.shields.io/badge/Platform-G32S10X-1F6FEB)](sdk-integration-examples/g32s10x)

本仓库提供 ESP32-S3、ESP32-P4 和 G32S10X 平台的 TiRTC 设备端代码，并按用途分成三个入口：

- [最小 TiRTC 集成示例](sdk-integration-examples/README.md)：聚焦联网、TiRTC 生命周期、
  连接、音视频收发和释放，适合移植 SDK 与理解最小调用链。
- [最小系统例子](minimal-system-examples/README.md)：在最小集成基础上提供串口 AT 控制、
  ThingConnect 绑定、AI 对讲、设备互呼和预录媒体，适合在没有真实音视频外设时体验完整协议流程。
- [完整应用](complete-applications/README.md)：加入设备绑定、ThingConnect、UI 和目标板音频，
  并按平台提供二维码摄像头、AI 对讲、微信 VoIP、设备互呼或 OTA 等完整业务能力。

第一次接入建议先看对应平台的最小示例；需要观察完整协议流程时使用最小系统例子；开发带屏设备
或演示样机时进入完整应用。

## 快速开始

只想把固件跑起来，不需要先准备编译环境：

- **ESP32-S3/P4**：在目标项目 Release 下载 `*-full-*.bin`、`SHA256SUMS.txt` 和
  `release-manifest.json`，核对 SHA-256 后，用
  [Espressif ESP Tool](https://espressif.github.io/esptool-js/) 从 `0x0` 烧录完整镜像。
- **G32S10X**：在目标项目 Release 下载版本化主固件；完整应用还要同时下载 `fs` 和 `data`
  镜像。核对 SHA-256 后，按项目 README 与目标板 policy 使用君正 Cloner 烧录。

烧录完成后回到项目 README，按“首次启动”配置网络、设备身份并观察成功日志。

ESP32 完整镜像会覆盖整片 Flash，并重置 NVS 中原有的 Wi-Fi、绑定和本地设置。烧录后重新
配置一次即可；需要保留现场数据时，先使用项目自己的备份或 OTA 流程。

需要修改代码时，从目标项目自己的 Release Tag 开始会更稳。Tag 命名统一为
`<project-id>-v<MAJOR>.<MINOR>.<PATCH>`：

```bash
git clone https://github.com/tangeai/tirtc-device-example.git
cd tirtc-device-example
git checkout esp32-s3-device-monitor-v1.9.6
```

然后进入下表中的项目目录。每个项目 README 都给出依赖、配置、构建、烧录、成功现象和排障入口。

## 当前项目

### 最小 TiRTC 集成示例

| 平台 | 示例 | 版本 | TiRTC SDK | 来源版本 | 公开 Release |
| --- | --- | --- | --- | --- | --- |
| ESP32-S3 | [最小 TiRTC 集成示例](sdk-integration-examples/esp32-s3/README.md) | `1.2.0` | `2.2.1` | Tag `v1.2.0` / commit `44b24a5` | [`esp32-s3-minimal-integration-v1.2.0`](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-s3-minimal-integration-v1.2.0) |
| ESP32-P4 | [最小 TiRTC 集成示例](sdk-integration-examples/esp32-p4/README.md) | `1.1.1` | `2.2.1` | Tag `v1.1.1` / commit `0f36ddb` | [`esp32-p4-minimal-integration-v1.1.1`](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-p4-minimal-integration-v1.1.1) |
| G32S10X | [最小 TiRTC 集成示例](sdk-integration-examples/g32s10x/README.md) | `0.8.3` | `2.2.1` | Tag `v0.8.3` / commit `b2abc22` | [`g32s10x-minimal-integration-v0.8.3`](https://github.com/tangeai/tirtc-device-example/releases/tag/g32s10x-minimal-integration-v0.8.3) |

### 最小系统例子

| 平台 | 示例 | 版本 | TiRTC SDK | 来源版本 | 公开 Release |
| --- | --- | --- | --- | --- | --- |
| ESP32-S3 | [TiRTC 最小系统例子（AT ThingConnect）](minimal-system-examples/esp32-s3/README.md) | `0.8.0` | `2.2.1` | 独立源码快照 / 公开 Tag | [`esp32-s3-minimal-system-v0.8.0`](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-s3-minimal-system-v0.8.0) |
| ESP32-P4 | [TiRTC 最小系统例子（AT ThingConnect）](minimal-system-examples/esp32-p4/README.md) | `0.2.0` | `2.3.0` | 独立源码快照 / 公开 Tag | [`esp32-p4-minimal-system-v0.2.0`](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-p4-minimal-system-v0.2.0) |

### 完整应用

| 平台 | 应用 | 版本 | TiRTC SDK | 来源版本 | 公开 Release |
| --- | --- | --- | --- | --- | --- |
| ESP32-S3 | [Device Monitor](complete-applications/esp32-s3/device-monitor/README.md) | `1.9.6` | `2.3.0 mini` | Tag `v1.9.6` / commit `cdb5d7b` | [`esp32-s3-device-monitor-v1.9.6`](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-s3-device-monitor-v1.9.6) |
| ESP32-P4 | [Device App](complete-applications/esp32-p4/device-monitor/README.md) | `1.3.2` | `2.3.0` 定制兼容快照 | Tag `esp32-p4-device-app-v1.3.2` / commit `bc1ae8f` | [`esp32-p4-device-monitor-v1.3.2`](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-p4-device-monitor-v1.3.2) |
| G32S10X | [Device Monitor](complete-applications/g32s10x/device-monitor/README.md) | `0.1.1` | `2.2.1` | Tag `v0.1.1` / commit `5630152` | [`g32s10x-device-monitor-v0.1.1`](https://github.com/tangeai/tirtc-device-example/releases/tag/g32s10x-device-monitor-v0.1.1) |

八个项目独立使用 SemVer Tag。某个项目更新时，只提升并发布该项目版本；共享 SDK 或公共代码
确实影响多个项目时，各项目分别升版并在同一 commit 创建多个 Tag。每个 Release 的源码范围、
构建证据和附件 SHA-256 见该 Release 自带的 `release-manifest.json` 与
[版本与证据清单](docs/VERSIONS_CN.md)。

## 选择路径

### 我只想看最小 TiRTC 调用

进入 `sdk-integration-examples/<platform>/`。示例包含 SDK 头文件和目标平台静态库、网络和时间、
TiRTC 生命周期、连接与订阅、测试媒体以及资源释放。

### 我需要可观察的协议联调

进入 `minimal-system-examples/<platform>/`。最小系统例子保持轻量设备形态，通过串口 AT 驱动配网、绑定、
AI 对讲和设备互呼，并用预录媒体验证链路；它不依赖完整 UI、摄像头、麦克风或扬声器。

### 我想使用完整应用

进入 `complete-applications/<platform>/device-monitor/` 阅读目标板、功能、配置和构建说明。
正式固件只通过 [GitHub Releases](https://github.com/tangeai/tirtc-device-example/releases) 分发，
不进入 Git 历史。ESP32 固件使用
[Espressif ESP Tool](https://espressif.github.io/esptool-js/) 按 Release 清单烧录。

ESP32-P4 Device App `1.3.2` 提供源码和
`esp32p4-tirtc-device-monitor-full-v1.3.2.bin`。完整镜像只在 GitHub Release 中分发，
从 `0x0` 烧录；它会清除现有 NVS、Wi-Fi 和绑定信息，具体步骤见
[固件下载与校验](docs/RELEASES_CN.md)。

ESP32-S3 Device Monitor `1.9.6` 提供 RTC 双向音频、二维码联系人扫描、小钛、微信 VoIP、
设备互呼和 OTA。本次重点改善首次完整烧录后的呼叫准备、通话状态提示、联系人名称输入和
音量触摸反馈，并移除设置页中的手动 RTC 测试入口。固件使用 TiRTC SDK `2.3.0 mini`，
完整镜像 `esp32s3-tirtc-device-monitor-full-v1.9.6.bin` 只通过对应 Release 分发。

本版本的服务发现、业务 HTTP、设备 MQTT 和 TiRTC HTTPS 都使用认证传输：只接受
HTTPS/MQTTS，并校验证书链与主机名；不安全地址会被拒绝并报错，不降级到明文协议。

Web IPC 和设备互呼的当前人耳试听仍可感知轻微“沙沙电流声”。线上格式核对和 A-law
编解码自检缩小了排查范围，但底噪根因尚未证实；`1.9.6` 不宣称这一问题已经解决。

## 构建入口

ESP32-S3/P4：

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py reconfigure build
```

请先进入对应项目目录。P4 使用 ESP32-C6 Hosted/SDIO 提供 Wi-Fi，首次构建会按工程锁定文件
获取相关组件。

G32S10X 的供应商 SDK、工具链和 Cloner 不随本仓分发，具体入口见对应项目 README。

## 仓库结构

```text
tirtc-device-example/
├── sdk-integration-examples/
├── minimal-system-examples/
├── complete-applications/
├── docs/
├── README.md
└── LICENSE
```

## 文档

- [文档入口](docs/README_CN.md)
- [版本与验证边界](docs/VERSIONS_CN.md)
- [版本变更记录](docs/CHANGELOG_CN.md)
- [固件下载与校验](docs/RELEASES_CN.md)
- [第三方组件与 SDK](docs/THIRD_PARTY_NOTICES_CN.md)

## License

原创代码与文档使用 [MIT License](LICENSE)。第三方组件、TiRTC SDK、供应商源码和素材保留
各自条款，详见[第三方组件与 SDK](docs/THIRD_PARTY_NOTICES_CN.md)。
