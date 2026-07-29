# TiRTC 设备端示例

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![ESP32-S3](https://img.shields.io/badge/Platform-ESP32--S3-000000?logo=espressif)](sdk-integration-examples/esp32-s3)
[![ESP32-P4](https://img.shields.io/badge/Platform-ESP32--P4-E7352C?logo=espressif)](sdk-integration-examples/esp32-p4)
[![G32S10X](https://img.shields.io/badge/Platform-G32S10X-1F6FEB)](sdk-integration-examples/g32s10x)

本仓库提供 ESP32-S3、ESP32-P4 和 G32S10X 三个平台的 TiRTC 设备端代码，分为两个清晰入口：

- [SDK 接入与历史示例](sdk-integration-examples/README.md)：聚焦联网、时间同步、TiRTC
  初始化、上线、连接、音视频收发和释放，适合移植与理解 SDK。
- [完整应用](complete-applications/README.md)：在 TiRTC 之外加入设备绑定、ThingConnect、
  UI、真实摄像头和音频、AI 对讲、微信 VoIP、设备互呼与 OTA 等业务能力。

第一次接触 TiRTC 设备端集成，建议先阅读对应平台的最小 TiRTC 集成示例；需要完整交互设备或
演示样机，再进入对应平台的完整应用。

## 可用项目

### SDK 接入与历史示例

| 平台 | 示例 | 版本 | TiRTC SDK | 来源版本 | Release 交付 |
| --- | --- | --- | --- | --- | --- |
| ESP32-S3 | [最小 TiRTC 集成示例](sdk-integration-examples/esp32-s3/README.md) | `1.2.0` | `2.2.1` | Tag `v1.2.0` / commit `44b24a5` | 源码与 `0x0` 完整镜像 |
| ESP32-P4 | [最小 TiRTC 集成示例](sdk-integration-examples/esp32-p4/README.md) | `1.1.1` | `2.2.1` | Tag `v1.1.1` / commit `0f36ddb` | 源码与 `0x0` 完整镜像 |
| G32S10X | [最小 TiRTC 集成示例](sdk-integration-examples/g32s10x/README.md) | `0.8.3` | `2.2.1` | Tag `v0.8.3` / commit `b2abc22` | 源码与 `rtos-with-spl.bin` |

### 完整应用

| 平台 | 应用 | 版本 | TiRTC SDK | 来源版本 | Release 交付 |
| --- | --- | --- | --- | --- | --- |
| ESP32-S3 | [Device Monitor](complete-applications/esp32-s3/device-monitor/README.md) | `1.7.6` | `2.2.0` | Tag `v1.7.6` / commit `04dfca5` | 源码、完整镜像与 OTA app |
| ESP32-P4 | [Device App](complete-applications/esp32-p4/device-monitor/README.md) | `1.2.3` | `2.3.0` | Tag `esp32-p4-device-app-v1.2.3` / commit `c1af1eb` | 本次按源码范围交付 |
| G32S10X | [Device Monitor](complete-applications/g32s10x/device-monitor/README.md) | `0.1.1` | `2.2.1` | Tag `v0.1.1` / commit `5630152` | 源码、主固件与两个 YAFFS 镜像 |

六个项目的版本、来源 Tag 和 commit 均已锁定。发布资产来自统一源码 commit 的正式干净
构建，文件用途、大小、Flash 地址和 SHA-256 见当次 `release-manifest.json` 与
[版本与证据清单](docs/VERSIONS_CN.md)。

## 选择路径

### 我只想看最小 TiRTC 调用

进入目标平台的 `sdk-integration-examples/<platform>/`。每个示例都包含：

- SDK 头文件和目标平台静态库。
- 网络、时间与 TiRTC 生命周期代码。
- 连接、订阅、音视频测试流和断开示例。
- 配置、版本、测试说明和来源边界。

示例中的 Wi-Fi、设备 ID、Secret 和 Token 使用空值或占位符。正式产品应从安全存储或业务
服务端提供凭据与 Token，不应把真实密钥提交到 Git。

### 我想使用完整应用

进入 `complete-applications/<platform>/device-monitor/` 阅读目标板、功能、配置和构建说明。
正式固件只通过 [GitHub Releases](https://github.com/tangeai/tirtc-device-example/releases)
分发，不进入 Git 历史。ESP32-S3 和 ESP32-P4 固件使用
[Espressif ESP Tool](https://espressif.github.io/esptool-js/) 按 Release 清单烧录；
G32S10X 固件使用君正 Cloner 和项目声明的烧录配置。

ESP32-P4 Device App `1.2.3` 本次按源码范围交付，开发者可从统一 Tag 的源码归档获取对应代码。

## 构建入口

ESP32-S3/P4：

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py reconfigure build
```

请先进入对应平台目录。P4 使用 ESP32-C6 Hosted/SDIO 提供 Wi-Fi，首次构建会按工程锁定文件
获取相关组件。

G32S10X：

```bash
G32_SDK_ROOT=/path/to/im_sdk/opensource/freertos \
G32_TOOLCHAIN_BIN=/path/to/g32-toolchain/bin \
bash scripts/build-in-sdk.sh
```

G32S10X 供应商 SDK、工具链和 Cloner 不随本仓分发，具体要求见对应项目说明。

## 仓库结构

```text
tirtc-device-example/
├── sdk-integration-examples/
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
