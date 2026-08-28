# 源码构建与配置

本文面向要修改 ESP32-S3 Device Monitor 源码的开发者。只想体验设备时，直接下载
`esp32s3-tirtc-device-monitor-full-v1.9.7.bin`，按[烧录与 OTA](FLASH_AND_OTA_CN.md)
从 `0x0` 烧录即可。

## 1. 环境与硬件

| 项目 | 要求 |
| --- | --- |
| 开发板 | 立创·实战派 `LCKFB-SZPI-ESP32-S3-VA` |
| Flash / PSRAM | 16 MB / 8 MB |
| 显示 | 320 x 240 触摸屏 |
| 音频 | 板载麦克风、扬声器和 Codec |
| 摄像头 | 用于二维码扫描；不是 RTC 视频源 |
| ESP-IDF | `5.5.4` |
| Xtensa 工具链 | `14.2.0_20260121` |
| TiRTC SDK | `2.3.0 mini` 基线；active-connect `db7290f`；HTTPS 认证 `13e34c3` |

普通 ESP32-S3 DevKit 可以编译部分通用代码，但缺少目标板显示、触摸和音频外设，不能替代
本项目的目标板验证。

## 2. 核对源码身份

从公开仓开始时先固定项目 Tag：

```powershell
git clone https://github.com/tangeai/tirtc-device-example.git
Set-Location .\tirtc-device-example
git checkout esp32-s3-device-monitor-v1.9.7
Set-Location .\complete-applications\esp32-s3\device-monitor
```

当前版本应为 `1.9.7`，开发来源为：

```text
Tag:    v1.9.7
Tag object: 73f6a3b18ac4ad5e95f813c6eba4f31907961be6
commit: 58c2d152a179fa382511f4217fb84858072ffbb0
tree:   206b2373a389fab39ece1893f23f1ddbf0e66dfe
public code commit:     a3c6d4c5bdecb215f8c8ebec554424de77ed193b
public repository tree: 7508d3f00fb436a0d9bfd716fc1fc152f2d0cf8a
public project tree:    ac13d559aae8f74e92afbf585fe79c5da02aa324
```

TiRTC 静态库核对：

```powershell
Get-FileHash .\components\tirtc_sdk\lib\esp32s3\libTiRTC.a -Algorithm SHA256
```

预期结果：

```text
f5eca6c3212a34d00ce5706e864f8112adc8e6b20b92bc175435299caf4b082b
```

库大小应为 `2,127,226` bytes。BuildInfo 仍为 `v2.3.0-db7290f`，HTTPS 服务端认证修复来自
`13e34c3e3e3dc6776be4713b5c1e3c17bd282766`。公开库已执行 `--strip-debug`，只移除 SDK
内部源码路径和行号级调试信息。哈希或大小不一致时，应恢复项目记录的完整 SDK 快照；不要
只换库、只换头文件，或用修改文档的方式掩盖混用。

## 3. 默认配置

`sdkconfig.defaults` 是公开构建契约。关键值如下：

| 配置 | 默认值 | 目的 |
| --- | --- | --- |
| Flash / PSRAM | 16 MB / Octal 80 MHz | 匹配目标板 |
| CPU | 240 MHz | 匹配实时音频和 UI 负载 |
| FreeRTOS tick | 1000 Hz | 对齐 TiRTC SDK |
| main task stack | 16,384 bytes | 容纳启动阶段的完整初始化链 |
| internal RAM reserve | 98,304 bytes | 留给 DMA、实时控制和同步对象 |
| external task stack | 开启 | 允许适合的后台任务使用 PSRAM |
| NVS cache | PSRAM | 降低 internal RAM 压力 |
| mbedTLS dynamic buffer | 开启，外部内存 | 降低 TLS 峰值 internal RAM 占用 |
| MQTT outbox | PSRAM | 避免长消息挤占实时内存 |
| ESP 证书包 | 完整证书包开启 | 为应用和 TiRTC SDK 的 HTTPS 提供 CA 信任源 |
| 不安全 TLS | 关闭 | 证书链或主机名失败时拒绝连接，不跳过验证 |
| MQTT SSL | 开启 | 支持正式设备与临时绑定的 MQTTS 连接 |
| TCPIP task stack / mailbox | 6,144 bytes / 64 | 承载 MQTT、HTTP 和 TiRTC 网络活动 |
| Task WDT panic | 开启 | 让阻塞问题保留明确失败证据 |
| 串口诊断 CLI | 开启 | 支持现场网络、RTC、音频和呼叫排障 |
| 局域网屏幕调试服务 | 关闭 | 不进入默认公开固件 |

公开源码不含 Wi-Fi 密码、设备 ID、设备密钥或 Token。设备正常启动后在屏幕上配网，通过
6 位验证码绑定，正式身份由设备写入 NVS。

服务入口在 `main/application/app_config.h`：

| 配置 | 默认值 |
| --- | --- |
| 服务发现 | `https://ep-open.tangeopen.com/services` |
| 绑定兜底 API | `https://srv-open.tangeopen.com` |
| MQTT 兜底入口 | `mqtts://mqtt-open.tangeopen.com:8883` |
| OTA 服务 | `https://tirtc-device-ota.tange365.com` |
| Wi-Fi SSID / password | 空，由设备 UI 配置 |
| 微信主动呼叫版本类型 | 体验版 `2` |

接入自己的环境时，成组核对服务发现、绑定、MQTT、OTA 和设备身份。不要把测试凭据写进
`sdkconfig.defaults`、源码、日志、截图或 Issue。

服务发现结果只接受 `https://` 和 `mqtts://`。通用 HTTP 客户端校验证书链与 hostname，
禁用自动重定向；临时绑定和正式设备 MQTT 在创建客户端前再次拒绝非 MQTTS 地址。TiRTC SDK
自有 HTTP 客户端强制 `MBEDTLS_SSL_VERIFY_REQUIRED` 并挂接 ESP-IDF 证书包，证书链、
hostname、握手或 verify flags 失败时返回 SSL 错误，不降级到 HTTP。

设备互呼继续使用独立音频配置：全双工高性能线性 AEC、PSRAM AEC 工作区、AEC 后
100 Hz 高通、更保守的通话 AGC 噪声底线，以及按缓冲水位工作的有界 `1.25%` 播放速率
微调。这些配置只属于 Device Call profile；修改时不要顺手套到 Web IPC、微信 VoIP 或小钛。
三条设备上行路径的公开线格式统一为
`8 kHz / 16 bit / mono / G.711 A-law / 20 ms / 160 bytes`。

## 4. 建立 ESP-IDF 环境

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py --version
xtensa-esp32s3-elf-gcc --version
```

确认输出是 ESP-IDF `5.5.4` 和 Xtensa `14.2.0_20260121`。版本不对时先切换环境；不要让
旧工具链继续使用已有 `build/`。

## 5. 做一次干净构建

本地开发验证建议使用从未存在过的构建目录：

```powershell
idf.py -B build --no-ccache reconfigure build
```

本次正式 Release 资产使用隔离候选，在全新目录构建一次。该候选与公开代码提交
`a3c6d4c5bdecb215f8c8ebec554424de77ed193b` 的 repository tree
`7508d3f00fb436a0d9bfd716fc1fc152f2d0cf8a`、项目 tree
`ac13d559aae8f74e92afbf585fe79c5da02aa324` 完全一致，构建输入因此可静态绑定到公开源码。
后续复现应检出该公开代码提交，并把生成的 `sdkconfig` 放在全新构建目录中：

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py -B <fresh-build-dir> --no-ccache `
  -D SDKCONFIG=<fresh-build-dir>/sdkconfig `
  -D SDKCONFIG_DEFAULTS=<project-dir>/sdkconfig.defaults `
  reconfigure build
```

这次构建同时产生 OTA app 和完整镜像所需的所有分段。源码、配置或工具链在构建后发生变化，
整套输出随即失效，必须重新从新目录构建，不能单独补编某个固件文件。

源码、配置、SDK、ESP-IDF 或编译器变化后，都要删除旧构建目录或换一个全新目录。成功后至少
应有：

```text
build/sample_project.bin
build/flasher_args.json
build/bootloader/bootloader.bin
build/partition_table/partition-table.bin
build/ota_data_initial.bin
```

`1.9.7` 不生成 `storage.bin`。分区表仍保留 `0xf00000` 的 1 MB storage 分区，但正式完整
镜像将未使用区域保持为 `0xFF`。不要从旧版本构建目录复制 `storage.bin` 混入本版本。

正式公开构建记录：

| 项目 | 值 |
| --- | --- |
| 构建步骤 | `1767/1767` |
| 编译 warning / error / ICE | `0 / 0 / 0` |
| app 大小 | `7,613,216` bytes（`0x742b20`） |
| app SHA-256 | `49349c1e1755ae5265e3ddea6f57166ccf60b90cdca7d1a1a004539f4faf43ff` |
| app 分区 | `7798784` bytes |
| 分区剩余 | `185,568` bytes（`2.38%`） |

本次正式构建使用 ESP-IDF `5.5.4`、GCC `14.2.0_20260121` 并禁用 ccache，完成
`1767/1767`；容量余量为 `185,568` bytes（`2.38%`），没有沿用 `1.9.6` 的大小和哈希。
如果增加图片、字体、日志、SDK 或调试功能，应把容量复核当作必做
步骤，而不是等 OTA 失败后再处理。

## 6. 多地址烧录本地构建

用 Chrome 或 Edge 打开
[Espressif ESP Tool](https://espressif.github.io/esptool-js/)，按本次构建的
`build/flasher_args.json` 添加文件。当前布局为：

| 地址 | 文件 |
| --- | --- |
| `0x0` | `build/bootloader/bootloader.bin` |
| `0x8000` | `build/partition_table/partition-table.bin` |
| `0xd000` | `build/ota_data_initial.bin` |
| `0x10000` | `build/sample_project.bin` |

地址不能互换。修改分区表或构建参数后，以新生成的 `flasher_args.json` 为准，不要继续照抄
旧表。首次验证建议先擦除 Flash；这会清除 NVS 中的 Wi-Fi、绑定和本地设置。

### 6.1 维护者生成 Release 资产

正式构建完成后，只从这一个构建目录生成一套资产：

1. 将 `sample_project.bin` 原样命名为
   `esp32s3-tirtc-device-monitor-ota-v1.9.7.bin`，不做二次编译或字节修改。
2. 读取本次 `flasher_args.json`，把 bootloader、partition table、OTA data 和 app 写入一份
   预填 `0xFF` 的 16 MB 镜像，生成
   `esp32s3-tirtc-device-monitor-full-v1.9.7.bin`。不要手写沿用旧版本地址。
3. 逐段比对完整镜像与四个构建分段；检查其余地址仍为 `0xFF`，并核对 app descriptor 中
   的版本为 `1.9.7`。
4. 生成 `release-manifest.json`，记录公开 commit、来源 Tag/commit/tree、逐文件源码清单与
   SHA-256、SDK 身份、构建环境、完整命令、分区和两个固件文件的大小与 SHA-256。
5. 生成 `SHA256SUMS.txt`，至少包含 full、OTA app 和 manifest。最终上传这四个文件，
   二进制不进入 Git 历史。

正式结果为：full `16777216` bytes / `48a92849ef0149a4b099e981c4e109fcfa81afcbf99d379da1f7de61ad996bd1`，OTA app
`7,613,216` bytes / `49349c1e1755ae5265e3ddea6f57166ccf60b90cdca7d1a1a004539f4faf43ff`。上传前仍要以
`SHA256SUMS.txt` 和 `release-manifest.json` 复核本地文件，不能只按文件名判断版本。

## 7. 串口诊断 CLI

默认构建包含 `APP_SERIAL_NET_CLI_ENABLE=y`。用 Monitor 连接设备：

```powershell
idf.py -p COMx monitor
```

输入 `AT+HELP` 获取当前命令表。常用命令：

| 目的 | 命令 |
| --- | --- |
| 网络概况 | `AT+NET?`、`AT+WIFI?`、`AT+NETPROBE` |
| Wi-Fi 扫描与重试 | `AT+WIFISCAN`、`AT+WIFIRETRY` |
| Socket 和内存 | `AT+SOCKETS?`、`AT+HEAP?` |
| RTC 与媒体 | `AT+MEDIA?`、`AT+RTCLOG?`、`AT+RTCLINK?` |
| 音频链路 | `AT+AUDIOPATH?`、`AT+AUDIOCHECK?`、`AT+AECDUMP?` |
| 呼叫状态 | `AT+CALL?` |

命令不会回显密码、Token 或设备密钥。量产固件不需要现场诊断时运行：

```powershell
idf.py menuconfig
```

进入 `TiRTC Device Monitor -> Debug utilities`，关闭 `Enable UART network diagnostics
command set`，保存后从全新目录重新构建。关闭 CLI 会改变最终 app，旧固件哈希随即失效。

## 8. 启动检查顺序

1. 串口镜像描述信息和设置页都显示 `1.9.7`。
2. 屏幕、触摸和主页切换正常。
3. 连接 2.4 GHz Wi-Fi，看到 IP、时间同步和服务发现结果。
4. 完成 6 位码绑定并看到正式设备 ID。
5. Web IPC 验证双向音频；本版本不期待 RTC 视频画面。
6. 分别进入小钛、微信 VoIP 和设备互呼，确认每次退出后资源可再次申请。
7. 在可控测试环境验证有效证书连接成功，同时验证错误证书或错误 hostname 连接失败。
8. 需要排障时，用 AT 命令先找第一处失败阶段，再决定修改网络、协议、媒体或驱动层。

构建通过说明源码和依赖能够完成编译链接。烧录、串口启动、Wi-Fi、云端在线、业务功能、
音频主观效果和长时间运行仍需分别验证。

## 9. 常见问题

| 现象 | 处理方式 |
| --- | --- |
| 找不到 `idf.py` | 重新加载 ESP-IDF `5.5.4` 的 `export.ps1` |
| SDK 哈希不符 | 恢复同一个 `2.3.0 mini` 包，不要混用头文件和库 |
| 组件下载失败 | 检查网络、代理和组件源，再执行 `reconfigure build` |
| app 超过分区 | 检查新增 SDK、图片、字体和调试功能；不要随意移动 OTA 分区 |
| Windows 路径过长 | 把仓库放到较短路径，从新目录构建 |
| 网页烧录后不启动 | 核对四个文件、地址、16 MB Flash 和 RESET 后的启动日志 |
| H5 没有画面 | `1.9.7` 的 RTC 产品能力是双向音频，这是预期行为 |
| Web IPC 或设备互呼有轻微沙沙声 | 先记录业务、方向和音量，再看 `AT+AUDIOPATH?`、`AT+AUDIOCHECK?` 与 `AT+AECDUMP?`；当前根因尚未闭环，不要仅凭线格式正常就宣称音质问题已修复 |
| 能联网但业务发现失败 | 先看 `AT+NET?` 和 `AT+NETPROBE`，再核对服务发现入口和设备时间 |
| HTTPS/MQTTS 证书校验失败 | 先核对设备时间、域名和证书链；不要改成 HTTP/MQTT 或关闭证书验证 |

排障时保留当前 commit、工具链版本、构建命令、首个错误和 AT 查询结果。一次只改一个变量，
比同时调整多项参数更容易找到真正原因。
