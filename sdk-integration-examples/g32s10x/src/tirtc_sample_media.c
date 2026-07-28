#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <driver/systick.h>
#include <os/thread.h>
#include <spinlock.h>

#include "tirtc_link_config.h"
#include "tirtc_link_defaults.h"
#include "tirtc_sample_avi.h"
#include "tirtc_sample_media.h"
#include "tirtc_test_log.h"

#define SAMPLE_AUDIO_PACKET_BYTES \
    (TIRTC_SAMPLE_AVI_AUDIO_BYTES_PER_MS * \
     TIRTC_LINK_SAMPLE_AUDIO_PACKET_MS)

extern const uint8_t g_tirtc_sample_asset_start[];
extern const uint8_t g_tirtc_sample_asset_end[];

typedef struct {
    tirtc_conn_t connection;
    uint32_t generation;
    uint64_t connected_at_ms;
    bool video_control_seen;
    bool video_subscribed;
    uint8_t video_stream_id;
    bool audio_control_seen;
    bool audio_subscribed;
    uint8_t audio_stream_id;
    uint32_t video_generation;
    uint32_t audio_generation;
} sample_target_t;

static DEFINE_SPINLOCK(g_sample_lock);
static thread_ptr_t g_sample_worker;
static tirtc_sample_media_ops_t g_sample_ops;
static sample_target_t g_sample_target;
static tirtc_sample_media_status_t g_sample_status;
static tirtc_sample_avi_t g_sample_avi;

static void sample_lock(unsigned long *flags)
{
    spin_lock_irqsave(&g_sample_lock, *flags);
}

static void sample_unlock(unsigned long flags)
{
    spin_unlock_irqrestore(&g_sample_lock, flags);
}

static sample_target_t sample_get_target(void)
{
    sample_target_t target;
    unsigned long flags;

    sample_lock(&flags);
    target = g_sample_target;
    sample_unlock(flags);
    return target;
}

static bool sample_record_send(int result, bool video)
{
    bool first_success = false;
    unsigned long flags;

    sample_lock(&flags);
    if (result >= 0) {
        if (video) {
            first_success = g_sample_status.sent_video_frames == 0U;
            ++g_sample_status.sent_video_frames;
        } else {
            first_success = g_sample_status.sent_audio_packets == 0U;
            ++g_sample_status.sent_audio_packets;
        }
        g_sample_status.last_error = 0;
    } else {
        ++g_sample_status.dropped_packets;
        g_sample_status.last_error = result;
    }
    sample_unlock(flags);
    return first_success;
}

static int sample_send_video(const sample_target_t *target,
                             tirtc_sample_avi_cursor_t *cursor,
                             uint32_t *next_timestamp_ms)
{
    const uint8_t *data;
    uint32_t length;
    uint32_t timestamp_ms;
    TIRTCFRAMEINFO frame = {0};
    uint8_t stream_id;
    size_t used;
    int result;

    if (!tirtc_sample_avi_next_video(&g_sample_avi, cursor, &data, &length,
                                     &timestamp_ms)) {
        return TIRTC_E_INVALID_PARAMETER;
    }
    *next_timestamp_ms =
        tirtc_sample_avi_video_position_ms(&g_sample_avi, cursor);
    stream_id = target->video_subscribed ? target->video_stream_id :
                                           TIRTC_LINK_SAMPLE_VIDEO_STREAM_ID;
    used = g_sample_ops.send_buffer_used(target->connection);
    if (used >= (TIRTC_LINK_MAX_SEND_BUFFER *
                 TIRTC_LINK_SAMPLE_VIDEO_BUFFER_PERCENT) / 100U) {
        (void)sample_record_send(TIRTC_E_BUSY, true);
        return TIRTC_E_BUSY;
    }
    frame.stream_id = stream_id;
    frame.media = TIRTC_VIDEO_JPEG;
    frame.flags = TIRTC_FRAME_FLAG_KEY_FRAME;
    frame.ts = timestamp_ms;
    frame.length = length;
    result = g_sample_ops.send_video(target->connection, &frame, data);
    if (sample_record_send(result, true)) {
        printf("[TEST][PASS] 视频输出 | 首帧已发送 stream=%u bytes=%lu\n",
               (unsigned)stream_id, (unsigned long)length);
    }
    return result;
}

static int sample_send_audio(const sample_target_t *target,
                             tirtc_sample_avi_cursor_t *cursor,
                             uint32_t *next_timestamp_ms)
{
    uint8_t data[SAMPLE_AUDIO_PACKET_BYTES];
    uint32_t length;
    uint32_t timestamp_ms;
    TIRTCFRAMEINFO frame = {0};
    uint8_t stream_id;
    size_t used;
    int result;

    if (!tirtc_sample_avi_next_audio(&g_sample_avi, cursor, data,
                                     sizeof(data), &length, &timestamp_ms)) {
        return TIRTC_E_INVALID_PARAMETER;
    }
    *next_timestamp_ms =
        tirtc_sample_avi_audio_position_ms(&g_sample_avi, cursor);
    stream_id = target->audio_subscribed ? target->audio_stream_id :
                                           TIRTC_LINK_SAMPLE_AUDIO_STREAM_ID;
    used = g_sample_ops.send_buffer_used(target->connection);
    if (used >= (TIRTC_LINK_MAX_SEND_BUFFER *
                 TIRTC_LINK_SAMPLE_AUDIO_BUFFER_PERCENT) / 100U) {
        (void)sample_record_send(TIRTC_E_BUSY, false);
        return TIRTC_E_BUSY;
    }
    frame.stream_id = stream_id;
    frame.media = TIRTC_AUDIO_ALAW;
    frame.flags = TIRTC_AUDIOSAMPLE_8K16B1C;
    frame.ts = timestamp_ms;
    frame.length = length;
    result = g_sample_ops.send_audio(target->connection, &frame, data);
    if (sample_record_send(result, false)) {
        printf("[TEST][PASS] 音频输出 | 首包已发送 stream=%u bytes=%lu\n",
               (unsigned)stream_id, (unsigned long)length);
    }
    return result;
}

static void sample_update_active_status(bool video_active, bool audio_active)
{
    unsigned long flags;

    sample_lock(&flags);
    g_sample_status.video_active = video_active;
    g_sample_status.audio_active = audio_active;
    sample_unlock(flags);
}

static void sample_worker(void *data)
{
    tirtc_sample_avi_cursor_t video_cursor;
    tirtc_sample_avi_cursor_t audio_cursor;
    uint32_t generation = UINT32_MAX;
    uint32_t video_generation = UINT32_MAX;
    uint32_t audio_generation = UINT32_MAX;
    uint64_t next_video_ms = 0U;
    uint64_t next_audio_ms = 0U;

    (void)data;
    tirtc_sample_avi_cursor_reset(&g_sample_avi, &video_cursor);
    tirtc_sample_avi_cursor_reset(&g_sample_avi, &audio_cursor);
    for (;;) {
        sample_target_t target = sample_get_target();
        uint64_t now_ms = systick_get_time_ms();
        bool startup_ready;
        bool video_active;
        bool audio_active;

        if (target.generation != generation) {
            generation = target.generation;
            tirtc_sample_avi_cursor_reset(&g_sample_avi, &video_cursor);
            tirtc_sample_avi_cursor_reset(&g_sample_avi, &audio_cursor);
            next_video_ms = target.connected_at_ms + TIRTC_LINK_SAMPLE_START_DELAY_MS;
            next_audio_ms = next_video_ms;
        }
        if (target.video_generation != video_generation) {
            video_generation = target.video_generation;
            tirtc_sample_avi_cursor_reset(&g_sample_avi, &video_cursor);
            next_video_ms = target.connection != NULL ?
                            target.connected_at_ms +
                            TIRTC_LINK_SAMPLE_START_DELAY_MS : now_ms;
        }
        if (target.audio_generation != audio_generation) {
            audio_generation = target.audio_generation;
            tirtc_sample_avi_cursor_reset(&g_sample_avi, &audio_cursor);
            next_audio_ms = target.connection != NULL ?
                            target.connected_at_ms +
                            TIRTC_LINK_SAMPLE_START_DELAY_MS : now_ms;
        }
        startup_ready = target.connection != NULL &&
                        now_ms >= target.connected_at_ms +
                                  TIRTC_LINK_SAMPLE_START_DELAY_MS;
        video_active = startup_ready &&
                       (target.video_subscribed ||
                        (TIRTC_LINK_SAMPLE_AUTO_PUBLISH &&
                         !target.video_control_seen));
        audio_active = startup_ready &&
                       (target.audio_subscribed ||
                        (TIRTC_LINK_SAMPLE_AUTO_PUBLISH &&
                         !target.audio_control_seen));
        sample_update_active_status(video_active, audio_active);

        if (video_active && now_ms >= next_video_ms) {
            uint32_t next_timestamp_ms = 0U;

            (void)sample_send_video(&target, &video_cursor,
                                    &next_timestamp_ms);
            next_video_ms = target.connected_at_ms +
                            TIRTC_LINK_SAMPLE_START_DELAY_MS +
                            next_timestamp_ms;
            if (next_video_ms <= now_ms) {
                next_video_ms = now_ms + 1U;
            }
        }
        if (audio_active && now_ms >= next_audio_ms) {
            uint32_t next_timestamp_ms = 0U;

            (void)sample_send_audio(&target, &audio_cursor,
                                    &next_timestamp_ms);
            next_audio_ms = target.connected_at_ms +
                            TIRTC_LINK_SAMPLE_START_DELAY_MS +
                            next_timestamp_ms;
            if (next_audio_ms <= now_ms) {
                next_audio_ms = now_ms + 1U;
            }
        }
        (void)thread_wait_timeout(TIRTC_LINK_SAMPLE_WORKER_POLL_MS);
    }
}

int tirtc_sample_media_init(const tirtc_sample_media_ops_t *ops)
{
    thread_ptr_t worker;
    unsigned long flags;
    size_t asset_length;

    if (ops == NULL || ops->send_video == NULL || ops->send_audio == NULL ||
        ops->send_buffer_used == NULL) {
        return TIRTC_E_INVALID_PARAMETER;
    }
    sample_lock(&flags);
    if (g_sample_worker != NULL) {
        sample_unlock(flags);
        return 0;
    }
    sample_unlock(flags);
    asset_length = (size_t)((uintptr_t)g_tirtc_sample_asset_end -
                            (uintptr_t)g_tirtc_sample_asset_start);
    if (asset_length == 0U || asset_length > UINT32_MAX) {
        return TIRTC_E_INVALID_PARAMETER;
    }
    g_sample_ops = *ops;
    if (!tirtc_sample_avi_open(&g_sample_avi, g_tirtc_sample_asset_start,
                               (uint32_t)asset_length,
                               TIRTC_LINK_MAX_SEND_BUFFER)) {
        tirtc_test_log_failure_detail(
            "演示素材", TIRTC_E_INVALID_PARAMETER,
            "AVI素材格式或边界无效", "恢复交付素材后重新编译");
        return TIRTC_E_INVALID_PARAMETER;
    }
    g_sample_status.asset_ready = true;
    g_sample_status.asset_video_frames = g_sample_avi.video_frames;
    g_sample_status.asset_audio_bytes = g_sample_avi.audio_bytes;
    worker = thread_create("tirtc_sample",
                           TIRTC_LINK_SAMPLE_WORKER_STACK_BYTES,
                           sample_worker, NULL);
    if (worker == NULL) {
        return TIRTC_E_LACK_OF_RESOURCE;
    }
    sample_lock(&flags);
    if (g_sample_worker == NULL) {
        g_sample_worker = worker;
        worker = NULL;
    }
    sample_unlock(flags);
    if (worker != NULL) {
        thread_delete(worker);
    }
    printf("[TEST][PASS] 演示素材 | MJPEG %ux%u@%ufps + PCMA 8kHz mono | bytes=%lu duration=%lums\n",
           (unsigned)TIRTC_SAMPLE_AVI_VIDEO_WIDTH,
           (unsigned)TIRTC_SAMPLE_AVI_VIDEO_HEIGHT,
           (unsigned)TIRTC_SAMPLE_AVI_VIDEO_FPS,
           (unsigned long)g_sample_avi.length,
           (unsigned long)g_sample_avi.duration_ms);
    return 0;
}

void tirtc_sample_media_set_connection(tirtc_conn_t connection)
{
    thread_ptr_t worker;
    unsigned long flags;

    sample_lock(&flags);
    if (connection != NULL && connection == g_sample_target.connection) {
        worker = g_sample_worker;
        sample_unlock(flags);
        if (worker != NULL) {
            thread_wakeup(worker);
        }
        return;
    }
    g_sample_target.connection = connection;
    ++g_sample_target.generation;
    g_sample_target.connected_at_ms = connection != NULL ?
                                      systick_get_time_ms() : 0U;
    g_sample_target.video_control_seen = false;
    g_sample_target.video_subscribed = false;
    g_sample_target.video_stream_id = TIRTC_LINK_SAMPLE_VIDEO_STREAM_ID;
    g_sample_target.audio_control_seen = false;
    g_sample_target.audio_subscribed = false;
    g_sample_target.audio_stream_id = TIRTC_LINK_SAMPLE_AUDIO_STREAM_ID;
    ++g_sample_target.video_generation;
    ++g_sample_target.audio_generation;
    g_sample_status.video_active = false;
    g_sample_status.audio_active = false;
    g_sample_status.sent_video_frames = 0U;
    g_sample_status.sent_audio_packets = 0U;
    g_sample_status.dropped_packets = 0U;
    g_sample_status.last_error = 0;
    worker = g_sample_worker;
    sample_unlock(flags);
    if (worker != NULL) {
        thread_wakeup(worker);
    }
}

void tirtc_sample_media_clear_connection(tirtc_conn_t connection)
{
    thread_ptr_t worker = NULL;
    unsigned long flags;

    sample_lock(&flags);
    if (connection != NULL && connection == g_sample_target.connection) {
        g_sample_target.connection = NULL;
        ++g_sample_target.generation;
        g_sample_target.connected_at_ms = 0U;
        g_sample_target.video_control_seen = false;
        g_sample_target.video_subscribed = false;
        g_sample_target.audio_control_seen = false;
        g_sample_target.audio_subscribed = false;
        ++g_sample_target.video_generation;
        ++g_sample_target.audio_generation;
        g_sample_status.video_active = false;
        g_sample_status.audio_active = false;
        worker = g_sample_worker;
    }
    sample_unlock(flags);
    if (worker != NULL) {
        thread_wakeup(worker);
    }
}

int tirtc_sample_media_subscribe_video(tirtc_conn_t connection,
                                       uint8_t stream_id)
{
    thread_ptr_t worker;
    bool accepted;
    unsigned long flags;

    sample_lock(&flags);
    if (connection != NULL && g_sample_target.connection == NULL) {
        g_sample_target.connection = connection;
        ++g_sample_target.generation;
        g_sample_target.connected_at_ms = systick_get_time_ms();
        g_sample_target.video_stream_id = TIRTC_LINK_SAMPLE_VIDEO_STREAM_ID;
        g_sample_target.audio_stream_id = TIRTC_LINK_SAMPLE_AUDIO_STREAM_ID;
        ++g_sample_target.video_generation;
        ++g_sample_target.audio_generation;
    }
    accepted = connection != NULL &&
               connection == g_sample_target.connection &&
               stream_id <= 15U &&
               (!g_sample_target.audio_subscribed ||
                stream_id != g_sample_target.audio_stream_id);
    if (accepted) {
        g_sample_target.video_control_seen = true;
        g_sample_target.video_subscribed = true;
        g_sample_target.video_stream_id = stream_id;
        ++g_sample_target.video_generation;
    }
    worker = g_sample_worker;
    sample_unlock(flags);
    if (worker != NULL) {
        thread_wakeup(worker);
    }
    return accepted ? 0 : TIRTC_E_INVALID_PARAMETER;
}

void tirtc_sample_media_unsubscribe_video(tirtc_conn_t connection,
                                          uint8_t stream_id)
{
    thread_ptr_t worker;
    unsigned long flags;

    sample_lock(&flags);
    if (connection == g_sample_target.connection &&
        stream_id == g_sample_target.video_stream_id) {
        g_sample_target.video_control_seen = true;
        g_sample_target.video_subscribed = false;
        ++g_sample_target.video_generation;
    }
    worker = g_sample_worker;
    sample_unlock(flags);
    if (worker != NULL) {
        thread_wakeup(worker);
    }
}

int tirtc_sample_media_subscribe_audio(tirtc_conn_t connection,
                                       uint8_t stream_id)
{
    thread_ptr_t worker;
    bool accepted;
    unsigned long flags;

    sample_lock(&flags);
    if (connection != NULL && g_sample_target.connection == NULL) {
        g_sample_target.connection = connection;
        ++g_sample_target.generation;
        g_sample_target.connected_at_ms = systick_get_time_ms();
        g_sample_target.video_stream_id = TIRTC_LINK_SAMPLE_VIDEO_STREAM_ID;
        g_sample_target.audio_stream_id = TIRTC_LINK_SAMPLE_AUDIO_STREAM_ID;
        ++g_sample_target.video_generation;
        ++g_sample_target.audio_generation;
    }
    accepted = connection != NULL &&
               connection == g_sample_target.connection &&
               stream_id <= 15U &&
               (!g_sample_target.video_subscribed ||
                stream_id != g_sample_target.video_stream_id);
    if (accepted) {
        g_sample_target.audio_control_seen = true;
        g_sample_target.audio_subscribed = true;
        g_sample_target.audio_stream_id = stream_id;
        ++g_sample_target.audio_generation;
    }
    worker = g_sample_worker;
    sample_unlock(flags);
    if (worker != NULL) {
        thread_wakeup(worker);
    }
    return accepted ? 0 : TIRTC_E_INVALID_PARAMETER;
}

void tirtc_sample_media_unsubscribe_audio(tirtc_conn_t connection,
                                          uint8_t stream_id)
{
    thread_ptr_t worker;
    unsigned long flags;

    sample_lock(&flags);
    if (connection == g_sample_target.connection &&
        stream_id == g_sample_target.audio_stream_id) {
        g_sample_target.audio_control_seen = true;
        g_sample_target.audio_subscribed = false;
        ++g_sample_target.audio_generation;
    }
    worker = g_sample_worker;
    sample_unlock(flags);
    if (worker != NULL) {
        thread_wakeup(worker);
    }
}

void tirtc_sample_media_get_status(tirtc_sample_media_status_t *status)
{
    unsigned long flags;

    if (status == NULL) {
        return;
    }
    sample_lock(&flags);
    *status = g_sample_status;
    sample_unlock(flags);
}
