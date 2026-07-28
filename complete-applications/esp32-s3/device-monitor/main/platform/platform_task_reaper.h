#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t platform_task_reaper_init(void);
void platform_task_reaper_delete_current_with_caps(const char *owner_tag);
