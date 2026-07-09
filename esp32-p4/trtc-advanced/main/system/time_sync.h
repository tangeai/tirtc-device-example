#pragma once

/* SNTP 时间同步接口：TiRTC 鉴权前调用一次即可。 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t time_sync_once(void);

#ifdef __cplusplus
}
#endif
