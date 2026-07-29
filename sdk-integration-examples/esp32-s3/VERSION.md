# TiRTC ESP32-S3 Demo Version

| Field | Value |
|---|---|
| 示例工程 | TiRTC ESP32-S3 Wi-Fi Link Demo |
| 示例版本 | 1.2.0 |
| 候选构建日期 | 2026-07-30 |
| TiRTC SDK 版本 | 2.2.1 |
| TiRTC SDK commit | `3a33bf4ae51b3ab9eb246648adb274d0fae32ebf` |
| TiRTC SDK BuildInfo | `{"tirtc":"v2.2.1","commit":"3a33bf4ae51b","tgtrp":"v1.4.6"}` |
| TiRTC SDK MD5 | `f417e769499b8b46b711387013d93125` |
| TiRTC SDK SHA256 | `dc9f869194584fd43fe091f887342d170a1422e657ce1fc2a144c90a3ade1f8e` |
| TiRTC SDK 文件大小 | 4,908,112 bytes |
| TiRTC SDK 形态 | KCP / noSCTP single static library，HTTP-only |
| 目标芯片 | ESP32-S3 |
| 目标 OS | FreeRTOS / ESP-IDF |
| ESP-IDF | 5.5.4 |
| 工具链 | `xtensa-esp32s3-elf-gcc-14.2.0_20260121` |
| FreeRTOS Tick | 1000 Hz |
| `StaticSemaphore_t` | 84 bytes |
| SDK PSRAM 变化 | 2.2.1 使用 ESP-IDF 扩展任务接口管理 PSRAM 线程栈 |

## Build Contract

```text
CONFIG_FREERTOS_HZ=1000
CONFIG_FREERTOS_USE_TRACE_FACILITY=n
CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS=n
CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=n
CONFIG_LWIP_TCPIP_TASK_STACK_SIZE=4096
```

正式组件的机器可读来源信息位于：

- `components/tirtc_sdk/manifest/build-info.json`
- `components/tirtc_sdk/manifest/build-contract.env`
- `components/tirtc_sdk/manifest/symbols-defined.txt`
- `components/tirtc_sdk/manifest/symbols-undefined-by-object.txt`

本文件描述源码和 SDK 身份。每次实际构建的固件 SHA256 应在对应验证记录中单独
保存，不能用历史 `firmware/` 文件代替当前源码构建证明。
