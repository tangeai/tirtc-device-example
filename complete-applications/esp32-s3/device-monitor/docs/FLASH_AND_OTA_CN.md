# ESP32-S3 烧录与 OTA

当前版本：`1.8.1`

固件下载：
[tirtc-device-example Releases](https://github.com/tangeai/tirtc-device-example/releases)

## 普通用户只下载这个文件

```text
esp32s3-tirtc-device-monitor-full-v1.8.1.bin
```

这是完整镜像，已经包含 bootloader、分区表、OTA 初始数据、应用和 storage。
烧录地址固定填写：

```text
0x0
```

不要只把 OTA app 写入 `0x0`，也不要把多个文件都填成同一个地址。

> 这个文件按 16 MB Flash 生成，会覆盖整片 Flash。原有 NVS、Wi-Fi、设备绑定和本地设置
> 都会被清除，烧录后要按新设备重新配置。需要保留这些数据时，请使用正常 OTA 升级，
> 不要烧录 `full` 镜像。

## 使用 Espressif ESP Tool 烧录

推荐 Chrome 或 Edge，使用 Espressif 官方工具：

[https://espressif.github.io/esptool-js/](https://espressif.github.io/esptool-js/)

1. 用支持数据传输的 USB 线连接开发板。
2. 打开 ESP Tool，点击 `Connect` 并选择开发板对应串口。
3. 点击 `Add File`，添加 `esp32s3-tirtc-device-monitor-full-v1.8.1.bin`。
4. 在 `Flash Address` 中填写 `0x0`。
5. 首次使用、版本跨度较大或设备状态不确定时，先擦除闪存。
6. 点击 `Program`，等待网页明确显示完成。
7. 按开发板 RESET，等待屏幕进入主页。

### 找不到串口

- 更换一根确认可以传输数据的 USB 线。
- 关闭正在占用串口的串口助手、IDE Monitor 或其他浏览器标签页。
- 按住 BOOT，点一下 RESET，松开 RESET 后再松开 BOOT，然后重新连接。
- Windows 设备管理器中确认串口已经出现。

### 烧录成功但设备没有启动

1. 确认文件是 `full` 完整镜像，不是 `ota` app。
2. 确认地址是 `0x0`。
3. 擦除闪存后重新烧录。
4. 烧录完成后按 RESET。
5. 仍无法启动时，使用命令行串口日志确认 bootloader、Flash 大小和分区表。

## Release 文件怎么选

| 文件 | 用途 |
| --- | --- |
| `esp32s3-tirtc-device-monitor-full-v1.8.1.bin` | 从 `0x0` 完整烧录；覆盖 16 MB Flash 并重置本地数据 |
| `esp32s3-tirtc-device-monitor-ota-v1.8.1.bin` | OTA 服务端 app 固件；不写 `0x0`，正常升级保留 NVS 和 storage |
| `SHA256SUMS.txt` | 下载后完整性校验 |
| `release-manifest.json` | 版本、来源、Flash 参数和资产清单 |

Windows 校验示例：

```powershell
Get-FileHash .\esp32s3-tirtc-device-monitor-full-v1.8.1.bin -Algorithm SHA256
```

结果应与同一 Release 中 `SHA256SUMS.txt` 记录的值一致。完整镜像按 16 MB Flash
布局合成，OTA app 必须小于或等于 `0x770000`；正式文件大小和完整资产清单以
`release-manifest.json` 为准。

## 开机后的首次体验

完整镜像烧录后，设备原有 Wi-Fi、绑定和本地设置已经重置，请重新走一遍首次配置：

1. 点击主页右下角箭头切换到第二页。
2. 进入“设置” -> “Wi-Fi 设置”，连接 2.4 GHz Wi-Fi。
3. 设备显示 6 位验证码后，打开 <https://mqtt-demo.tange-ai.com/>。
4. 注册或登录，在设备页面输入验证码。
5. 绑定成功后，在 H5 设备列表点击设备，确认可以出图、出声和对讲。

设备 ID 和密钥由绑定流程自动写入。普通体验者不需要在设置页手动录入。

## 维护者多地址烧录

源码构建后的多地址烧录以 `build/flasher_args.json` 为准，不要凭记忆手写地址。本工程
分区表对应的主要地址为：

| 地址 | 文件 |
| --- | --- |
| `0x0` | `bootloader.bin` |
| `0x8000` | `partition-table.bin` |
| `0xd000` | `ota_data_initial.bin` |
| `0x10000` | app 固件 |
| `0xf00000` | `storage.bin` |

源码构建：

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py -B build --no-ccache reconfigure build
```

构建完成后仍使用
[Espressif ESP Tool](https://espressif.github.io/esptool-js/)，按 `build/flasher_args.json`
列出的文件与地址进行多地址烧录。详细步骤见 [源码构建与配置](BUILD_AND_CONFIG_CN.md)。
需要查看串口日志时执行 `idf.py -p COMx monitor`。

## OTA

设备请求：

```text
GET https://tirtc-device-ota.tange365.com/api/ota/manifest?device_id=<设备ID>&chip=s3&version=<当前版本>
```

OTA 服务只保存 app 固件，不包含 NVS、storage、bootloader 或分区表。线上
manifest 的 `version` 必须为 `1.8.1`，`size` 和 `sha256` 必须分别与同一
Release 的 `release-manifest.json`、`SHA256SUMS.txt` 中 OTA app 记录一致。

正常 OTA 只切换 app 分区，不主动覆盖 NVS 和 storage，因此适合保留 Wi-Fi、设备身份和
本地设置的版本升级。OTA app 不能当作完整镜像从 `0x0` 烧录。

设备已经运行 `1.8.1` 时，服务端应返回 `update=false` 和 `reason=up_to_date`。

## 发布边界

- 源码仓不保存固件二进制。
- GitHub Releases 保存人工下载和完整烧录资产。
- OTA 服务保存在线升级所需的 app 固件和 manifest。
- GitHub Actions artifact 只适合短期构建验证，不作为长期正式下载地址。
- 构建通过、Release 上传、网页烧录、OTA 升级和真机功能回归是五个独立检查点。

每个版本的公开 Release 只提供以下资产：

- `esp32s3-tirtc-device-monitor-full-v1.8.1.bin`
- `esp32s3-tirtc-device-monitor-ota-v1.8.1.bin`
- `SHA256SUMS.txt`
- `release-manifest.json`

固件大小和 SHA-256 由正式构建产生，不在构建前预填。
