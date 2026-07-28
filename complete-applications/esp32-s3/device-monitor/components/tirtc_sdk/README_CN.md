# TiRTC SDK 组件

这个目录存放 ESP32-S3 固件直接链接的 TiRTC SDK，结构对齐官方示例 `tirtc_esp32s3_wifi_link_demo/components/tirtc_sdk`。

## 文件结构

- `include/tiRTC.h`: SDK 主头文件，所有 TiRTC API 以这里为准。
- `include/basedef.h`: `tiRTC.h` 依赖的基础类型定义。
- `include/tirtc_voip_cmdw.h`: 官方 SDK 附带的命令字辅助头文件。
- `lib/esp32s3/libTiRTC.a`: ESP32-S3 静态库。
- `VERSION.md`: 当前 SDK 版本和库文件信息。
- `CMakeLists.txt`: ESP-IDF 组件定义和静态库链接规则。

## 维护规则

更新 SDK 时，从官方示例目录整体同步 `include/`、`lib/esp32s3/libTiRTC.a` 和 `VERSION.md`。不要恢复旧的 `tirtc_compat.c` 或 `*_sndv` 临时补丁。
