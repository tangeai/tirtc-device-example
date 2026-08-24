# Firmware Layering

This project keeps hardware, network, protocol, service, application, and UI
responsibilities separated.

## Dependency Direction

- `ui/` renders pages and emits user actions. It does not initialize hardware.
- `application/` owns app lifecycle, snapshots, and policy decisions.
- `services/` owns feature workflows such as AI Chat, WeChat VoIP, OTA,
  media sink, sender test, and QR scanning.
- `debug/serial_net_cli/` exposes bounded diagnostic commands. It observes
  network, RTC, audio, AEC, and call state without owning those workflows.
- `protocols/rtc/` is the application-facing RTC transport boundary.
- `protocols/tirtc/` contains the TiRTC-specific session implementation.
- `connectivity/` owns network state and hides the Wi-Fi driver details.
- `drivers/` owns hardware devices: display/touch, microphone/speaker, the QR
  scanner camera, virtual audio test sources, and the PCM audio backend.
- `hardware/` owns board pin maps and shared hardware resource assignment.

Lower layers must not include `application/` or `ui/`.

## Driver Boundaries

- Display and touch are initialized by `drivers/display/display_driver.*`.
  UI receives LVGL handles only.
- Microphone and speaker are exposed through
  `drivers/audio/audio_device.*`; the lower `audio.*` implementation remains
  the hardware codec/I2S PCM backend.
- Camera capture is exposed through `drivers/camera/camera_driver.*`.
  QR scanning is its only product consumer. `main/product_capabilities.h`
  prevents RTC services from treating the camera as a video source.
- Shared LEDC resources are assigned in `hardware/hardware_board_config.h`.

## Network Boundary

Application code uses `network.*` only. Wi-Fi details stay inside
`connectivity/wifi.*` and are converted by `connectivity/network.c`.

## App Lifecycle Resource Boundary

`application/app.c` owns the app resource table. Opening an app acquires the
declared resources, then starts app-specific services. Returning home first
stops app-specific services, then releases the declared resources in reverse:
QR camera, audio, RTC, and network when configured. Switching from one app to
another goes through the same home zero-resource baseline before acquiring the
new app's resources, so hardware, network, and TiRTC state do not leak across
apps. Boot initialization keeps only global UI, OTA, callback, and policy
state; Wi-Fi, TiRTC runtime, audio devices, and camera resources are not kept
alive just because the home screen is visible.

The current product enables RTC audio and disables RTC video. Web IPC, AI
Chat, WeChat VoIP, and Device Call must all honor this capability at their
application and protocol boundaries. Unsupported video requests are rejected;
they are not silently downgraded.

## RTC Boundary

Application and generic debug code use `rtc_transport.*`. TiRTC-specific
connection handles, command words, WHIP requests, and service requests remain
inside `protocols/tirtc/` and the protocol-specific AI Chat / WeChat VoIP
services that require those contracts.

The serial CLI calls public status and diagnostic interfaces only. It must not
reach into private protocol state, bypass authentication, or turn a failed
workflow into a successful response.
