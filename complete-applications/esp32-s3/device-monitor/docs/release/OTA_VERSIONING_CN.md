# OTA 版本规则

固件 OTA 版本号使用 `A.B.C` 三段数字，例如 `0.7.5`。该版本写在根目录 `CMakeLists.txt` 的 `PROJECT_VER` 中，设备端 OTA 会从 ESP app description 读取它，并在检查更新时作为 `version` 参数传给 OTA 服务端。

## 字段含义

- `A`：兼容域。用于标记分区、资源布局、协议或本地数据格式是否发生破坏性变化。
- `B`：功能版本。用于同一兼容域下的主要功能迭代。
- `C`：补丁版本。用于同一功能版本下的修复、发布候选或小范围增强。

## 当前版本

当前版本：`0.7.5`

含义：第 `0` 个兼容域下的第 `7` 次功能收口版本，补丁号为 `5`。

## 发布要求

- 每次发布 OTA 前，先更新根目录 `CMakeLists.txt` 中的 `PROJECT_VER`。
- 云端 manifest 中的 `version`、`firmware.size` 和 `firmware.sha256` 需要与发布固件保持一致。
- OTA 只更新 app 分区。如果同时改动图片、SPIFFS、bootloader 或分区表，需要发布完整烧录包。
- 对外发布资产只保留当前发布版本，避免用户拿错非当前发布包。
