# ESP32-S3 OTA 版本规则

本规则适用于 ESP32-S3 Device Monitor `1.9.7` 及后续版本。烧录文件的选择、校验和操作步骤
见[烧录与 OTA](../FLASH_AND_OTA_CN.md)。

## 版本唯一入口

- 应用版本由根目录 `CMakeLists.txt` 的 `PROJECT_VER` 定义。
- 固件运行时通过 ESP-IDF app descriptor 读取同一个版本，不另设一份手写运行版本。
- `1.9.7` 的开发来源是 annotated Tag `v1.9.7`、Tag object
  `73f6a3b18ac4ad5e95f813c6eba4f31907961be6`、commit
  `58c2d152a179fa382511f4217fb84858072ffbb0`。
- 统一公开代码提交是 `a3c6d4c5bdecb215f8c8ebec554424de77ed193b`；公开仓使用项目化
  Tag `esp32-s3-device-monitor-v1.9.7`，避免与同仓其他项目的版本混淆。正式构建输入与该
  公开提交的 repository tree `7508d3f00fb436a0d9bfd716fc1fc152f2d0cf8a`、项目 tree
  `ac13d559aae8f74e92afbf585fe79c5da02aa324` 完全一致。

只要源码、构建配置、SDK 或会改变固件行为的资源发生变化，就应提升应用版本并创建新的不可移动
Tag。已经发布的 Tag、Release 资产和 SHA-256 记录不覆盖、不替换。

## 两种发布资产

| 资产 | 用途 | 使用方式 |
| --- | --- | --- |
| `esp32s3-tirtc-device-monitor-full-v1.9.7.bin` | 新设备、恢复出厂式重刷 | 使用 [Espressif ESP Tool](https://espressif.github.io/esptool-js/) 从 `0x0` 烧录；会覆盖 16 MB Flash 并清除本地 Wi-Fi、绑定和设置 |
| `esp32s3-tirtc-device-monitor-ota-v1.9.7.bin` | 已运行设备的 OTA 升级 | 交给设备 OTA 流程；它只是 app 镜像，不能写入 `0x0` |

本版本不单独生成或分发 `storage.bin`。完整镜像中未使用的 storage 分区保持擦除态，不能混入旧版本
文件。

## 发布一致性

每次发布同时固定并核对：

1. 公开 Git Tag 指向的唯一 commit。
2. `PROJECT_VER`、项目 README、`VERSION.md` 和 Release 标题中的版本。
3. 完整镜像、OTA app、`SHA256SUMS.txt` 与 `release-manifest.json` 中的文件名、大小和
   SHA-256。
4. OTA app 是同一正式构建产生的 app 镜像；完整镜像由该次构建的实际烧录地址合并生成。
5. 完整镜像逐段等于同次构建的 bootloader、partition table、OTA data 和 app；未使用区域
   保持 `0xFF`，镜像描述信息显示 `1.9.7`。

正式构建只执行一次，使用全新构建目录和 `--no-ccache`。本次完成 `1767/1767`，compiler
warning、error 和 ICE 均为 `0`；app 为 `7,613,216` bytes /
`49349c1e1755ae5265e3ddea6f57166ccf60b90cdca7d1a1a004539f4faf43ff`，完整镜像为
`16777216` bytes / `48a92849ef0149a4b099e981c4e109fcfa81afcbf99d379da1f7de61ad996bd1`。
这两个文件来自同一次构建；开发侧增量构建和 `1.9.6` 资产都没有复用。

静态哈希可以证明下载文件和发布清单一致。源码来源还需要正式构建记录进行绑定；目标板烧录、
启动、联网、OTA 切换和业务运行分别留证。

## 升级前检查

1. 核对当前设备版本和目标版本，不把同版本文件当成新的升级包。
2. 下载同一 Release 的 OTA app，并按 `SHA256SUMS.txt` 校验。
3. 保证设备供电和网络稳定，升级过程中不要断电。
4. 升级完成后核对设备显示或日志中的应用版本，再按需要验证联网和业务功能。

需要重置本地数据或 OTA 路径不可用时，改用 `full` 镜像从 `0x0` 完整烧录。
