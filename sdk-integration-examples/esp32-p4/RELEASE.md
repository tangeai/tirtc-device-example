# ESP32-P4 最小 TiRTC 集成示例 Release

## Release Scope

| Field | Value |
|---|---|
| Demo name | ESP32-P4 最小 TiRTC 集成示例 |
| Demo version | 1.0.2 |
| Release date | 2026-06-24 |
| TiRTC SDK version | 0.1.4 |
| Target chip | ESP32-P4 |
| Target OS | FreeRTOS / ESP-IDF |
| ESP-IDF version | 5.5.4 |
| Toolchain | riscv32-esp-elf-gcc-14.2.0_20260121 |

This release provides an ESP32-P4 Wi-Fi sample for TiRTC integration. It includes Wi-Fi startup, SNTP time sync, TiRTC startup, BOOT-key connection toggling, inbound connection handling, local H264/PCMA media sending, and remote media subscribe/unsubscribe controls.

The firmware prints the demo version at boot:

```text
TiRTC ESP32-P4 Wi-Fi Link Demo v1.0.2 release=2026-06-24 sdk=0.1.4
```

以上是当前固件保留的旧启动日志标识，不作为公开项目名称或目录类别。

## SDK Package

The SDK is stored in `components/tirtc_sdk/`.

| File | Purpose |
|---|---|
| `include/tiRTC.h` | Public TiRTC API |
| `include/basedef.h` | Basic SDK definitions |
| `lib/esp32p4/libTiRTC.a` | ESP32-P4 static library |
| `VERSION.md` | SDK and sample metadata |

`libTiRTC.a` is linked by `components/tirtc_sdk/CMakeLists.txt`; no extra linker step is required.

This package uses a KCP single-library SDK. The archive already contains the
TiRTC wrapper objects and the P4 `libwebrtc_nosctp.a` objects, so integration
projects should link `libTiRTC.a` only.

## Configuration

Update these files before building for a target device:

- `main/app_config.h`: Wi-Fi SSID and password.
- `main/tirtc/tirtc_config.h`: device ID, device secret, remote device ID, remote device secret, and token signing credentials for local validation.
- `TIRTC_AUTO_PUSH_LOCAL_MEDIA_ON_CONNECT`: default `1` pushes local H264/PCMA after a connection is established; set to `0` to wait for remote subscription.

Do not publish real device IDs, Wi-Fi passwords, access IDs, or secret keys in a public package. The checked-in project uses placeholders.

## Build

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py build
```

Expected firmware output:

```text
build/tirtc_esp32p4_wifi_link_demo.bin
firmware/tirtc_esp32p4_wifi_link_demo.bin
```

## Runtime Smoke Test

1. Boot the board and confirm Wi-Fi gets an IP address.
2. Confirm SNTP time sync finishes before TiRTC startup.
3. Confirm the log prints `TiRTC 版本: 0.1.4`.
4. Confirm the log prints `TiRTC 已上线`.
5. Use a remote device to call in, or press BOOT to actively connect `TIRTC_REMOTE_DEVICE_ID`.
6. Confirm H264 video and PCMA audio send logs continue after the connection is established.
7. Subscribe/unsubscribe video or audio streams and confirm the matching media task follows the control command.
8. Re-subscribe and confirm media sending continues.
9. Press BOOT again while connected and confirm both sides disconnect.

## Notes

- The local token signing path is for quick validation only. Production firmware should request tokens from a business service.
- The default TiRTC service endpoint is `http://ep-tirtc.tange365.com`.
- Wi-Fi power save is disabled for link stability.
- NVS is not required by TiRTC itself in this sample.
- The package uses placeholders for Wi-Fi, device IDs, access IDs, and secret keys.
