# ESP32-S3 烧录与 OTA

当前版本：`0.7.5`

固件下载：
[ESP32-S3 v0.7.5 GitHub Release](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-s3-trtc-advanced-v0.7.5)

## 普通用户只下载这个文件

```text
esp32s3-tirtc-device-monitor-full-v0.7.5.bin
```

这是 16 MB 完整镜像，已经包含 bootloader、分区表、OTA 初始数据、应用和 storage。
烧录地址固定填写：

```text
0x0
```

不要只把 OTA app 写入 `0x0`，也不要把多个文件都填成同一个地址。

## 使用 ESP Launchpad 烧录

推荐 Chrome 或 Edge，使用 Espressif 官方工具：

[https://espressif.github.io/esp-launchpad/](https://espressif.github.io/esp-launchpad/)

1. 用支持数据传输的 USB 线连接开发板。
2. 打开 ESP Launchpad，进入 DIY 烧录模式。
3. 点击连接设备，选择开发板对应串口。
4. 添加 `esp32s3-tirtc-device-monitor-full-v0.7.5.bin`。
5. 在地址栏填写 `0x0`。
6. 首次使用、版本跨度较大或设备状态不确定时，先擦除闪存。
7. 开始烧录，等待网页明确显示完成。
8. 按开发板 RESET，等待屏幕进入主页。

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

| 文件 | 用途 | 大小 | SHA-256 |
| --- | --- | ---: | --- |
| `esp32s3-tirtc-device-monitor-full-v0.7.5.bin` | 普通用户使用 ESP Launchpad 从 `0x0` 烧录 | `16777216` | `81AA9FE1E1CC0E8A6F428A1D675BE78DA56558F09BD5C45CAD819C46ADA95B46` |
| `esp32s3-tirtc-device-monitor-webinstall-v0.7.5.zip` | 完整镜像和简明烧录说明 | `5487666` | `83473E73399F5080FAEDEA605A745A73B3F8F52D759EC1FC0EF5511323DE7F79` |
| `esp32s3-tirtc-device-monitor-webflash-v0.7.5.zip` | 维护者多地址排障包 | `5477614` | `8C6769BDA7BB0EC3BF950F7C4F73B3901C4DA2C6892DD5C7F26D993C52076534` |
| `esp32s3-tirtc-device-monitor-ota-v0.7.5.bin` | OTA 服务端 app 固件，不用于 `0x0` 烧录 | `7291120` | `E813347914D70D284D82E3AB798E54A6292530CE97A20C30FE4EBA58FE32BFFC` |
| `SHA256SUMS.txt` | 下载后完整性校验 | - | Release 内提供 |
| `release-manifest.json` | 版本、来源、Flash 参数和资产清单 | - | Release 内提供 |

Windows 校验示例：

```powershell
Get-FileHash .\esp32s3-tirtc-device-monitor-full-v0.7.5.bin -Algorithm SHA256
```

结果应与上表一致。

## 开机后的首次体验

1. 点击主页右下角箭头切换到第二页。
2. 进入“设置” -> “Wi-Fi 设置”，连接 2.4 GHz Wi-Fi。
3. 设备显示 6 位验证码后，打开 <https://mqtt-demo.tange-ai.com/>。
4. 注册或登录，在设备页面输入验证码。
5. 绑定成功后，在 H5 设备列表点击设备，确认可以出图、出声和对讲。

设备 ID 和密钥由绑定流程自动写入。普通体验者不需要在设置页手动录入。

## 维护者多地址烧录

`webflash` zip 只给维护者排障。解压后以包内 `flash_args.txt` 为准，不要凭记忆手写地址。
本次构建的主要地址为：

| 地址 | 文件 |
| --- | --- |
| `0x0` | `bootloader.bin` |
| `0x8000` | `partition-table.bin` |
| `0xd000` | `ota_data_initial.bin` |
| `0x10000` | app 固件 |
| `0xf00000` | `storage.bin` |

命令行从源码构建和烧录：

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py build
idf.py -p COMx flash monitor
```

## OTA

设备请求：

```text
GET https://tirtc-device-ota.tange365.com/api/ota/manifest?device_id=<设备ID>&chip=s3&version=<当前版本>
```

OTA 服务只保存 app 固件，不包含 NVS、storage、bootloader 或分区表。线上 manifest 的
`version`、`size` 和 `sha256` 必须与本次构建的 OTA app 完全一致：

```json
{
  "version": "0.7.5",
  "update": true,
  "firmware": {
    "size": 7291120,
    "sha256": "E813347914D70D284D82E3AB798E54A6292530CE97A20C30FE4EBA58FE32BFFC"
  }
}
```

设备已经运行 `0.7.5` 时，服务端应返回 `update=false` 和 `reason=up_to_date`。

## 发布边界

- 源码仓不保存 `.bin/.zip`。
- GitHub Releases 保存人工下载和完整烧录资产。
- OTA 服务保存在线升级所需的 app 固件和 manifest。
- GitHub Actions artifact 只适合短期构建验证，不作为长期正式下载地址。
- 构建通过、Release 上传、网页烧录、OTA 升级和真机功能回归是五个独立检查点。
