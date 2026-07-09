#pragma once

#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

/*
 * ESP32-S3 runtime ownership:
 * - CPU1 stays responsive for LVGL/UI and time-sensitive audio I/O.
 * - CPU0 carries Wi-Fi, TiRTC, MQTT/HTTP, camera QR decode, OTA, and background work.
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
#define APP_TASK_CORE_UI         1
#define APP_TASK_CORE_AUDIO      1
#define APP_TASK_CORE_NETWORK    0
#define APP_TASK_CORE_RTC        0
#define APP_TASK_CORE_CAMERA     0
#define APP_TASK_CORE_BACKGROUND 0
#endif
