# 源码来源与发布边界

本文档记录 TiRTC ESP32-P4 完整设备应用在统一示例仓中的来源快照、公开筛选范围和静态核验边界。

## 来源快照

| 项目 | 内容 |
| --- | --- |
| 应用版本 | `1.2.3` |
| 来源 Tag | `esp32-p4-device-app-v1.2.3` |
| 来源 commit | `c1af1eb1bcfa9da4b359ee24d25afb30f75d24b0` |
| 公开路径 | `complete-applications/esp32-p4/device-monitor/` |
| 目标开发板 | Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 |
| ESP-IDF | `5.5.4` |
| TiRTC SDK | `2.3.0` ESP32-P4 test package |

来源 Tag 指向上述 commit。导入审阅时，来源仓工作树处于干净状态。

## 公开筛选

统一仓保留应用源码、第三方组件、TiRTC SDK test package、必要配置、辅助脚本和面向开发者的文档。
以下内容不进入公开源码快照：

- 来源仓 Git 元数据、内部协作文档和本机 IDE 配置。
- `build/`、`managed_components/`、日志、固件镜像和发布附件目录。
- 与单台开发机绑定的 `.clangd` 和完整 `sdkconfig`；可复用配置由 `sdkconfig.defaults` 提供。

第三方组件及其许可证按来源快照保留；公开卫生整理只把上游示例日志中的开发机绝对路径
改为 `<esp-hosted-source>` 占位符。

## 静态一致性

首次导入后，候选目录包含来源 Tag 中的 `1,193` 个文件：`222` 个文件逐字节一致，
`971` 个文件仅换行符不同，没有发现实质内容差异。来源 Tag 中未纳入候选的两个文件为
`.clangd` 和 `sdkconfig`。

TiRTC SDK 关键文件校验值：

| 文件 | SHA-256 |
| --- | --- |
| `components/tirtc_sdk/lib/esp32p4/libTiRTC.a` | `70b54792399ab983b8ec5b6ccdd295eab7b0c13e98c4769e17b82a3c05eace89` |
| `components/tirtc_sdk/include/tiRTC.h` | `b8338a07532e09a8f5ccd6f0270a9d1fa3f227f8bc1a57fd663115b222984801` |

首方源码和文档未发现指向来源工程开发者个人目录的路径，也未发现硬编码的真实 Wi-Fi、
设备或 TiRTC 凭据；对应默认配置保持为空。ESP-Hosted 上游栈回溯样例的绝对路径已经
占位符化，不作为本项目构建配置。

## 发布边界

本项目按源码范围发布，不生成或上传 P4 APP 预编译 BIN。静态核验可以确认公开源码与来源
Tag 的文件内容关系，以及 SDK 文件的校验值；构建、烧录和目标板运行结果需要由开发者在
对应环境中另行验证。
