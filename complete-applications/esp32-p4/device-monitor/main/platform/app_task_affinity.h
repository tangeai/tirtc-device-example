#pragma once

#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#include "app_memory_policy.h"

/*
 * ESP32-P4 runtime ownership:
 * - CPU0 carries network/control, camera uplink, and the bounded H264 decoder
 *   caller. The decoder is allowed to outrank camera/video TX briefly so it
 *   can preserve the remote H264 reference chain.
 * - CPU1 carries LVGL/audio and the bounded TinyH264 dual-task helper.
 * - Frame conversion is SMP-migratable because it can run on either CPU and
 *   should consume whichever core has headroom at that instant.
 *
 * Keep task creation sites using these names instead of raw core numbers so the
 * scheduling contract stays visible when modules are moved or added.
 */
#if CONFIG_FREERTOS_UNICORE
#define APP_TASK_CORE_UI         tskNO_AFFINITY
#define APP_TASK_CORE_AUDIO      tskNO_AFFINITY
#define APP_TASK_CORE_NETWORK    tskNO_AFFINITY
#define APP_TASK_CORE_RTC        tskNO_AFFINITY
#define APP_TASK_CORE_CAMERA     tskNO_AFFINITY
#define APP_TASK_CORE_BACKGROUND tskNO_AFFINITY
#define APP_TASK_CORE_VIDEO_DECODE  tskNO_AFFINITY
#define APP_TASK_CORE_VIDEO_CONVERT tskNO_AFFINITY
#else
#define APP_TASK_CORE_UI         1
#define APP_TASK_CORE_AUDIO      1
#define APP_TASK_CORE_NETWORK    0
#define APP_TASK_CORE_RTC        0
#define APP_TASK_CORE_CAMERA     0
#define APP_TASK_CORE_BACKGROUND 0
#define APP_TASK_CORE_VIDEO_DECODE  0
#define APP_TASK_CORE_VIDEO_CONVERT tskNO_AFFINITY
#endif
