# 版本与证据清单

本文件把开发来源、公开项目 Tag、正式构建、Release 附件和目标板运行证据分开记录。
八个项目分别使用自己的 SemVer；一个项目更新，不改变其他项目的 Tag。

## 当前版本

| 分类 | 项目 | 版本 | TiRTC SDK | 开发来源 | 公开项目 Tag |
| --- | --- | --- | --- | --- | --- |
| 最小示例 | ESP32-S3 最小 TiRTC 集成示例 | `1.2.0` | `2.2.1` | `v1.2.0` / `44b24a5` | `esp32-s3-minimal-integration-v1.2.0` |
| 最小示例 | ESP32-P4 最小 TiRTC 集成示例 | `1.1.1` | `2.2.1` | `v1.1.1` / `0f36ddb` | `esp32-p4-minimal-integration-v1.1.1` |
| 最小示例 | G32S10X 最小 TiRTC 集成示例 | `0.8.3` | `2.2.1` | `v0.8.3` / `b2abc22` | `g32s10x-minimal-integration-v0.8.3` |
| 最小系统例子 | ESP32-S3 最小系统例子 | `0.8.0` | `2.2.1` | 独立开发目录快照 | `esp32-s3-minimal-system-v0.8.0` |
| 最小系统例子 | ESP32-P4 最小系统例子 | `0.2.0` | `2.3.0` | 独立开发目录快照 | `esp32-p4-minimal-system-v0.2.0` |
| 完整应用 | ESP32-S3 Device Monitor | `1.9.6` | `2.3.0 mini` | `v1.9.6` / `cdb5d7b` / tree `2b032dc` | `esp32-s3-device-monitor-v1.9.6` |
| 完整应用 | ESP32-P4 Device Monitor | `1.5.1` | `2.3.0` 官方源码重建版 | `esp32-p4-device-app-v1.5.1` / `8d26a2bc5267f6bf1db721730a210ce8bc2f7ccc` | `esp32-p4-device-monitor-v1.5.1` |
| 完整应用 | G32S10X Device Monitor | `0.1.1` | `2.2.1` | `v0.1.1` / `5630152` | `g32s10x-device-monitor-v0.1.1` |

ESP32-S3 最小系统例子的开发目录没有可公开引用的 Git 历史。当前版本保留其 `88` 文件核心源码
快照 SHA-256：
`8cdcb658f6e7eddcc72c2b4f3b0259a5e2175276d4b766d5ca237e51018b5956`。
该项目不虚构来源 Tag 或 commit；公开项目 Tag 是它正式、可复核的源码身份。

ESP32-P4 最小系统例子同样来自独立开发目录。开发快照 SHA-256、文件清单、公开筛选边界和
C6/C61 兼容配置记录在项目 `SOURCE_PROVENANCE.md` 与 Release manifest 中；公开项目 Tag 是
正式、可复核的源码身份。

ESP32-S3 Device Monitor `1.9.6` 的完整开发来源为 annotated Tag `v1.9.6`、Tag object
`c251806b8904672bac07ea16cca2f3099e2426e4`、commit
`cdb5d7bec9c955227a1259281686e30377332fb3`、tree
`2b032dc07ada0a2e15cfb82e320ac69b53272374`；统一公开代码提交为
`ccae5fd495109244a679c5ffe1e17f98d6589f52`。正式构建输入与该提交的 repository tree
`a0fde688fb4dbd2b7803fde487046f4de6cccd0d`、项目 tree
`d5d5a8fb3f818106b77eeccc2599482943260be9` 完全一致。微信主动呼叫继续使用体验版
`wx_version_type=2`。

TiRTC SDK 以 `db7290f` 为功能基础，另含 `13e34c3` 的 HTTPS 服务端认证修复。公开静态库
为 `2,125,366` bytes，SHA-256 为
`83556eeee0c6cae45961899a4c5d1255a5d0d33f8e636104a946ce41ff3e20d7`；BuildInfo 仍为
`v2.3.0-db7290f`。归档只移除了 SDK 内部调试信息，成员、全局符号和运行代码契约保持一致。

ESP32-P4 Device Monitor `1.5.1` 的开发来源为 annotated Tag
`esp32-p4-device-app-v1.5.1`、Tag object
`4a0fa821b53551432c8341763cf68a61c81285b5`、commit
`8d26a2bc5267f6bf1db721730a210ce8bc2f7ccc`、tree
`03e8f4708f69a96df13b8115376351075e5adf78`。本补丁把绑定弹窗和二维码切换到独立用户门户
`https://demo-open.tange-ai.com/devices`；设备内部服务发现和业务 API 保持独立。TiRTC API 版本为 `2.3.0`，Nano 源码为
`v2.3.0 / 1baf7c95f3ca715c9367b9c998417f647934dc35`，TGWebRTC 源码基线为
`tag.v1.5.12 / 41c9a25768ffe265c07f17ef78a6439607b19364`。当前静态库内嵌的
TGTRP BuildInfo 仍为 `tagv1.5.11`，因此源码基线和运行时 BuildInfo 分开记录；P4
`libTiRTC.a` 为 `1,827,850` bytes，SHA-256 为
`6dc4d437ea444761ca21e203fc9babb1799bb1f7fc261d7c523248fde0a96e67`。

## 构建与交付证据

| 项目 | 静态来源核验 | 正式构建证据 | Release 交付 |
| --- | --- | --- | --- |
| S3 最小示例 `1.2.0` | 来源 Tag、SDK `2.2.1`、配置和凭据范围已核对 | ESP-IDF `5.5.4` 正式干净构建通过 | 源码与 `0x0` 完整镜像 |
| P4 最小示例 `1.1.1` | 来源 Tag、SDK `2.2.1`、Hosted/SDIO 配置和凭据范围已核对 | ESP-IDF `5.5.4` 正式干净构建通过 | 源码与 `0x0` 完整镜像 |
| G32S10X 最小示例 `0.8.3` | 来源 Tag、SDK `2.2.1`、君正覆盖层和凭据范围已核对 | 君正 SDK 正式构建通过 | 源码与主固件 |
| S3 最小系统例子 `0.8.0` | 目录快照、SDK、媒体来源、图片脱敏、凭据和公开路径已核对 | ESP-IDF `5.5.4` 的 4 MB 配置干净构建通过；app 大小见 Release manifest | 源码与 `0x0` 4 MB 完整镜像 |
| P4 最小系统例子 `0.2.0` | 目录快照、SDK、C6/C61 Hosted 兼容配置、媒体和凭据已核对 | ESP-IDF `5.5.4` 的 4 MB 配置干净构建通过；app 大小见 Release manifest | 源码与 `0x0` 4 MB 完整镜像 |
| S3 Device Monitor `1.9.6` | 来源 Tag/commit/tree、SDK HTTPS 认证、公开筛选和凭据范围已核对 | 已从与公开代码提交 tree 完全一致的隔离候选执行唯一 ESP-IDF `5.5.4`、GCC `14.2.0_20260121`、`--no-ccache` 正式干净构建，完成 `1767/1767` 且编译器 warning/error/ICE 均为 0；app `7,611,216` 字节、SHA-256 `fa87ae46ffa93bd01da6deffade8cf23b22022f19040815b3b305ce7a56f0eec`、分区剩余 `187,568` 字节（`2.41%`） | 源码、16 MB 完整镜像与 OTA app；容量余量较紧 |
| P4 Device Monitor `1.5.1` | 来源 Tag/commit/tree、P4 公开包 SDK 6 项哈希、绑定门户与设备端点职责、凭据范围已核对 | 已完成唯一 ESP-IDF `5.5.4`、GCC `14.2.0_20260121`、`--no-ccache` 正式干净构建，完成 `1837/1837` 且编译 warning/error/ICE 均为 0；app `6,955,776` 字节、SHA-256 `1542c19052ae9bdb8804e38916c54f928e405ac06e6ae796faa5d46cd7c7e515`、分区剩余 `580,864` 字节（`7.71%`） | 源码与 `0x0` 16 MB 完整镜像 |
| G32S10X Device Monitor `0.1.1` | 来源 Tag、SDK、君正覆盖层、文件系统打包范围已核对 | 固件构建通过；文件系统在同一 SDK 树恢复打包，发布树与构建树为 `compile-input-equivalent` | 源码、主固件与两个 YAFFS 镜像 |

本次最小系统项目改名没有改变功能代码、SDK、媒体或板级配置。S3 与 P4 仍分别从最终公开
Tag 做一次正式干净构建，重新生成 4 MB 完整镜像、manifest 和 SHA256SUMS；构建结果不替代
烧录、联网、音视频或长时间运行证据。

G32S10X Device Monitor 的主固件构建与最终文件系统打包来自同一套编译输入，但不是同一个
Git tree：最终打包树只增加 `.gitattributes`、data 占位文件和 `fs.mk` 三项打包资料，主固件没有
重新编译。manifest 会保留这一恢复过程，不把它写成源码树逐字节相同。

ESP32-S3 Device Monitor `1.9.5` 的音频链路做过双板媒体包核对和 20,000 次 A-law 编解码
自检，但 `1.9.6` 的正式 Release 固件尚未完成烧录、冷启动呼叫、联系人键盘或音量触摸回归。当前 Web IPC 和设备
互呼仍可能听到轻微“沙沙电流声”，底噪根因尚未证实。构建、包格式和 Codec 自检不能替代
最终固件的真机听感，也不能把该现象写成已经修复。

SDK 的证书链、hostname 和 verify flags 处理已核对到源码、对象、归档和 APP 链接。当前尚无
目标板上的有效证书成功、错误证书或错误主机名失败测试，也没有本版本绑定、MQTT 和 RTC 业务
回归；这些运行证据不能由静态检查或构建结果替代。

## P4 媒体参数

媒体方向以设备端为观察点：

- IPC 上行：`1280x960@20fps`、`4Mbps` H264。
- 设备呼叫上行：`384x256@12fps`、`256kbps` H264；设备下行解码上限同为
  `384x256`，显示到 `480x320`。
- 微信设备上行：`480x320@15fps`、`480kbps` H264。
- 微信下行：请求 `640x480` MJPEG，由 P4 硬件 JPEG 解码后以 `cover` 显示到
  `480x320` 屏幕。

P4 固件没有配置或证明微信客户端侧 `720p` 原始采集分辨率。网页、小程序或手机端的采集参数
属于对端能力，不能从设备固件的上行编码配置推导。

## 一致性契约

每个项目的 `release-manifest.json` 至少记录：

- 公开项目 Tag、公开 commit 和项目路径。
- 开发来源 Tag/commit；没有 Git 来源时记录可核验的目录快照。
- 项目版本、TiRTC SDK 版本和 SDK 静态库 SHA-256。
- 正式构建环境、命令、构建来源 commit 和源码等价边界。
- Release notes 以及实际上传附件的文件名、用途、大小和 SHA-256。
- 烧录、联网、音视频和稳定性测试是否实际执行。

静态校验可以证明发布源码身份、版本记录、Release notes、附件和 manifest 相互一致。证明固件
由指定源码构建，还需要可复现构建，或在固件中嵌入 commit/版本元数据并完成比对。目标板烧录、
联网和业务验证仍是独立证据层。
