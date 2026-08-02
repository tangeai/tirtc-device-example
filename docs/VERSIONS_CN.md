# 版本与证据清单

本文件把开发来源、统一公开源码、正式构建、Release 资产和目标板运行证据分开记录。开发侧
Tag 锁定来源版本；统一公开 Tag 锁定筛选后的源码与文档；Release manifest 记录附件及 SHA-256。

## 2026-08-02 更新

| 分类 | 项目 | 发布仓路径 | 版本 | TiRTC SDK | 来源身份 |
| --- | --- | --- | --- | --- | --- |
| 完整应用 | ESP32-P4 Device App | `complete-applications/esp32-p4/device-monitor` | `1.3.1` | `2.3.0` 定制兼容快照 | `esp32-p4-device-app-v1.3.1` / `7391464` |

本次只更新 P4 完整应用。SDK 对外 API 版本仍为 `2.3.0`，静态库来自 Nano baseline
`aaad3da251bac90e0642b51b3279a1f40ca9fa9a`，并带有从
`fde4f1c58d2dc28f3d3d04e25dd49bc3a399fea6` 回移的 HTTP DNS cache disable 兼容补丁。
P4 `libTiRTC.a` SHA-256 为
`b0a38061b0c63ad0c556f73bb2ecc47c6fd84823b7524f774fe09916f577b4c6`。

| 项目 | 静态来源核验 | 正式构建 | 发布交付 |
| --- | --- | --- | --- |
| ESP32-P4 Device App `1.3.1` | Tag、版本、SDK 快照、源码增量、媒体参数和凭据已核对 | 最终公开快照执行 ESP-IDF `5.5.4` 干净构建，结果见本次 manifest | 源码 |

本次统一发布不上传 P4 BIN，也不重复上传旧 S3 固件。目标板运行、通话长稳和故障恢复是独立
证据层；尤其 TinyH264 helper 真正卡死后的任务回收仍需真机重复通话与长时间运行验证。

## 2026-07-31 更新

| 分类 | 项目 | 发布仓路径 | 版本 | TiRTC SDK | 来源身份 |
| --- | --- | --- | --- | --- | --- |
| 日志示例 | ESP32-S3 日志示例（AT ThingConnect） | `logging-examples/esp32-s3` | `0.3.0` | `2.2.1` | 独立源码快照，public commit 建立正式身份 |
| 完整应用 | ESP32-S3 Device Monitor | `complete-applications/esp32-s3/device-monitor` | `1.8.0` | `2.2.0` | `v1.8.0` / `8211e3b` |
| 完整应用 | ESP32-P4 Device App | `complete-applications/esp32-p4/device-monitor` | `1.3.0` | `2.3.0` | `esp32-p4-device-app-v1.3.0` / `dacf8e6` |

ESP32-S3 日志示例开发目录没有 Git 历史。公开仓记录其 `93` 文件核心源码快照 SHA-256
`8d0dc2cab067952e5f40e7b60d7da60012a2c96830cd79c885ed76dc791a37db`，并由本次 public
commit 和统一 Tag 建立正式源码身份，不虚构来源 commit。

日志示例与 ESP32-S3 最小示例使用同一字节的 TiRTC SDK `2.2.1` 包：

- Build label：`v2.2.1_tag_3a33bf4ae51b_1khz_http_20260729_105424`
- `libTiRTC.a`：`4,908,112` 字节
- SHA-256：`dc9f869194584fd43fe091f887342d170a1422e657ce1fc2a144c90a3ade1f8e`

| 项目 | 静态来源核验 | 正式构建 | 发布交付 |
| --- | --- | --- | --- |
| ESP32-S3 日志示例 `0.3.0` | 目录、版本、SDK、媒体来源、凭据和公开路径已核对 | ESP-IDF `5.5.4` 正式干净构建通过；app `1,442,496` 字节 | 源码与 `0x0` 完整镜像 |
| ESP32-S3 Device Monitor `1.8.0` | Tag、版本、SDK、源码增量、资源和凭据已核对 | ESP-IDF `5.5.4` 正式干净构建通过；app `7,449,184` 字节，最小 app 分区余 `349,600` 字节 | 源码、完整镜像与 OTA app |
| ESP32-P4 Device App `1.3.0` | Tag、版本、SDK、源码增量、媒体参数和凭据已核对 | 本项目按源码范围交付 | 源码 |

P4 媒体方向以设备端为观察点：IPC 上行是 `1280x960@20fps`、`4Mbps` H264；设备呼叫和
微信设备上行是 `480x320@15fps`、`800kbps` 起始码率 H264；微信下行请求 `640x480`
MJPEG，并由 P4 硬件 JPEG 解码后以 `cover` 显示到 `480x320` 屏幕。P4 固件没有配置或
证明微信客户端侧 `720p` 上行。

## 保持上一版本的项目

| 分类 | 项目 | 版本 | TiRTC SDK | 来源身份 |
| --- | --- | --- | --- | --- |
| 最小示例 | ESP32-S3 最小 TiRTC 集成示例 | `1.2.0` | `2.2.1` | `v1.2.0` / `44b24a5` |
| 最小示例 | ESP32-P4 最小 TiRTC 集成示例 | `1.1.1` | `2.2.1` | `v1.1.1` / `0f36ddb` |
| 最小示例 | G32S10X 最小 TiRTC 集成示例 | `0.8.3` | `2.2.1` | `v0.8.3` / `b2abc22` |
| 完整应用 | G32S10X Device Monitor | `0.1.1` | `2.2.1` | `v0.1.1` / `5630152` |

这些项目继续由 `tirtc-device-examples-v2026.07.30` 的来源记录和资产清单约束，本次没有重新
构建或替换其固件。

## 一致性契约

每次 `release-manifest.json` 记录：

- 统一发布 Tag 和 commit。
- 本次更新项目的发布仓路径、来源身份、版本和 TiRTC SDK。
- 每个项目的源码文件清单及 SHA-256。
- Release notes 和实际上传附件的文件名、用途、大小和 SHA-256。
- 构建项目的 ESP-IDF、命令、产物用途和 Flash 地址；P4 APP 记录源码交付形态。

静态校验可以证明发布源码、版本记录、Release notes、附件和 manifest 相互一致。若要证明
固件由指定源码构建，还需要受控或可复现构建，或在固件中嵌入 commit/版本元数据并完成比对。
目标板烧录、联网和业务验证仍是独立证据层。
