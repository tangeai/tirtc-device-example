# TiRTC SDK Version

- SDK version: 2.2.0
- Channel: localfix delivery
- Target chips: ESP32-S3, ESP32-P4
- Chip vendor: Espressif
- Target OS: FreeRTOS / ESP-IDF
- ESP-IDF: 5.5.4
- S3 toolchain: xtensa-esp32s3-elf-gcc-14.2.0_20260121
- P4 toolchain: riscv32-esp-elf-gcc-14.2.0_20260121
- P2P stack: kcp / noSCTP merged into libTiRTC.a
- HTTP mode: built with nossl=y, CONFIG_SSL_SUPPORT=0
- Source repo: https://gitlab.tange-ai.com/tirtc-nexus/tirtc-nano.git
- Upstream base: 2ca19cf1bb401104e6892cceef273696d431628d
- Local delivery patch: 6c8f02580761e6f3991975e36b14aaf8b808bf05
- Local branch: local/esp32-s3-p4-delivery-patches
- CONFIG_FREERTOS_HZ: 1000
- CONFIG_FREERTOS_USE_TRACE_FACILITY: n for S3 demo build config
- CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS: n for S3 demo build config

## Libraries

| Target | Size | MD5 | SHA256 |
| --- | ---: | --- | --- |
| ESP32-S3 | 4908580 | d4433a250b988902ebdf0687020be98b | f258f6fa48d6fd167d88cd5b09210828c663ceb451dea10e6f33a16c8407e037 |
| ESP32-P4 | 2943636 | 23661a4ee27debc667f904f7118dd270 | 97d4285474e301f5e2846c837241632b9d2bca09a64d96d2ec385c6c68c40ec6 |
