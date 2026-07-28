# TiRTC SDK Component Version

| Field | Value |
|---|---|
| Chip vendor | Espressif Systems / Espressif |
| Chip model | ESP32-P4 |
| SDK version | 0.1.4 |
| SDK source | release package built from tag `v0.1.4`, commit `e2c7020` |
| Library file | lib/esp32p4/libTiRTC.a |
| MD5 | e6d84176e3962f5374177de5112378e9 |
| SHA256 | da0a165c062cbb3f2e176aea2f517e8ecd5ff116f10fb5e9f8c4db729c1afe88 |
| File size | 2,929,690 bytes |
| Target OS | FreeRTOS / ESP-IDF |
| FreeRTOS tick | 1000 Hz |
| ESP-IDF | 5.5.4 |
| Toolchain | riscv32-esp-elf-gcc-14.2.0_20260121 |
| Package status | Formal release package archived on 2026-07-01; KCP/noSCTP objects are included in the single library |
| Header note | Candidate `tiRTC.h` is the package header plus `#include <stddef.h>` for the public `size_t` declaration |
| Init note | FreeRTOS platform init is guarded and task startup waits until the thread handle is registered |
