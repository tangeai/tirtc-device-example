#pragma once

/* 主动连接任务：生成测试 token 后调用 TiRtcConnect()。 */

#include <stdbool.h>

#include "esp_err.h"
#include "tiRTC.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tirtc_connect_start(TIRTCCONNECTCALLBACK callback, void *user_data);
bool tirtc_connect_is_connecting(void);
void tirtc_connect_on_tirtc_started(void);
void tirtc_connect_cancel(void);

#ifdef __cplusplus
}
#endif
