# TiRTC ESP32-P4 Device Monitor Demo

## Overview

This project runs the TiRTC device monitor demo on the Waveshare
ESP32-P4-WIFI6-Touch-LCD-3.5 board.

The P4 build keeps the same application feature set as the ESP32-S3 monitor
demo while replacing the board-specific layers for the ESP32-P4 hardware.

## Board

- Target: `esp32p4`
- ESP-IDF: `5.5.4`
- Board: Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5
- Flash: 16 MB
- PSRAM: 32 MB
- Wi-Fi: ESP32-C6H8 over ESP-Hosted SDIO
- Display: ST7796, physical `320 x 480`, landscape UI viewport `480 x 320`
- Touch: FT5x06 compatible controller through the board BSP
- Audio codec: ES8311, 16 kHz, 16-bit, mono application path
- Camera: OV5647 through ESP Video / MIPI-CSI

## Feature Set

- Home dashboard and LVGL application navigation
- Wi-Fi configuration and connection state display
- TiRTC configuration, startup, online state, connect, disconnect, subscribe,
  unsubscribe, and key-frame request flow
- Local media sender control for audio and video test streams
- RTC media bridge for received audio/video state reporting
- Device binding and online state service
- WeChat VoIP service flow
- AI Chat service flow
- OTA check, download, verify, and reboot-to-apply flow
- QR scan entry backed by the P4 camera driver
- Device state reporting, including boot button and CPU usage

## Layering

- `hardware/` owns board pinout, power control, and BSP entry points.
- `drivers/` owns display, touch input, camera, audio, and virtual media
  sources.
- `connectivity/` owns network readiness and Wi-Fi state.
- `protocols/tirtc/` owns TiRTC runtime, callbacks, session state, and media
  send/receive gates.
- `protocols/rtc/` is a thin facade used by the application layer.
- `services/` owns device binding, online state, media bridge, AI Chat,
  WeChat VoIP, sender test, QR scan, and OTA flows.
- `application/` owns app lifecycle and resource acquisition/release.
- `ui/` renders state and routes user actions.

## Build

Use ESP-IDF PowerShell for ESP-IDF `5.5.4`:

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py set-target esp32p4
idf.py build
```

The build generates firmware outputs under `build/` and copies the main
firmware artifacts into `firmware/`.

## Flash

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py -p COM14 flash
```

Use the serial port that appears for the local board.

## Partition Layout

The application requires NVS for persisted device identity and configuration.

- `nvs`: `0x9000`, size `0x4000`
- `otadata`: `0xD000`, size `0x2000`
- `phy_init`: `0xF000`, size `0x1000`
- `ota_0`: `0x10000`, size `0x730000`
- `ota_1`: `0x740000`, size `0x730000`
- `coredump`: `0xE70000`, size `0x10000`
- `storage`: `0xE80000`, size `0x180000`

## Runtime Configuration

Set product credentials and server values in the existing project configuration
headers or NVS-backed UI flows before validating network services.

Important defaults:

- FreeRTOS tick rate: `CONFIG_FREERTOS_HZ=1000`
- TiRTC SDK component: `components/tirtc_sdk`
- TiRTC P4 library: `components/tirtc_sdk/lib/esp32p4/libTiRTC.a`
- Auto local media push macro:
  `TIRTC_AUTO_PUSH_LOCAL_MEDIA_ON_CONNECT`
- LAN screen debug server:
  `CONFIG_APP_DEBUG_SCREEN_SERVER_ENABLE`
  exposes `http://<device-ip>:8080/` after Wi-Fi obtains an IP address.

## Validation Checklist

1. Build completes for `esp32p4`.
2. Flash completes with verified hashes.
3. Boot log reaches `system ready: ESP32-P4 TiRTC dashboard`.
4. NVS initializes successfully.
5. LVGL display initializes and backlight turns on.
6. Hosted Wi-Fi starts and can enter scan or connected state.
7. Wi-Fi credentials can be configured and the board obtains an IP address.
8. SNTP sync succeeds after network connection.
9. TiRTC initializes, starts, and reaches online state.
10. Connect/disconnect works through the TiRTC UI flow.
11. Subscribe, unsubscribe, and key-frame request controls reach the TiRTC
    session layer.
12. Local audio/video sender waits for a valid connection and then sends through
    the TiRTC media gate.
13. Camera open/capture/release works through the P4 camera driver.
14. Audio capture/playback works through the P4 audio driver.
15. OTA manifest check and download path use `chip=p4`.

## Notes

Keep application behavior aligned with the S3 monitor demo, but keep hardware
drivers and board initialization P4-specific. Do not move TiRTC handle ownership
into UI or driver code; lifecycle control belongs in the application and
protocol layers.
