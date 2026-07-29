#ifndef TIRTC_SAMPLE_MEDIA_H
#define TIRTC_SAMPLE_MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "TiRTC/tiRTC.h"

typedef struct {
    /* All callbacks are serialized with TiRTC disconnect by the link layer. */
    int (*send_video)(tirtc_conn_t connection,
                      const TIRTCFRAMEINFO *frame, const void *data);
    int (*send_audio)(tirtc_conn_t connection,
                      const TIRTCFRAMEINFO *frame, const void *data);
    size_t (*send_buffer_used)(tirtc_conn_t connection);
} tirtc_sample_media_ops_t;

typedef struct {
    bool asset_ready;
    bool video_active;
    bool audio_active;
    uint32_t asset_video_frames;
    uint32_t asset_audio_bytes;
    uint32_t sent_video_frames;
    uint32_t sent_audio_packets;
    uint32_t dropped_packets;
    int last_error;
} tirtc_sample_media_status_t;

/* Validate the embedded AVI and create the bounded sample publisher worker. */
int tirtc_sample_media_init(const tirtc_sample_media_ops_t *ops);

/* Assign or clear the single connection that owns this sample publisher. */
void tirtc_sample_media_set_connection(tirtc_conn_t connection);
void tirtc_sample_media_clear_connection(tirtc_conn_t connection);

/* TiRTC subscription callbacks. The calls are non-blocking and allocation-free. */
int tirtc_sample_media_subscribe_video(tirtc_conn_t connection,
                                       uint8_t stream_id);
void tirtc_sample_media_unsubscribe_video(tirtc_conn_t connection,
                                          uint8_t stream_id);
int tirtc_sample_media_subscribe_audio(tirtc_conn_t connection,
                                       uint8_t stream_id);
void tirtc_sample_media_unsubscribe_audio(tirtc_conn_t connection,
                                          uint8_t stream_id);

/* Copy a lock-protected publisher snapshot into caller-owned storage. */
void tirtc_sample_media_get_status(tirtc_sample_media_status_t *status);

#endif
