# 0.7.5 发布说明

## 版本定位

`0.7.5` 是当前发布到统一示例仓的 ESP32-S3 设备监控示例版本。这个版本已经完成本地构建、发布资产整理和源码仓收口。

对外沟通、烧录包、OTA manifest、发布说明和 Git tag 都统一使用 `0.7.5`。不要再引用中间测试包或未发布版本。

## 核心改进

- 固件版本号收口到 `0.7.5`。
- 全屏设备二维码从开机同步创建改为点击时按需创建，降低启动阶段 LVGL 对象和内存压力。
- 优化屏幕键盘触摸体验：Wi-Fi、联系人、微信联系人和 TiRTC 配置编辑键盘整体扩大可点击区域，改善左上角数字 `1` 等边缘按键不好点的问题。
- 集成 TiRTC SDK `2.2.0` 本地包。
- 保留 1 kHz FreeRTOS tick 配置，适配 TiRTC 音视频实时链路。
- 继续收敛 AI 对讲、微信 VoIP、IPC 查看三条链路的资源边界和关键日志。

## OTA 发布资产

OTA 服务端只发布 app 固件，不包含 NVS、storage、bootloader 或分区表。

| 项目 | 状态 |
| --- | --- |
| OTA 版本 | `0.7.5` |
| OTA app | 已重新构建，服务端部署以当前 OTA manifest 为准 |
| OTA app size | `7251104` bytes |
| OTA app SHA-256 | `993CE232062086A0DEF80E26DAFDABDEDCDF4DF4FB4A68112F6E15F00483A756` |
| web-flash zip | `release_assets/web-flash/esp32s3-tirtc-device-monitor-webflash-v0.7.5.zip` |
| web-flash zip SHA-256 | `571493E2275C9F2051178C6FDF7223F9EB7746432C1734AD5A87DCF87DA9F473` |
| 完整合一烧录包 | `release_assets/web-install/v0.7.5/esp32s3-tirtc-device-monitor-full-v0.7.5.bin` |
| 完整合一烧录包 size | `16777216` bytes |
| 完整合一烧录包 SHA-256 | `B3B280C9282A91DF94BE244C70426717363378065FBA01E639B4875E0F8B167A` |
| ESP Launchpad zip | `release_assets/web-install/esp32s3-tirtc-device-monitor-webinstall-v0.7.5.zip` |
| ESP Launchpad zip SHA-256 | `E42B7D733E7EF7F3669573956F58D13B40830ABF2A1B003EEAE493C1559550CE` |
| 从上一已发布版本升级 | 发布验收项，复测时按 OTA 页面状态和串口日志确认 |

## 验证记录

| 项目 | 结果 |
| --- | --- |
| `idf.py reconfigure build` | 通过 |
| `build/project_description.json` | `project_version=0.7.5` |
| OTA manifest | 部署后校验 `version/size/sha256` |
| 上一已发布版本 -> 0.7.5 OTA | 复测时确认 `Checking -> Downloading -> Verifying -> Ready -> Reboot` |
| ESP Launchpad 0x0 合一烧录 | 已生成完整合一资产，使用官方 ESP Launchpad DIY 模式烧录 |
| AI 对讲 | 已纳入 0.7.5 体验链路 |
| 微信 VoIP | 已纳入 0.7.5 体验链路 |
| IPC 查看 | 已纳入 0.7.5 体验链路 |

## 发布边界

- 对外说明统一使用 `0.7.5`，不再引用中间测试版本或旧烧录包。
- 网页烧录统一推荐 Espressif 官方 ESP Launchpad，使用 `0x0` 完整合一镜像。
- OTA 服务端只发布 app 固件；如果改动 bootloader、分区表或资源分区，需要重新发布完整烧录包。
