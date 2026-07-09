#include "platform_task_reaper.h"

#include <stdbool.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"

static const char *TAG = "task_reaper";

#define PLATFORM_TASK_REAPER_QUEUE_LEN 16U
#define PLATFORM_TASK_REAPER_STACK     (3U * 1024U)
#define PLATFORM_TASK_REAPER_PRIORITY  6

static StaticQueue_t s_queue_buffer;
static TaskHandle_t s_queue_items[PLATFORM_TASK_REAPER_QUEUE_LEN];
static StackType_t s_stack[PLATFORM_TASK_REAPER_STACK];
static StaticTask_t s_tcb;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;

static void platform_task_reaper_task(void *ctx)
{
    (void)ctx;

    while (true) {
        TaskHandle_t task = NULL;
        if (xQueueReceive(s_queue, &task, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (task == NULL || task == xTaskGetCurrentTaskHandle()) {
            continue;
        }
        vTaskDeleteWithCaps(task);
    }
}

esp_err_t platform_task_reaper_init(void)
{
    if (s_queue == NULL) {
        s_queue = xQueueCreateStatic(PLATFORM_TASK_REAPER_QUEUE_LEN,
                                     sizeof(TaskHandle_t),
                                     (uint8_t *)s_queue_items,
                                     &s_queue_buffer);
    }
    if (s_queue != NULL && s_task == NULL) {
        s_task = xTaskCreateStaticPinnedToCore(platform_task_reaper_task,
                                              "task_reaper",
                                              PLATFORM_TASK_REAPER_STACK,
                                              NULL,
                                              PLATFORM_TASK_REAPER_PRIORITY,
                                              s_stack,
                                              &s_tcb,
                                              tskNO_AFFINITY);
    }
    bool ready = (s_queue != NULL && s_task != NULL);

    return ready ? ESP_OK : ESP_ERR_NO_MEM;
}

void platform_task_reaper_delete_current_with_caps(const char *owner_tag)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    TickType_t last_log = 0;

    if (self == NULL) {
        return;
    }

    while (true) {
        if (platform_task_reaper_init() == ESP_OK &&
            xQueueSend(s_queue, &self, pdMS_TO_TICKS(100)) == pdTRUE) {
            vTaskSuspend(NULL);
        }

        TickType_t now = xTaskGetTickCount();
        if (last_log == 0 || now - last_log >= pdMS_TO_TICKS(2000)) {
            last_log = now;
            ESP_LOGW(TAG,
                     "waiting to reap WithCaps task: owner=%s task=%p",
                     owner_tag != NULL ? owner_tag : "unknown",
                     self);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
