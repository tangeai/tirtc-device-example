# ESP32-S3 烧录与 OTA

当前版本：`1.9.6`

版本提升、Tag 和资产不可变规则见 [OTA 版本规则](release/OTA_VERSIONING_CN.md)。

固件下载：
[`esp32-s3-device-monitor-v1.9.6` Release](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-s3-device-monitor-v1.9.6)

## 普通用户下载什么

下载这三个文件：

```text
esp32s3-tirtc-device-monitor-full-v1.9.6.bin
SHA256SUMS.txt
release-manifest.json
```

完整镜像从 `0x0` 烧录。它覆盖 16 MB Flash 地址空间，并清除 NVS 中已有的 Wi-Fi、绑定和
本地设置。需要保留现场数据时，使用正常 OTA，不要烧录 `full` 镜像。

## 先校验文件

Windows PowerShell：

```powershell
Get-FileHash .\esp32s3-tirtc-device-monitor-full-v1.9.6.bin -Algorithm SHA256
```

预期完整镜像信息：

| 项目 | 值 |
| --- | --- |
| 文件 | `esp32s3-tirtc-device-monitor-full-v1.9.6.bin` |
| 大小 | `16777216` bytes |
| SHA-256 | `0fa03360dc65281903af0854a3624f4de208ac0f83956047416916442eea0bdc` |

计算结果必须与同一 Release 的 `SHA256SUMS.txt` 和 `release-manifest.json` 同时一致。文件名、
版本或哈希任一不一致，都先停止烧录并重新下载。

## 使用 Espressif ESP Tool

使用支持 Web Serial 的 Chrome 或 Edge，打开官方工具：

[Espressif ESP Tool](https://espressif.github.io/esptool-js/)

1. 用支持数据传输的 USB 线连接开发板。
2. 关闭串口助手、IDE Monitor 和占用同一串口的其他浏览器标签页。
3. 点击 `Connect`，选择 ESP32-S3 开发板对应串口。
4. 点击 `Add File`，添加 `esp32s3-tirtc-device-monitor-full-v1.9.6.bin`。
5. 在 `Flash Address` 填写 `0x0`。
6. 首次使用、跨项目切换或设备状态不明确时，先执行 `Erase Flash`。
7. 点击 `Program`，等待网页明确显示完成。
8. 按开发板 `RESET`，等待屏幕进入主页。

### 找不到串口

- 更换一根已确认支持数据传输的 USB 线。
- 在 Windows 设备管理器确认串口已经出现。
- 关闭所有可能占用串口的程序。
- 按住 `BOOT`，点一下 `RESET`，松开 `RESET` 后再松开 `BOOT`，然后重新连接。

### 烧录完成但设备不启动

1. 确认文件名含 `full-v1.9.6`，不是 `ota-v1.9.6`。
2. 确认烧录地址为 `0x0`。
3. 擦除 Flash 后重新烧录。
4. 烧录结束后按 `RESET`。
5. 仍无法启动时，使用 ESP-IDF Monitor 查看 bootloader、Flash、分区表和首个错误。

## Release 文件区别

| 文件 | 用途 |
| --- | --- |
| `esp32s3-tirtc-device-monitor-full-v1.9.6.bin` | 从 `0x0` 完整烧录；重置整片 Flash 的本地数据 |
| `esp32s3-tirtc-device-monitor-ota-v1.9.6.bin` | OTA 服务端 app；不能写入 `0x0` |
| `SHA256SUMS.txt` | 校验 full、OTA app 和 manifest |
| `release-manifest.json` | 记录 Tag/commit、源码、SDK、构建环境、分区和资产 SHA-256 |

OTA app 信息：

| 项目 | 值 |
| --- | --- |
| 文件 | `esp32s3-tirtc-device-monitor-ota-v1.9.6.bin` |
| 大小 | `7,611,216` bytes |
| SHA-256 | `fa87ae46ffa93bd01da6deffade8cf23b22022f19040815b3b305ce7a56f0eec` |
| app 分区 | `7798784` bytes |
| 分区剩余 | `187,568` bytes（`2.41%`） |

正式构建确认 app 分区余量为 `187,568` bytes（`2.41%`）。OTA 服务必须使用该次构建
产生的 app，不能复用来源工程、测试构建或旧版本同名文件。

## 首次启动

完整镜像会把设备恢复为待配置状态：

1. 进入“设置” -> “Wi-Fi 设置”，连接 2.4 GHz Wi-Fi。
2. 等待设备显示 6 位验证码。
3. 打开 <https://demo-open.tange-ai.com/>，登录后输入验证码。
4. 绑定成功后确认设备显示正式设备 ID。
5. 在 H5 进入设备页面，验证设备麦克风上行和网页按住说话的音频下行。

`1.9.6` 是 RTC 双向音频产品，H5 不显示设备 RTC 视频是预期行为。板载摄像头只在联系人
二维码扫描时使用。

Web IPC 和设备互呼当前仍可能听到轻微“沙沙电流声”。线格式核对和 A-law 编解码自检未见
异常，但底噪根因尚未证实；烧录后请把实际听感作为单独验证项，不要把哈希或烧录成功当成
音质结论。

## 维护者多地址烧录

源码构建后的烧录地址以本次 `build/flasher_args.json` 为准。`1.9.6` 默认构建使用四段：

| 地址 | 文件 |
| --- | --- |
| `0x0` | `bootloader.bin` |
| `0x8000` | `partition-table.bin` |
| `0xd000` | `ota_data_initial.bin` |
| `0x10000` | `sample_project.bin` |

本版本不生成 `storage.bin`。分区表仍保留 `0xf00000` 的 storage 分区，正式完整镜像在该区域
保持擦除态 `0xFF`。不要把旧版本的 `storage.bin` 混入 `1.9.6`。

构建后仍使用
[Espressif ESP Tool](https://espressif.github.io/esptool-js/)，把每个文件与地址逐项添加。
详细构建说明见 [源码构建与配置](BUILD_AND_CONFIG_CN.md)。

## OTA

设备请求：

```text
GET https://tirtc-device-ota.tange365.com/api/ota/manifest?device_id=<设备ID>&chip=s3&version=<当前版本>
```

OTA 服务只保存 app 固件和 manifest，不提供 bootloader、分区表、NVS 或 storage。线上
manifest 的 `version` 必须为 `1.9.6`，`size` 和 `sha256` 必须与同一 GitHub Release 的
`release-manifest.json`、`SHA256SUMS.txt` 一致。

正常 OTA 只切换 app 分区，保留 NVS 和 storage。设备已经运行 `1.9.6` 时，服务端应返回
`update=false` 和 `reason=up_to_date`。

## 如何判断证据

- SHA-256 一致说明下载文件与 Release 清单一致。
- 网页显示 Program 完成说明烧录操作完成。
- 设备版本日志和设置页说明实际启动的是 `1.9.6`。
- Wi-Fi、绑定、云端在线、Web IPC、小钛、微信、设备互呼和 OTA 要逐项观察。
- 构建、上传、下载校验、烧录、联网和业务结果是独立检查点。

固件二进制只通过 GitHub Releases 分发，不进入普通 Git 历史。
