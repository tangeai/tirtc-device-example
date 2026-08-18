# 源码构建与配置

这份文档给准备修改 ESP32-S3 设备端示例的开发者使用。目标很直接：在不写入真实凭据、
不改变正式 SDK 快照的前提下，从公开源码完成一次可解释的构建，并知道接下来该烧录哪些
文件、从哪里看问题。

如果只是体验设备，请使用 Release 中的 `esp32s3-tirtc-device-monitor-full-v1.8.1.bin`，
不必安装 ESP-IDF。完整镜像的下载与烧录见 [烧录与 OTA](FLASH_AND_OTA_CN.md)。

## 1. 硬件和软件

| 项目 | 要求 |
| --- | --- |
| 开发板 | 立创·实战派 ESP32-S3，`LCKFB-SZPI-ESP32-S3-VA` |
| Flash / PSRAM | 16 MB / 8 MB，Octal PSRAM 80 MHz |
| 屏幕 | 320 x 240 触摸屏及本工程对应板级驱动 |
| 音视频 | 板载摄像头、麦克风、扬声器和音频 Codec |
| ESP-IDF | `5.5.4` |
| 编译器 | `xtensa-esp32s3-elf 14.2.0_20260121` |
| TiRTC SDK | `2.2.0` |
| 主机 | Windows PowerShell；建议把仓库放在较短、无特殊字符的路径 |

普通 ESP32-S3 DevKit 可以编译部分通用代码，但没有这块板子的屏幕、触摸、音频和摄像头，
不能据此判断完整应用是否正常。

## 2. 先确认拿到的是同一份源码

从 Git 仓使用本项目时，先在统一仓根目录确认状态，再进入项目目录：

```powershell
git rev-parse HEAD
git status --short
Set-Location .\complete-applications\esp32-s3\device-monitor
```

如果下载的是 GitHub 自动生成的源码归档，其中没有 `.git`，跳过前两条 Git 命令，并使用
同一 Release 的 `release-manifest.json` 核对版本和文件哈希。

当前项目版本应为 `1.8.1`，来源和依赖身份见 [版本信息](../VERSION.md) 与
[来源说明](../SOURCE_PROVENANCE.md)。TiRTC 静态库可以这样核对：

```powershell
Get-FileHash .\components\tirtc_sdk\lib\esp32s3\libTiRTC.a -Algorithm SHA256
```

预期 SHA-256：

```text
0686e5d7f5bfea18b7b3ee6ae1701061b3afcfb0ef2030642ebebab88af0413d
```

哈希不一致时先停下来确认文件来源，不要靠修改 `VERSION.md` 把版本“对齐”。

## 3. 默认配置能做什么

公开源码不带 Wi-Fi 密码、设备 ID、设备密钥或服务端密钥。正常体验不需要先改头文件：

1. 固件启动后在屏幕上选择 2.4 GHz Wi-Fi。
2. 设备通过服务发现获取业务入口。
3. 屏幕显示 6 位验证码后，在 ThingConnect H5 完成绑定。
4. `device_id/device_key` 写入 NVS，后续自动上线。

关键服务默认值在 `main/application/app_config.h`，微信主动呼叫版本类型在
`main/services/wechat_voip/wechat_voip_config.h`：

| 配置 | 默认值 | 什么时候需要改 |
| --- | --- | --- |
| `APP_CONFIG_THING_SERVICE_DISCOVERY_URL` | `http://ep-open.tangeopen.com/services` | 接入自己的服务发现环境 |
| `APP_CONFIG_DEVICE_BINDING_API_BASE` | `https://mqtt-demo.tange-ai.com` | 更换发现失败时的绑定兜底服务 |
| `APP_CONFIG_DEVICE_BINDING_MQTT_URI` | `mqtts://mqtt-demo.tange-ai.com:8883` | 更换发现失败时的 MQTT 兜底入口 |
| `APP_CONFIG_OTA_DEFAULT_URL` | `https://tirtc-device-ota.tange365.com` | 使用自己的 OTA 服务 |
| `APP_CONFIG_WIFI_SSID/PASSWORD` | 空 | 通常保持为空，在设备屏幕上配置 |
| `APP_CONFIG_WECHAT_VOIP_ACTIVE_CALL_VERSION_TYPE` | `WECHAT_VOIP_VERSION_TRIAL`（`2`） | 选择设备主动呼叫微信时使用的正式版 `0`、开发版 `1` 或体验版 `2` |
| `CONFIG_APP_DEBUG_SCREEN_SERVER_ENABLE` | 关闭 | 仅局域网 UI 调试时临时开启 |

这个版本类型只写入设备主动调用 `/v1/voip/device/call` 的请求。微信小程序呼叫设备的
来电链路不读取它；修改后应重新构建，并从主动呼叫日志确认 `version_type` 的实际值。

使用自建服务时，应成组核对服务发现、绑定、MQTT、OTA 和服务端设备身份，避免把测试环境的
设备身份带到正式环境。真实 Wi-Fi、设备密钥、Token 和用户身份不要写入提交、截图或 Issue。

## 4. 建立 ESP-IDF 环境

打开 ESP-IDF `5.5.4` 对应的 PowerShell，进入本项目目录：

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py --version
xtensa-esp32s3-elf-gcc --version
```

版本不对时先切换环境，不要继续复用旧 `build/`。组件管理器会根据 `main/idf_component.yml`
和 `dependencies.lock` 解析依赖，因此首次构建需要能访问对应组件源。

## 5. 做一次干净构建

```powershell
idf.py -B build --no-ccache reconfigure build
```

命令成功结束后，至少应看到：

```text
build/sample_project.bin
build/flasher_args.json
build/bootloader/bootloader.bin
build/partition_table/partition-table.bin
build/ota_data_initial.bin
build/storage.bin
```

`build/flasher_args.json` 是本次构建的烧录事实来源。应用镜像必须能放进任一 `0x770000`
OTA app 分区；看到分区空间不足时，应先处理体积，不能通过随意改分区地址绕过。

如果只修改了文档，无需重编译。源码、配置、SDK、编译器或 ESP-IDF 版本发生变化时，旧构建
输出就不再代表当前代码，应清理 `build/` 后重新构建。

## 6. 烧录源码构建结果

用 Chrome 或 Edge 打开
[Espressif ESP Tool](https://espressif.github.io/esptool-js/)，连接开发板串口，然后按
`build/flasher_args.json` 添加文件。当前分区布局为：

| Flash 地址 | 文件 |
| --- | --- |
| `0x0` | `build/bootloader/bootloader.bin` |
| `0x8000` | `build/partition_table/partition-table.bin` |
| `0xd000` | `build/ota_data_initial.bin` |
| `0x10000` | `build/sample_project.bin` |
| `0xf00000` | `build/storage.bin` |

五个文件的地址不能互换，也不能都写成 `0x0`。更换分区表后，应重新读取本次构建的
`flasher_args.json`，不要继续照抄上表。

这组文件会写入启动、分区、OTA、应用和资源区；首次验证建议先擦除 Flash。擦除或写入完整
镜像会清除 NVS 中的 Wi-Fi、绑定和本地设置，设备重启后需要重新配置。

烧录完成后按 RESET。需要串口日志时执行：

```powershell
idf.py -p COMx monitor
```

将 `COMx` 替换为实际串口，按 `Ctrl+]` 退出 Monitor。

## 7. 第一次启动看什么

按这个顺序确认，每一步通过后再继续：

1. 屏幕进入主页，触摸和翻页正常。
2. 设置页显示版本 `1.8.1`。
3. 连接 2.4 GHz Wi-Fi，能看到 SSID、IP 和真实信号状态。
4. 获取 6 位验证码，在 ThingConnect H5 绑定设备。
5. H5 设备列表出现设备，进入实时查看后验证画面、声音和对讲。
6. 再分别验证小钛、微信 VoIP、设备互呼和 OTA。

完整操作见 [从 Wi-Fi 到各项功能](GETTING_STARTED_CN.md)。构建成功说明源码和依赖能够
完成编译链接；屏幕、联网和各业务能力要以上述运行现象逐项判断。

## 8. 常见问题

| 现象 | 处理方式 |
| --- | --- |
| 找不到 `idf.py` | 重新执行 ESP-IDF 5.5.4 的 `export.ps1` |
| 编译器版本不对 | 关闭当前终端，重新打开正确 ESP-IDF 环境，不复用旧 `build/` |
| Windows 报路径过长 | 把仓库移到较短路径后重新生成 `build/`，不要移动单个生成文件 |
| TiRTC 静态库缺失或哈希不符 | 恢复公开仓中的正式文件，并对照 `VERSION.md`；不要混用其他平台库 |
| 组件下载失败 | 检查网络、代理和组件源，再重新执行 `reconfigure build` |
| app 超过分区 | 先检查新增资源、字体、图片和调试功能，保持 `0x770000` 分区契约 |
| 网页烧录后不启动 | 核对五个文件、地址和 16 MB Flash，按 RESET 后查看串口启动日志 |
| 启动后没有原 Wi-Fi 或绑定 | 擦除或完整烧录会清空 NVS，这是预期现象；重新完成首次配置 |
| 能联网但无法发现业务 | 确认服务发现仍为 `http://ep-open.tangeopen.com/services`，再检查设备时间和公网访问 |

排障时一次只改一个变量，并保留当前 commit、ESP-IDF 版本、构建命令和首个明确错误。这样
别人接手日志时，能直接从失败点继续，而不是重新猜一遍环境。
