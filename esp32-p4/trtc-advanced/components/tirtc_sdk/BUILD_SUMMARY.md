# Build Summary

## Source

Upstream master base:

    2ca19cf Merge branch 'work' into 'master'

Local delivery patch:

    6c8f025 local: keep ESP32 S3/P4 delivery fixes

This package is not a pure upstream-master package. It is built from upstream master plus our local delivery patch commit 6c8f025.

## Local Fixes Included

1. platform/plat_freertos.c

   Use ESP-IDF xTaskCreateWithCaps/vTaskDeleteWithCaps on ESP32-S3 and ESP32-P4. The task stack uses PSRAM-capable allocation through IDF, while task control ownership stays inside IDF. This avoids the previous static-task self TCB free risk.

2. esp32p4.mak

   Use the ESP-IDF 5.5.4 P4 register include path:

       soc/esp32p4/register/hw_ver1

   Without this path, P4 build fails with soc/reg_base.h not found.

## Build Commands

ESP32-S3:

~~~bash
make -C TiRTC SHELL=/bin/bash ARCH=esp32s3 \
  IDFPATH=/mnt/c/esp/v5.5.4/esp-idf \
  SDKCONFIG_PATH=/mnt/c/Users/61852/Desktop/tirtc_esp32s3_wifi_link_demo/build/config \
  CROSS=/home/wty/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin/xtensa-esp32s3-elf- \
  p2p=kcp nossl=y
~~~

ESP32-P4:

~~~bash
make -C TiRTC SHELL=/bin/bash ARCH=esp32p4 \
  IDFPATH=/mnt/c/esp/v5.5.4/esp-idf \
  SDKCONFIG_PATH=/mnt/c/Users/61852/Desktop/demo_p4/build/config \
  CROSS=/home/wty/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin/riscv32-esp-elf- \
  p2p=kcp nossl=y
~~~

## Result

- ESP32-S3 build passed.
- ESP32-P4 build passed.
- Temporary diagnostic strings from earlier debug builds were not found in the libraries.
