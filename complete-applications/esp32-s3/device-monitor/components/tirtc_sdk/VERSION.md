# TiRTC SDK Version

| Field | Value |
|---|---|
| Vendor | TiRTC / Espressif ESP32-S3 |
| Package label | v2.3.0 mini + active-connect cleanup + HTTPS peer authentication + bounded receive fairness patches, public stripped archive |
| Source baseline | 1baf7c95f3ca715c9367b9c998417f647934dc35 |
| Active-connect fix commit | db7290f2404b15f2ab8567dd221853ca2ec054ab |
| HTTPS authentication fix commit | 13e34c3e3e3dc6776be4713b5c1e3c17bd282766 |
| Embedded BuildInfo version | v2.3.0-db7290f |
| Embedded BuildInfo commit | db7290f2404b15f2ab8567dd221853ca2ec054ab |
| Embedded TGTRP BuildInfo | tagv1.5.11-41c9a257 |
| Package metadata TGWebRTC ref | tag.v1.5.12 / 41c9a25768ffe265c07f17ef78a6439607b19364 |
| Base package file | tirtc__espressif_esp32s3__esp-idf5.5.4-xtensa-esp14.2.0_20260121__v2.3.0__mini.tgz |
| Base package SHA256 | 7e8cd974d7819bf0a9995347953520522c8ad25047c73e39457b4b53bc7bccbc |
| Library file | lib/esp32s3/libTiRTC.a |
| Library SHA256 | f5eca6c3212a34d00ce5706e864f8112adc8e6b20b92bc175435299caf4b082b |
| Library size | 2127226 bytes |
| Archive hygiene | GNU `xtensa-esp32s3-elf-strip --strip-debug` |
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

## Receive Fairness Contract

- One ICE UDP callback handles at most 8 datagrams or 4 ms before returning to
  the RTC scheduler. It yields for 1 ms only when that budget is actually hit.
- One TGTRP audio-jitter poll releases at most 4 ordered work items. A remaining
  ready backlog schedules a 1 ms continuation instead of draining every queued
  frame synchronously in one socket callback.
- Missing-frame recovery remains ordered and loss accounting is unchanged. The
  limit only splits a recovery burst across scheduler turns; normal one-frame
  traffic receives no fixed delay.

## Public Archive Hygiene

The public static archive has debug information removed after the functional
SDK patches are applied. Archive member order and global defined/undefined
symbols are unchanged. The allocated section images of the patched `tiRTC.o`
and `httpclt.o` are byte-identical before and after stripping, while personal
build paths are absent from the resulting archive.

## HTTPS Trust Contract

The ESP32-S3 SDK HTTP transport uses `MBEDTLS_SSL_VERIFY_REQUIRED`, validates the
requested hostname, and attaches the ESP-IDF certificate bundle. TLS handshake,
hostname, CA-chain, or certificate verification failures return the existing
`CONN_E_SSL` failure path; callers must not downgrade the request to plaintext.
