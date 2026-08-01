# TiRTC 设备端示例

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![ESP32-S3](https://img.shields.io/badge/Platform-ESP32--S3-000000?logo=espressif)](sdk-integration-examples/esp32-s3)
[![ESP32-P4](https://img.shields.io/badge/Platform-ESP32--P4-E7352C?logo=espressif)](sdk-integration-examples/esp32-p4)
[![G32S10X](https://img.shields.io/badge/Platform-G32S10X-1F6FEB)](sdk-integration-examples/g32s10x)

本仓库提供 ESP32-S3、ESP32-P4 和 G32S10X 平台的 TiRTC 设备端代码，并按用途分成三个入口：

- [最小 TiRTC 集成示例](sdk-integration-examples/README.md)：聚焦联网、TiRTC 生命周期、
  连接、音视频收发和释放，适合移植 SDK 与理解最小调用链。
- [日志示例](logging-examples/README.md)：在最小集成基础上提供可读、可脱敏、可自动检查的
  设备日志与串口 AT 控制面，适合联调 ThingConnect、AI 对讲和设备互呼。
- [完整应用](complete-applications/README.md)：加入设备绑定、ThingConnect、UI、真实摄像头和
  音频，并提供 AI 对讲、微信 VoIP、设备互呼或 OTA 等完整业务能力。

第一次接入建议先看对应平台的最小示例；需要观察完整协议流程时使用日志示例；开发带屏设备
或演示样机时进入完整应用。

## 快速开始

只想把固件跑起来，不需要先准备 ESP-IDF：

1. 打开 [GitHub Releases](https://github.com/tangeai/tirtc-device-example/releases)，选择目标版本。
2. 下载项目对应的 `*-full-*.bin`、`SHA256SUMS.txt` 和 `release-manifest.json`。
3. 先核对 SHA-256，再用 [Espressif ESP Tool](https://espressif.github.io/esptool-js/) 将完整镜像写入 `0x0`。
4. 回到项目 README，按“首次启动”配置网络、设备身份并观察成功日志。

本次 S3 完整镜像会覆盖整片 Flash，并重置 NVS 中原有的 Wi-Fi、绑定和本地设置。烧录后重新
配置一次即可；需要保留现场数据时，先使用项目自己的备份或 OTA 流程。

需要修改代码，从统一 Tag 开始会更稳：

```bash
git clone https://github.com/tangeai/tirtc-device-example.git
cd tirtc-device-example
git checkout tirtc-device-examples-v2026.07.31
```

然后进入下表中的项目目录。每个项目 README 都给出依赖、配置、构建、烧录、成功现象和排障入口。

## 当前项目

### 最小 TiRTC 集成示例

| 平台 | 示例 | 版本 | TiRTC SDK | 来源版本 | Release 交付 |
| --- | --- | --- | --- | --- | --- |
| ESP32-S3 | [最小 TiRTC 集成示例](sdk-integration-examples/esp32-s3/README.md) | `1.2.0` | `2.2.1` | Tag `v1.2.0` / commit `44b24a5` | 源码与 `0x0` 完整镜像 |
| ESP32-P4 | [最小 TiRTC 集成示例](sdk-integration-examples/esp32-p4/README.md) | `1.1.1` | `2.2.1` | Tag `v1.1.1` / commit `0f36ddb` | 源码与 `0x0` 完整镜像 |
| G32S10X | [最小 TiRTC 集成示例](sdk-integration-examples/g32s10x/README.md) | `0.8.3` | `2.2.1` | Tag `v0.8.3` / commit `b2abc22` | 源码与 `rtos-with-spl.bin` |

### 日志示例

| 平台 | 示例 | 版本 | TiRTC SDK | 来源版本 | Release 交付 |
| --- | --- | --- | --- | --- | --- |
| ESP32-S3 | [TiRTC 日志示例（AT ThingConnect）](logging-examples/esp32-s3/README.md) | `0.3.0` | `2.2.1` | 独立源码快照 / public commit | 源码与 `0x0` 完整镜像 |

### 完整应用

| 平台 | 应用 | 版本 | TiRTC SDK | 来源版本 | Release 交付 |
| --- | --- | --- | --- | --- | --- |
| ESP32-S3 | [Device Monitor](complete-applications/esp32-s3/device-monitor/README.md) | `1.8.0` | `2.2.0` | Tag `v1.8.0` / commit `8211e3b` | 源码、完整镜像与 OTA app |
| ESP32-P4 | [Device App](complete-applications/esp32-p4/device-monitor/README.md) | `1.3.0` | `2.3.0` | Tag `esp32-p4-device-app-v1.3.0` / commit `dacf8e6` | 源码 |
| G32S10X | [Device Monitor](complete-applications/g32s10x/device-monitor/README.md) | `0.1.1` | `2.2.1` | Tag `v0.1.1` / commit `5630152` | 源码、主固件与两个 YAFFS 镜像 |

本次更新由 `tirtc-device-examples-v2026.07.31` 锁定，范围为 ESP32-S3 日志示例 `0.3.0`、
ESP32-S3 Device Monitor `1.8.0` 和 ESP32-P4 Device App `1.3.0`。未列入本次范围的项目保持
上一公开版本。固件资产来自最终公开候选的正式干净构建，文件用途、大小、Flash 地址和
SHA-256 见当次 `release-manifest.json` 与[版本与证据清单](docs/VERSIONS_CN.md)。

## 选择路径

### 我只想看最小 TiRTC 调用

进入 `sdk-integration-examples/<platform>/`。示例包含 SDK 头文件和目标平台静态库、网络和时间、
TiRTC 生命周期、连接与订阅、测试媒体以及资源释放。

### 我需要可观察的协议联调

进入 `logging-examples/<platform>/`。日志示例保持轻量设备形态，通过串口 AT 驱动配网、绑定、
AI 对讲和设备互呼，并输出稳定、脱敏的状态日志；它不是完整 UI 应用。

### 我想使用完整应用

进入 `complete-applications/<platform>/device-monitor/` 阅读目标板、功能、配置和构建说明。
正式固件只通过 [GitHub Releases](https://github.com/tangeai/tirtc-device-example/releases) 分发，
不进入 Git 历史。ESP32 固件使用
[Espressif ESP Tool](https://espressif.github.io/esptool-js/) 按 Release 清单烧录。

ESP32-P4 Device App `1.3.0` 按源码范围交付，不提供预编译 BIN。

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
├── logging-examples/
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
