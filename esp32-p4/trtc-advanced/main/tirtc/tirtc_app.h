#pragma once

/* TiRTC 应用层 API：main 和按键逻辑通过这里使用 SDK。 */

#include <stdint.h>

#include "esp_err.h"
#include "tiRTC.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tirtc_start(void);
void tirtc_deinit(void);

/* 下面这些函数是常用控制接口的最小示例。 */
esp_err_t tirtc_toggle_connection(void);
esp_err_t tirtc_connect_configured(void);
int tirtc_disconnect_current(void);
int tirtc_subscribe_remote_video(uint8_t stream_id);
int tirtc_unsubscribe_remote_video(uint8_t stream_id);
int tirtc_subscribe_remote_audio(uint8_t stream_id);
int tirtc_unsubscribe_remote_audio(uint8_t stream_id);
int tirtc_request_remote_key_frame(uint8_t stream_id);

void tirtc_log_status(void);

#ifdef __cplusplus
}
#endif
