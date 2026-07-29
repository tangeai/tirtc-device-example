# 固件下载与校验

## 下载入口

正式源码和固件统一在
[GitHub Releases](https://github.com/tangeai/tirtc-device-example/releases) 按版本发布。
Git `main` 保存源码、文档、必要脚本和 SDK 静态库；构建生成的固件、维护包和其他二进制
附件不进入 Git 历史。

2026-07-30 Release 包含六个项目：

| 项目 | 版本 | 发布交付 |
| --- | --- | --- |
| ESP32-S3 最小 TiRTC 集成示例 | `1.2.0` | 源码与 `0x0` 完整镜像 |
| ESP32-P4 最小 TiRTC 集成示例 | `1.1.1` | 源码与 `0x0` 完整镜像 |
| G32S10X 最小 TiRTC 集成示例 | `0.8.3` | 源码与 `rtos-with-spl.bin` |
| ESP32-S3 Device Monitor | `1.7.6` | 源码、`0x0` 完整镜像与 OTA app |
| ESP32-P4 Device App | `1.2.3` | 源码随统一 Tag 交付 |
| G32S10X Device Monitor | `0.1.1` | 源码、主固件与两个 YAFFS 镜像 |

ESP32-P4 Device App `1.2.3` 本次按源码范围发布。其余项目的正式固件由统一公开 commit
完成干净构建，附件名称、大小、用途、Flash 地址和 SHA-256 以 Release 页面、
`SHA256SUMS.txt` 和 `release-manifest.json` 为准。

## 本次资产

| 项目 | Release 文件 | 烧录用途 |
| --- | --- | --- |
| ESP32-S3 最小示例 | `esp32s3-tirtc-minimal-full-v1.2.0.bin` | ESP Tool，地址 `0x0` |
| ESP32-P4 最小示例 | `esp32p4-tirtc-minimal-full-v1.1.1.bin` | ESP Tool，地址 `0x0` |
| G32S10X 最小示例 | `g32s10x-tirtc-minimal-rtos-with-spl-v0.8.3.bin` | 君正 Cloner 主固件 policy |
| ESP32-S3 Device Monitor | `esp32s3-tirtc-device-monitor-full-v1.7.6.bin` | ESP Tool，地址 `0x0` |
| ESP32-S3 Device Monitor | `esp32s3-tirtc-device-monitor-ota-v1.7.6.bin` | OTA app |
| G32S10X Device Monitor | `g32s10x-tirtc-device-monitor-rtos-with-spl-v0.1.1.bin` | 君正 Cloner 主固件 policy |
| G32S10X Device Monitor | `g32s10x-tirtc-device-monitor-fs-v0.1.1.yaffs2` | 君正 Cloner 文件系统 policy |
| G32S10X Device Monitor | `g32s10x-tirtc-device-monitor-data-v0.1.1.yaffs2` | 君正 Cloner 数据 policy |

## Release 资产规则

每次 Release 只上传本次版本实际生成并完成校验的资产：

| 内容 | 用途 |
| --- | --- |
| 项目固件 | 按项目和目标板提供烧录或集成验证所需的实际构建产物 |
| `SHA256SUMS.txt` | 校验附件下载完整性 |
| `release-manifest.json` | 记录统一 Tag/commit、项目来源、版本、文件用途、大小和 SHA-256 |

项目没有生成固件资产时，manifest 记录源码交付形态，不创建占位 BIN，也不沿用其他版本产物。

## 标签约定

六个项目使用一个统一公开 Tag：

```text
tirtc-device-examples-v2026.07.30
```

各项目的本地 Tag 和 commit 作为源码来源凭据，记录在
[版本与证据清单](VERSIONS_CN.md) 和 release manifest 中，不替代统一公开 Tag。

## ESP32-S3/P4 烧录

1. 打开 [Espressif ESP Tool](https://espressif.github.io/esptool-js/)。
2. 选择 `Connect` 并连接目标开发板串口。
3. 使用 `Add File` 添加从同一 Release 下载的目标板固件。
4. 按 Release manifest 或项目说明填写烧录地址。
5. 首次使用或设备状态不确定时执行 `Erase Flash`。
6. 选择 `Program`，完成后复位开发板。

固件类型和烧录地址必须以当次 Release 的实际资产说明为准，不能从旧版本文件名推断。

## G32S10X 烧录

G32S10X 使用君正 Cloner。请从同一 Release 获取对应项目资产，并按项目 README、Cloner
配置和 release manifest 选择固件及烧录布局。君正供应商 SDK、工具链和 Cloner 需要由
开发者按授权渠道单独获取。

## 下载后校验

1. 从同一 Release 下载目标项目资产、`SHA256SUMS.txt` 和 `release-manifest.json`。
2. 核对项目、版本、目标板和用途。
3. 计算附件 SHA-256，并与清单逐字比对。
4. 核对 manifest 中的统一 Tag/commit、项目来源 Tag/commit 和附件记录。
5. 烧录后核对设备可读取的应用版本、TiRTC SDK 版本和目标板信息。

PowerShell：

```powershell
Get-FileHash .\downloaded-firmware.bin -Algorithm SHA256
```

Linux/macOS：

```bash
sha256sum ./downloaded-firmware.bin
```

静态校验可以证明发布源码、版本记录、Release notes、附件和 manifest 相互一致。若要证明
固件由指定源码构建，还需要可复现构建，或在固件中嵌入 commit/版本元数据并完成比对。
