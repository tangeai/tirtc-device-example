#pragma once

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

/*
 * ESP32-P4 runtime ownership:
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

/*
 * Memory ownership policy:
 * - realtime/driver paths keep internal stacks;
 * - control/UI workers keep internal stacks because they can trigger NVS or
 *   flash operations while cache is disabled;
 * - background workers prefer PSRAM so H264, CSI, I2S and SDIO can
 *   reserve larger contiguous internal/DMA blocks;
 * - every PSRAM move must still keep an internal fallback at the creation site.
 */
#define APP_TASK_STACK_CAPS_REALTIME   (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define APP_TASK_STACK_CAPS_INTERNAL   (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define APP_TASK_STACK_CAPS_CONTROL    (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define APP_TASK_STACK_CAPS_BACKGROUND (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define APP_QUEUE_CAPS_BACKGROUND      (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
