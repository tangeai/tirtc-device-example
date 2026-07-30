#ifndef MEDIA_RUNTIME_H
#define MEDIA_RUNTIME_H

#include <stdbool.h>

#include "device/device_media.h"
#include "esp_err.h"
#include "tirtc_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

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

typedef void (*media_runtime_error_callback_t)(uint32_t session_generation,
                                               esp_err_t error,
                                               const char *stage,
                                               void *user_data);
void media_runtime_set_error_callback(media_runtime_error_callback_t callback,
                                      void *user_data);

#ifdef __cplusplus
}
#endif

#endif
