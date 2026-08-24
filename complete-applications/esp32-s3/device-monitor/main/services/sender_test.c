#include "sender_test.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "virtual_audio_source.h"
#include "network.h"
#include "platform_task_reaper.h"
#include "tirtc_session.h"
#include "tiRTC.h"

void tirtc_session_refresh_media_policy(void);

static const char *TAG = "sender_test";

#define SENDER_TEST_TASK_STACK            (24 * 1024)
#define SENDER_TEST_TASK_PRIORITY         10
#define SENDER_TEST_TASK_CORE             1
#define SENDER_TEST_INVALID_STREAM_ID     0xFFU
#define SENDER_TEST_WAIT_POLL_MS          5
#define SENDER_TEST_AUDIO_WAIT_LOG_US     2000000ULL
#define SENDER_TEST_AUDIO_CATCH_UP_BURST  12U
#define SENDER_TEST_AUDIO_JANK_THRESHOLD_US 30000ULL
#define SENDER_TEST_OK                    VIRTUAL_AUDIO_OK
#define SENDER_TEST_ERR_INVALID_ARG       VIRTUAL_AUDIO_ERR_INVALID_ARG
#define SENDER_TEST_ERR_IO                VIRTUAL_AUDIO_ERR_IO

#define SENDER_TEST_STAGE_LEN 64

typedef struct {
    uint64_t started_at_ms;
    uint64_t ended_at_ms;
    uint64_t bytes_sent;
    uint64_t frames_sent;
    uint64_t send_failures;
    int last_error_code;
    char last_error_stage[SENDER_TEST_STAGE_LEN];
} sender_test_stats_t;

static uint64_t sender_test_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

static void sender_test_sleep_ms(int ms)
{
    if (ms <= 0) {
        return;
    }

    TickType_t ticks = pdMS_TO_TICKS((uint32_t)ms);
    vTaskDelay(ticks > 0 ? ticks : 1);
}

static void sender_test_stats_init(sender_test_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    memset(stats, 0, sizeof(*stats));
    stats->started_at_ms = sender_test_now_ms();
}

static void sender_test_stats_mark_error(sender_test_stats_t *stats, int code, const char *stage)
{
    if (stats == NULL) {
        return;
    }

    stats->last_error_code = code;
    strlcpy(stats->last_error_stage, stage != NULL ? stage : "", sizeof(stats->last_error_stage));
}

typedef struct {
    virtual_audio_source_t source;
    sender_test_stats_t stats;
    uint64_t next_send_at_us;
    uint64_t media_pts_us;
    uint64_t last_sent_media_pts_us;
    uint32_t jank_samples;
    uint32_t jank_count;
    bool jank_window_active;
    uint32_t pending_packet_duration_us;
    int packet_pending;
    int packet_enqueued;
    uint8_t pending_send_failures;
    int first_packet_read_logged;
    int first_packet_sent_logged;
    uint64_t last_wait_log_us;
} sender_test_audio_session_t;

static portMUX_TYPE s_sender_test_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_sender_test_task;
static bool s_sender_test_initialized;
static bool s_sender_test_running;
static bool s_sender_test_restart_requested;
static bool s_sender_test_stop_requested;
static bool s_sender_test_force_audio_restart;
static bool s_sender_test_use_alaw;
static sender_test_mode_t s_sender_test_requested_mode = SENDER_TEST_MODE_NONE;
static char s_sender_test_status[SENDER_TEST_STATUS_MAX] = "Idle";
static EXT_RAM_BSS_ATTR sender_test_stats_t s_sender_test_live_stats;
static EXT_RAM_BSS_ATTR uint32_t s_sender_test_last_sequence;
static EXT_RAM_BSS_ATTR bool s_sender_test_last_sequence_valid;

static void sender_test_task_entry(void *ctx);

static void sender_test_log_audio_jank_summary(sender_test_audio_session_t *session,
                                                        const char *reason)
{
    if (session == NULL || !session->jank_window_active) {
        return;
    }

    if (session->jank_count > 0U && session->jank_samples > 0U) {
        ESP_LOGW(TAG,
                 "sender test audio jank: rate=%.2f count=%lu samples=%lu threshold_us=%lu frames=%llu bytes=%llu reason=%s",
                 (double)session->jank_count * 100.0 / (double)session->jank_samples,
                 (unsigned long)session->jank_count,
                 (unsigned long)session->jank_samples,
                 (unsigned long)SENDER_TEST_AUDIO_JANK_THRESHOLD_US,
                 (unsigned long long)session->stats.frames_sent,
                 (unsigned long long)session->stats.bytes_sent,
                 reason != NULL ? reason : "unknown");
    } else if (session->jank_samples > 0U) {
        ESP_LOGD(TAG,
                 "sender test audio jank clear: samples=%lu threshold_us=%lu frames=%llu bytes=%llu reason=%s",
                 (unsigned long)session->jank_samples,
                 (unsigned long)SENDER_TEST_AUDIO_JANK_THRESHOLD_US,
                 (unsigned long long)session->stats.frames_sent,
                 (unsigned long long)session->stats.bytes_sent,
                 reason != NULL ? reason : "unknown");
    }

    session->jank_window_active = false;
    session->jank_samples = 0U;
    session->jank_count = 0U;
    session->last_sent_media_pts_us = 0U;
}

static void sender_test_log_heap(const char *stage)
{
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGW(TAG,
             "%s: heap internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             stage != NULL ? stage : "sender_test",
             (unsigned)internal_free,
             (unsigned)internal_largest,
             (unsigned)psram_free,
             (unsigned)psram_largest);
}

static BaseType_t sender_test_create_task(TaskHandle_t *task_handle)
{
#if CONFIG_FREERTOS_UNICORE
    return xTaskCreateWithCaps(sender_test_task_entry,
                               "sender_test",
                               SENDER_TEST_TASK_STACK,
                               NULL,
                               SENDER_TEST_TASK_PRIORITY,
                               task_handle,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    BaseType_t task_ok = xTaskCreatePinnedToCoreWithCaps(sender_test_task_entry,
                                                         "sender_test",
                                                         SENDER_TEST_TASK_STACK,
                                                         NULL,
                                                         SENDER_TEST_TASK_PRIORITY,
                                                         task_handle,
                                                         SENDER_TEST_TASK_CORE,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (task_ok != pdPASS) {
        task_ok = xTaskCreateWithCaps(sender_test_task_entry,
                                      "sender_test",
                                      SENDER_TEST_TASK_STACK,
                                      NULL,
                                      SENDER_TEST_TASK_PRIORITY,
                                      task_handle,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return task_ok;
#endif
}

static const char *sender_test_mode_label(sender_test_mode_t mode)
{
    return mode == SENDER_TEST_MODE_AUDIO ? "Audio" : "Sender";
}

static uint64_t sender_test_now_us(void)
{
    struct timeval tv = {0};

    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static bool sender_test_should_log_audio_wait(sender_test_audio_session_t *session, uint64_t now_us)
{
    if (session == NULL) {
        return false;
    }
    if (session->last_wait_log_us == 0U || now_us - session->last_wait_log_us >= SENDER_TEST_AUDIO_WAIT_LOG_US) {
        session->last_wait_log_us = now_us;
        return true;
    }
    return false;
}

static int sender_test_audio_sleep_until(uint64_t target_us)
{
    uint64_t now_us = sender_test_now_us();
    uint64_t wait_us = 0U;

    if (target_us == 0U || now_us >= target_us) {
        return 0;
    }

    wait_us = target_us - now_us;

    if (wait_us >= 2000ULL) {
        uint32_t coarse_ms = (uint32_t)((wait_us - 1000ULL) / 1000ULL);
        if (coarse_ms > 0U) {
            return (int)coarse_ms;
        }
    }

    esp_rom_delay_us((uint32_t)wait_us);
    return 0;
}

static void sender_test_audio_reset_timing(sender_test_audio_session_t *session)
{
    if (session == NULL) {
        return;
    }

    session->next_send_at_us = 0U;
    session->media_pts_us = 0U;
    session->packet_pending = 0;
    session->packet_enqueued = 0;
    session->pending_send_failures = 0;
}

static void sender_test_set_status_locked(const char *text)
{
    strlcpy(s_sender_test_status, text != NULL ? text : "", sizeof(s_sender_test_status));
}

static void sender_test_set_status(const char *fmt, ...)
{
    char text[SENDER_TEST_STATUS_MAX] = {0};
    va_list args;

    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    taskENTER_CRITICAL(&s_sender_test_lock);
    sender_test_set_status_locked(text);
    taskEXIT_CRITICAL(&s_sender_test_lock);
}

static bool sender_test_restart_pending(void)
{
    bool restart_requested = false;

    taskENTER_CRITICAL(&s_sender_test_lock);
    restart_requested = s_sender_test_restart_requested || s_sender_test_stop_requested;
    taskEXIT_CRITICAL(&s_sender_test_lock);
    return restart_requested;
}

static bool sender_test_consume_audio_restart_request(void)
{
    bool restart_requested = false;

    taskENTER_CRITICAL(&s_sender_test_lock);
    restart_requested = s_sender_test_force_audio_restart;
    s_sender_test_force_audio_restart = false;
    taskEXIT_CRITICAL(&s_sender_test_lock);
    return restart_requested;
}

static esp_err_t sender_test_validate_mode(sender_test_mode_t mode)
{
    if (mode != SENDER_TEST_MODE_AUDIO) {
        sender_test_set_status("Invalid test mode");
        return ESP_ERR_INVALID_ARG;
    }

    if (!network_is_connected()) {
        sender_test_set_status("Connect WiFi first");
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

static void sender_test_publish_audio_progress(const sender_test_audio_session_t *session)
{
    if (session == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_sender_test_lock);
    s_sender_test_live_stats = session->stats;
    s_sender_test_last_sequence = session->source.last_sequence;
    s_sender_test_last_sequence_valid = session->source.last_sequence_valid;
    taskEXIT_CRITICAL(&s_sender_test_lock);
}

static void sender_test_audio_reset_source(sender_test_audio_session_t *session)
{
    if (session == NULL) {
        return;
    }

    virtual_audio_source_reset(&session->source);
    session->packet_pending = 0;
    session->pending_packet_duration_us = 0U;
    sender_test_audio_reset_timing(session);
}

static int sender_test_audio_reopen_source(sender_test_audio_session_t *session)
{
    if (session == NULL) {
        return SENDER_TEST_ERR_INVALID_ARG;
    }

    virtual_audio_source_close(&session->source);

    int rc = virtual_audio_source_open(&session->source);
    if (rc != SENDER_TEST_OK) {
        sender_test_stats_mark_error(&session->stats, rc, "audio_reopen_virtual");
        return rc;
    }

    session->packet_pending = 0;
    session->pending_packet_duration_us = 0U;
    sender_test_audio_reset_timing(session);
    return SENDER_TEST_OK;
}

static int sender_test_audio_send_one_packet(sender_test_audio_session_t *session)
{
    const uint8_t *data_ptr = NULL;
    size_t data_len = 0U;
    const tirtc_session_audio_format_t *format = NULL;
    uint64_t now_us = 0U;
    uint64_t scheduled_send_at_us = 0U;
    int rc = SENDER_TEST_OK;
    tirtc_session_stats_t rtc_stats = {0};

    if (session == NULL) {
        return SENDER_TEST_ERR_INVALID_ARG;
    }

    now_us = sender_test_now_us();
    if (sender_test_consume_audio_restart_request()) {
        session->packet_pending = 0;
        session->packet_enqueued = 0;
        session->pending_send_failures = 0;
        session->first_packet_sent_logged = 0;
        sender_test_audio_reset_source(session);
        now_us = sender_test_now_us();
    }

    if (session->packet_pending && session->next_send_at_us == 0U) {
        session->next_send_at_us = now_us;
    }

    if (session->packet_pending && now_us < session->next_send_at_us) {
        return SENDER_TEST_OK;
    }

    if (!session->packet_pending) {
        rc = virtual_audio_source_next_packet(&session->source,
                                              &data_ptr,
                                              &data_len,
                                              &format,
                                              &session->pending_packet_duration_us);
        if (rc == SENDER_TEST_ERR_IO) {
            if (sender_test_should_log_audio_wait(session, now_us)) {
                ESP_LOGW(TAG, "sender test virtual audio source error: reopen and retry");
            }
            return sender_test_audio_reopen_source(session);
        }
        if (rc != SENDER_TEST_OK) {
            sender_test_stats_mark_error(&session->stats, rc, "audio_next_packet");
            return rc;
        }
        session->first_packet_read_logged = 1;
        session->packet_pending = 1;
        if (session->next_send_at_us == 0U) {
            session->next_send_at_us = now_us;
        }
        if (session->media_pts_us == 0U) {
            session->media_pts_us = session->next_send_at_us;
        }
    }

    if (now_us < session->next_send_at_us) {
        return SENDER_TEST_OK;
    }

    scheduled_send_at_us = session->next_send_at_us;
    data_ptr = session->source.packet_buffer;
    data_len = session->source.packet_length;
    format = format != NULL ? format : &session->source.format;

    bool use_alaw = false;
    taskENTER_CRITICAL(&s_sender_test_lock);
    use_alaw = s_sender_test_use_alaw;
    taskEXIT_CRITICAL(&s_sender_test_lock);
    esp_err_t send_ret = use_alaw ?
        tirtc_session_send_test_audio_alaw_frame(data_ptr,
                                                 data_len,
                                                 format,
                                                 session->media_pts_us) :
        tirtc_session_send_test_audio_pcm_frame(data_ptr,
                                                data_len,
                                                format,
                                                session->media_pts_us);
    if (send_ret == ESP_ERR_INVALID_STATE) {
        tirtc_session_get_stats(&rtc_stats);
        if (sender_test_should_log_audio_wait(session, now_us)) {
            ESP_LOGD(TAG,
                     "sender test audio waiting: ret=%s len=%u stream=%u active=%d call=%d sdk=%d tx_audio=%lu tx_fail=%lu last=%s",
                     esp_err_to_name(send_ret),
                     (unsigned)session->source.packet_length,
                     (unsigned)rtc_stats.local_audio_stream_id,
                     rtc_stats.active_connection,
                     rtc_stats.call_active,
                     rtc_stats.sdk_started,
                     (unsigned long)rtc_stats.tx_audio_frames,
                     (unsigned long)rtc_stats.tx_failures,
                     rtc_stats.last_event);
        }
        if (!rtc_stats.sdk_started) {
            sender_test_set_status("Audio ready, waiting RTC");
        } else if (!rtc_stats.active_connection || !rtc_stats.call_active) {
            sender_test_set_status("Audio ready, waiting connection");
            session->next_send_at_us = 0U;
        } else {
            sender_test_set_status("Audio ready, waiting media");
            if (!session->first_packet_sent_logged) {
                session->packet_pending = 0;
                session->packet_enqueued = 0;
                session->pending_send_failures = 0;
                session->next_send_at_us = now_us;
                session->media_pts_us = now_us;
            } else {
                session->next_send_at_us = scheduled_send_at_us;
            }
        }
        return SENDER_TEST_OK;
    }
    if (send_ret == ESP_ERR_TIMEOUT) {
        if (sender_test_should_log_audio_wait(session, now_us)) {
            ESP_LOGW(TAG, "sender test audio queue full: len=%u", (unsigned)data_len);
        }
        if (!session->first_packet_sent_logged) {
            session->packet_pending = 0;
            session->packet_enqueued = 0;
            session->pending_send_failures = 0;
            session->next_send_at_us = now_us;
            session->media_pts_us = now_us;
        } else {
            session->next_send_at_us = scheduled_send_at_us;
        }
        return SENDER_TEST_OK;
    }
    if (send_ret != ESP_OK) {
        if (sender_test_should_log_audio_wait(session, now_us)) {
            ESP_LOGW(TAG,
                     "sender test audio enqueue failed: ret=%s len=%u",
                     esp_err_to_name(send_ret),
                     (unsigned)data_len);
        }
        session->stats.send_failures++;
        if (!session->first_packet_sent_logged) {
            session->packet_pending = 0;
            session->packet_enqueued = 0;
            session->pending_send_failures = 0;
            session->next_send_at_us = now_us;
            session->media_pts_us = now_us;
        } else {
            session->next_send_at_us = scheduled_send_at_us;
        }
        return SENDER_TEST_OK;
    }
    if (!session->first_packet_sent_logged) {
        session->first_packet_sent_logged = 1;
        session->jank_window_active = true;
        sender_test_set_status("Audio uploading...");
    } else {
        uint64_t interval_us = session->media_pts_us - session->last_sent_media_pts_us;
        session->jank_samples++;
        if (interval_us >= SENDER_TEST_AUDIO_JANK_THRESHOLD_US) {
            session->jank_count++;
        }
    }

    session->last_sent_media_pts_us = session->media_pts_us;

    session->stats.frames_sent++;
    session->stats.bytes_sent += data_len;
    sender_test_publish_audio_progress(session);
    session->packet_pending = 0;
    session->packet_enqueued = 0;
    session->pending_send_failures = 0;
    session->next_send_at_us = scheduled_send_at_us + session->pending_packet_duration_us;
    session->media_pts_us += session->pending_packet_duration_us;
    return SENDER_TEST_OK;
}

static int sender_test_run_audio(sender_test_stats_t *stats)
{
    sender_test_audio_session_t session = {0};
    int rc = SENDER_TEST_OK;

    if (stats == NULL) {
        return SENDER_TEST_ERR_INVALID_ARG;
    }

    sender_test_stats_init(&session.stats);
    rc = virtual_audio_source_open(&session.source);
    if (rc != SENDER_TEST_OK) {
        sender_test_stats_mark_error(&session.stats, rc, "audio_open_virtual");
        *stats = session.stats;
        return rc;
    }

    sender_test_set_status("Audio ready, waiting connection");

    while (!sender_test_restart_pending()) {
        tirtc_session_stats_t rtc_stats = {0};
        bool packet_sent = false;
        uint32_t burst_count = 0U;

        tirtc_session_get_stats(&rtc_stats);
        if (!rtc_stats.sdk_started) {
            sender_test_log_audio_jank_summary(&session, "rtc-stopped");
            sender_test_set_status("Audio ready, waiting RTC");
            sender_test_audio_reset_timing(&session);
            sender_test_sleep_ms(SENDER_TEST_WAIT_POLL_MS);
            continue;
        }
        if (!rtc_stats.active_connection || !rtc_stats.call_active) {
            sender_test_log_audio_jank_summary(&session, "disconnect");
            sender_test_set_status("Audio ready, waiting connection");
            sender_test_audio_reset_timing(&session);
            sender_test_sleep_ms(SENDER_TEST_WAIT_POLL_MS);
            continue;
        }

        do {
            uint32_t frames_before = session.stats.frames_sent;

            rc = sender_test_audio_send_one_packet(&session);
            if (rc != SENDER_TEST_OK) {
                if (session.stats.last_error_code == 0) {
                    sender_test_stats_mark_error(&session.stats, rc, "audio_session_run");
                }
                break;
            }

            packet_sent = session.stats.frames_sent != frames_before;
            if (!packet_sent) {
                break;
            }

            burst_count++;
        } while (burst_count < SENDER_TEST_AUDIO_CATCH_UP_BURST &&
                 !sender_test_restart_pending() &&
                 session.next_send_at_us != 0U &&
                 sender_test_now_us() >= session.next_send_at_us);

        if (rc != SENDER_TEST_OK) {
            break;
        }

        if (packet_sent) {
            int wait_ms = sender_test_audio_sleep_until(session.next_send_at_us);
            if (wait_ms > 0) {
                sender_test_sleep_ms(wait_ms);
            }
        } else {
            sender_test_sleep_ms(SENDER_TEST_WAIT_POLL_MS);
        }
    }

    session.stats.ended_at_ms = sender_test_now_ms();
    sender_test_publish_audio_progress(&session);
    virtual_audio_source_close(&session.source);
    sender_test_log_audio_jank_summary(&session, "exit");
    *stats = session.stats;
    return rc;
}

static void sender_test_finalize_run(sender_test_mode_t mode,
                                              int rc,
                                              const sender_test_stats_t *stats,
                                              bool restart_requested,
                                              bool stop_requested)
{
    bool refresh_policy = false;

    taskENTER_CRITICAL(&s_sender_test_lock);
    if (!restart_requested) {
        s_sender_test_running = false;
        s_sender_test_task = NULL;
        s_sender_test_requested_mode = SENDER_TEST_MODE_NONE;
        s_sender_test_stop_requested = false;
        refresh_policy = true;
        if (stop_requested) {
            sender_test_set_status_locked("Stopped");
        } else if (rc == SENDER_TEST_OK) {
            sender_test_set_status_locked("Audio exited");
        } else if (stats != NULL) {
            snprintf(s_sender_test_status,
                     sizeof(s_sender_test_status),
                     "%s failed %s (%d)",
                     sender_test_mode_label(mode),
                     stats->last_error_stage[0] != '\0' ? stats->last_error_stage : "unknown",
                     stats->last_error_code != 0 ? stats->last_error_code : rc);
        }
    }
    taskEXIT_CRITICAL(&s_sender_test_lock);

    if (refresh_policy) {
        tirtc_session_refresh_media_policy();
    }
}

static void sender_test_task_entry(void *ctx)
{
    (void)ctx;

    while (true) {
        sender_test_mode_t mode = SENDER_TEST_MODE_NONE;
        sender_test_stats_t stats = {0};
        int rc = SENDER_TEST_OK;
        bool restart_requested = false;
        bool stop_requested = false;

        taskENTER_CRITICAL(&s_sender_test_lock);
        mode = s_sender_test_requested_mode;
        s_sender_test_restart_requested = false;
        s_sender_test_force_audio_restart = false;
        taskEXIT_CRITICAL(&s_sender_test_lock);

        if (!s_sender_test_initialized) {
            esp_err_t init_ret = sender_test_init();
            if (init_ret != ESP_OK) {
                sender_test_set_status("Sender init failed (%s)", esp_err_to_name(init_ret));
                sender_test_finalize_run(mode, SENDER_TEST_ERR_IO, &stats, false, false);
                break;
            }
        }

        if (sender_test_validate_mode(mode) != ESP_OK) {
            sender_test_finalize_run(mode, SENDER_TEST_ERR_INVALID_ARG, &stats, false, false);
            break;
        }

        sender_test_set_status("Audio ready, waiting connection");
        rc = sender_test_run_audio(&stats);

        taskENTER_CRITICAL(&s_sender_test_lock);
        restart_requested = s_sender_test_restart_requested;
        stop_requested = s_sender_test_stop_requested;
        taskEXIT_CRITICAL(&s_sender_test_lock);
        if (restart_requested && !stop_requested) {
            sender_test_finalize_run(mode, rc, &stats, true, false);
            continue;
        }

        sender_test_finalize_run(mode, rc, &stats, false, stop_requested);
        break;
    }

    platform_task_reaper_delete_current_with_caps(TAG);
}

esp_err_t sender_test_init(void)
{
    if (s_sender_test_initialized) {
        return ESP_OK;
    }

    sender_test_set_status("Virtual audio ready");
    s_sender_test_initialized = true;
    return ESP_OK;
}

esp_err_t sender_test_start(sender_test_mode_t mode)
{
    BaseType_t task_ok = pdFAIL;
    bool should_restart = false;

    if (mode != SENDER_TEST_MODE_AUDIO) {
        sender_test_set_status("Invalid test mode");
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_sender_test_lock);
    s_sender_test_requested_mode = mode;
    s_sender_test_stop_requested = false;
    memset(&s_sender_test_live_stats, 0, sizeof(s_sender_test_live_stats));
    s_sender_test_last_sequence = 0U;
    s_sender_test_last_sequence_valid = false;
    if (s_sender_test_task != NULL) {
        s_sender_test_running = true;
        s_sender_test_restart_requested = true;
        should_restart = true;
        sender_test_set_status_locked("Restarting audio source...");
    } else {
        s_sender_test_running = true;
        s_sender_test_restart_requested = false;
        sender_test_set_status_locked("Launching test source...");
    }
    taskEXIT_CRITICAL(&s_sender_test_lock);

    if (should_restart) {
        return ESP_OK;
    }

    task_ok = sender_test_create_task(&s_sender_test_task);
    if (task_ok != pdPASS) {
        sender_test_log_heap("sender task alloc failed");
        taskENTER_CRITICAL(&s_sender_test_lock);
        s_sender_test_running = false;
        s_sender_test_task = NULL;
        sender_test_set_status_locked("Sender task alloc failed");
        taskEXIT_CRITICAL(&s_sender_test_lock);
        return ESP_ERR_NO_MEM;
    }

    tirtc_session_refresh_media_policy();

    return ESP_OK;
}

void sender_test_set_audio_alaw(bool enabled)
{
    taskENTER_CRITICAL(&s_sender_test_lock);
    s_sender_test_use_alaw = enabled;
    taskEXIT_CRITICAL(&s_sender_test_lock);
}

void sender_test_stop(void)
{
    bool refresh_policy = false;

    taskENTER_CRITICAL(&s_sender_test_lock);
    if (s_sender_test_task != NULL || s_sender_test_running) {
        s_sender_test_stop_requested = true;
        s_sender_test_restart_requested = false;
        s_sender_test_force_audio_restart = false;
        s_sender_test_running = false;
        s_sender_test_requested_mode = SENDER_TEST_MODE_NONE;
        sender_test_set_status_locked("Stopping...");
        refresh_policy = true;
    } else {
        s_sender_test_stop_requested = false;
        s_sender_test_requested_mode = SENDER_TEST_MODE_NONE;
    }
    taskEXIT_CRITICAL(&s_sender_test_lock);

    if (refresh_policy) {
        tirtc_session_refresh_media_policy();
    }
}

bool sender_test_is_mode_active(sender_test_mode_t mode)
{
    bool active = false;

    taskENTER_CRITICAL(&s_sender_test_lock);
    active = s_sender_test_running && s_sender_test_requested_mode == mode;
    taskEXIT_CRITICAL(&s_sender_test_lock);

    return active;
}

void sender_test_request_audio_restart(void)
{
    taskENTER_CRITICAL(&s_sender_test_lock);
    if (s_sender_test_running && s_sender_test_requested_mode == SENDER_TEST_MODE_AUDIO) {
        s_sender_test_force_audio_restart = true;
    }
    taskEXIT_CRITICAL(&s_sender_test_lock);
}

void sender_test_get_snapshot(sender_test_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));

    taskENTER_CRITICAL(&s_sender_test_lock);
    snapshot->running = s_sender_test_running;
    snapshot->use_alaw = s_sender_test_use_alaw;
    snapshot->frames_sent = s_sender_test_live_stats.frames_sent;
    snapshot->bytes_sent = s_sender_test_live_stats.bytes_sent;
    snapshot->send_failures = s_sender_test_live_stats.send_failures;
    snapshot->last_sequence = s_sender_test_last_sequence;
    snapshot->last_sequence_valid = s_sender_test_last_sequence_valid;
    strlcpy(snapshot->status, s_sender_test_status, sizeof(snapshot->status));
    taskEXIT_CRITICAL(&s_sender_test_lock);
}
