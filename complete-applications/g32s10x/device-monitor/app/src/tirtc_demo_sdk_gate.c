#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <FreeRTOS.h>
#include <os.h>
#include <semphr.h>
#include <spinlock.h>
#include <task.h>

#include "tirtc_demo_sdk_gate.h"

static DEFINE_SPINLOCK(g_tirtc_sdk_gate_init_lock);
static SemaphoreHandle_t g_tirtc_sdk_gate;

int tirtc_demo_sdk_gate_init(void)
{
    SemaphoreHandle_t created;
    unsigned long flags;

    spin_lock_irqsave(&g_tirtc_sdk_gate_init_lock, flags);
    if (g_tirtc_sdk_gate != NULL) {
        spin_unlock_irqrestore(&g_tirtc_sdk_gate_init_lock, flags);
        return 0;
    }
    spin_unlock_irqrestore(&g_tirtc_sdk_gate_init_lock, flags);

    created = xSemaphoreCreateRecursiveMutex();
    if (created == NULL) {
        return -1;
    }

    spin_lock_irqsave(&g_tirtc_sdk_gate_init_lock, flags);
    if (g_tirtc_sdk_gate == NULL) {
        g_tirtc_sdk_gate = created;
        created = NULL;
    }
    spin_unlock_irqrestore(&g_tirtc_sdk_gate_init_lock, flags);
    if (created != NULL) {
        vSemaphoreDelete(created);
    }
    return 0;
}

bool tirtc_demo_sdk_gate_take(uint32_t timeout_ms)
{
    if (tirtc_demo_sdk_gate_init() != 0) {
        return false;
    }
    return xSemaphoreTakeRecursive(g_tirtc_sdk_gate,
                                   pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void tirtc_demo_sdk_gate_give(void)
{
    if (g_tirtc_sdk_gate != NULL &&
        xSemaphoreGiveRecursive(g_tirtc_sdk_gate) != pdTRUE) {
        printf("[tirtc_gate] recursive mutex give rejected task=%s\n",
               pcTaskGetName(NULL));
    }
}
