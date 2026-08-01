#ifndef MEDIA_RUNTIME_H
#define MEDIA_RUNTIME_H

#include <stdbool.h>

#include "device/device_media.h"
#include "esp_err.h"
#include "tirtc_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MEDIA_RUNTIME_AI_PROMPT_STORY = 0,
    MEDIA_RUNTIME_AI_PROMPT_JOKE,
    MEDIA_RUNTIME_AI_PROMPT_WEATHER,
    MEDIA_RUNTIME_AI_PROMPT_CALL_XIAOZHANG,
    MEDIA_RUNTIME_AI_PROMPT_CALL_XIAOLI,
    MEDIA_RUNTIME_AI_PROMPT_COUNT,
} media_runtime_ai_prompt_t;

esp_err_t media_runtime_init(void);
esp_err_t media_runtime_start(void);
void media_runtime_stop(void);
bool media_runtime_ready(void);
const device_media_config_t *media_runtime_config(void);
esp_err_t media_runtime_set_session(tirtc_adapter_media_profile_t profile,
                                    uint32_t session_generation,
                                    bool uplink_video_enabled);
void media_runtime_clear_session(uint32_t session_generation);
void media_runtime_set_uplink_active(bool active);
bool media_runtime_uplink_active(void);
bool media_runtime_ai_prompt_parse(const char *name,
                                   media_runtime_ai_prompt_t *prompt);
const char *media_runtime_ai_prompt_name(media_runtime_ai_prompt_t prompt);
esp_err_t media_runtime_play_ai_prompt(media_runtime_ai_prompt_t prompt,
                                       uint32_t session_generation);
void media_runtime_cancel_ai_prompt(uint32_t session_generation);

typedef void (*media_runtime_error_callback_t)(uint32_t session_generation,
                                               esp_err_t error,
                                               const char *stage,
                                               void *user_data);
typedef void (*media_runtime_prompt_callback_t)(
    uint32_t session_generation,
    media_runtime_ai_prompt_t prompt,
    esp_err_t status,
    void *user_data);
void media_runtime_set_error_callback(media_runtime_error_callback_t callback,
                                      void *user_data);
void media_runtime_set_prompt_callback(media_runtime_prompt_callback_t callback,
                                       void *user_data);

#ifdef __cplusplus
}
#endif

#endif
