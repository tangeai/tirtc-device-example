# 固件下载与校验

## 下载入口

正式源码和固件统一在
[GitHub Releases](https://github.com/tangeai/tirtc-device-example/releases) 发布。
每个项目使用独立的 SemVer Tag；只更新某个项目时，只创建该项目的新 Tag 和 Release。
固件、文件系统镜像和维护附件不进入 Git 历史。

| 项目 | 版本 | 项目 Release |
| --- | --- | --- |
| ESP32-S3 最小 TiRTC 集成示例 | `1.2.0` | [`esp32-s3-minimal-integration-v1.2.0`](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-s3-minimal-integration-v1.2.0) |
| ESP32-P4 最小 TiRTC 集成示例 | `1.1.1` | [`esp32-p4-minimal-integration-v1.1.1`](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-p4-minimal-integration-v1.1.1) |
| G32S10X 最小 TiRTC 集成示例 | `0.8.3` | [`g32s10x-minimal-integration-v0.8.3`](https://github.com/tangeai/tirtc-device-example/releases/tag/g32s10x-minimal-integration-v0.8.3) |
| ESP32-S3 最小系统例子 | `0.8.0` | [`esp32-s3-minimal-system-v0.8.0`](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-s3-minimal-system-v0.8.0) |
| ESP32-P4 最小系统例子 | `0.2.0` | [`esp32-p4-minimal-system-v0.2.0`](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-p4-minimal-system-v0.2.0) |
| ESP32-S3 Device Monitor | `1.9.5` | [`esp32-s3-device-monitor-v1.9.5`](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-s3-device-monitor-v1.9.5) |
| ESP32-P4 Device Monitor | `1.3.2` | [`esp32-p4-device-monitor-v1.3.2`](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-p4-device-monitor-v1.3.2) |
| G32S10X Device Monitor | `0.1.1` | [`g32s10x-device-monitor-v0.1.1`](https://github.com/tangeai/tirtc-device-example/releases/tag/g32s10x-device-monitor-v0.1.1) |

每个 Release 只包含该项目的固件，以及两份核验资料：

| 文件 | 用途 |
| --- | --- |
| 项目固件 | 烧录或 OTA 使用的实际构建产物 |
| `SHA256SUMS.txt` | 核对下载文件是否完整 |
| `release-manifest.json` | 记录项目 Tag/commit、开发来源、SDK、构建证据、附件大小和 SHA-256 |

## 当前固件清单

| 项目 | 文件 | 大小 | SHA-256 | 用途 |
| --- | --- | ---: | --- | --- |
| S3 最小示例 | `esp32s3-tirtc-minimal-full-v1.2.0.bin` | `16,777,216` | `083d59a05268da48bfb434c13e578a8920d095b8fe13831d9a9744064a955f8a` | ESP Tool，地址 `0x0` |
| P4 最小示例 | `esp32p4-tirtc-minimal-full-v1.1.1.bin` | `16,777,216` | `1409cc0d72d470b54363909fd608468b4f1e7c84d7e0d622f46f376fa27dbc3d` | ESP Tool，地址 `0x0` |
| G32S10X 最小示例 | `g32s10x-tirtc-minimal-rtos-with-spl-v0.8.3.bin` | `6,695,236` | `ef79659daf2f176eb6642c01af8fdb901e5a5499c861da9f9e5f322f0dde180f` | 君正 Cloner 主固件区，按项目 policy 烧录 |
| S3 最小系统例子 | `esp32s3-tirtc-minimal-system-full-v0.8.0.bin` | `4,194,304` | 见本 Release 的 `SHA256SUMS.txt` | ESP Tool，地址 `0x0` |
| P4 最小系统例子 | `esp32p4-tirtc-minimal-system-full-v0.2.0.bin` | `4,194,304` | 见本 Release 的 `SHA256SUMS.txt` | ESP Tool，地址 `0x0` |
| S3 Device Monitor | `esp32s3-tirtc-device-monitor-full-v1.9.5.bin` | `16777216` | `fae989a721e076eed7ba8d2d31cdec0c040acf133124199989f381e7854a162e` | ESP Tool，地址 `0x0` |
| S3 Device Monitor | `esp32s3-tirtc-device-monitor-ota-v1.9.5.bin` | `7,608,608` | `51a7599942f06556e33ef4820499885d6213ff15fce0f3ed2f11e38e44146503` | OTA app，不写入 `0x0`；分区剩余 `190,176` bytes（`2.44%`） |
| P4 Device Monitor | `esp32p4-tirtc-device-monitor-full-v1.3.2.bin` | `16,777,216` | `87bfb67d1ba30d7f79663f63891e29f7f4f4367c80ff0d5cecb1b46f301d40e9` | ESP Tool，地址 `0x0` |
| G32S10X Device Monitor | `g32s10x-tirtc-device-monitor-rtos-with-spl-v0.1.1.bin` | `6,965,860` | `34f926d4fd6173a9433d3652503d66a6cb0df2e961dbd3e79dc846cfe3bbe29a` | 君正 Cloner 主固件区，按项目 policy 烧录 |
| G32S10X Device Monitor | `g32s10x-tirtc-device-monitor-fs-v0.1.1.yaffs2` | `12,808,192` | `5a8f1fbcbd6f70291e63ae00480ea545f388c6b2eaebd72c93ccb828a6ea60f2` | 君正 Cloner 文件系统区，按项目 policy 烧录 |
| G32S10X Device Monitor | `g32s10x-tirtc-device-monitor-data-v0.1.1.yaffs2` | `4,096` | `8f6039987ea3d8a96a686f9b3f5f26e22028c918492b6890b336bde2dac90876` | 君正 Cloner 数据区，按项目 policy 烧录 |

S3 Device Monitor `1.9.5` 的大小和 SHA-256 来自唯一正式干净构建。下载后仍应以同一
Release 的 `SHA256SUMS.txt` 复核文件，不要混用 `1.9.0` 资产。

## ESP32-S3/P4 完整镜像烧录

准备一根支持数据传输的 USB 线，并使用支持 Web Serial 的 Chrome 或 Edge。

1. 从目标项目 Release 下载文件名包含 `-full-` 的完整镜像和 `SHA256SUMS.txt`。
2. 先计算固件 SHA-256，确认与清单完全一致。
3. 断开其他串口终端，打开 [Espressif ESP Tool](https://espressif.github.io/esptool-js/)。
4. 点击 `Connect`，在浏览器弹窗中选择开发板串口。
5. 点击 `Add File`，加入完整镜像，`Flash Address` 填写 `0x0`。
6. 首次使用、跨项目切换或设备状态不确定时，先执行 `Erase Flash`。
7. 点击 `Program`。网页明确显示完成后，按开发板 `RESET`。

完整镜像会覆盖目标项目声明的整片 Flash，也会清除 NVS 中原有的 Wi-Fi、绑定和本地设置。
最小系统例子使用 4 MB 完整镜像，Device Monitor 使用 16 MB 完整镜像。
名称含 `ota` 的文件只是应用 OTA 镜像，不能作为完整镜像写入 `0x0`。

S3 Device Monitor `1.9.5` 的 RTC 业务是双向音频，摄像头只用于二维码扫描。该版本不生成
`storage.bin`；完整镜像中的未使用分区保持擦除态。不同版本的 full、OTA app 或分段文件不能
混用。Web IPC 和设备互呼仍可能听到轻微“沙沙电流声”，底噪根因尚未证实；下载校验和烧录
成功不等同于音频主观验收。

浏览器看不到串口时，先确认 USB 线支持数据传输、串口没有被其他程序占用、驱动已经安装。
必要时按住板卡 `BOOT` 后复位进入下载模式；不同板卡的按键组合以项目 README 和板卡资料为准。

## G32S10X 烧录

G32S10X 使用君正 Cloner。请从同一个项目 Release 下载完整的项目资产，并按项目 README、
目标板 Cloner 配置和 `release-manifest.json` 选择对应 policy。主固件、文件系统和 data 镜像
必须来自同一个 Release，不能跨版本混用。君正供应商 SDK、工具链和 Cloner 需要从授权渠道获取。

## 下载后校验

Windows PowerShell：

```powershell
Get-FileHash .\downloaded-firmware.bin -Algorithm SHA256
```

Linux/macOS：

```bash
sha256sum ./downloaded-firmware.bin
```

核对顺序：

1. 文件名、项目、版本和目标板一致。
2. 固件 SHA-256 与同一 Release 的 `SHA256SUMS.txt` 一致。
3. `release-manifest.json` 中的项目 Tag/commit、来源版本、SDK 和附件记录一致。
4. 烧录后再核对设备显示或日志中的应用版本、TiRTC SDK 版本和目标板信息。

静态校验可以证明源码身份、版本记录、Release notes、附件和 manifest 相互一致。证明固件确实
由指定源码构建，还需要可复现构建，或者在固件中嵌入 commit/版本元数据并完成比对。构建、
烧录、联网、真实媒体和长时间运行仍是相互独立的证据层。
