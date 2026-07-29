#ifndef _TIRTC_DEMO_MEDIA_H
#define _TIRTC_DEMO_MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef CONFIG_TIRTC
#include "tiRTC.h"
#else
typedef void *tirtc_conn_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TIRTC_DEMO_MEDIA_ENABLE_VIDEO
#define TIRTC_DEMO_MEDIA_ENABLE_VIDEO 1
#endif

#define TIRTC_DEMO_DEVICE_AUDIO_STREAM_ID 10U
#define TIRTC_DEMO_DEVICE_VIDEO_STREAM_ID 11U
#define TIRTC_DEMO_AI_AUDIO_STREAM_ID 1U
#define TIRTC_DEMO_REMOTE_VIDEO_MAX_BYTES (256U * 1024U)

typedef enum {
    TIRTC_DEMO_MEDIA_NONE = 0,
    TIRTC_DEMO_MEDIA_MONITOR,
    TIRTC_DEMO_MEDIA_DEVICE_CALL,
    TIRTC_DEMO_MEDIA_WECHAT,
    TIRTC_DEMO_MEDIA_AI,
} tirtc_demo_media_mode_t;

typedef struct {
    bool initialized;
    bool capture_ready;
    bool playback_ready;
    bool camera_ready;
    bool uplink_enabled;
    bool video_enabled;
    uint32_t tx_audio_frames;
    uint32_t tx_audio_dropped;
    uint32_t rx_audio_frames;
    uint32_t rx_audio_dropped;
    uint32_t tx_video_frames;
    uint32_t tx_video_dropped;
    uint32_t rx_video_frames;
    uint32_t rx_video_dropped;
    uint32_t last_send_buffer_used;
    int last_error;
} tirtc_demo_media_stats_t;

typedef struct {
    const void *data;
    size_t length;
    uint32_t timestamp_ms;
    uint32_t token;
} tirtc_demo_remote_video_t;

int tirtc_demo_media_init(void);
int tirtc_demo_media_start(tirtc_conn_t connection,
                           tirtc_demo_media_mode_t mode,
                           bool enable_video);
void tirtc_demo_media_stop(tirtc_conn_t connection);
int tirtc_demo_media_set_uplink(bool enabled);
int tirtc_demo_media_set_video(bool enabled);
void tirtc_demo_media_request_key_frame(void);
int tirtc_demo_media_submit_remote_audio(tirtc_conn_t connection,
                                         const TIRTCFRAMEINFO *frame,
                                         const void *data);
int tirtc_demo_media_submit_remote_video(tirtc_conn_t connection,
                                         const TIRTCFRAMEINFO *frame,
                                         const void *data);
bool tirtc_demo_media_acquire_remote_video(
    tirtc_demo_remote_video_t *video);
void tirtc_demo_media_release_remote_video(uint32_t token);
void tirtc_demo_media_get_stats(tirtc_demo_media_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
