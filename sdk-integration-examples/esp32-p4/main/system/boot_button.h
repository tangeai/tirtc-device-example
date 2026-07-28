#pragma once

/* BOOT 键监听接口：用于触发一次主动连接。 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*boot_button_callback_t)(void *user_data);

esp_err_t boot_button_start(boot_button_callback_t callback, void *user_data);

#ifdef __cplusplus
}
#endif
