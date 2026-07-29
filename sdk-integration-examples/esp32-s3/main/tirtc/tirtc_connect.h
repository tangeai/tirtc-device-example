#pragma once

/* 主动连接任务：优先使用缓存，未命中时提交服务端签发的一次性 token。 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "tiRTC.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tirtc_connect_prepare(void);
esp_err_t tirtc_connect_start(TIRTCCONNECTCALLBACK callback, void *user_data);
bool tirtc_connect_is_connecting(void);
void tirtc_connect_on_tirtc_started(void);
bool tirtc_connect_cancel_and_wait(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
