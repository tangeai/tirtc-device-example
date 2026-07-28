/*
 * BOOT 键轮询：简单去抖后回调给应用层。
 */
#include "boot_button.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"

static const char *TAG = "boot_button";

#define BOOT_BUTTON_POLL_MS 20
#define BOOT_BUTTON_DEBOUNCE_MS 60
#define BOOT_BUTTON_TASK_STACK 4096
#define BOOT_BUTTON_TASK_PRIORITY 5

typedef struct
{
    boot_button_callback_t callback;
    void *user_data;
} boot_button_context_t;

static void boot_button_task(void *arg)
{
    boot_button_context_t *ctx = (boot_button_context_t *)arg;
    bool was_pressed = false;

    while (true)
    {
        bool pressed = (gpio_get_level((gpio_num_t)APP_BOOT_BUTTON_GPIO) == APP_BOOT_BUTTON_ACTIVE_LEVEL);
        if (pressed && !was_pressed)
        {
            /* 板载 BOOT 键没有业务级响应要求，轮询加短延时去抖足够稳定。 */
            vTaskDelay(pdMS_TO_TICKS(BOOT_BUTTON_DEBOUNCE_MS));
            if (gpio_get_level((gpio_num_t)APP_BOOT_BUTTON_GPIO) == APP_BOOT_BUTTON_ACTIVE_LEVEL)
            {
                ESP_LOGI(TAG, "BOOT 键已按下，准备切换 TiRTC 连接状态");
                if (ctx->callback != NULL)
                {
                    ctx->callback(ctx->user_data);
                }
                was_pressed = true;
            }
        }
        else if (!pressed)
        {
            was_pressed = false;
        }

        vTaskDelay(pdMS_TO_TICKS(BOOT_BUTTON_POLL_MS));
    }
}

esp_err_t boot_button_start(boot_button_callback_t callback, void *user_data)
{
    static boot_button_context_t button_ctx;
    static TaskHandle_t button_task;

    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (button_task != NULL)
    {
        ESP_LOGW(TAG, "BOOT 键监听任务已经启动");
        return ESP_OK;
    }

    gpio_config_t gpio_cfg = {
        .pin_bit_mask = 1ULL << APP_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&gpio_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "配置 BOOT 键 GPIO 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    button_ctx.callback = callback;
    button_ctx.user_data = user_data;

    BaseType_t task_ret = xTaskCreate(boot_button_task,
                                      "boot_button",
                                      BOOT_BUTTON_TASK_STACK,
                                      &button_ctx,
                                      BOOT_BUTTON_TASK_PRIORITY,
                                      &button_task);
    if (task_ret != pdPASS)
    {
        button_task = NULL;
        ESP_LOGE(TAG, "创建 BOOT 键监听任务失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "BOOT 键监听已启动，GPIO=%d active_level=%d",
             APP_BOOT_BUTTON_GPIO,
             APP_BOOT_BUTTON_ACTIVE_LEVEL);
    return ESP_OK;
}
