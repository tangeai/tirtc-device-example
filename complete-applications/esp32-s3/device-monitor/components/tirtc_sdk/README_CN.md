# TiRTC SDK 组件

这个目录存放 ESP32-S3 固件直接链接的 TiRTC SDK，结构对齐官方示例 `tirtc_esp32s3_wifi_link_demo/components/tirtc_sdk`。

## 文件结构

- `include/tiRTC.h`: SDK 主头文件，所有 TiRTC API 以这里为准。
- `include/basedef.h`: `tiRTC.h` 依赖的基础类型定义。
- `include/tgtrp.h`: TGTRP 传输接口、链路模式和网络质量统计定义。
- `include/tiRTC_stat.h`: TiRTC 连接时延统计接口定义。
- `include/tirtc_voip_cmdw.h`: 官方 SDK 附带的命令字辅助头文件。
- `lib/esp32s3/libTiRTC.a`: ESP32-S3 静态库。
- `VERSION.md`: 当前 SDK 版本和库文件信息。
- `CMakeLists.txt`: ESP-IDF 组件定义和静态库链接规则。

## 维护规则

更新 SDK 时，从同一个官方发布包整体同步 `include/`、`lib/esp32s3/libTiRTC.a` 和 `manifest/build-contract.env`，并按包元数据更新 `VERSION.md`。不要混用不同版本的头文件和静态库，也不要恢复旧的 `tirtc_compat.c` 或 `*_sndv` 临时补丁。
