# 源码来源

## 当前发布身份

- 项目：TiRTC ESP32-P4 AT ThingConnect 日志示例
- 版本：`0.1.0`
- 公开路径：`logging-examples/esp32-p4`
- 公开 Tag：`esp32-p4-logging-v0.1.0`
- 公共仓基线：`3749f7dc19cd58439b56d2fe0c99dd6f5c5926a4`

## 开发来源

开发来源是独立源码目录，没有可公开引用的 Git commit 或 Tag。公开发布不虚构开发提交；
`release-manifest.json` 会记录筛选前的开发快照 SHA-256、文件数量、公开 commit 和逐文件哈希。

公开整理只纳入：

- ESP32-P4 应用源码、TiRTC SDK 和预录测试媒体；
- C6/C61 同包兼容所需的 ESP-Hosted host/common 源码；
- 必要的构建配置、第三方许可证和开发者使用说明。

以下内容不进入公开源码：开发构建目录、生成的 `sdkconfig`、组件下载缓存、工具链、临时发布目录、
固件、串口原始记录、内部回归脚本、文本资料中的个人绝对路径，以及含 Wi-Fi 或设备身份的本地截图。

TiRTC 正式静态库原件含供应方构建路径字符串。公开筛选不改写二进制，以免破坏正式交付哈希和
链接契约；该文件按本页记录的 SHA-256 锁定身份，其中的路径不作为运行配置，也不含发布凭据。

## 依赖身份

- ESP-IDF：`5.5.4`
- TiRTC SDK：`2.3.0`
- TiRTC P4 静态库 SHA-256：
  `b0a38061b0c63ad0c556f73bb2ecc47c6fd84823b7524f774fe09916f577b4c6`
- ESP-Hosted：`2.12.11`，含 C6/C61 运行时兼容改动
- ESP-WiFi-Remote：`1.6.3`

ESP-Hosted 的公开改动限定为三个文件，便于升级时逐项复核：

- `Kconfig`：`77e55e73e9717c54a9b95fa3da9551977dc7f36aef4c85f42a6311d15ccb1cdf`
- `host/drivers/transport/transport_drv.c`：
  `fd99a601f45d126c1c3fdada554e80720ffd82c7d979e14478813d0a6254ebdb`
- `host/drivers/transport/sdio/sdio_drv.c`：
  `b8798cb5c1234fb8c9702e29f1acf0268ccfbe109281726378e9726e3e70447f`

这些改动用于 C6/C61 芯片身份兼容及 GPIO 54/13 复位脚回退，不改变公共 SDIO
`4-bit / 40 MHz / CLK18 / CMD19 / D0-D3 14-17` 配置。

BIN、ELF 和构建目录不提交到 Git。正式完整镜像只通过对应 GitHub Release 分发，并由 manifest
绑定到公开 commit、构建命令、SDK、配置和 SHA-256。
