# TiRTC SDK Version

| Item | Value |
| --- | --- |
| Package channel | ESP32-P4 test package |
| TiRTC version | 2.3.0 |
| Target | ESP32-P4 / FreeRTOS / ESP-IDF |
| ESP-IDF | 5.5.4 |
| Toolchain | riscv32-esp-elf-gcc-14.2.0_20260121 |
| Nano branch | origin/tgmp |
| Nano baseline | aaad3da251bac90e0642b51b3279a1f40ca9fa9a |
| Nano compatibility patch | HTTP DNS disable backport from fde4f1c58d2dc28f3d3d04e25dd49bc3a399fea6 |
| tgwebrtc commit | e39114731ad488c88573d16f0855a1326d97c989 |
| TGTRP interface | tag v1.5.10 |
| Transport | KCP / noSCTP / noDTLS |
| Build date | 2026-08-01 |

## Build Contract

- `CONFIG_FREERTOS_HZ=1000`
- FreeRTOS trace facility disabled
- FreeRTOS stats formatting functions disabled
- FreeRTOS runtime stats disabled
- `sizeof(StaticSemaphore_t)=84`
- `CONFIG_LWIP_MAX_SOCKETS=10`
- `libwebrtc_nosctp.a` is already bundled into `libTiRTC.a`
- The custom HTTP DNS cache is disabled. `/v1/connect` uses the platform DNS
  resolver, avoiding a recursive lock when a cached entry expires.
- Public 2.3.0 headers and the TGMP callback contract are unchanged.

The upstream SDK validation project used `CONFIG_ESP_HOST_WIFI_ENABLED=y`.
This Waveshare ESP32-P4 board uses an ESP32-C6 over ESP-Hosted SDIO instead, so
the application keeps `CONFIG_ESP_HOST_WIFI_ENABLED=n`,
`CONFIG_ESP_WIFI_REMOTE_ENABLED=y`, and
`CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED=y`. These IDF modes are mutually
exclusive; enabling Host Wi-Fi would disable the board's working Wi-Fi Remote
transport. The TiRTC socket interface remains provided by lwIP through
ESP-WiFi-Remote.

## Integration Contract

- Set `TIRTC_OPT_DEVICE_SECRET_KEY` and a stable physical
  `TIRTC_OPT_CLIENT_ID` before `TiRtcStart()`.
- Register `TiRtcConnSetVideoBitrateParams()` after a connection is accepted
  and before video transmission starts.
- Treat `on_video_bitrate_required()` as an SDK-thread callback: post the
  absolute target bitrate to an application worker and return immediately.
- Keep `TIRTC_OPT_TGTRP_POLL_TIMEOUT` at its default `1 ms` for initial
  integration.
- This package passed static and final-link checks upstream. Device-side TGMP
  behavior still requires runtime verification.

The auxiliary `lib/esp32s3/libTiRTC.a` file is not linked by this ESP32-P4
component and is not part of the 2.3.0 package contract.
