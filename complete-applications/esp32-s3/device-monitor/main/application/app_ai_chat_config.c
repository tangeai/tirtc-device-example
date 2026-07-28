#include "app_ai_chat_config.h"

#include "esp_check.h"
#include "nvs.h"
#include "platform_nvs_async.h"
#include "platform_storage.h"

static const char *TAG = "app_ai_chat_config";

#define APP_AI_CHAT_NVS_NAMESPACE  "ai_chat_ui"
#define APP_AI_CHAT_NVS_KEY_AVATAR "avatar"

static app_ai_chat_config_t s_ai_chat_config = {
    .avatar = APP_AI_CHAT_AVATAR_BUDDY,
};

static uint8_t app_ai_chat_config_normalize_avatar(uint8_t avatar)
{
    return avatar < APP_AI_CHAT_AVATAR_COUNT ? avatar : APP_AI_CHAT_AVATAR_BUDDY;
}

static void app_ai_chat_config_fill_defaults(app_ai_chat_config_t *config)
{
    if (config == NULL) {
        return;
    }

    config->avatar = APP_AI_CHAT_AVATAR_BUDDY;
}

esp_err_t app_ai_chat_config_load(app_ai_chat_config_t *config)
{
    nvs_handle_t nvs_handle = 0;
    app_ai_chat_config_t loaded = {0};

    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid ai chat config");
    app_ai_chat_config_fill_defaults(&loaded);

    ESP_RETURN_ON_ERROR(platform_storage_init(), TAG, "nvs init failed");

    esp_err_t ret = nvs_open(APP_AI_CHAT_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        *config = loaded;
        s_ai_chat_config = loaded;
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t saved_avatar = 0;
    ret = nvs_get_u8(nvs_handle, APP_AI_CHAT_NVS_KEY_AVATAR, &saved_avatar);
    nvs_close(nvs_handle);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    } else if (ret == ESP_OK) {
        loaded.avatar = app_ai_chat_config_normalize_avatar(saved_avatar);
    }
    if (ret != ESP_OK) {
        return ret;
    }

    *config = loaded;
    s_ai_chat_config = loaded;
    return ESP_OK;
}

uint8_t app_ai_chat_config_get_avatar(void)
{
    return s_ai_chat_config.avatar;
}

esp_err_t app_ai_chat_config_set_avatar(uint8_t avatar)
{
    uint8_t normalized = app_ai_chat_config_normalize_avatar(avatar);

    s_ai_chat_config.avatar = normalized;
    return platform_nvs_async_set_u8(APP_AI_CHAT_NVS_NAMESPACE,
                                     APP_AI_CHAT_NVS_KEY_AVATAR,
                                     normalized);
}
