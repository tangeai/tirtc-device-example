# TiRTC 设备端示例

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![ESP32-S3](https://img.shields.io/badge/Platform-ESP32--S3-000000?logo=espressif)](sdk-integration-examples/esp32-s3)
[![ESP32-P4](https://img.shields.io/badge/Platform-ESP32--P4-E7352C?logo=espressif)](sdk-integration-examples/esp32-p4)
[![G32S10X](https://img.shields.io/badge/Platform-G32S10X-1F6FEB)](sdk-integration-examples/g32s10x)

本仓库提供两类 TiRTC 设备端代码：

- [SDK 接入与历史示例](sdk-integration-examples/README.md)：尽量直接展示 Wi-Fi、时间同步、
  TiRTC 初始化、上线、连接、音视频收发和释放，适合移植与理解 SDK。
- [完整应用](complete-applications/README.md)：在 TiRTC 之外加入设备绑定、ThingConnect、
  UI、真实摄像头和音频、AI 对讲、微信 VoIP、设备互呼与 OTA 等业务能力。

第一次接触 TiRTC 设备端集成，建议先阅读对应平台的最小 TiRTC 集成示例；需要完整交互设备或演示样机，
再进入 `device-monitor`。

## 可用项目

### SDK 接入与历史示例

| 平台 | 示例 | 示例版本 | TiRTC SDK 声明 | 构建体系 | 当前候选状态 |
| --- | --- | --- | --- | --- | --- |
| ESP32-S3 | [最小 TiRTC 集成示例](sdk-integration-examples/esp32-s3/README.md) | `1.1.1` | `2.2.1` | ESP-IDF `5.5.4` | SDK 精确快照已确认，干净构建通过 |
| ESP32-P4 | [最小 TiRTC 集成示例](sdk-integration-examples/esp32-p4/README.md) | `1.0.2` | `0.1.4` | ESP-IDF `5.5.4` | SDK 正式包契约已确认，干净构建通过 |
| G32S10X | [最小 TiRTC 集成示例](sdk-integration-examples/g32s10x/README.md) | `0.8.1` | `2.2.0` | Ingenic SDK `im_sdk_v0.4.0` | 已在隔离的干净 SDK 副本中完成构建 |

三个示例都来自当前开发源工作树快照。源工程有未提交变更时，项目中的
`SOURCE_PROVENANCE.md` 会明确记录基线 commit、复制范围和验证边界。

### 完整应用

| 平台 | 应用 | 版本 | TiRTC SDK | 当前候选状态 |
| --- | --- | --- | --- | --- |
| ESP32-S3 | [Device Monitor](complete-applications/esp32-s3/device-monitor/README.md) | `0.7.5` | `2.2.0` | 当前发布目录干净构建通过 |
| ESP32-P4 | [Device Monitor](complete-applications/esp32-p4/device-monitor/README.md) | `1.1.0` | `2.2.0` | 当前发布目录干净构建通过 |

G32S10X 完整应用仍在内部收口，本次没有纳入公开源码。

## 选择路径

### 我只想看最小 TiRTC 调用

进入目标平台的 `sdk-integration-examples/<platform>/`。每个示例都包含：

- SDK 头文件和目标平台静态库。
- 网络、时间与 TiRTC 生命周期代码。
- 连接、订阅、音视频测试流和断开示例。
- 配置、版本、测试说明和来源边界。

示例中的 Wi-Fi、设备 ID、Secret 和 Token 都是空值或 `your_*` 占位符。正式产品应由安全
存储或业务服务端提供凭据与 Token，不要把真实密钥提交到 Git。

### 我想直接体验完整设备

从 [GitHub Releases](https://github.com/tangeai/tirtc-device-example/releases) 下载与开发板匹配、
文件名包含 `full` 的完整镜像：

| 平台 | 开发板 | 完整镜像 | 地址 |
| --- | --- | --- | --- |
| ESP32-S3 | 嘉立创/立创·实战派 `LCKFB-SZPI-ESP32-S3-VA` | `esp32s3-tirtc-device-monitor-full-v0.7.5.bin` | `0x0` |
| ESP32-P4 | Waveshare `ESP32-P4-WIFI6-Touch-LCD-3.5` | `tirtc-esp32p4-device-monitor-full-v1.1.0.bin` | `0x0` |

烧录后连接 2.4 GHz Wi-Fi，在 ThingConnect H5 输入设备显示的 6 位验证码，再测试实时查看和
对讲。详细步骤与完成标志见对应 Device Monitor README。

## 构建入口

ESP32-S3/P4：

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py reconfigure build
```

请先进入对应平台目录或 `device-monitor` 目录。P4 当前实现使用 ESP32-C6 Hosted/SDIO 提供 Wi-Fi，
首次构建会按工程锁定文件获取相关组件。

G32S10X：

```bash
SDK_ROOT=/path/to/im_sdk/opensource/freertos \
TOOLCHAIN_BIN=/path/to/g32-toolchain/bin \
bash scripts/build-in-sdk.sh
```

G32S10X 供应商 SDK、工具链和烧录工具不随本仓分发，具体要求见
[SDK_REQUIREMENTS.md](sdk-integration-examples/g32s10x/SDK_REQUIREMENTS.md)。

五个项目均已从当前发布目录完成干净构建，可供开发者在目标板上继续开展烧录、联网、
TiRTC 媒体链路和业务集成验证。构建结果对应的源码、SDK 契约、版本和证据边界见
[版本与证据清单](docs/VERSIONS_CN.md)。

## 仓库结构

```text
tirtc-device-example/
├── sdk-integration-examples/
│   ├── esp32-s3/
│   ├── esp32-p4/
│   └── g32s10x/
├── complete-applications/
│   ├── esp32-s3/device-monitor/
│   └── esp32-p4/device-monitor/
├── docs/
├── README.md
└── LICENSE
```

## 文档

- [文档入口](docs/README_CN.md)
- [版本与验证边界](docs/VERSIONS_CN.md)
- [版本变更记录](docs/CHANGELOG_CN.md)
- [固件下载与校验](docs/RELEASES_CN.md)

## License

MIT © 探鸽智能
