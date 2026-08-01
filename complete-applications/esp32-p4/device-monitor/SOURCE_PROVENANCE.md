# 源码来源与发布边界

本文档记录 TiRTC ESP32-P4 完整设备应用在统一示例仓中的来源快照、公开筛选范围和静态核验边界。

## 来源快照

| 项目 | 内容 |
| --- | --- |
| 应用版本 | `1.3.0` |
| 来源 Tag | `esp32-p4-device-app-v1.3.0` |
| 来源 commit | `dacf8e65b25ea4d0282fc9314e46a18607691fb4` |
| 公开路径 | `complete-applications/esp32-p4/device-monitor/` |
| 目标开发板 | Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 |
| ESP-IDF | `5.5.4` |
| TiRTC SDK | `2.3.0` ESP32-P4 test package |

来源 Tag 是本次公开整理的唯一源码基线，并指向上述 commit。来源仓 Tag 之外的工作树状态、
本地构建目录和未提交内容均不属于本次公开范围。

## 公开筛选

统一仓保留应用源码、第三方组件、TiRTC SDK test package、必要配置、辅助脚本和面向开发者的文档。
以下内容不进入公开源码快照：

- 来源仓 Git 元数据、内部协作文档和本机 IDE 配置。
- `build/`、`managed_components/`、日志、固件镜像和发布附件目录。
- 与单台开发机绑定的 `.clangd` 和完整 `sdkconfig`；可复用配置由 `sdkconfig.defaults` 提供。

第三方组件及其许可证按来源快照保留；公开卫生整理只把上游示例日志中的开发机绝对路径
改为 `<esp-hosted-source>` 占位符。

## 静态一致性

来源 Tag 共包含 `1,198` 个文件。公开候选保留其中 `1,196` 个来源路径；未纳入的两个文件
为 `.clangd` 和 `sdkconfig`。相对上一公开版本，本次新增或更新的 `31` 个非文档代码与配置
路径在统一换行符后均与来源 Tag 一致；README、版本说明、开发者上手指南和媒体架构文档
按统一仓的公开格式整理，来源事实保持一致。

TiRTC SDK 关键文件校验值：

| 文件 | SHA-256 |
| --- | --- |
| `components/tirtc_sdk/lib/esp32p4/libTiRTC.a` | `70b54792399ab983b8ec5b6ccdd295eab7b0c13e98c4769e17b82a3c05eace89` |
| `components/tirtc_sdk/include/tiRTC.h` | `b8338a07532e09a8f5ccd6f0270a9d1fa3f227f8bc1a57fd663115b222984801` |

首方源码和文档未发现指向来源工程开发者个人目录的路径，也未发现硬编码的真实 Wi-Fi、
设备或 TiRTC 凭据；对应默认配置保持为空。ESP-Hosted 上游栈回溯样例的绝对路径已经
占位符化，不作为本项目构建配置。

## 发布边界

本项目按源码范围发布，不生成或上传 P4 APP 预编译 BIN。静态核验确认公开源码与来源 Tag
的路径和内容关系，以及 SDK 文件的校验值。开发者可在 ESP-IDF `5.5.4` 环境完成构建；
构建结果用于确认编译与链接，烧录、ESP-Hosted/SDIO、联网、音视频和长时间运行结果应在
目标板上分别验证。

面向开发者的环境、构建、烧录、首次启动和排障步骤见
[开发者上手指南](docs/GETTING_STARTED_CN.md)。
