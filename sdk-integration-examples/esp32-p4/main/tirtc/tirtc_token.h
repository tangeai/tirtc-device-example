#pragma once

/* 测试用本地 token 签发接口：生产环境建议替换为业务服务端返回 token。 */

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tirtc_token_fetch_connect(const char *peer_id, char *out_token, size_t out_token_size);

#ifdef __cplusplus
}
#endif
