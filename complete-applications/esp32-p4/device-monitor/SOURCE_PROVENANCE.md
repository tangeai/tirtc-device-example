# 源码来源与公开边界

本页说明 ESP32-P4 Device Monitor `1.5.3` 从哪个源码快照整理而来，以及公开包做了哪些变换。

## 唯一来源

| 项目 | 内容 |
| --- | --- |
| 来源 Tag | `esp32-p4-device-app-v1.5.3` |
| Tag object | `4ab50c48d685d3537c8f9bebdc0b2417ad35338d` |
| 来源 commit | `06583eaf18d9326b9602684a7c509a3badf7dadc` |
| 来源 tree | `1acc583f3601448e1656f18716924f70125fe0c7` |
| 上一公开版本来源 | `esp32-p4-device-app-v1.5.1` / `8d26a2bc5267f6bf1db721730a210ce8bc2f7ccc` |
| 安全修复父提交 | `esp32-p4-device-app-v1.5.2` / `48faa7b5c6e37dac7bf9f0aaf28d1142bef1f220` |
| 公开 Tag | `esp32-p4-device-monitor-v1.5.3` |
| 公开项目路径 | `complete-applications/esp32-p4/device-monitor/` |

从上一公开来源 `1.5.1` 到本版共有 26 个文件变化、1227 行新增和 128 行删除。来源 Tag 固定了
功能代码、版本、SDK 和配置；开发仓之后的任何未提交内容都不属于这个版本。

## 文件映射

来源 Tag 的 `git archive` 导出包含 `1,210` 个文件、`106,759,995` bytes；按路径排序的
`sha256  path\n` 导出字节清单摘要为：

`343e6d36d293b4fdc19f02e27cf61b2935aeb26c4a6167f48bb604be1057c2f1`

公开保留 `1,207` 个来源路径、导出大小 `101,743,573` bytes；在公开文档改写前，导出清单摘要为：

`2a3f83781a99650c1e284b6880108fa8fce7ce209efbe4f9d92a0aaaec7860e3`

导出时的换行规则会使文本字节与 Git blob 不同。上述数值用于复核导出过程；manifest 另外
保留原始 Git blob 清单，并直接对照来源 Tag 与公开提交，不把两种清单混作同一哈希。

公开映射保持明确：

- `1,193` 个来源文件直接保留 Git blob 字节，不改业务或构建逻辑。
- 14 个来源路径由公开仓维护：项目 README、VERSION、媒体架构、SDK 校验清单、9 份
  ESP-Hosted 文档及 1 份 ESP-LVGL 测试 README。
- 额外提供本页和入门说明 `docs/GETTING_STARTED_CN.md`，因此公开项目共有 `1,209` 个文件。
- 组件文档的既有变换限于相对链接、目录锚点和个人路径，不改变组件源码。

以下 3 个来源文件明确排除：

| 文件 | 原因 |
| --- | --- |
| `.clangd` | 开发机编辑器索引配置 |
| `sdkconfig` | 生成配置；公开构建从 `sdkconfig.defaults` 展开 |
| `components/tirtc_sdk/lib/esp32s3/libTiRTC.a` | P4 不使用的 S3 静态库 |

公开 SDK 的 `SHA256SUMS.txt` 因此从来源 7 项变为 6 项。S3 开发库没有被修改或删除。
开发仓 Git 历史、凭据、IDE 配置、构建目录、日志、生成依赖锁和固件均不随源码导入。
完整 BIN 只放在 GitHub Releases，不进入 Git 历史。

## SDK 契约

本版 SDK 为 TiRTC `2.3.0` P4 validation rebuild，含项目补丁。Nano source 为
`13e34c3e3e3dc6776be4713b5c1e3c17bd282766`；TGWebRTC source 为
`e39114731ad488c88573d16f0855a1326d97c989`，另有补丁集摘要
`e5b3109cc0dee3f0d8958c23a60f69b236d87acb909cac95c4d6bb24812dbbaf`。

`1.5.3` 开发源已对 P4 库完成 `--strip-debug`，发布仓直接使用该 Tag 内的库，没有再次改写。

| 归档 | 大小 | SHA-256 |
| --- | --- | --- |
| 脱敏前 | `14,815,866` bytes | `738c969244ab39c2b0eacc21068ecebc9bad736a4a5d713794836605d8e9f982` |
| Tag 内公开库 | `4,748,802` bytes | `a7a01ffd496a55364c7e4d665ff3884d078147bba96752a965d97befca12e451` |

99 个归档成员的顺序、2803 个 allocated sections、51554 个运行重定位和 1304 个归档符号
索引条目保持等价。处理移除了个人路径和 SDK 源码行号调试信息，未重编 SDK，未修改代码或
运行数据。校验工具随项目 `tools/check_sdk_debug_strip.py` 提供。

RTC 配置使用 IDF URL parser 接受明确的 HTTPS 地址，原有按 SDK 版本降级 HTTP 的分支已移除。
SDK 的服务端证书、CA bundle、hostname 与验证结果检查保留；连接失败不能通过明文重试掩盖。
这项变更不涵盖设备原有的无凭据 HTTP HEAD 校时兜底。

## 构建与运行证据

公开正式构建输入为 commit `6aff5fe9bf807bdb71a6d4dbc4bd6f8d24f563b6`，使用 ESP-IDF
`5.5.4`、GCC `14.2.0_20260121`、独立生成配置和 `--no-ccache`。主工程 `1837/1837`、
bootloader `133/133` 均通过；compiler warning/error/ICE 均为 0。应用 `6,973,376` bytes，
APP 分区剩余 `563,264` bytes（`7.47%`）。构建后仅补充文档证据；manifest 验证编译输入
仍与原构建提交一致，并记录配置、解析依赖、命令与固件哈希。

24 项主机测试直接编译实际 RTC 配置函数与 IDF URL parser，SDK/RTOS 副作用使用 stub。
它证明地址策略及拒绝时的状态行为，不证明 P4 上的 TLS 握手。设备端验收应分别检查有效证书
建连、错误主机名和不可信证书拒绝、失败无 HTTP 回退，以及 TLS 内存峰值和跨应用恢复。

开发侧已有的业务测试只按原测试条件引用。构建、烧录、TLS 握手、真实音视频和长时间运行
属于不同证据层；公开说明不把其中一种写成另一种。
