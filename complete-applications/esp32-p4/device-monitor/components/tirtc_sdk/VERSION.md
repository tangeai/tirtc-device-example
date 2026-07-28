# TiRTC SDK Version

- SDK version: 2.2.0
- Package channel: ESP32 device delivery
- Target chips: ESP32-S3, ESP32-P4
- Chip vendor: Espressif
- Target OS: FreeRTOS / ESP-IDF
- ESP-IDF: 5.5.4
- S3 toolchain: xtensa-esp32s3-elf-gcc-14.2.0_20260121
- P4 toolchain: riscv32-esp-elf-gcc-14.2.0_20260121
- P2P stack: KCP / noSCTP
- HTTP mode: `nossl=y`, `CONFIG_SSL_SUPPORT=0`
- FreeRTOS tick contract: 1000 Hz

## Libraries

| Target | Size | MD5 | SHA256 |
| --- | ---: | --- | --- |
| ESP32-S3 | 4908580 | d4433a250b988902ebdf0687020be98b | f258f6fa48d6fd167d88cd5b09210828c663ceb451dea10e6f33a16c8407e037 |
| ESP32-P4 | 2943636 | 23661a4ee27debc667f904f7118dd270 | 97d4285474e301f5e2846c837241632b9d2bca09a64d96d2ec385c6c68c40ec6 |

## Integration Contract

- Set `TIRTC_OPT_DEVICE_SECRET_KEY` before calling `TiRtcStart()`.
- Use a stable physical identifier, such as the base MAC, for `TIRTC_OPT_CLIENT_ID`.
- Keep the headers, target static library, this version file and `SHA256SUMS.txt` from the same package.
