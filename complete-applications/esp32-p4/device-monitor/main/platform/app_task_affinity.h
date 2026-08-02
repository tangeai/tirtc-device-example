#pragma once

#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#include "app_memory_policy.h"

/*
 * ESP32-P4 runtime ownership:
 * - CPU0 carries network/control and the bounded H264 decoder caller. The
 *   decoder is allowed to preserve the remote H264 reference chain without
 *   permanently trapping camera work on the same core.
 * - CPU1 carries LVGL/audio and downlink frame conversion. Audio remains at a
 *   higher priority; conversion shares LVGL's priority and yields after each
 *   frame so neither the screen nor touch input is starved.
 * - Camera uplink is SMP-migratable at a lower priority than decode, audio,
 *   conversion, and UI. Full-duplex calls can therefore consume residual time
 *   on either CPU instead of losing an entire capture deadline while CPU0 is
 *   decoding a complex remote frame.
 * - The application video-TX worker is also SMP-migratable. Its SDK call can
 *   overlap a long decode window, so pinning it to CPU0 turns normal scheduler
 *   preemption into artificial send latency and leaves less time for transport
 *   receive processing. RTC control and audio-TX ownership remain on CPU0.
 * - TinyH264 uses CPU1 for its bounded helper while the caller remains on
 *   CPU0. The helper priority is part of the renderer policy because the
 *   precompiled decoder waits synchronously for it at slice barriers.
 *
 * Keep task creation sites using these names instead of raw core numbers so the
 * scheduling contract stays visible when modules are moved or added.
 */
#define APP_TASK_PRIORITY_AUDIO_CAPTURE  18U
#define APP_TASK_PRIORITY_AUDIO_PLAYBACK 17U

#if CONFIG_FREERTOS_UNICORE
#define APP_TASK_CORE_UI         tskNO_AFFINITY
#define APP_TASK_CORE_AUDIO      tskNO_AFFINITY
#define APP_TASK_CORE_NETWORK    tskNO_AFFINITY
#define APP_TASK_CORE_RTC        tskNO_AFFINITY
#define APP_TASK_CORE_RTC_VIDEO_TX tskNO_AFFINITY
#define APP_TASK_CORE_CAMERA     tskNO_AFFINITY
#define APP_TASK_CORE_BACKGROUND tskNO_AFFINITY
#define APP_TASK_CORE_VIDEO_DECODE  tskNO_AFFINITY
#define APP_TASK_CORE_VIDEO_CONVERT tskNO_AFFINITY
#else
#define APP_TASK_CORE_UI         1
#define APP_TASK_CORE_AUDIO      1
#define APP_TASK_CORE_NETWORK    0
#define APP_TASK_CORE_RTC        0
#define APP_TASK_CORE_RTC_VIDEO_TX tskNO_AFFINITY
#define APP_TASK_CORE_CAMERA     tskNO_AFFINITY
#define APP_TASK_CORE_BACKGROUND 0
#define APP_TASK_CORE_VIDEO_DECODE  0
#define APP_TASK_CORE_VIDEO_CONVERT 1
#endif
