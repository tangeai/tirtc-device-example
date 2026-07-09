# TiRTC SDK Version

| Field | Value |
|---|---|
| Vendor | TiRTC / Espressif ESP32-S3 |
| Package label | 1df9e04 / 20260709-165841 mini |
| Embedded BuildInfo version | v2.2.0 |
| Embedded BuildInfo commit | 1df9e045a9dc |
| Embedded BuildInfo | {"tirtc":"v2.2.0","commit":"1df9e045a9dc","tgtrp":"v1.4.6"} |
| Package file | tirtc__espressif_esp32s3__esp-idf5.5.4-xtensa-esp14.2.0_20260121__1df9e04__20260709-165841__mini.tgz |
| Package SHA256 | b9c50de4c3fc7225d24766d06c02c6d1fe068395179ced5d04ee90fa7c4474a8 |
| Library file | lib/esp32s3/libTiRTC.a |
| Library MD5 | f1a8e36168ad779f5e6a78275296fb67 |
| Library SHA256 | 0686e5d7f5bfea18b7b3ee6ae1701061b3afcfb0ef2030642ebebab88af0413d |
| Library size | 4723856 bytes |
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
