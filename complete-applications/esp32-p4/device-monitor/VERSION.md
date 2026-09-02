# TiRTC ESP32-P4 Device Monitor 版本契约

## 应用与平台

| 项目 | 内容 |
| --- | --- |
| 应用版本 / 日期 | `1.5.3` / `2026-09-02` |
| 来源 Tag | `esp32-p4-device-app-v1.5.3` |
| 来源 Tag object | `4ab50c48d685d3537c8f9bebdc0b2417ad35338d` |
| 来源 commit | `06583eaf18d9326b9602684a7c509a3badf7dadc` |
| 来源 tree | `1acc583f3601448e1656f18716924f70125fe0c7` |
| 上一公开版本的来源 | `1.5.1` / `8d26a2bc5267f6bf1db721730a210ce8bc2f7ccc` |
| 安全补丁比较基线 | `1.5.2` / `48faa7b5c6e37dac7bf9f0aaf28d1142bef1f220` |
| 公开项目 Tag | `esp32-p4-device-monitor-v1.5.3` |
| 发布内容 | 源码、文档与从 `0x0` 烧录的 16 MiB 完整镜像 |
| 目标板 | Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 |
| 网络架构 | ESP32-P4 host + ESP32-C6 ESP-Hosted/SDIO slave |
| ESP-IDF / 编译器 | `5.5.4` / `riscv32-esp-elf-gcc 14.2.0_20260121` |

## TiRTC SDK

| 项目 | 内容 |
| --- | --- |
| API 版本 | `2.3.0` |
| 交付性质 | P4 validation rebuild，包含传输与 RTC 线程栈补丁 |
| Nano source | `13e34c3e3e3dc6776be4713b5c1e3c17bd282766` |
| TGWebRTC source | `e39114731ad488c88573d16f0855a1326d97c989` |
| TGWebRTC 补丁集 SHA-256 | `e5b3109cc0dee3f0d8958c23a60f69b236d87acb909cac95c4d6bb24812dbbaf` |
| P4 库大小 | `4,748,802` bytes |
| P4 库 MD5 | `fe83d55cfe856807f2775be568a73ec4` |
| P4 库 SHA-256 | `a7a01ffd496a55364c7e4d665ff3884d078147bba96752a965d97befca12e451` |
| 脱敏前归档 SHA-256 | `738c969244ab39c2b0eacc21068ecebc9bad736a4a5d713794836605d8e9f982` |
| `tiRTC.h` SHA-256 | `a53fa3392f71c8fd15c77891a772cc20939b5d253b995b3382486e514c134473` |

本次使用 `--strip-debug` 去掉 SDK 调试信息中的个人路径。99 个成员的顺序、代码与数据节、
运行重定位、归档符号索引和可链接符号保持一致；没有重编 SDK 或修改运行数据。公开库保留
链接所需符号，但 SDK 内部源码行号调试需要脱敏前归档。完整来源见组件
[VERSION.md](components/tirtc_sdk/VERSION.md)，公开 6 项校验和见
[SHA256SUMS.txt](components/tirtc_sdk/SHA256SUMS.txt)。未使用的 S3 库不随 P4 工程分发。

这份库包含 P4 项目补丁，不能只凭 `2.3.0` 字符串把它当作未经修改的官方原包。
库中还保留不同下层组件的版本字符串，确定交付身份时应使用上述源码、补丁集和完整归档哈希。

## 配置与安全

- FreeRTOS tick 为 `1000Hz`，trace/runtime stats 关闭，最大 socket 数为 `16`。
- Wi-Fi 由 C6 的 ESP-Hosted/SDIO 提供；`libwebrtc_nosctp.a` 已合入 TiRTC 库，不能重复链接。
- RTC 配置只接受带主机名的显式 HTTPS URL。非法地址在修改配置或排队重置之前返回错误；
  RTC 关闭时允许空地址。SDK 证书或握手错误保持失败，不自动回落 HTTP。
- SDK 使用必需的服务端证书验证、ESP-IDF CA bundle、hostname 和验证结果检查。
- P4 主动微信呼叫使用正式小程序 `version_type=0`。
- SDK/TGMP 码率反馈默认开启，旧本地自动弱网降级关闭，避免两个控制器同时改编码器。
- 当前依赖不包含 Hosted 事件头文件，事件驱动恢复分支未启用；现行恢复入口是 RPC 错误触发
  的重建及 SDIO 防护。板级物理故障的根因不由这些防护证明。

RTC 地址约束不等于整机完全没有 HTTP 流量：原有无凭据的 HTTP HEAD 校时兜底仍保留。

## 媒体与组件

| 场景 | 参数 |
| --- | --- |
| IPC 上行 | `1280x960@20fps`，目标 `4Mbps`，H264，GOP `40` |
| 设备呼叫上行 | `384x256@12fps`，目标 `256kbps`，H264，GOP `192` |
| 设备呼叫下行 | constrained-baseline H264，解码 `384x256`，显示 `480x320` |
| 微信上行 | `640x480@15fps`，目标 `800kbps`，H264，GOP `30` |
| 微信下行 | 请求 `640x480` MJPEG，按实际来帧解码后 `cover` 到 `480x320` |
| H264 输入/输出池 | 输入 `24 x 256KB`；decoded `4` 帧；output `20` 帧；playout 上限 `16` |
| 控制层自动隐藏 | `5s`，点击视频恢复 |

码率是目标值，实际发送量取决于画面、编码器和传输反馈。
H264 组件保持 Espressif `1.3.8` / `8e86030d` 基线及已有 P4 输出边界、参考帧池和码率控制补丁。

## 本次变化与验证

相对上一公开版 `1.5.1`，本版包含微信上行档位、C6 RPC/SDIO 防护、摄像头和显示资源调整，
以及 `1.5.3` 的 HTTPS 策略和 SDK 调试信息脱敏。精确来源差异为 26 个文件，1227 行新增、
128 行删除；其中安全补丁自身为 12 个文件，包含两个可复用维护测试脚本。

主机侧已通过 24 项实际 RTC C 函数与 IDF URL parser 测试，覆盖地址原样保留、非法输入拒绝
及拒绝时无配置/重置副作用。归档等价性与 SDK 校验是独立的静态证据。

已从公开输入 commit `6aff5fe9bf807bdb71a6d4dbc4bd6f8d24f563b6` 完成唯一正式干净构建，
主工程 `1837/1837`、bootloader `133/133`，compiler warning/error/ICE 均为 0。应用大小
`6,973,376` bytes，SHA-256 为 `65051e036250537f0cfee274944b6cc6d036c1aaee673c723b8420cf18f88664`，
APP 分区剩余 `563,264` bytes（`7.47%`）。后续仅补文档证据，编译输入不变。

完整构建及固件记录见本次 Release 的 `release-manifest.json`；烧录、P4 TLS 正反向握手、
峰值内存与跨应用运行需在目标板分别验证。主机 URL 测试不模拟 TLS，也不替代真机握手。
