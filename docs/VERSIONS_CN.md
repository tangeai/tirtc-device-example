# 版本与证据清单

本文件把源码来源、TiRTC SDK、统一构建、Release 资产和目标板证据分开记录。开发侧
本地 Tag 锁定各项目来源版本；统一公开 commit 锁定筛选后的源码与文档，正式干净构建生成
Release 资产并由 manifest 记录。

## ESP32-S3 AT ThingConnect Demo 0.2.0

| 分类 | 项目 | 发布仓路径 | 版本 | TiRTC SDK | 正式源码身份 |
| --- | --- | --- | --- | --- | --- |
| 协议示例 | ESP32-S3 AT ThingConnect 示例 | `sdk-integration-examples/esp32-s3-at-thingconnect` | `0.2.0` | `2.2.1` | Public tag `esp32-s3-at-thingconnect-v0.2.0` |

开发交接来自没有 Git 历史的独立源码树，因此不虚构来源 Tag 或 commit。项目级公开 Tag
所指向的统一仓 commit 是首个正式源码身份；当次 `release-manifest.json` 记录完整 commit、
源码逐文件 SHA-256、正式构建命令和资产 SHA-256。

本例的 TiRTC SDK build label 为
`v2.2.1_tag_3a33bf4ae51b_1khz_http_20260729_105424`，ESP32-S3 archive 大小为
`4908112` 字节，SHA-256 为
`dc9f869194584fd43fe091f887342d170a1422e657ce1fc2a144c90a3ade1f8e`。
它与同为 `2.2.1` 的 ESP32-S3 最小示例不是同一字节包，必须按项目各自的 SDK manifest
和构建契约核对。

| 来源交接 | 统一仓静态核验 | 正式构建 | 交付形态 |
| --- | --- | --- | --- |
| 独立源码树快照，公开 Tag 建立正式身份 | 目录、版本、SDK、媒体、文档、凭据和来源边界核验 | ESP-IDF `5.5.4` 隔离干净构建 | 源码与 `0x0` 完整镜像 |

正式构建和串口目标板验证是分开的证据层。完整镜像只用于下载、烧录和继续集成，不把
编译成功写成平台绑定、AI、呼叫或真实媒体硬件结论。

## 2026-07-30 Release

| 分类 | 项目 | 发布仓路径 | 版本 | TiRTC SDK | 来源 Tag | 来源 commit |
| --- | --- | --- | --- | --- | --- | --- |
| 最小示例 | ESP32-S3 最小 TiRTC 集成示例 | `sdk-integration-examples/esp32-s3` | `1.2.0` | `2.2.1` | `v1.2.0` | `44b24a5` |
| 最小示例 | ESP32-P4 最小 TiRTC 集成示例 | `sdk-integration-examples/esp32-p4` | `1.1.1` | `2.2.1` | `v1.1.1` | `0f36ddb` |
| 最小示例 | G32S10X 最小 TiRTC 集成示例 | `sdk-integration-examples/g32s10x` | `0.8.3` | `2.2.1` | `v0.8.3` | `b2abc22` |
| 完整应用 | ESP32-S3 Device Monitor | `complete-applications/esp32-s3/device-monitor` | `1.7.6` | `2.2.0` | `v1.7.6` | `04dfca5` |
| 完整应用 | ESP32-P4 Device App | `complete-applications/esp32-p4/device-monitor` | `1.2.3` | `2.3.0` | `esp32-p4-device-app-v1.2.3` | `c1af1eb` |
| 完整应用 | G32S10X Device Monitor | `complete-applications/g32s10x/device-monitor` | `0.1.1` | `2.2.1` | `v0.1.1` | `5630152` |

六个来源版本均按开发侧正式本地 Tag 和 commit 交接。统一仓在纳入源码时应核对 Tag 指向、
真实 diff、公开文件范围、版本字段和文档说明，不能只依据修改说明推断发布内容。

G32S10X 两个项目使用的 SDK manifest 将 `delivery_status` 标记为 `candidate`；统一仓固定其
`2.2.1` 版本、source commit 和静态库 SHA-256，不把该状态改写为正式 SDK 包。

## 发布证据状态

| 项目 | 来源版本 | 统一仓静态核验 | 正式构建 | 交付形态 |
| --- | --- | --- | --- | --- |
| ESP32-S3 最小 TiRTC 集成示例 `1.2.0` | Tag/commit 已锁定 | 目录、版本、SDK、来源和凭据门禁通过 | ESP-IDF `5.5.4` 干净构建 | 源码与 `0x0` 完整镜像 |
| ESP32-P4 最小 TiRTC 集成示例 `1.1.1` | Tag/commit 已锁定 | 目录、版本、SDK、来源和凭据门禁通过 | ESP-IDF `5.5.4` 干净构建 | 源码与 `0x0` 完整镜像 |
| G32S10X 最小 TiRTC 集成示例 `0.8.3` | Tag/commit 已锁定 | 目录、版本、SDK、来源和凭据门禁通过 | IM SDK `v0.4.0` 干净构建 | 源码与 `rtos-with-spl.bin` |
| ESP32-S3 Device Monitor `1.7.6` | Tag/commit 已锁定 | 目录、版本、SDK、来源和凭据门禁通过 | ESP-IDF `5.5.4` 干净构建 | 源码、完整镜像与 OTA app |
| ESP32-P4 Device App `1.2.3` | Tag/commit 已锁定 | 目录、版本、SDK、来源和凭据门禁通过 | 源码范围 | 源码随统一 Tag 交付 |
| G32S10X Device Monitor `0.1.1` | Tag/commit 已锁定 | 目录、版本、SDK、来源和凭据门禁通过 | IM SDK `v0.4.0` 干净构建 | 源码、主固件与两个 YAFFS 镜像 |

目标板烧录、联网和业务验证使用独立证据层。Release 中的构建资产用于开发者在目标板上
继续验证和集成，不把干净构建结果写成板级功能结论。

## 一致性契约

本次 `release-manifest.json` 记录：

- 统一发布 Tag 和 commit。
- 六个项目的发布仓路径、来源 Tag、来源 commit、版本和 TiRTC SDK。
- 发布仓源码文件清单及 SHA-256。
- 实际上传附件的文件名、用途、大小和 SHA-256。
- 固件项目的构建环境、命令、产物用途、Flash 地址和校验值；项目没有固件资产时记录
  源码交付形态。

静态校验可以证明发布源码、版本记录、Release notes、附件和 manifest 相互一致。若要证明
固件由指定源码构建，还需要可复现构建，或在固件中嵌入 commit/版本元数据并完成比对。
