#include "app_audio_policy.h"

#include "app_memory_policy.h"
#include "audio_device.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"

static const char *TAG = "app_audio_policy";

#define APP_AUDIO_AEC_SOURCE_MASK \
    (APP_AUDIO_AEC_SOURCE_RTC_MEDIA | APP_AUDIO_AEC_SOURCE_AI_CHAT_MEDIA)

static SemaphoreHandle_t s_policy_lock;
static uint32_t s_active_aec_sources;
static bool s_aec_runtime_active;
static esp_err_t s_last_aec_apply_error = ESP_OK;

static bool app_audio_policy_aec_source_valid(app_audio_aec_source_t source)
{
    const uint32_t value = (uint32_t)source;

    return value != 0U && (value & (value - 1U)) == 0U &&
           (value & ~APP_AUDIO_AEC_SOURCE_MASK) == 0U;
}

static const char *app_audio_policy_aec_source_name(app_audio_aec_source_t source)
{
    switch (source) {
    case APP_AUDIO_AEC_SOURCE_RTC_MEDIA:
        return "rtc-media";
    case APP_AUDIO_AEC_SOURCE_AI_CHAT_MEDIA:
        return "ai-chat-media";
    default:
        return "unknown";
    }
}

esp_err_t app_audio_policy_init(void)
{
    if (s_policy_lock != NULL) {
        return ESP_OK;
    }

    s_policy_lock = xSemaphoreCreateMutexWithCaps(APP_SYNC_CAPS_CONTROL);
    return s_policy_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t app_audio_policy_set_aec_source_active(app_audio_aec_source_t source, bool active)
{
    ESP_RETURN_ON_FALSE(app_audio_policy_aec_source_valid(source),
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid AEC source");
    ESP_RETURN_ON_FALSE(s_policy_lock != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "audio policy is not initialized");

    xSemaphoreTake(s_policy_lock, portMAX_DELAY);

    const uint32_t previous_sources = s_active_aec_sources;
    if (active) {
        s_active_aec_sources |= (uint32_t)source;
    } else {
        s_active_aec_sources &= ~(uint32_t)source;
    }

    const bool requested_active = s_active_aec_sources != 0U;
    const bool should_apply = requested_active != s_aec_runtime_active ||
                              (requested_active && s_last_aec_apply_error != ESP_OK);
    esp_err_t ret = ESP_OK;

    if (should_apply) {
        ret = audio_device_set_echo_cancel_active(requested_active);
        if (ret == ESP_OK) {
            s_aec_runtime_active = requested_active;
        } else if (!requested_active) {
            /* The driver disables new processing before waiting for in-flight users. */
            s_aec_runtime_active = false;
        }
        s_last_aec_apply_error = ret;
    }

    if (previous_sources != s_active_aec_sources) {
        ESP_LOGI(TAG,
                 "AEC source %s %s: sources=0x%02lx runtime=%d",
                 app_audio_policy_aec_source_name(source),
                 active ? "active" : "idle",
                 (unsigned long)s_active_aec_sources,
                 s_aec_runtime_active ? 1 : 0);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "apply AEC policy failed: requested=%d sources=0x%02lx ret=%s",
                 requested_active ? 1 : 0,
                 (unsigned long)s_active_aec_sources,
                 esp_err_to_name(ret));
    }

    xSemaphoreGive(s_policy_lock);
    return ret;
}
