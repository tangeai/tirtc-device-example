# ESP32-P4 Media Architecture

本文档说明 ESP32-P4 工程中摄像头、音频、TiRTC、UI 的分层和运行策略。目标是保证业务入口清晰、资源所有权明确、异常时可恢复。

## Layer Ownership

| Layer | Responsibility |
| --- | --- |
| `drivers` | Camera, display, touch, audio codec, DMA buffer and hardware lifecycle. |
| `media` | Camera H264 pipeline, media profile, weak-network policy and runtime metrics. |
| `services` | Feature-facing service APIs, such as QR scan, RTC bridge, device binding, AI Chat and VoIP. |
| `protocols/tirtc` | TiRTC SDK lifecycle, connection handle, stream state and packet submission. |
| `application` | App entry/exit orchestration and resource acquire/release decisions. |
| `ui` | Visual state and user actions only. |

## Camera Pipeline

`main/media/camera_pipeline.c` is the owner of live RTC camera capture.

- RTC camera uplink uses OV5647 YUV420 frames as the H264 encoder input.
- The hot path is `camera_driver -> camera_pipeline -> ESP32-P4 H264 encoder -> tirtc_session`.
- RTC video does not render a local preview. This keeps display refresh from competing with H264 encoding and ESP-Hosted Wi-Fi.
- QR scanning owns the camera only while the scan page is active and releases it before RTC video starts.
- H264 input/output buffers that may live in PSRAM are synchronized with `esp_cache_msync` around encoder submission and output reads.
- The first valid upstream frame must be a key frame before the TiRTC video stream is considered started.

Public wrappers remain stable:

- `camera_pipeline_set_rtc_video_enabled()` controls the RTC upstream camera source.
- `camera_pipeline_set_rtc_video_sink()` binds the encoded H264 sink.
- `media_governor_set_rtc_video_config()` adjusts width, height, FPS and bitrate.
- `media_governor_apply_weak_network_level()` maps weak-network levels to video profiles.

## Default Policy

| Item | Default |
| --- | --- |
| Capture / RTC target | `1920x1080@20fps` |
| H264 bitrate | `6Mbps` |
| GOP | `40` |
| H264 output buffer | `4MB` |
| Max delta payload | `256KB` |
| Startup max delta payload | `128KB` for the first `2500ms` |
| Auto weak-network adaptation | Disabled |
| Wait remote subscribe before capture | Disabled |
| Stale camera frame drain | Enabled |

Weak-network support is an interface and policy layer, not a default behavior. Normal demonstration builds should run the full configured video profile unless the upper layer explicitly applies a weak-network level.

## TiRTC Send Path

`main/protocols/tirtc/tirtc_session.c` owns the TiRTC connection handle and the media send queues.

- Video frames enter a preallocated PSRAM-backed TX pool.
- The TX task drops stale queued video before sending, rather than letting old frames pile up.
- Audio uses a separate queue and may be deferred briefly during video bootstrap.
- Send-buffer watermarks are logged and used to protect the connection from long backpressure.
- Invalid handle, remote close and teardown are handled in the protocol/application layers, not in UI callbacks.

## Failure Handling

The media path should degrade before it disconnects or reboots:

- A dropped QR preview frame must not stop RTC.
- A dropped or stale RTC video frame must not close the TiRTC connection.
- ESP-Hosted TX pressure should be visible through runtime snapshots and TiRTC send-buffer logs.
- Stage logs should remain one-shot or periodic summaries: first frame, encoder ready, bitrate/fps stats, DMA largest block and stop reason.

## Validation Checklist

1. Boot to home page, confirm display orientation and touch coordinate mapping.
2. Bind device or confirm cached credentials are loaded.
3. Connect TiRTC, confirm `H264 encoder ready` and `camera pipeline first upstream frame`.
4. Keep one connection alive for at least 5 minutes while watching FPS, bitrate, DMA largest block and send-buffer pressure.
5. Return home, confirm `camera=0 rtc=0 fps=0.0 bitrate=0kbps`.
6. Re-enter IPC view repeatedly, confirm no stale camera task or connection handle remains.
