# TiRTC ESP32-S3 Demo Release

## Release Scope

- Demo name: ESP32-S3 最小 TiRTC 集成示例
- Demo version: 1.1.1
- Release date: 2026-07-10
- SDK version: 2.2.1
- Target chip: ESP32-S3
- Target OS: FreeRTOS / ESP-IDF
- Toolchain: xtensa-esp32s3-elf-gcc-14.2.0_20260121
- ESP-IDF version used for validation: 5.5.4

This release provides an ESP32-S3 Wi-Fi demo for TiRTC integration. It includes Wi-Fi startup, time sync, TiRTC startup, BOOT-key connection toggling, inbound connection handling, subscription-driven inbound-side H264/PCMA test media sending, and active-side remote media subscription.

The firmware prints the demo version at boot:

```text
TiRTC ESP32-S3 Wi-Fi Link Demo v1.1.1 release=2026-07-10 sdk=2.2.1
```

以上是当前固件保留的旧启动日志标识，不作为公开项目名称或目录类别。

## SDK Package

The SDK is stored in `components/tirtc_sdk/`.

| File | Purpose |
|---|---|
| `include/tiRTC.h` | Public TiRTC API |
| `include/basedef.h` | Basic SDK definitions |
| `lib/libTiRTC.a` | ESP32-S3 static library |
| `VERSION.md` | SDK version and package metadata |

`libTiRTC.a` is the ESP32-S3 KCP single static library. It already contains the required WebRTC no-SCTP static library content, so customer projects only need this one TiRTC static library.

The SDK archive also contains the platform initialization entry `SA_platInit()`. The demo does not need an extra application-side platform init step before calling the public TiRTC APIs.

TiRTC 2.2.x requires both `TIRTC_OPT_DEVICE_SECRET_KEY` and `TIRTC_OPT_CLIENT_ID` to be set after `TiRtcInit()` and before `TiRtcStart()`. Bound devices use the cloud-issued device ID as `client_id`. `TiRtcStart()` receives only the device ID; the legacy `device_id,secret_key` license string is no longer used by this demo.

## Customer Configuration

Before building for a real device, update these placeholders:

- `main/app_config.h`: Wi-Fi SSID and password.
- `main/tirtc/tirtc_config.h`: device ID, device secret, remote device ID, remote device secret, and local token test credentials.

Do not publish real device IDs, Wi-Fi passwords, access IDs, or secret keys in the public demo. The checked-in project uses placeholders only.

## Build

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py build
```

Expected firmware output:

```text
build/tirtc_esp32s3_wifi_link_demo.bin
```

## Runtime Smoke Test

1. Boot the board and confirm Wi-Fi gets an IP address.
2. Confirm SNTP time sync finishes before TiRTC startup.
3. Confirm the log prints `TiRTC 版本: v2.2.1`, `device_secret_key length=...`, and `client_id length=...`.
4. Confirm `TiRTC 已上线`.
5. Use a remote client to call the device, or press BOOT to actively connect `TIRTC_REMOTE_DEVICE_ID`.
6. Confirm local H264 video and PCMA audio send logs continue after the connection is established.
7. Press BOOT again while connected and confirm both sides disconnect.

## Notes

- This demo can locally sign a test token for quick debugging. Production products should request tokens from the customer's own service.
- The default service endpoint is HTTP to match the current ESP32-S3 SDK validation path.
- Wi-Fi power save is disabled for link stability.
- NVS is not required by TiRTC itself in this demo.
- Public package contents are listed in `PACKAGE_CONTENTS.md`.
