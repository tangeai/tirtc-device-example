#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    APP_AUDIO_AEC_SOURCE_RTC_MEDIA = 1U << 0,
    APP_AUDIO_AEC_SOURCE_AI_CHAT_MEDIA = 1U << 1,
} app_audio_aec_source_t;

esp_err_t app_audio_policy_init(void);
esp_err_t app_audio_policy_set_aec_source_active(app_audio_aec_source_t source, bool active);
