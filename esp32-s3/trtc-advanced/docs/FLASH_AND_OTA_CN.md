# ESP32-S3 烧录与 OTA 说明

当前源码版本：`0.7.5`

## 当前 0.7.5 发布资产

| 用途 | 文件 | SHA-256 |
| --- | --- | --- |
| OTA app | `release_assets/web-flash/v0.7.5/sample_project.bin` | `993CE232062086A0DEF80E26DAFDABDEDCDF4DF4FB4A68112F6E15F00483A756` |
| ESP Launchpad 0x0 合一镜像 | `release_assets/web-install/v0.7.5/esp32s3-tirtc-device-monitor-full-v0.7.5.bin` | `B3B280C9282A91DF94BE244C70426717363378065FBA01E639B4875E0F8B167A` |
| ESP Launchpad zip | `release_assets/web-install/esp32s3-tirtc-device-monitor-webinstall-v0.7.5.zip` | `E42B7D733E7EF7F3669573956F58D13B40830ABF2A1B003EEAE493C1559550CE` |
| 维护者多地址烧录 zip | `release_assets/web-flash/esp32s3-tirtc-device-monitor-webflash-v0.7.5.zip` | `571493E2275C9F2051178C6FDF7223F9EB7746432C1734AD5A87DCF87DA9F473` |

## 推荐烧录方式

普通体验者只推荐使用 Espressif 官方 ESP Launchpad：

[https://espressif.github.io/esp-launchpad/](https://espressif.github.io/esp-launchpad/)

进入 DIY 模式后选择完整合一镜像，烧录地址填写 `0x0`。不要使用项目早期自建的一键烧录页面。

## 源码仓和烧录包的关系

源码仓只负责维护可构建源码、文档、示例结构和必要 SDK/资源文件。

正式烧录包需要从当前源码重新构建后生成，不能把历史 bin/zip 改名当作新版本发布。生成烧录资产后，再补齐：

- ESP Launchpad zip
- `0x0` 完整合一 bin
- 手动备用多地址烧录 zip
- `SHA256SUMS.txt`
- OTA app 固件大小和 SHA-256

## 完整合一镜像

完整合一镜像应包含：

- bootloader
- 分区表
- 空 NVS
- OTA 初始状态
- 应用固件
- storage / 字库 / 图片资源

完整合一镜像写入地址统一为：

```text
0x0
```

## 手动备用烧录包

手动多地址烧录包只给维护者排障使用，不作为普通体验者的一键烧录入口。

手动烧录时，app 固件必须同时写入 OTA 双分区：

| Flash 地址 | 内容 |
| --- | --- |
| `0x10000` | `ota_0` app 固件 |
| `0x780000` | `ota_1` app 固件 |

其他 bootloader、partition table、NVS、otadata、phy init 和 storage 地址以本次 `build/flasher_args.json` 和发布包内 `flash_args.txt` 为准。

## OTA 包

设备会请求：

```text
GET https://tirtc-device-ota.tange365.com/api/ota/manifest?device_id=<设备ID>&chip=s3&version=<当前版本>
```

云端 manifest 必须和本次构建出的 app 固件一致：

```json
{
  "version": "0.7.5",
  "update": true,
  "firmware": {
    "size": "<本次 app bin size>",
    "sha256": "<本次 app bin sha256>"
  }
}
```

如果设备当前已经是 `0.7.5`，服务端应返回 `update=false` 和 `reason=up_to_date`。

## 发布检查

- `CMakeLists.txt` 的 `PROJECT_VER` 必须是 `0.7.5`。
- `build/project_description.json` 的版本必须是 `0.7.5`。
- 线上 manifest 的 size 和 SHA-256 必须与本地 `build/sample_project.bin` 一致。
- `release_assets` 只保留当前发布版本的 bin/zip 包。
- 没有重新构建和校验前，不要声明烧录包或 OTA 包已经发布。
