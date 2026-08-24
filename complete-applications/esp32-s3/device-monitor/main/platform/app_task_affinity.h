#pragma once

#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

/*
 * ESP32-S3 runtime ownership:
 * - Time-sensitive audio I/O stays on CPU1.
 * - Wi-Fi, TiRTC, MQTT/HTTP, camera QR decode, OTA, and background work stay on CPU0.
 * - LVGL remains unpinned so it can use whichever core is not occupied by the
 *   higher-priority media path. Its priority is still kept below audio I/O.
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
#else
#define APP_TASK_CORE_UI         tskNO_AFFINITY
#define APP_TASK_CORE_AUDIO      1
#define APP_TASK_CORE_NETWORK    0
#define APP_TASK_CORE_RTC        0
#define APP_TASK_CORE_CAMERA     0
#define APP_TASK_CORE_BACKGROUND 0
#endif
