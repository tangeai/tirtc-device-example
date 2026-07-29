# ESP32-S3 TiRTC SDK 0.1.4 -> 2.2.1 Upgrade

## Authority

- Consumer repo: this `tirtc_esp32s3_wifi_link_demo` checkout
- Formal delivery: `ESP32-S3_v2.2.1_20260729`
- SDK source commit:
  `3a33bf4ae51b3ab9eb246648adb274d0fae32ebf`

## Package Closure

The repository previously contained a mixed state: 2.2.0 documentation, 2.2.1
headers, a non-formal 2.2.1 library variant, and hashes matching neither file.
The entire formal ESP-IDF component is now used as one unit.

```text
libTiRTC.a size:   4,908,112 bytes
MD5:               f417e769499b8b46b711387013d93125
SHA256:            dc9f869194584fd43fe091f887342d170a1422e657ce1fc2a144c90a3ade1f8e
BuildInfo:         {"tirtc":"v2.2.1","commit":"3a33bf4ae51b","tgtrp":"v1.4.6"}
```

The component also carries `build-info.json`, `build-contract.env`, and symbol
manifests. The obsolete flat `lib/libTiRTC.a` path is no longer used.

## API Migration

The formal 2.2.1 public headers match the 2.2.x headers already present in the
working tree, so there is no new or removed C API for this patch upgrade.

The active 2.2.x lifecycle is:

```text
MAX_SEND_BUFFER before Init
-> TiRtcInit
-> DEVICE_SECRET_KEY
-> CLIENT_ID
-> optional endpoint
-> TiRtcStart(device_id, callbacks)
-> wait for SYS_STARTED
```

String option lengths exclude the trailing NUL. The demo validates the
`client_id` 1..64 printable-ASCII contract and keeps the callback table static.

## 2.2.1 Adaptation

- Prints `TiRtcGetVersion()` and `TiRtcGetBuildInfo()`.
- Pins `MAX_CONNECTIONS=1` because the minimal demo owns one global connection.
- Sets Wi-Fi network type explicitly.
- Enables connection parameter caching and retries with a newly issued token
  when the cache returns `TIRTC_E_CACHE_EXPIRED`.
- Reserves and retires the token fingerprint and CRC in two rotating SPIFFS
  slots before calling `TiRtcConnect()`. The device rejects values found in
  either valid slot and retains the previous valid record across an interrupted
  write. This is a development guard for the two most recent records;
  server-side one-time validation remains authoritative.
- Handles `TIRTC_EVENT_ACCESS_HIJACKING`.
- Uses normal SDK log level 5 instead of enabling low-level WebRTC logging.
- Stops audio and video independently when the corresponding stream is
  unsubscribed.
- Drops non-key video frames after `TIRTC_E_BUSY` and restores a keyframe.
- Calls `TiRtcUninit()` only after `TIRTC_EVENT_SYS_STOPPED`.

The 2.2.1 S3 release change itself is inside the SDK: ESP-IDF extended task APIs
manage PSRAM thread stacks. Application code does not create a replacement API
for this behavior; board-side loop and memory testing must validate it.

## Security Migration

The old demo generated tokens inside firmware and therefore required the remote
device secret plus application signing credentials. That path has been removed.

Tracked headers contain placeholders only. Local Wi-Fi and device credentials
live in ignored `main/local_config.h`. Active connection tokens are short-lived,
one-time opaque values issued by a business backend or TiRTC DevTools.

Application `AccessKeyId/SecretKeyId` must remain server-side and must never appear
in device source, firmware, or logs.

## Build Contract

```text
ESP-IDF 5.5.4
xtensa-esp32s3-elf-gcc-14.2.0_20260121
CONFIG_FREERTOS_HZ=1000
CONFIG_FREERTOS_USE_TRACE_FACILITY=n
CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS=n
CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=n
CONFIG_LWIP_TCPIP_TASK_STACK_SIZE=4096
KCP / noSCTP
HTTP-only
```

Fresh `reconfigure build` evidence, firmware hashes, flash logs, real playback,
stream-specific unsubscribe/re-subscribe, and repeated connection memory trends
must be recorded separately.
