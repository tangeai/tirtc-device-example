/*
 * SPDX-FileCopyrightText: 2026 TiRTC project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32P4

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "H264_SYNC";

#define TINYH264_FILTER_TASK_NAME "h264FilterTask"

/* Provided by GNU ld when --wrap=xTaskGenericNotify is enabled. */
BaseType_t __real_xTaskGenericNotify(TaskHandle_t task,
                                     UBaseType_t index,
                                     uint32_t value,
                                     eNotifyAction action,
                                     uint32_t *previous_value);

static bool is_tinyh264_phase_notification(TaskHandle_t task,
                                           UBaseType_t index,
                                           uint32_t value,
                                           eNotifyAction action,
                                           uint32_t *previous_value)
{
    if (task == NULL || index != 0U || value != UINT32_MAX ||
        action != eSetValueWithoutOverwrite || previous_value != NULL) {
        return false;
    }

    const char *task_name = pcTaskGetName(task);
    return task_name != NULL &&
           strcmp(task_name, TINYH264_FILTER_TASK_NAME) == 0;
}

BaseType_t __wrap_xTaskGenericNotify(TaskHandle_t task,
                                     UBaseType_t index,
                                     uint32_t value,
                                     eNotifyAction action,
                                     uint32_t *previous_value)
{
    BaseType_t result = __real_xTaskGenericNotify(task,
                                                  index,
                                                  value,
                                                  action,
                                                  previous_value);
    if (result == pdPASS ||
        !is_tinyh264_phase_notification(task,
                                        index,
                                        value,
                                        action,
                                        previous_value)) {
        return result;
    }

    /*
     * The prebuilt TinyH264 archive sends two phase notifications with
     * eSetValueWithoutOverwrite.  On SMP, the caller can reach the next send
     * while the helper still owns the previous pending notification.  The
     * archive logs the failed send and continues, which drops a phase and can
     * later leave the caller and helper waiting at different event barriers.
     *
     * Wait for the helper to consume the pending phase, then deliver this
     * exact phase.  Replacing it with eSetValueWithOverwrite would still lose
     * one wake-up and only hide the protocol error.
     */
    const TickType_t started_at = xTaskGetTickCount();
    uint32_t attempts = 0U;
    do {
        vTaskDelay(1);
        attempts++;
        result = __real_xTaskGenericNotify(task,
                                           index,
                                           value,
                                           action,
                                           previous_value);
    } while (result != pdPASS);

    static uint32_t recovered_collisions;
    static uint32_t maximum_wait_ms;
    const uint32_t wait_ms = pdTICKS_TO_MS(xTaskGetTickCount() - started_at);
    const uint32_t recovered = __atomic_add_fetch(&recovered_collisions,
                                                   1U,
                                                   __ATOMIC_RELAXED);
    uint32_t observed_max = __atomic_load_n(&maximum_wait_ms, __ATOMIC_RELAXED);
    while (wait_ms > observed_max &&
           !__atomic_compare_exchange_n(&maximum_wait_ms,
                                        &observed_max,
                                        wait_ms,
                                        false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }

    /* A synchronous UART warning in the decoder task can cost more than the
     * recovered race itself. Keep first-occurrence evidence, then report only
     * a compact aggregate after every 64 recoveries. */
    if (recovered == 1U || (recovered % 64U) == 0U) {
        ESP_LOGW(TAG,
                 "TinyH264 sync guard: recovered=%lu last=%lums max=%lums attempts=%lu",
                 (unsigned long)recovered,
                 (unsigned long)wait_ms,
                 (unsigned long)__atomic_load_n(&maximum_wait_ms,
                                                __ATOMIC_RELAXED),
                 (unsigned long)attempts);
    }
    return result;
}

#endif /* CONFIG_IDF_TARGET_ESP32P4 */
