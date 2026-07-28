#pragma once

/* Wi-Fi STA 连接接口：返回成功时已经拿到 IP。 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_sta_connect(void);

#ifdef __cplusplus
}
#endif
