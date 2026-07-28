#pragma once

/* 本地测试媒体源：从 SPIFFS 读取 H264/PCMA 文件并通过 TiRTC 发送。 */

#include <stdint.h>

#include "esp_err.h"
#include "tiRTC.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t local_media_stream_prepare(void);
esp_err_t local_media_stream_start_video(tirtc_conn_t hconn, uint8_t stream_id);
esp_err_t local_media_stream_start_audio(tirtc_conn_t hconn, uint8_t stream_id);
void local_media_stream_request_key_frame(tirtc_conn_t hconn);
void local_media_stream_stop_video(tirtc_conn_t hconn);
void local_media_stream_stop_audio(tirtc_conn_t hconn);
void local_media_stream_stop(tirtc_conn_t hconn);
void local_media_stream_stop_and_wait(tirtc_conn_t hconn, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
