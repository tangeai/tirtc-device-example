# TiRTC SDK Version

| Field | Value |
|---|---|
| Vendor | TiRTC / Espressif ESP32-S3 |
| Package label | v2.3.0 mini |
| Embedded BuildInfo version | v2.3.0-1baf7c95 |
| Embedded BuildInfo commit | 1baf7c95f3ca715c9367b9c998417f647934dc35 |
| Embedded TGTRP BuildInfo | tagv1.5.11-41c9a257 |
| Package metadata TGWebRTC ref | tag.v1.5.12 / 41c9a25768ffe265c07f17ef78a6439607b19364 |
| Package file | tirtc__espressif_esp32s3__esp-idf5.5.4-xtensa-esp14.2.0_20260121__v2.3.0__mini.tgz |
| Package SHA256 | 7e8cd974d7819bf0a9995347953520522c8ad25047c73e39457b4b53bc7bccbc |
| Library file | lib/esp32s3/libTiRTC.a |
| Library SHA256 | 43b06d1da421c7d24cc7fdb1385d600ecdffbfd2d3801f7faf0c540fb5cdbaa2 |
| Library size | 8079682 bytes |
| Target OS | FreeRTOS / ESP-IDF |
| ESP-IDF | 5.5.4 |
| Toolchain | xtensa-esp32s3-elf-gcc 14.2.0_20260121 |

## Build Contract

```text
CONFIG_FREERTOS_HZ=1000
CONFIG_FREERTOS_USE_TRACE_FACILITY=off
CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS=off
CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=off
STATIC_SEMAPHORE_T_SIZE=84
```

The project sdkconfig must keep the FreeRTOS contract aligned with this SDK package.
