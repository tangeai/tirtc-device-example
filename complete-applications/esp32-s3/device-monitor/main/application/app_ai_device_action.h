#pragma once

#include "esp_err.h"

#include "ai_chat.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_ai_device_action_execute(const ai_chat_device_action_t *action,
                                       ai_chat_device_action_result_t *result);

#ifdef __cplusplus
}
#endif
