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
| 完整应用 | ESP32-S3 Device Monitor | `1.9.5` | `2.3.0 mini` | `v1.9.5` / `45db394` / tree `4a67607` | `esp32-s3-device-monitor-v1.9.5` |
| 完整应用 | ESP32-P4 Device Monitor | `1.3.2` | `2.3.0` 定制兼容快照 | `esp32-p4-device-app-v1.3.2` / `bc1ae8fbd9b64090503128985129a72e024c0551` | `esp32-p4-device-monitor-v1.3.2` |
| 完整应用 | G32S10X Device Monitor | `0.1.1` | `2.2.1` | `v0.1.1` / `5630152` | `g32s10x-device-monitor-v0.1.1` |

ESP32-S3 最小系统例子的开发目录没有可公开引用的 Git 历史。当前版本保留其 `88` 文件核心源码
快照 SHA-256：
`8cdcb658f6e7eddcc72c2b4f3b0259a5e2175276d4b766d5ca237e51018b5956`。
该项目不虚构来源 Tag 或 commit；公开项目 Tag 是它正式、可复核的源码身份。

ESP32-P4 最小系统例子同样来自独立开发目录。开发快照 SHA-256、文件清单、公开筛选边界和
C6/C61 兼容配置记录在项目 `SOURCE_PROVENANCE.md` 与 Release manifest 中；公开项目 Tag 是
正式、可复核的源码身份。

ESP32-S3 Device Monitor `1.9.5` 的完整开发来源为 Tag `v1.9.5`、commit
`45db394cae399967a9c3b882d595cdecb80321be`、tree
`4a6760708d4fd2bdb973c4bc77d789d45f2bc2be`；统一公开代码提交为
`c27914eafab6f700cecf196da48987200cd54d37`。正式构建输入与该提交的 repository tree
`b0b93ddc0c82ae65130ae6dab17ff6ee6dbfc86a`、项目 tree
`9fa7f85ce92a0e5fcd27c4a36fabe02609191391` 完全一致。微信主动呼叫继续使用体验版
`wx_version_type=2`。公开导入的 `532` 文件清单 SHA-256 为
`4f62119b14935198128abb809142beb3bf91367339ae9f052acecb1d93499b00`。

TiRTC SDK 以 `db7290f` 为功能基础，另含 `13e34c3` 的 HTTPS 服务端认证修复。公开静态库
为 `2,125,366` bytes，SHA-256 为
`83556eeee0c6cae45961899a4c5d1255a5d0d33f8e636104a946ce41ff3e20d7`；BuildInfo 仍为
`v2.3.0-db7290f`。归档只移除了 SDK 内部调试信息，成员、全局符号和运行代码契约保持一致。

## 构建与交付证据

| 项目 | 静态来源核验 | 正式构建证据 | Release 交付 |
| --- | --- | --- | --- |
| S3 最小示例 `1.2.0` | 来源 Tag、SDK `2.2.1`、配置和凭据范围已核对 | ESP-IDF `5.5.4` 正式干净构建通过 | 源码与 `0x0` 完整镜像 |
| P4 最小示例 `1.1.1` | 来源 Tag、SDK `2.2.1`、Hosted/SDIO 配置和凭据范围已核对 | ESP-IDF `5.5.4` 正式干净构建通过 | 源码与 `0x0` 完整镜像 |
| G32S10X 最小示例 `0.8.3` | 来源 Tag、SDK `2.2.1`、君正覆盖层和凭据范围已核对 | 君正 SDK 正式构建通过 | 源码与主固件 |
| S3 最小系统例子 `0.8.0` | 目录快照、SDK、媒体来源、图片脱敏、凭据和公开路径已核对 | ESP-IDF `5.5.4` 的 4 MB 配置干净构建通过；app 大小见 Release manifest | 源码与 `0x0` 4 MB 完整镜像 |
| P4 最小系统例子 `0.2.0` | 目录快照、SDK、C6/C61 Hosted 兼容配置、媒体和凭据已核对 | ESP-IDF `5.5.4` 的 4 MB 配置干净构建通过；app 大小见 Release manifest | 源码与 `0x0` 4 MB 完整镜像 |
| S3 Device Monitor `1.9.5` | 来源 Tag/commit/tree、SDK HTTPS 认证、公开归档卫生、A-law 线格式、公开筛选和凭据范围已核对 | 已从与公开代码提交 tree 完全一致的隔离候选执行唯一 `--no-ccache` 正式干净构建，完成 `1767/1767`；app `7,608,608` 字节、SHA-256 `51a7599942f06556e33ef4820499885d6213ff15fce0f3ed2f11e38e44146503`、分区剩余 `190,176` 字节（`2.44%`） | 源码、16 MB 完整镜像与 OTA app；容量余量较紧 |
| P4 Device Monitor `1.3.2` | 来源 Tag、定制 SDK 快照、源码增量、媒体参数和凭据已核对 | ESP-IDF `5.5.4` 正式干净构建通过；app `6,927,360` 字节，SHA-256 `2df6d9d626a05f19a4fd1f15eb854c54119a32ccd475090f6713f2629afc90e2` | 源码与 `0x0` 完整镜像 |
| G32S10X Device Monitor `0.1.1` | 来源 Tag、SDK、君正覆盖层、文件系统打包范围已核对 | 固件构建通过；文件系统在同一 SDK 树恢复打包，发布树与构建树为 `compile-input-equivalent` | 源码、主固件与两个 YAFFS 镜像 |

本次最小系统项目改名没有改变功能代码、SDK、媒体或板级配置。S3 与 P4 仍分别从最终公开
Tag 做一次正式干净构建，重新生成 4 MB 完整镜像、manifest 和 SHA256SUMS；构建结果不替代
烧录、联网、音视频或长时间运行证据。

G32S10X Device Monitor 的主固件构建与最终文件系统打包来自同一套编译输入，但不是同一个
Git tree：最终打包树只增加 `.gitattributes`、data 占位文件和 `fs.mk` 三项打包资料，主固件没有
重新编译。manifest 会保留这一恢复过程，不把它写成源码树逐字节相同。

ESP32-S3 Device Monitor `1.9.5` 的相同功能代码在版本号收口前做过双板媒体包核对和 20,000
次 A-law 编解码自检，但正式版本号收口后的 Release 固件没有重新烧录。当前 Web IPC 和设备
互呼仍可能听到轻微“沙沙电流声”，底噪根因尚未证实。构建、包格式和 Codec 自检不能替代
最终固件的真机听感，也不能把该现象写成已经修复。

SDK 的证书链、hostname 和 verify flags 处理已核对到源码、对象、归档和 APP 链接。当前尚无
目标板上的有效证书成功、错误证书或错误主机名失败测试，也没有本版本绑定、MQTT 和 RTC 业务
回归；这些运行证据不能由静态检查或构建结果替代。

## P4 媒体参数

媒体方向以设备端为观察点：

- IPC 上行：`1280x960@20fps`、`4Mbps` H264。
- 设备呼叫和微信设备上行：`480x320@15fps`、`800kbps` 起始码率 H264。
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
