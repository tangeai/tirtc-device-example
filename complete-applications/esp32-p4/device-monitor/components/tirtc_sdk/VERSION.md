# TiRTC SDK Version

| Item | Value |
| --- | --- |
| Package channel | Official 2.3.0 source rebuild with P4 transport stability fixes |
| TiRTC version | 2.3.0 |
| Target | ESP32-P4 / FreeRTOS / ESP-IDF |
| ESP-IDF | 5.5.4 |
| Toolchain | riscv32-esp-elf-gcc-14.2.0_20260121 |
| Nano source | `v2.3.0` / `1baf7c95f3ca715c9367b9c998417f647934dc35` |
| TGWebRTC source | `tag.v1.5.12` / `41c9a25768ffe265c07f17ef78a6439607b19364` |
| Official archive SHA-256 | `6daa39e04edf552283360f6a7defa6d12de8c8dd8d8094f8a6bbbdbb64a3f190` |
| Integrated library SHA-256 | `6dc4d437ea444761ca21e203fc9babb1799bb1f7fc261d7c523248fde0a96e67` |
| Build date | 2026-08-26 |

## Build Contract

- `CONFIG_FREERTOS_HZ=1000`
- FreeRTOS trace facility disabled
- FreeRTOS stats formatting functions disabled
- FreeRTOS runtime stats disabled
- `sizeof(StaticSemaphore_t)=84`
- `CONFIG_LWIP_MAX_SOCKETS=16`
- `libwebrtc_nosctp.a` is bundled into `libTiRTC.a`
- ESP32-P4 and ESP32-S3 libraries are not interchangeable

The rebuild uses this application's generated ESP-IDF configuration. The
application uses ESP32-C6 Wi-Fi over ESP-Hosted SDIO, so it keeps
`CONFIG_ESP_HOST_WIFI_ENABLED=n`, `CONFIG_ESP_WIFI_REMOTE_ENABLED=y`, and
`CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED=y`.

## P4 Compatibility Patch

- Use the ESP-IDF 5.5.4 P4 register include directory
  `soc/esp32p4/register/hw_ver1`.
- Bound one ICE UDP receive callback to at most 8 datagrams or 4 ms, then ask
  the RTC poll loop to run again without waiting. This prevents a continuously
  readable media socket from monopolizing the RTC thread while retaining all
  packets in the socket queue for subsequent polls.
- Bound one TGTRP audio-jitter poll to at most 4 ordered work items. When a
  missing frame repairs a queued run, the remaining ready frames continue on
  the next 1 ms scheduler turn instead of being released synchronously in one
  socket callback. Ordering and loss accounting remain unchanged.
- Preserve a slow-path diagnostic when one receive callback still takes at
  least 100 ms: `[ICE_RX_DIAG]` reports packet count, bytes, receive time,
  `ice_agent_input()` time, media callback time, and whether the fairness
  budget yielded. Nested `[TGTRP_*_DIAG]` records separate segment parsing,
  frame completion, jitter polling, assembly, and application callback costs.
- Run only the P4 RTC receive/assembly task at priority 17. The audio capture
  task remains above it, while the H264 decode worker remains below it. This
  prevents every queued frame in a jitter-buffer recovery burst from waking
  the decoder and preempting the RTC task before the remaining frames can be
  delivered.
- After the P4 ICE receive callback consumes its packet/time budget, yield the
  RTC task for one 1 ms tick. No fixed delay is added to ordinary one-packet
  traffic. This gives CPU0 IDLE and downstream workers a bounded scheduling
  window without changing the normal receive cadence.
- Keep TGTRP NACK batch scratch memory on the connection heap instead of the
  RTC task stack, preventing stack-protection faults during severe loss bursts.
- Pass the real NACK scratch capacity after that storage became a pointer.
  Using `sizeof(pointer)` limited the encoder to 4 bytes on P4, so every real
  frame-NACK request failed with `TGTRP_ERR_BUFFER_TOO_SMALL (-7)` and recovery
  fell back to the much slower high-RTT retransmission timeout path.
- Keep normal TGTRP diagnostics at a 10-second maintenance cadence while
  retaining one-second warnings when receive progress is genuinely stalled.
- Keep recovery probes bounded by clean loss, delay, RTT, and sender-backlog
  gates. A static or quantizer-limited encoder no longer stalls recovery
  forever; the controller retries after 3 seconds, raises a clean-window target
  by at most 40 percent, and caps the minimum-bitrate recovery hold at 12
  seconds. Congested feedback cannot consume a pending recovery probe.

## Bitrate Adaptation Contract

- Register `TiRtcConnSetVideoBitrateParams()` after a TGTRP connection is
  accepted and before sending the video stream.
- Valid ranges satisfy `0 < min_bps <= start_bps <= max_bps`.
- `TIRTCCALLBACKS.on_update_bitrate()` supplies an absolute target bitrate in
  bps. The callback runs on an SDK thread, so the application posts the target
  to its media-control task and returns immediately.
- The callback is a recommendation. The application remains responsible for
  applying the target to its encoder, frame rate, or resolution policy.
- The API is TGTRP-specific in this SDK. Unsupported transports or invalid
  parameters return `TIRTC_E_INVALID_PARAMETER`.
- Automatic application-side adaptation remains disabled by default through
  `CONFIG_APP_RTC_SDK_VIDEO_ADAPT_ENABLE`; enable it only for the target test or
  product policy.
