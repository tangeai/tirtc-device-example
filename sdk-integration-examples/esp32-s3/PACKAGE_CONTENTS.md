# TiRTC ESP32-S3 Demo Package Contents

This handoff package contains the ESP32-S3 TiRTC Wi-Fi demo source, the TiRTC SDK component, test media, release notes, and prebuilt firmware images.

## Included

| Path | Purpose |
|---|---|
| `components/tirtc_sdk/` | TiRTC public headers and ESP32-S3 KCP single static library |
| `main/` | Demo application source code |
| `main/tirtc/test_assets/` | H264 and PCMA media files packed into SPIFFS |
| `firmware/` | Prebuilt bootloader, partition table, app image, storage image, and flash args |
| `README.md` | Integration overview and build guide |
| `TEST_GUIDE.md` | Bring-up and validation steps |
| `RELEASE.md` | Release scope and runtime smoke test |
| `VERSION.md` | Demo and SDK version summary |

## Not Included

- `.git/`, `.agents/`, `.vscode/`, `.clangd`
- `build/` and other temporary build logs
- `sdk_source/` local SDK source snapshots
- Real Wi-Fi SSID/password
- Real device IDs, access IDs, token secret keys, or device secret keys
- Historical zip packages

## Notes

The source uses placeholders such as `your_wifi_ssid`, `your_device_id`, and `your_token_secret_key`. Replace them in `main/app_config.h` and `main/tirtc/tirtc_config.h` before building for a real device.
