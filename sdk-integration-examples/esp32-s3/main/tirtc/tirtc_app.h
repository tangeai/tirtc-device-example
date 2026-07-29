#pragma once

/* TiRTC 应用层 API：main 和按键逻辑通过这里使用 SDK。 */

#include <stdbool.h>
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

/* 本地媒体任务用同一门禁保护 SDK 连接句柄的检查与调用。 */
bool tirtc_connection_guard_lock(tirtc_conn_t hconn);
void tirtc_connection_guard_unlock(void);

/* SDK 回调把迟到或未接管的句柄交给普通关闭任务。 */
void tirtc_release_unowned_connection(tirtc_conn_t hconn);

void tirtc_log_status(void);

#ifdef __cplusplus
}
#endif
