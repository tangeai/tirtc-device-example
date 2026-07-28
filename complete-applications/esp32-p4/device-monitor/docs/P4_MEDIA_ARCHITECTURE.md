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

## Memory Ownership

P4 uses capability-based allocation rather than treating all heap as interchangeable. The common policy is defined in `main/platform/app_memory_policy.h`.

| Memory class | Long-term owners | Allocation rule |
| --- | --- | --- |
| Internal RAM | DMA descriptors, realtime control queues, mutexes, audio I/O control, flash/NVS task stacks | Explicit `MALLOC_CAP_INTERNAL`; never used as a fallback for a failed media allocation. |
| DMA-capable internal RAM | ESP-Hosted, camera/H264 driver DMA and the DMA escrow | Protected by the `192KB` IDF reserve and a `64KB` runtime escrow. |
| PSRAM | H264 payloads, RTC TX pools, decoded video frames, HTTP/MQTT payload copies, UI snapshots and background task stacks | Explicit `MALLOC_CAP_SPIRAM`; allocation failure is returned to the owning service. |

The RTC send path allocates its pools once and reuses them:

- Video TX starts with four `512KB` PSRAM slots and grows a slot only when an encoded frame requires it.
- Audio TX owns eighteen fixed `8KB` PSRAM slots. Per-packet `malloc/free` is not used on the live path.
- Queue storage contains descriptors and slot indexes only. Payload bytes stay in the corresponding PSRAM pool.

Tasks that may execute while flash cache is unavailable, including OTA and NVS persistence workers, keep internal stacks. Network, media, UI snapshot and other background workers use PSRAM stacks. This distinction is intentional and must be preserved when adding a task.

Task memory is reviewed at three levels:

- Every task creation declares its stack capability explicitly. Background and media stacks use PSRAM; realtime audio, flash/NVS and small application control stacks remain internal.
- Compiler stack-usage output is used to inspect task entries and their callees. The build warns when a single function needs more than `8KB`, so large snapshots and protocol payloads must be moved into an owner-specific PSRAM workspace.
- Stack sizes are reduced only after runtime high-water marks prove enough reserve. Static frame size alone is not a safe reason to shrink a task stack.

The device online worker and its HTTP/MQTT credentials use PSRAM. Device binding keeps an internal stack because it persists pending sessions, but its network/session workspace is PSRAM-backed and only the small NVS blobs stay on the internal stack. Long-lived call, contact and service-registry caches use external BSS; mutexes, queues, spinlocks and hot RTC configuration remain internal.

LVGL renders through PSRAM draw buffers and copies through one bounded internal DMA transfer buffer. That transfer buffer uses the same internal-RAM budget as the former pair of small DMA draw buffers, while larger transactions reduce synchronous SPI overhead during call video.

H264 is prewarmed before service discovery, binding, MQTT, TiRTC and UI workloads allocate their long-lived state. The DMA escrow is temporarily lent to H264 during encoder or decoder bootstrap and reclaimed afterward only when the contiguous block is still available. An escrow already lent to the retained encoder is a valid state and must not block decoder recovery.

Runtime snapshots report current, largest-block and minimum-ever values for internal RAM, DMA RAM and PSRAM, together with PSRAM allocation failures and media pool occupancy. The minimum-ever values are the acceptance metric for long calls; current free memory alone does not reveal transient pressure or fragmentation.

## Camera Pipeline

`main/media/camera_pipeline.c` is the owner of live RTC camera capture.

- RTC camera uplink uses OV5647 YUV420 frames as the H264 encoder input.
- The sensor and RTC encoder share the supported `1280x960` YUV420 frame directly; no scaling, crop buffer or per-frame copy is used.
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
| Sensor capture | `1280x960` binning mode |
| RTC target | `1280x960@20fps` |
| H264 bitrate | `4Mbps` |
| GOP | `40` |
| H264 output buffer | `1MB` |
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
