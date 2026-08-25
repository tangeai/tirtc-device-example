#include "tirtc_session_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "sdkconfig.h"

#include "audio_alaw_codec.h"
#include "system_time.h"
#include "tirtc_connect.h"
#include "tirtc_session_options.h"
#include "tiRTC.h"
#include "tiRTC_stat.h"
#include "tgtrp.h"

static const char *TAG = "tirtc_session";
static const char *TIRTC_SDK_LOG_TAG = "tirtc_sdk";

#define TIRTC_SESSION_OBSERVER_MAX 4
#define TIRTC_SESSION_RX_LOG_INTERVAL_MS 1000U
#define TIRTC_SESSION_TX_LOG_INTERVAL_MS 1000U
#define TIRTC_SESSION_MESSAGE_PREVIEW_BYTES 8U
#define TIRTC_SESSION_MESSAGE_PREVIEW_TEXT_LEN ((TIRTC_SESSION_MESSAGE_PREVIEW_BYTES * 3U) + 1U)
#define TIRTC_SESSION_REMOTE_AUDIO_MAX_PAYLOAD 8192U
#define TIRTC_SESSION_BAD_REMOTE_AUDIO_LOG_INTERVAL_MS 5000U
#define TIRTC_SESSION_SHA256_HEX_LEN 65U
#define TIRTC_SESSION_ALAW_SAMPLE_RATE_HZ 8000U
#define TIRTC_SESSION_BUILTIN_AUDIO_STACK_ALAW_BYTES 512U
#define TIRTC_SESSION_CLIENT_ID_CONFLICT_RETRY_DELAY_US (60ULL * 1000ULL * 1000ULL)

#define TIRTC_RESERVED_CMD_REQUEST_KEY_FRAME_NEW   0x0400U
#define TIRTC_RESERVED_CMD_SUBSCRIBE_VIDEO_NEW     0x0402U
#define TIRTC_RESERVED_CMD_UNSUBSCRIBE_VIDEO_NEW   0x0404U
#define TIRTC_RESERVED_CMD_SUBSCRIBE_AUDIO_NEW     0x0406U
#define TIRTC_RESERVED_CMD_UNSUBSCRIBE_AUDIO_NEW   0x0408U
#define TIRTC_RESERVED_CMD_REQUEST_KEY_FRAME_OLD   0x0200U
#define TIRTC_RESERVED_CMD_SUBSCRIBE_VIDEO_OLD     0x0201U
#define TIRTC_RESERVED_CMD_UNSUBSCRIBE_VIDEO_OLD   0x0202U
#define TIRTC_RESERVED_CMD_SUBSCRIBE_AUDIO_OLD     0x0203U
#define TIRTC_RESERVED_CMD_UNSUBSCRIBE_AUDIO_OLD   0x0204U

static bool tirtc_session_sha256_hex(const char *input, char *out_hex, size_t out_size)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[32] = {0};

    if (input == NULL || out_hex == NULL || out_size < TIRTC_SESSION_SHA256_HEX_LEN) {
        return false;
    }

    if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                   (const unsigned char *)input,
                   strlen(input),
                   digest) != 0) {
        return false;
    }

    for (size_t i = 0; i < sizeof(digest); ++i) {
        out_hex[i * 2U] = hex[(digest[i] >> 4) & 0x0F];
        out_hex[i * 2U + 1U] = hex[digest[i] & 0x0F];
    }
    out_hex[sizeof(digest) * 2U] = '\0';
    return true;
}

static void tirtc_session_log_start_resources(const char *stage)
{
    ESP_LOGI(TAG,
             "rtc start resource: stage=%s internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u worker_stack_hwm=%u tcp_wnd=%u tcp_recvmbox=%u tcpip_recvmbox=%u tcpip_stack=%u",
             stage != NULL ? stage : "unknown",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)CONFIG_LWIP_TCP_WND_DEFAULT,
             (unsigned)CONFIG_LWIP_TCP_RECVMBOX_SIZE,
             (unsigned)CONFIG_LWIP_TCPIP_RECVMBOX_SIZE,
             (unsigned)CONFIG_LWIP_TCPIP_TASK_STACK_SIZE);
}

static void tirtc_session_log_connection_time_stats(tirtc_conn_t hconn, const char *stage)
{
    tgtrp_connection_time_stats_t stats = {0};
    int ret;

    if (hconn == NULL) {
        return;
    }

    ret = TiRtcConnGetTimeStats(hconn, &stats);
    if (ret < 0) {
        ESP_LOGW(TAG,
                 "rtc timing unavailable: stage=%s hconn=%p ret=%d",
                 stage != NULL ? stage : "unknown",
                 hconn,
                 ret);
        return;
    }

    ESP_LOGI(TAG,
             "rtc timing: stage=%s total_rtt=%lu/%lu/%lu/%lu/%lu total_oneway=%lu/%lu/%lu/%lu/%lu",
             stage != NULL ? stage : "unknown",
             (unsigned long)stats.total.debug_rtt.sample_count,
             (unsigned long)stats.total.debug_rtt.latest_ms,
             (unsigned long)stats.total.debug_rtt.min_ms,
             (unsigned long)stats.total.debug_rtt.max_ms,
             (unsigned long)stats.total.debug_rtt.avg_ms,
             (unsigned long)stats.total.debug_oneway_delay.sample_count,
             (unsigned long)stats.total.debug_oneway_delay.latest_ms,
             (unsigned long)stats.total.debug_oneway_delay.min_ms,
             (unsigned long)stats.total.debug_oneway_delay.max_ms,
             (unsigned long)stats.total.debug_oneway_delay.avg_ms);
    ESP_LOGI(TAG,
             "rtc audio timing: stage=%s rtt=%lu/%lu/%lu/%lu/%lu oneway=%lu/%lu/%lu/%lu/%lu",
             stage != NULL ? stage : "unknown",
             (unsigned long)stats.audio.debug_rtt.sample_count,
             (unsigned long)stats.audio.debug_rtt.latest_ms,
             (unsigned long)stats.audio.debug_rtt.min_ms,
             (unsigned long)stats.audio.debug_rtt.max_ms,
             (unsigned long)stats.audio.debug_rtt.avg_ms,
             (unsigned long)stats.audio.debug_oneway_delay.sample_count,
             (unsigned long)stats.audio.debug_oneway_delay.latest_ms,
             (unsigned long)stats.audio.debug_oneway_delay.min_ms,
             (unsigned long)stats.audio.debug_oneway_delay.max_ms,
             (unsigned long)stats.audio.debug_oneway_delay.avg_ms);
}

typedef enum {
    TIRTC_SESSION_AUDIO_TX_GATE_SUBSCRIBED = 0,
    TIRTC_SESSION_AUDIO_TX_GATE_CALL,
    TIRTC_SESSION_AUDIO_TX_GATE_TEST,
    TIRTC_SESSION_AUDIO_TX_GATE_TEST_ALAW,
} tirtc_session_audio_tx_gate_t;

typedef enum {
    TIRTC_SESSION_MEDIA_PROFILE_AV = 0,
    TIRTC_SESSION_MEDIA_PROFILE_EXTERNAL_AUDIO,
} tirtc_session_media_profile_t;

typedef struct {
    uint32_t generation;
    tirtc_conn_t expected_conn;
    tirtc_session_audio_format_t format;
    uint64_t pts_us;
    uint8_t *data;
    size_t data_len;
    tirtc_session_audio_tx_gate_t gate;
} tirtc_session_local_audio_packet_t;

typedef struct {
    uint32_t generation;
    uint16_t width;
    uint16_t height;
    uint64_t pts_us;
    TIRTCFRAMEINFO frame_info;
    uint8_t buffer_slot;
    uint8_t *data;
    size_t data_len;
    uint8_t media;
    uint8_t flags;
    bool has_frame_info;
    bool test_frame;
} tirtc_session_local_video_packet_t;

typedef struct {
    bool enabled;
    bool sdk_started;
    bool sdk_initialized;
    bool start_in_progress;
    bool stop_in_progress;
    tirtc_conn_t active_conn;
    tirtc_conn_t closing_conn;
} tirtc_session_runtime_snapshot_t;

typedef struct {
    TIRTCCONNECTCALLBACK cb;
    void *user_data;
    char *service_desc;
    char *token;
} tirtc_session_whip_request_t;

typedef struct {
    uint32_t magic;
    uint32_t generation;
    uint64_t accepted_at_us;
} tirtc_session_conn_user_data_t;

typedef struct {
    tirtc_session_observer_t observer;
    void *ctx;
    bool used;
} tirtc_session_observer_slot_t;

static QueueHandle_t s_event_queue;
static QueueHandle_t s_local_video_tx_queue;
static QueueHandle_t s_local_video_tx_free_queue;
static QueueHandle_t s_local_audio_tx_queue;
static SemaphoreHandle_t s_tirtc_api_mutex;
static TaskHandle_t s_worker_task;
static TaskHandle_t s_local_video_tx_task;
static TaskHandle_t s_local_audio_tx_task;
static tirtc_session_config_t s_config;
static tirtc_session_media_ops_t s_media_ops;
static void *s_media_ctx;
static tirtc_session_hooks_t s_hooks;
static void *s_hooks_ctx;
static tirtc_session_control_ops_t s_control_ops;
static void *s_control_ctx;
static tirtc_session_mode_t s_session_mode;
static tirtc_session_state_t s_state = TIRTC_SESSION_STATE_STOPPED;
static tirtc_conn_t s_active_conn;
static uint64_t s_active_conn_accepted_at_us;
static tirtc_session_conn_user_data_t s_active_conn_user_data;
static bool s_state_error_override;
static tirtc_conn_t s_closing_conn;
static tirtc_conn_t s_remote_media_suppressed_conn;
static tirtc_conn_t s_remote_media_drain_conn;
static bool s_initialized;
static bool s_sdk_initialized;
static bool s_sdk_prepare_in_progress;
static bool s_sdk_started;
static bool s_sdk_stop_notified = true;
static uint32_t s_sdk_generation;
static uint32_t s_peer_connect_generation;
static int s_sdk_log_level = TIRTC_SESSION_SDK_LOG_LEVEL;
static tirtc_session_link_mode_t s_link_mode = TIRTC_SESSION_LINK_MODE_DEFAULT;
static uint32_t s_pending_stop_generation;
static bool s_network_connected;
static bool s_identity_ready = true;
static bool s_start_in_progress;
static uint64_t s_next_start_allowed_us;
static bool s_restart_runtime_requested;
static bool s_restart_runtime_full_requested;
static bool s_stop_in_progress;
static bool s_force_wall_clock_sync_requested;
static bool s_local_video_send_enabled;
static bool s_local_audio_send_enabled;
static esp_timer_handle_t s_time_message_initial_timer;
static esp_timer_handle_t s_time_message_periodic_timer;
static esp_timer_handle_t s_media_bootstrap_timer;
static esp_timer_handle_t s_disconnect_watchdog_timer;
static esp_timer_handle_t s_deferred_full_reset_timer;
static esp_timer_handle_t s_deferred_start_after_full_reset_timer;
static bool s_deferred_full_reset_pending;
static bool s_deferred_start_after_full_reset_pending;
static uint64_t s_deferred_full_reset_due_at_us;
static uint64_t s_deferred_start_after_full_reset_due_at_us;
static bool s_peer_wants_video;
static bool s_peer_wants_audio;
static bool s_peer_audio_control_seen;
static bool s_builtin_capture_enabled;
static tirtc_session_media_profile_t s_media_profile = TIRTC_SESSION_MEDIA_PROFILE_AV;
static tirtc_session_builtin_audio_format_t s_builtin_audio_format =
    TIRTC_SESSION_BUILTIN_AUDIO_FORMAT_ALAW_8K;
static uint8_t s_local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
static uint8_t s_local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
static bool s_local_video_publish_forced;
static bool s_local_audio_publish_forced;
static bool s_test_video_publish_forced;
static bool s_test_audio_publish_forced;
static bool s_next_connection_auto_media = true;
static bool s_active_conn_auto_media = true;
static bool s_next_connection_defer_media;
static bool s_active_conn_defer_media;
static bool s_call_media_deferred;
static bool s_call_active;
static bool s_incoming_call_pending;
static uint32_t s_pending_call_cmdw;
static bool s_remote_video_requested;
static bool s_remote_audio_requested;
static uint8_t s_next_remote_audio_stream_id = TIRTC_SESSION_REMOTE_AUDIO_STREAM_ID;
static uint8_t s_active_remote_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
static uint8_t s_remote_audio_requested_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
static bool s_media_bootstrap_pending;
static bool s_remote_video_first_packet_logged;
static bool s_remote_audio_first_packet_logged;
static bool s_remote_message_first_packet_logged;
static bool s_local_audio_first_packet_logged;
static bool s_closing_conn_was_sdk_started;
static char s_started_device_id[TIRTC_SESSION_DEVICE_ID_MAX_LEN];
static char s_started_credential_hash[TIRTC_SESSION_SHA256_HEX_LEN];
static uint32_t s_started_secret_len;
static uint64_t s_test_video_retry_after_us;
static uint64_t s_test_audio_retry_after_us;
static uint8_t s_local_rgb[3] = {0x2E, 0x8F, 0x6B};
static tirtc_session_peer_state_t s_last_peer_state;
static tirtc_session_stats_t s_stats = {
    .local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID,
    .local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID,
};
static tirtc_session_observer_slot_t s_observers[TIRTC_SESSION_OBSERVER_MAX];
static uint8_t *s_local_video_tx_buffers[TIRTC_SESSION_VIDEO_TX_BUFFER_POOL_SIZE];
static size_t s_local_video_tx_buffer_capacities[TIRTC_SESSION_VIDEO_TX_BUFFER_POOL_SIZE];
static portMUX_TYPE s_rtc_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_observer_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_local_video_tx_generation;
static uint32_t s_local_audio_tx_generation;
static TickType_t s_last_local_audio_queue_fail_log_tick;
static TickType_t s_last_send_buffer_log_tick;
static TickType_t s_last_remote_audio_rx_log_tick;
static TickType_t s_last_local_audio_tx_log_tick;
static TickType_t s_last_bad_remote_audio_log_tick;
static TickType_t s_last_remote_message_rx_log_tick;
static uint32_t s_sys_started_callback_count;
static uint64_t s_last_sys_started_callback_us;
static uint32_t s_local_audio_tx_window_frames;
static size_t s_local_audio_tx_window_payload_bytes;
static uint32_t s_local_audio_tx_window_peak_percent;
static uint32_t s_remote_audio_rx_window_frames;
static size_t s_remote_audio_rx_window_payload_bytes;
static size_t s_remote_audio_rx_window_playback_bytes;
static uint32_t s_remote_message_rx_window_frames;
static size_t s_remote_message_rx_window_bytes;


static void tirtc_session_worker_task(void *ctx);
static void tirtc_session_local_video_tx_task(void *ctx);
static void tirtc_session_local_audio_tx_task(void *ctx);
static void tirtc_session_init_stats(void);
static esp_err_t tirtc_session_create_queue(QueueHandle_t *queue, UBaseType_t length, UBaseType_t item_size);
static esp_err_t tirtc_session_create_timer(const char *name,
                                           esp_timer_cb_t callback,
                                           esp_timer_handle_t *handle);
static esp_err_t tirtc_session_create_task(TaskFunction_t task_entry,
                                           const char *name,
                                           uint32_t stack_size,
                                           UBaseType_t priority,
                                           TaskHandle_t *handle);
static esp_err_t tirtc_session_create_event_resources(void);
static esp_err_t tirtc_session_create_local_video_tx_resources(void);
static esp_err_t tirtc_session_create_local_audio_tx_resources(void);
static esp_err_t tirtc_session_create_timers(void);
static esp_err_t tirtc_session_create_tasks(void);
static esp_err_t tirtc_session_create_runtime_resources(void);
static void tirtc_session_configure_runtime_callbacks(void);
static void tirtc_session_get_runtime_snapshot(tirtc_session_runtime_snapshot_t *snapshot);
static void tirtc_session_bind_connection_user_data(tirtc_conn_t conn);
static void tirtc_session_log_connection_user_data(const char *phase, tirtc_conn_t conn);
static bool tirtc_session_check_send_buffer(tirtc_conn_t conn, const char *media_name, bool can_drop);
static int tirtc_session_disconnect_with_sdk_lock(tirtc_conn_t conn);
static esp_err_t tirtc_session_request_remote_key_frame(tirtc_conn_t conn, uint8_t stream_id, const char *reason);
static bool tirtc_session_should_retry_media_request_after_invalid_handle(tirtc_conn_t conn, const char *operation);
static void tirtc_session_set_last_event_locked(const char *event_text);
static void tirtc_session_reset_call_state_locked(void);
static void tirtc_session_build_local_peer_state_locked(tirtc_session_peer_state_t *state);
static tirtc_session_state_t tirtc_session_compute_state_locked(void);
static void tirtc_session_sync_stats_locked(void);
static uint32_t tirtc_session_next_peer_connect_generation_locked(void);
static void tirtc_session_return_to_listen_mode(void);
static bool tirtc_session_try_accept_connection_internal(tirtc_conn_t conn,
                                                         bool require_peer_attempt,
                                                         uint32_t peer_generation);
static void tirtc_session_free_event_payload(tirtc_session_event_t *event);
static void tirtc_session_handle_remote_message(const tirtc_session_event_t *event);
static const char *tirtc_session_media_name(uint8_t media);
static void tirtc_session_format_payload_head(const uint8_t *data, size_t data_len, char *out, size_t out_len);
static void tirtc_session_free_local_video_packet(tirtc_session_local_video_packet_t *packet);
static void tirtc_session_free_local_audio_packet(tirtc_session_local_audio_packet_t *packet);
static void tirtc_session_flush_local_video_tx_queue(void);
static void tirtc_session_flush_local_audio_tx_queue(void);
static void tirtc_session_trim_local_audio_tx_queue(UBaseType_t max_packets);
static esp_err_t tirtc_session_acquire_local_video_buffer_slot(uint8_t *slot_out);
static void tirtc_session_release_local_video_buffer_slot(uint8_t slot);
static esp_err_t tirtc_session_ensure_local_video_buffer_capacity(uint8_t slot, size_t required_size);
static bool tirtc_session_enqueue_event(const tirtc_session_event_t *event, TickType_t wait_ticks);
static bool tirtc_session_enqueue_start_if_ready(void);
static bool tirtc_session_enqueue_disconnect_request(tirtc_conn_t conn,
                                                     bool complete_shutdown,
                                                     bool was_sdk_started);
static void tirtc_session_copy_config_snapshot(tirtc_session_config_t *config);
static bool tirtc_session_extract_start_identity(const tirtc_session_config_t *config,
                                                 char *device_id,
                                                 size_t device_id_size,
                                                 char *secret_key,
                                                 size_t secret_key_size);
static void tirtc_session_clear_start_in_progress(void);
static esp_err_t tirtc_session_prepare_sdk_with_lock(void);
static esp_err_t tirtc_session_start_sdk_from_worker(void);
static const char *tirtc_session_start_error_name(int error);
static void tirtc_session_handle_disconnect_request(const tirtc_session_event_t *event);
static bool tirtc_session_complete_connection_shutdown(tirtc_conn_t hconn, bool was_sdk_started);
static void tirtc_session_release_remote_media(void);
static bool tirtc_session_begin_connection_shutdown(tirtc_conn_t hconn,
                                                   int error,
                                                   bool *was_sdk_started,
                                                   bool *newly_detached_out);
static bool tirtc_session_remote_media_allowed(tirtc_conn_t hconn);
static bool tirtc_session_enqueue_teardown_event(const tirtc_session_event_t *event);
static void tirtc_session_stop_disconnect_watchdog(void);
static bool tirtc_session_schedule_disconnect_watchdog(const char *reason, uint64_t delay_us);
static bool tirtc_session_maybe_force_local_video_publish_locked(void);
static bool tirtc_session_maybe_force_local_audio_publish_locked(void);
static void tirtc_session_sync_test_media_publish_locked(bool test_video_active, bool test_audio_active);
static uint8_t tirtc_session_get_effective_local_video_stream_id_locked(void);
static uint8_t tirtc_session_get_effective_local_audio_stream_id_locked(void);
static uint8_t tirtc_session_normalize_local_video_stream_id(uint8_t stream_id);
static uint8_t tirtc_session_normalize_local_audio_stream_id(uint8_t stream_id);
bool tirtc_session_should_reset_after_send_error(int error);
static bool tirtc_session_is_test_media_window_open_locked(uint64_t now_us, uint64_t retry_after_us);
static bool tirtc_session_is_media_bootstrap_ready_locked(void);
static bool tirtc_session_media_profile_allows_remote_video_locked(void);
static void tirtc_session_local_audio_cb(const uint8_t *data,
                                        size_t data_len,
                                        const tirtc_session_audio_format_t *format,
                                        void *ctx);
#if TIRTC_SESSION_ENABLE_TIME_STREAM_MESSAGES
static uint32_t tirtc_session_get_unix_time_s(void);
#endif
static esp_err_t tirtc_session_send_time_stream_message(void);
static void tirtc_session_time_message_initial_timer_cb(void *arg);
static void tirtc_session_time_message_periodic_timer_cb(void *arg);
static void tirtc_session_media_bootstrap_timer_cb(void *arg);
static void tirtc_session_disconnect_watchdog_timer_cb(void *arg);
static void tirtc_session_deferred_full_reset_timer_cb(void *arg);
static void tirtc_session_deferred_start_after_full_reset_timer_cb(void *arg);
static void tirtc_session_configure_sdk_logs(bool announce);
static void tirtc_session_sdk_log_cb(const char *log, uint32_t length);
static void tirtc_session_on_peer_connect_result(int error, tirtc_conn_t hconn, void *user_data);
static void tirtc_session_on_whip_connect_result(int error, tirtc_conn_t hconn, void *user_data);
static void tirtc_session_on_event(int event, const void *data, int len);
static void tirtc_session_on_conn_accepted(tirtc_conn_t hconn);
static void tirtc_session_on_conn_error(tirtc_conn_t hconn, int error);
static void tirtc_session_on_disconnected(tirtc_conn_t hconn);
static void tirtc_session_on_audio(tirtc_conn_t hconn, const TIRTCFRAMEINFO *frame_info, void *data);
static void tirtc_session_on_video(tirtc_conn_t hconn, const TIRTCFRAMEINFO *frame_info, void *data);
static void tirtc_session_on_message(tirtc_conn_t hconn, const TIRTCFRAMEINFO *frame_info, void *data);
static void tirtc_session_on_command(tirtc_conn_t hconn, uint32_t cmdw, const void *data, uint32_t len);
static int tirtc_session_on_subscribe_video(tirtc_conn_t hconn, uint8_t stream_id);
static void tirtc_session_on_unsubscribe_video(tirtc_conn_t hconn, uint8_t stream_id);
static int tirtc_session_on_subscribe_audio(tirtc_conn_t hconn, uint8_t stream_id);
static void tirtc_session_on_unsubscribe_audio(tirtc_conn_t hconn, uint8_t stream_id);
static void tirtc_session_on_request_key_frame(tirtc_conn_t hconn, uint8_t stream_id);
static bool tirtc_session_try_handle_reserved_control_command(tirtc_conn_t hconn,
                                                              uint32_t cmdw,
                                                              const void *data,
                                                              uint32_t len);
static bool tirtc_session_notify_command(tirtc_conn_t conn, uint32_t cmdw, const void *data, uint32_t data_len);
static void tirtc_session_notify_connection_error(tirtc_conn_t conn, int error);
static void tirtc_session_notify_disconnected(tirtc_conn_t conn);
static void tirtc_session_on_external_whip_connect_result(int error, tirtc_conn_t hconn, void *user_data);
static bool tirtc_session_should_tolerate_invalid_handle(tirtc_conn_t conn, uint64_t *conn_age_us);
bool tirtc_session_should_retry_message_stream_after_invalid_handle(tirtc_conn_t conn, const char *operation);
static void tirtc_session_retry_remote_media_request_after_delay(bool retry_video,
                                                                bool retry_audio,
                                                                const char *reason,
                                                                uint64_t delay_us);

static const TIRTCCALLBACKS s_tirtc_callbacks = {
    .on_event = tirtc_session_on_event,
    .on_conn_accepted = tirtc_session_on_conn_accepted,
    .on_conn_error = tirtc_session_on_conn_error,
    .on_disconnected = tirtc_session_on_disconnected,
    .on_audio = tirtc_session_on_audio,
    .on_video = tirtc_session_on_video,
    .on_message = tirtc_session_on_message,
    .on_command = tirtc_session_on_command,
    .on_request_key_frame = tirtc_session_on_request_key_frame,
    .on_subscribe_video = tirtc_session_on_subscribe_video,
    .on_unsubscribe_video = tirtc_session_on_unsubscribe_video,
    .on_subscribe_audio = tirtc_session_on_subscribe_audio,
    .on_unsubscribe_audio = tirtc_session_on_unsubscribe_audio,
};

static bool tirtc_session_has_media_bridge(void)
{
    return s_media_ops.init != NULL ||
           s_media_ops.set_capture_frame_cb != NULL ||
           s_media_ops.set_capture_enabled != NULL ||
           s_media_ops.prepare_playback_path != NULL ||
           s_media_ops.submit_remote_audio != NULL ||
           s_media_ops.submit_remote_video_jpeg != NULL ||
           s_media_ops.flush != NULL;
}

static size_t tirtc_session_copy_observers(tirtc_session_observer_slot_t *out, size_t out_count)
{
    size_t count = 0;

    if (out == NULL || out_count == 0U) {
        return 0;
    }

    taskENTER_CRITICAL(&s_observer_lock);
    for (size_t index = 0; index < TIRTC_SESSION_OBSERVER_MAX && count < out_count; ++index) {
        if (s_observers[index].used) {
            out[count++] = s_observers[index];
        }
    }
    taskEXIT_CRITICAL(&s_observer_lock);

    return count;
}

static bool tirtc_session_notify_command(tirtc_conn_t conn, uint32_t cmdw, const void *data, uint32_t data_len)
{
    tirtc_session_observer_slot_t observers[TIRTC_SESSION_OBSERVER_MAX] = {0};
    size_t count = tirtc_session_copy_observers(observers, TIRTC_SESSION_OBSERVER_MAX);

    for (size_t index = 0; index < count; ++index) {
        if (observers[index].observer.on_command != NULL &&
            observers[index].observer.on_command(conn, cmdw, data, data_len, observers[index].ctx)) {
            return true;
        }
    }

    return false;
}

static bool tirtc_session_notify_message(tirtc_conn_t conn,
                                         uint8_t media,
                                         uint8_t stream_id,
                                         uint8_t flags,
                                         const void *data,
                                         uint32_t data_len)
{
    tirtc_session_observer_slot_t observers[TIRTC_SESSION_OBSERVER_MAX] = {0};
    size_t count = tirtc_session_copy_observers(observers, TIRTC_SESSION_OBSERVER_MAX);

    for (size_t index = 0; index < count; ++index) {
        if (observers[index].observer.on_message != NULL &&
            observers[index].observer.on_message(conn,
                                                 media,
                                                 stream_id,
                                                 flags,
                                                 data,
                                                 data_len,
                                                 observers[index].ctx)) {
            return true;
        }
    }

    return false;
}

void tirtc_session_notify_connection_accepted(tirtc_conn_t conn)
{
    tirtc_session_observer_slot_t observers[TIRTC_SESSION_OBSERVER_MAX] = {0};
    size_t count = tirtc_session_copy_observers(observers, TIRTC_SESSION_OBSERVER_MAX);

    for (size_t index = 0; index < count; ++index) {
        if (observers[index].observer.on_connection_accepted != NULL) {
            observers[index].observer.on_connection_accepted(conn, observers[index].ctx);
        }
    }
}

static void tirtc_session_notify_connection_error(tirtc_conn_t conn, int error)
{
    tirtc_session_observer_slot_t observers[TIRTC_SESSION_OBSERVER_MAX] = {0};
    size_t count = tirtc_session_copy_observers(observers, TIRTC_SESSION_OBSERVER_MAX);

    for (size_t index = 0; index < count; ++index) {
        if (observers[index].observer.on_connection_error != NULL) {
            observers[index].observer.on_connection_error(conn, error, observers[index].ctx);
        }
    }
}

static void tirtc_session_notify_disconnected(tirtc_conn_t conn)
{
    tirtc_session_observer_slot_t observers[TIRTC_SESSION_OBSERVER_MAX] = {0};
    size_t count = tirtc_session_copy_observers(observers, TIRTC_SESSION_OBSERVER_MAX);

    for (size_t index = 0; index < count; ++index) {
        if (observers[index].observer.on_disconnected != NULL) {
            observers[index].observer.on_disconnected(conn, observers[index].ctx);
        }
    }
}

static void tirtc_session_notify_start_error(int error, const char *device_id, const char *client_id)
{
    tirtc_session_observer_slot_t observers[TIRTC_SESSION_OBSERVER_MAX] = {0};
    size_t count = tirtc_session_copy_observers(observers, TIRTC_SESSION_OBSERVER_MAX);

    for (size_t index = 0; index < count; ++index) {
        if (observers[index].observer.on_start_error != NULL) {
            observers[index].observer.on_start_error(error,
                                                     device_id != NULL ? device_id : "",
                                                     client_id != NULL ? client_id : "",
                                                     observers[index].ctx);
        }
    }
}

static esp_err_t tirtc_session_media_init(void)
{
    if (s_media_ops.init == NULL) {
        return ESP_OK;
    }

    return s_media_ops.init(s_media_ctx);
}

static void tirtc_session_media_set_capture_cb(tirtc_session_capture_frame_cb_t cb, void *cb_ctx)
{
    if (s_media_ops.set_capture_frame_cb != NULL) {
        s_media_ops.set_capture_frame_cb(cb, cb_ctx, s_media_ctx);
    }
}

static esp_err_t tirtc_session_media_set_capture_enabled(bool enabled)
{
    ESP_RETURN_ON_FALSE(s_media_ops.set_capture_enabled != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "rtc media capture bridge not configured");

    return s_media_ops.set_capture_enabled(enabled, s_media_ctx);
}

static esp_err_t tirtc_session_media_prepare_playback_path(void)
{
    ESP_RETURN_ON_FALSE(s_media_ops.prepare_playback_path != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "rtc media playback bridge not configured");

    return s_media_ops.prepare_playback_path(s_media_ctx);
}

static esp_err_t tirtc_session_media_submit_remote_audio(uint8_t media,
                                                         uint8_t flags,
                                                         uint32_t source_timestamp_ms,
                                                         const uint8_t *data,
                                                         size_t data_len,
                                                         size_t *playback_data_len)
{
    ESP_RETURN_ON_FALSE(s_media_ops.submit_remote_audio != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "rtc media audio sink not configured");

    return s_media_ops.submit_remote_audio(media,
                                           flags,
                                           source_timestamp_ms,
                                           data,
                                           data_len,
                                           playback_data_len,
                                           s_media_ctx);
}

static esp_err_t tirtc_session_media_submit_remote_video_jpeg(const uint8_t *data, size_t data_len)
{
    ESP_RETURN_ON_FALSE(s_media_ops.submit_remote_video_jpeg != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "rtc media video sink not configured");

    return s_media_ops.submit_remote_video_jpeg(data, data_len, s_media_ctx);
}

static void tirtc_session_media_flush(void)
{
    if (s_media_ops.flush != NULL) {
        s_media_ops.flush(s_media_ctx);
    }
}

void tirtc_session_flush_remote_media(void)
{
    tirtc_session_media_flush();
}

void tirtc_session_suppress_remote_media(tirtc_conn_t conn, bool suppress)
{
    bool changed = false;

    if (suppress && conn == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (suppress) {
        if (s_remote_media_suppressed_conn != conn) {
            s_remote_media_suppressed_conn = conn;
            changed = true;
        }
    } else if (s_remote_media_suppressed_conn != NULL &&
               (conn == NULL || s_remote_media_suppressed_conn == conn)) {
        s_remote_media_suppressed_conn = NULL;
        changed = true;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (changed && suppress) {
        ESP_LOGI(TAG, "remote media suppress begin: conn=%p", conn);
        tirtc_session_media_flush();
        ESP_LOGI(TAG, "remote media suppressed: conn=%p", conn);
    } else if (changed) {
        tirtc_session_media_flush();
        ESP_LOGI(TAG, "remote media restored: conn=%p", conn);
    }
}

void tirtc_session_preserve_remote_media_on_disconnect(tirtc_conn_t conn, bool preserve)
{
    bool changed = false;

    if (preserve && conn == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (preserve) {
        if (conn == s_active_conn && s_remote_media_drain_conn != conn) {
            s_remote_media_drain_conn = conn;
            changed = true;
        }
    } else if (s_remote_media_drain_conn != NULL &&
               (conn == NULL || s_remote_media_drain_conn == conn)) {
        s_remote_media_drain_conn = NULL;
        changed = true;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (changed) {
        ESP_LOGI(TAG,
                 "remote media disconnect policy: hconn=%p preserve=%u",
                 conn,
                 preserve ? 1U : 0U);
    }
}

static uint8_t *tirtc_session_alloc_tx_buffer(size_t size)
{
    uint8_t *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = malloc(size);
    }
    return buffer;
}

static uint32_t tirtc_session_load_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static char *tirtc_session_strdup_async_arg(const char *value)
{
    size_t len = 0;
    char *copy = NULL;

    if (value == NULL) {
        return NULL;
    }

    len = strlen(value) + 1U;
    /*
     * TiRTC consumes WHIP arguments from SDK-owned rtc_thread after this wrapper
     * returns. Keep the request envelope and its strings in internal RAM: they
     * are small, cross the SDK boundary, and must not depend on PSRAM/cache
     * availability during connection setup.
     */
    copy = heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (copy != NULL) {
        memcpy(copy, value, len);
    }
    return copy;
}

static void tirtc_session_free_whip_request(tirtc_session_whip_request_t *request)
{
    if (request == NULL) {
        return;
    }

    free(request->service_desc);
    free(request->token);
    free(request);
}

static tirtc_session_whip_request_t *tirtc_session_alloc_whip_request(const char *service_desc,
                                                                      const char *token,
                                                                      TIRTCCONNECTCALLBACK cb,
                                                                      void *user_data)
{
    tirtc_session_whip_request_t *request =
        heap_caps_calloc(1, sizeof(*request), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (request == NULL) {
        return NULL;
    }

    request->service_desc = tirtc_session_strdup_async_arg(service_desc);
    request->token = tirtc_session_strdup_async_arg(token);
    if (request->service_desc == NULL || request->token == NULL) {
        tirtc_session_free_whip_request(request);
        return NULL;
    }

    request->cb = cb;
    request->user_data = user_data;

    return request;
}

static void tirtc_session_init_stats(void)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    s_stats.local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    tirtc_session_set_last_event_locked("init");
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);
}

static esp_err_t tirtc_session_create_queue(QueueHandle_t *queue, UBaseType_t length, UBaseType_t item_size)
{
    ESP_RETURN_ON_FALSE(queue != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid queue target");

    if (*queue == NULL) {
        *queue = xQueueCreateWithCaps(length,
                                      item_size,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    return *queue != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t tirtc_session_create_timer(const char *name,
                                           esp_timer_cb_t callback,
                                           esp_timer_handle_t *handle)
{
    esp_timer_create_args_t timer_args = {
        .callback = callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = name,
        .skip_unhandled_events = true,
    };

    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid timer target");

    if (*handle != NULL) {
        return ESP_OK;
    }

    return esp_timer_create(&timer_args, handle);
}

static esp_err_t tirtc_session_create_task(TaskFunction_t task_entry,
                                           const char *name,
                                           uint32_t stack_size,
                                           UBaseType_t priority,
                                           TaskHandle_t *handle)
{
    BaseType_t task_ok = pdPASS;

    ESP_RETURN_ON_FALSE(task_entry != NULL && handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid task args");

    if (*handle != NULL) {
        return ESP_OK;
    }

    task_ok = xTaskCreatePinnedToCoreWithCaps(task_entry,
                                              name,
                                              stack_size,
                                              NULL,
                                              priority,
                                              handle,
                                              0,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t tirtc_session_create_event_resources(void)
{
    ESP_RETURN_ON_ERROR(tirtc_session_create_queue(&s_event_queue,
                                                  TIRTC_SESSION_EVENT_QUEUE_LEN,
                                                  sizeof(tirtc_session_event_t)),
                        TAG,
                        "rtc event queue alloc failed");
    return ESP_OK;
}

static esp_err_t tirtc_session_create_sdk_api_lock(void)
{
    if (s_tirtc_api_mutex == NULL) {
        s_tirtc_api_mutex = xSemaphoreCreateRecursiveMutexWithCaps(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(s_tirtc_api_mutex != NULL,
                            ESP_ERR_NO_MEM,
                            TAG,
                            "rtc sdk api mutex alloc failed");
    }
    return ESP_OK;
}

static esp_err_t tirtc_session_create_local_video_tx_resources(void)
{
    if (!APP_PRODUCT_RTC_VIDEO_ENABLED) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(tirtc_session_create_queue(&s_local_video_tx_queue,
                                                  TIRTC_SESSION_VIDEO_TX_QUEUE_LEN,
                                                  sizeof(tirtc_session_local_video_packet_t)),
                        TAG,
                        "rtc local video queue alloc failed");

    if (s_local_video_tx_free_queue == NULL) {
        s_local_video_tx_free_queue = xQueueCreateWithCaps(TIRTC_SESSION_VIDEO_TX_BUFFER_POOL_SIZE,
                                                           sizeof(uint8_t),
                                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(s_local_video_tx_free_queue != NULL,
                            ESP_ERR_NO_MEM,
                            TAG,
                            "rtc local video free queue alloc failed");

        for (uint8_t slot = 0; slot < TIRTC_SESSION_VIDEO_TX_BUFFER_POOL_SIZE; ++slot) {
            BaseType_t queued = xQueueSend(s_local_video_tx_free_queue, &slot, 0);
            ESP_RETURN_ON_FALSE(queued == pdTRUE,
                                ESP_ERR_NO_MEM,
                                TAG,
                                "rtc local video free queue fill failed");
        }
    }

    return ESP_OK;
}

static esp_err_t tirtc_session_create_local_audio_tx_resources(void)
{
    ESP_RETURN_ON_ERROR(tirtc_session_create_queue(&s_local_audio_tx_queue,
                                                  TIRTC_SESSION_AUDIO_TX_QUEUE_LEN,
                                                  sizeof(tirtc_session_local_audio_packet_t)),
                        TAG,
                        "rtc local audio queue alloc failed");
    return ESP_OK;
}

static esp_err_t tirtc_session_create_timers(void)
{
    ESP_RETURN_ON_ERROR(tirtc_session_create_timer("rtc_time_once",
                                                  tirtc_session_time_message_initial_timer_cb,
                                                  &s_time_message_initial_timer),
                        TAG,
                        "rtc time initial timer create failed");
    ESP_RETURN_ON_ERROR(tirtc_session_create_timer("rtc_time_periodic",
                                                  tirtc_session_time_message_periodic_timer_cb,
                                                  &s_time_message_periodic_timer),
                        TAG,
                        "rtc time periodic timer create failed");
    ESP_RETURN_ON_ERROR(tirtc_session_create_timer("rtc_media_boot",
                                                  tirtc_session_media_bootstrap_timer_cb,
                                                  &s_media_bootstrap_timer),
                        TAG,
                        "rtc media bootstrap timer create failed");
    ESP_RETURN_ON_ERROR(tirtc_session_create_timer("rtc_disc_watch",
                                                   tirtc_session_disconnect_watchdog_timer_cb,
                                                   &s_disconnect_watchdog_timer),
                        TAG,
                        "rtc disconnect watchdog timer create failed");
    ESP_RETURN_ON_ERROR(tirtc_session_create_timer("rtc_full_reset",
                                                   tirtc_session_deferred_full_reset_timer_cb,
                                                   &s_deferred_full_reset_timer),
                        TAG,
                        "rtc deferred full reset timer create failed");
    ESP_RETURN_ON_ERROR(tirtc_session_create_timer("rtc_full_reset_start",
                                                  tirtc_session_deferred_start_after_full_reset_timer_cb,
                                                  &s_deferred_start_after_full_reset_timer),
                        TAG,
                        "rtc deferred full reset start timer create failed");
    return ESP_OK;
}

static esp_err_t tirtc_session_create_tasks(void)
{
    if (APP_PRODUCT_RTC_VIDEO_ENABLED) {
        ESP_RETURN_ON_ERROR(tirtc_session_create_task(tirtc_session_local_video_tx_task,
                                                     "rtc_video_tx",
                                                     TIRTC_SESSION_VIDEO_TX_TASK_STACK,
                                                     TIRTC_SESSION_VIDEO_TX_TASK_PRIORITY,
                                                     &s_local_video_tx_task),
                            TAG,
                            "rtc local video task create failed");
    }
    ESP_RETURN_ON_ERROR(tirtc_session_create_task(tirtc_session_local_audio_tx_task,
                                                 "rtc_audio_tx",
                                                 TIRTC_SESSION_AUDIO_TX_TASK_STACK,
                                                 TIRTC_SESSION_AUDIO_TX_TASK_PRIORITY,
                                                 &s_local_audio_tx_task),
                        TAG,
                        "rtc local audio task create failed");
    /*
     * rtc_worker does not perform flash/NVS/OTA writes itself; it serializes RTC
     * state transitions and SDK calls. Keep its stack in PSRAM so internal RAM is
     * reserved for flash-safe tasks, DMA/audio and SDK allocations.
     */
    ESP_RETURN_ON_ERROR(tirtc_session_create_task(tirtc_session_worker_task,
                                                 "rtc_worker",
                                                 TIRTC_SESSION_WORKER_TASK_STACK,
                                                 TIRTC_SESSION_WORKER_TASK_PRIORITY,
                                                 &s_worker_task),
                        TAG,
                        "rtc worker task create failed");
    return ESP_OK;
}

static esp_err_t tirtc_session_create_runtime_resources(void)
{
    ESP_RETURN_ON_ERROR(tirtc_session_create_event_resources(), TAG, "rtc event resources init failed");
    ESP_RETURN_ON_ERROR(tirtc_session_create_sdk_api_lock(), TAG, "rtc sdk api lock init failed");
    ESP_RETURN_ON_ERROR(tirtc_session_create_local_video_tx_resources(),
                        TAG,
                        "rtc local video resources init failed");
    ESP_RETURN_ON_ERROR(tirtc_session_create_local_audio_tx_resources(),
                        TAG,
                        "rtc local audio resources init failed");
    ESP_RETURN_ON_ERROR(tirtc_session_create_timers(), TAG, "rtc timers init failed");
    ESP_RETURN_ON_ERROR(tirtc_session_media_init(), TAG, "rtc media bridge init failed");
    ESP_RETURN_ON_ERROR(tirtc_session_create_tasks(), TAG, "rtc worker init failed");
    return ESP_OK;
}

bool tirtc_session_take_sdk_api_lock(TickType_t wait_ticks)
{
    return s_tirtc_api_mutex != NULL && xSemaphoreTakeRecursive(s_tirtc_api_mutex, wait_ticks) == pdTRUE;
}

void tirtc_session_give_sdk_api_lock(void)
{
    if (s_tirtc_api_mutex != NULL) {
        xSemaphoreGiveRecursive(s_tirtc_api_mutex);
    }
}

static int tirtc_session_disconnect_with_sdk_lock(tirtc_conn_t conn)
{
    int ret = TIRTC_E_BUSY;

    if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        ret = TiRtcDisconnect(conn);
        tirtc_session_give_sdk_api_lock();
    } else {
        ESP_LOGW(TAG, "rtc sdk api lock unavailable for disconnect");
    }

    return ret;
}

static void tirtc_session_configure_runtime_callbacks(void)
{
    tirtc_session_media_set_capture_cb(tirtc_session_local_audio_cb, NULL);
}

static void tirtc_session_restore_builtin_capture_callback(void)
{
    /*
     * audio_release() intentionally clears the primary microphone callback when
     * an app leaves the audio resource. IPC viewing depends on that primary
     * callback, while AI Chat and WeChat VoIP use observer/external-audio paths.
     * Rebind it whenever the built-in media owner is restored so the second IPC
     * entry can upload microphone audio without relying on a full TiRTC reinit.
     */
    tirtc_session_media_set_capture_cb(tirtc_session_local_audio_cb, NULL);
}

static void tirtc_session_get_runtime_snapshot(tirtc_session_runtime_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));

    taskENTER_CRITICAL(&s_rtc_lock);
    snapshot->enabled = s_config.enabled;
    snapshot->sdk_started = s_sdk_started;
    snapshot->sdk_initialized = s_sdk_initialized;
    snapshot->start_in_progress = s_start_in_progress;
    snapshot->stop_in_progress = s_stop_in_progress;
    snapshot->active_conn = s_active_conn;
    snapshot->closing_conn = s_closing_conn;
    taskEXIT_CRITICAL(&s_rtc_lock);
}

bool tirtc_session_try_get_active_conn(tirtc_conn_t *conn)
{
    bool ready = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    ready = s_sdk_started && !s_start_in_progress && !s_stop_in_progress && s_closing_conn == NULL &&
            s_active_conn != NULL;
    if (ready && conn != NULL) {
        *conn = s_active_conn;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
    return ready;
}

bool tirtc_session_is_connection_usable(tirtc_conn_t conn)
{
    bool usable = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    usable = conn != NULL && conn == s_active_conn && s_sdk_started && !s_start_in_progress &&
             !s_stop_in_progress && s_closing_conn == NULL;
    taskEXIT_CRITICAL(&s_rtc_lock);
    return usable;
}

static void tirtc_session_bind_connection_user_data(tirtc_conn_t conn)
{
    uint32_t generation = 0;
    uint64_t accepted_at_us = 0;

    if (conn == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (conn == s_active_conn) {
        s_active_conn_user_data.magic = TIRTC_SESSION_CONN_USER_MAGIC;
        s_active_conn_user_data.generation++;
        s_active_conn_user_data.accepted_at_us = s_active_conn_accepted_at_us;
        generation = s_active_conn_user_data.generation;
        accepted_at_us = s_active_conn_user_data.accepted_at_us;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (generation == 0U) {
        return;
    }

    int ret = TiRtcConnSetUserData(conn, &s_active_conn_user_data);
    if (ret < 0) {
        tirtc_session_set_last_error(ret);
        ESP_LOGW(TAG, "bind rtc connection user data failed: %s", TiRtcGetErrorStr(ret));
        return;
    }

    ESP_LOGD(TAG,
             "rtc connection user data bound: hconn=%p gen=%lu accepted_at_us=%llu",
             conn,
             (unsigned long)generation,
             (unsigned long long)accepted_at_us);
}

static void tirtc_session_log_connection_user_data(const char *phase, tirtc_conn_t conn)
{
    const char *safe_phase = phase != NULL ? phase : "connection";
    void *user_data = NULL;

    if (conn == NULL) {
        return;
    }

    user_data = TiRtcConnGetUserData(conn);
    if (user_data == NULL) {
        ESP_LOGD(TAG, "rtc %s: hconn=%p user_data=NULL", safe_phase, conn);
        return;
    }

    const tirtc_session_conn_user_data_t *conn_data = (const tirtc_session_conn_user_data_t *)user_data;
    if (conn_data->magic != TIRTC_SESSION_CONN_USER_MAGIC) {
        ESP_LOGD(TAG, "rtc %s: hconn=%p user_data=%p", safe_phase, conn, user_data);
        return;
    }

    uint64_t now_us = esp_timer_get_time();
    uint64_t age_ms = now_us >= conn_data->accepted_at_us
                          ? (now_us - conn_data->accepted_at_us) / 1000ULL
                          : 0ULL;
    ESP_LOGI(TAG,
             "rtc %s: hconn=%p gen=%lu age_ms=%llu",
             safe_phase,
             conn,
             (unsigned long)conn_data->generation,
             (unsigned long long)age_ms);
}

static bool tirtc_session_check_send_buffer(tirtc_conn_t conn, const char *media_name, bool can_drop)
{
    size_t used = 0;
    size_t warn_level = 0;
    size_t drop_level = 0;
    size_t policy_limit = TIRTC_SESSION_MAX_SEND_BUFFER;
    bool should_log = false;
    TickType_t now_tick = 0;
    const char *safe_media_name = media_name != NULL ? media_name : "media";

    if (conn == NULL) {
        return false;
    }

    if (TIRTC_SESSION_MAX_SEND_BUFFER == 0U) {
        return true;
    }

    used = TiRtcGetSendBufferUsed(conn);
    warn_level = (size_t)(((uint64_t)policy_limit * TIRTC_SESSION_SEND_BUFFER_WARN_PCT) / 100ULL);
    drop_level = (size_t)(((uint64_t)TIRTC_SESSION_MAX_SEND_BUFFER * TIRTC_SESSION_SEND_BUFFER_DROP_PCT) / 100ULL);
    if (can_drop && strcmp(safe_media_name, "audio") == 0 &&
        TIRTC_SESSION_DROPPABLE_AUDIO_SEND_BUFFER < drop_level) {
        /* Stale microphone PCM cannot help a live conversation. Bound only
         * droppable audio; control messages and video retain the SDK limit. */
        drop_level = TIRTC_SESSION_DROPPABLE_AUDIO_SEND_BUFFER;
        warn_level = (drop_level * TIRTC_SESSION_SEND_BUFFER_WARN_PCT) / 100U;
        policy_limit = drop_level;
    }

    if (used >= warn_level && warn_level > 0U) {
        now_tick = xTaskGetTickCount();
        taskENTER_CRITICAL(&s_rtc_lock);
        if (s_last_send_buffer_log_tick == 0 ||
            now_tick - s_last_send_buffer_log_tick >= pdMS_TO_TICKS(TIRTC_SESSION_SEND_BUFFER_LOG_PERIOD_MS)) {
            s_last_send_buffer_log_tick = now_tick;
            should_log = true;
        }
        taskEXIT_CRITICAL(&s_rtc_lock);

        if (should_log) {
            ESP_LOGW(TAG,
                     "rtc send buffer high: media=%s used=%u limit=%lu",
                     safe_media_name,
                     (unsigned)used,
                     (unsigned long)policy_limit);
        }
    }

    if (can_drop && used >= drop_level && drop_level > 0U) {
        taskENTER_CRITICAL(&s_rtc_lock);
        s_stats.tx_failures++;
        tirtc_session_set_last_event_locked("send backpressure");
        taskEXIT_CRITICAL(&s_rtc_lock);
        ESP_LOGD(TAG,
                 "rtc %s frame throttled by send buffer backpressure: send_buffer=%u/%lu",
                 safe_media_name,
                 (unsigned)used,
                 (unsigned long)policy_limit);
        return false;
    }

    return true;
}

static esp_err_t tirtc_session_request_remote_key_frame(tirtc_conn_t conn, uint8_t stream_id, const char *reason)
{
    if (!tirtc_session_is_connection_usable(conn)) {
        return ESP_ERR_INVALID_STATE;
    }

    int ret = TIRTC_E_BUSY;
    if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        if (!tirtc_session_is_connection_usable(conn)) {
            tirtc_session_give_sdk_api_lock();
            return ESP_ERR_INVALID_STATE;
        }
        ret = TiRtcRequestKeyFrame(conn, stream_id);
        tirtc_session_give_sdk_api_lock();
    }
    if (ret >= 0) {
        tirtc_session_note_event("key frame req");
        ESP_LOGD(TAG,
                 "remote key frame requested: stream=%u reason=%s",
                 (unsigned)stream_id,
                 reason != NULL ? reason : "unspecified");
        return ESP_OK;
    }

    if (ret == TIRTC_E_INVALID_HANDLE &&
        tirtc_session_should_retry_media_request_after_invalid_handle(conn, "request remote key frame")) {
        return ESP_ERR_INVALID_STATE;
    }

    tirtc_session_set_last_error(ret);
    if (ret == TIRTC_E_INVALID_HANDLE) {
        tirtc_session_handle_connection_loss(conn, ret);
    } else {
        ESP_LOGW(TAG, "request remote key frame failed: %s", TiRtcGetErrorStr(ret));
    }
    return ESP_FAIL;
}

static void tirtc_session_cancel_media_bootstrap(void)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_media_bootstrap_pending = false;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (s_media_bootstrap_timer != NULL) {
        (void)esp_timer_stop(s_media_bootstrap_timer);
    }
}

static void tirtc_session_stop_disconnect_watchdog(void)
{
    if (s_disconnect_watchdog_timer != NULL) {
        (void)esp_timer_stop(s_disconnect_watchdog_timer);
    }
}

static void tirtc_session_reset_call_state_locked(void)
{
    s_call_active = false;
    s_incoming_call_pending = false;
    s_pending_call_cmdw = 0;
    s_remote_video_requested = false;
    s_remote_audio_requested = false;
    s_remote_audio_requested_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    s_media_bootstrap_pending = false;
    s_remote_video_first_packet_logged = false;
    s_remote_audio_first_packet_logged = false;
    s_remote_message_first_packet_logged = false;
    s_last_remote_audio_rx_log_tick = 0;
    s_last_remote_message_rx_log_tick = 0;
    s_remote_audio_rx_window_frames = 0;
    s_remote_audio_rx_window_payload_bytes = 0;
    s_remote_audio_rx_window_playback_bytes = 0;
    s_remote_message_rx_window_frames = 0;
    s_remote_message_rx_window_bytes = 0;
    s_remote_media_suppressed_conn = NULL;
    s_remote_media_drain_conn = NULL;
    s_local_audio_first_packet_logged = false;
    s_test_video_retry_after_us = 0U;
    s_test_audio_retry_after_us = 0U;
    s_peer_wants_video = false;
    s_peer_wants_audio = false;
    s_peer_audio_control_seen = false;
    s_builtin_capture_enabled = false;
    s_media_profile = TIRTC_SESSION_MEDIA_PROFILE_AV;
    s_next_connection_auto_media = true;
    s_active_conn_auto_media = true;
    s_next_connection_defer_media = false;
    s_active_conn_defer_media = false;
    s_call_media_deferred = false;
    s_local_video_publish_forced = false;
    s_local_audio_publish_forced = false;
    s_test_video_publish_forced = false;
    s_test_audio_publish_forced = false;
    memset(&s_last_peer_state, 0, sizeof(s_last_peer_state));
}

static bool tirtc_session_is_media_bootstrap_ready_locked(void)
{
    return s_active_conn != NULL && s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
           s_closing_conn == NULL && s_call_active;
}

static bool tirtc_session_media_profile_allows_remote_video_locked(void)
{
    return APP_PRODUCT_RTC_VIDEO_ENABLED &&
           s_media_profile != TIRTC_SESSION_MEDIA_PROFILE_EXTERNAL_AUDIO;
}

static bool tirtc_session_media_profile_uses_builtin_capture_locked(void)
{
    return s_media_profile != TIRTC_SESSION_MEDIA_PROFILE_EXTERNAL_AUDIO;
}

static bool tirtc_session_remote_media_allowed(tirtc_conn_t hconn)
{
    bool allowed = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    allowed = hconn != NULL &&
              s_sdk_started &&
              !s_start_in_progress &&
              !s_stop_in_progress &&
              s_closing_conn == NULL &&
              s_active_conn == hconn &&
              s_remote_media_suppressed_conn != hconn;
    taskEXIT_CRITICAL(&s_rtc_lock);
    return allowed;
}

#if TIRTC_SESSION_ENABLE_TIME_STREAM_MESSAGES
static uint32_t tirtc_session_get_unix_time_s(void)
{
    time_t now = 0;

    time(&now);
    if (now < 0) {
        return 0;
    }

    return (uint32_t)now;
}
#endif

static void tirtc_session_queue_peer_connect_error(int error)
{
    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_CONN_ERROR,
        .payload.conn = {
            .conn = NULL,
            .error = error,
        },
    };

    /* TiRtcConnect reports a terminal failure with a NULL handle. Route that
     * result through the normal worker/observer path so the owning service can
     * end its business generation without doing blocking work in the SDK
     * callback. There is no application-owned connection to disconnect here. */
    if (!tirtc_session_enqueue_teardown_event(&rtc_event)) {
        tirtc_session_note_event("peer error drop");
        ESP_LOGE(TAG, "rtc teardown queue failed after peer connect error: err=%d", error);
    }
}

static void tirtc_session_on_peer_connect_result(int error, tirtc_conn_t hconn, void *user_data)
{
    uint32_t peer_generation = (uint32_t)(uintptr_t)user_data;

    tirtc_session_mode_t mode = TIRTC_SESSION_MODE_LISTEN;
    tirtc_session_state_t state = TIRTC_SESSION_STATE_STOPPED;
    bool sdk_started = false;
    bool start_in_progress = false;
    bool stop_in_progress = false;
    tirtc_conn_t active_conn = NULL;
    tirtc_conn_t closing_conn = NULL;
    uint32_t current_peer_generation = 0;
    bool current_attempt = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    mode = s_session_mode;
    state = s_state;
    sdk_started = s_sdk_started;
    start_in_progress = s_start_in_progress;
    stop_in_progress = s_stop_in_progress;
    active_conn = s_active_conn;
    closing_conn = s_closing_conn;
    current_peer_generation = s_peer_connect_generation;
    current_attempt = peer_generation != 0U &&
                      peer_generation == s_peer_connect_generation &&
                      s_session_mode == TIRTC_SESSION_MODE_CONNECT;
    taskEXIT_CRITICAL(&s_rtc_lock);

    ESP_LOGI(TAG,
             "rtc peer connect result: error=%d %s hconn=%p mode=%u state=%u sdk_started=%d start=%d stop=%d active=%p closing=%p peer_gen=%lu/%lu current=%d",
             error,
             error == 0 ? "OK" : TiRtcGetErrorStr(error),
             hconn,
             (unsigned)mode,
             (unsigned)state,
             sdk_started ? 1 : 0,
             start_in_progress ? 1 : 0,
             stop_in_progress ? 1 : 0,
             active_conn,
             closing_conn,
             (unsigned long)peer_generation,
             (unsigned long)current_peer_generation,
             current_attempt ? 1 : 0);

    if (!current_attempt) {
        ESP_LOGW(TAG,
                 "rtc peer connect result ignored after cancellation: hconn=%p peer_gen=%lu current_gen=%lu",
                 hconn,
                 (unsigned long)peer_generation,
                 (unsigned long)current_peer_generation);
        if (error == 0 && hconn != NULL) {
            if (!tirtc_session_drain_unowned_connection(hconn)) {
                (void)tirtc_session_disconnect_with_sdk_lock(hconn);
            }
        }
        return;
    }

    if (error != 0) {
        tirtc_session_return_to_listen_mode();
        tirtc_session_set_last_error(error);
        tirtc_session_note_event("peer connect fail");
        ESP_LOGW(TAG, "rtc peer connect failed: %s (%d)", TiRtcGetErrorStr(error), error);
        /* The SDK owns failed active-connect handles that were never exposed to
         * the application. Its asynchronous failure cleanup runs after this
         * callback, while the listening runtime remains started and reusable. */
        tirtc_session_queue_peer_connect_error(error);
        return;
    }

    if (hconn == NULL) {
        tirtc_session_return_to_listen_mode();
        tirtc_session_set_last_error(TIRTC_E_INVALID_PARAMETER);
        tirtc_session_note_event("peer conn empty");
        ESP_LOGW(TAG, "rtc peer connect returned empty handle");
        tirtc_session_queue_peer_connect_error(TIRTC_E_INVALID_PARAMETER);
        return;
    }

    if (!tirtc_session_try_accept_connection_internal(hconn, true, peer_generation)) {
        bool canceled_after_callback = false;

        taskENTER_CRITICAL(&s_rtc_lock);
        canceled_after_callback = s_session_mode != TIRTC_SESSION_MODE_CONNECT ||
                                  s_peer_connect_generation != peer_generation;
        taskEXIT_CRITICAL(&s_rtc_lock);
        tirtc_session_note_event("peer conn reject");
        ESP_LOGW(TAG, "rtc peer connection rejected: hconn=%p", hconn);
        if (!canceled_after_callback || !tirtc_session_drain_unowned_connection(hconn)) {
            (void)tirtc_session_disconnect_with_sdk_lock(hconn);
        }
        return;
    }

    tirtc_session_bind_connection_user_data(hconn);

    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_CONN_ACCEPTED,
        .payload.conn = {
            .conn = hconn,
            .error = 0,
        },
    };

    if (!tirtc_session_enqueue_event(&rtc_event, 0)) {
        tirtc_session_note_event("peer accept inline");
        ESP_LOGW(TAG, "rtc event queue full: peer connection handled inline");
        tirtc_session_handle_runtime_event(&rtc_event);
    }
}

static void tirtc_session_on_whip_connect_result(int error, tirtc_conn_t hconn, void *user_data)
{
    tirtc_session_whip_request_t *request = (tirtc_session_whip_request_t *)user_data;
    bool accepted = false;

    ESP_LOGI(TAG,
             "WHIP connect callback: error=%s (%d) hconn=%p",
             error == 0 ? "OK" : TiRtcGetErrorStr(error),
             error,
             hconn);

    if (error == 0 && hconn != NULL) {
        if (tirtc_session_try_accept_connection(hconn)) {
            accepted = true;
            bool auto_media = tirtc_session_connection_auto_media_enabled(hconn);
            tirtc_session_bind_connection_user_data(hconn);
            if (!auto_media) {
                ESP_LOGI(TAG, "WHIP connection uses external media owner: hconn=%p", hconn);
            }

            tirtc_session_event_t rtc_event = {
                .type = TIRTC_SESSION_EVENT_CONN_ACCEPTED,
                .payload.conn = {
                    .conn = hconn,
                    .error = 0,
                },
            };
            if (!tirtc_session_enqueue_event(&rtc_event, 0)) {
                tirtc_session_note_event("whip accept inline");
                ESP_LOGW(TAG, "rtc event queue full: WHIP connection handled inline");
                tirtc_session_handle_runtime_event(&rtc_event);
            }
        } else {
            ESP_LOGW(TAG, "WHIP connection rejected by runtime: hconn=%p", hconn);
            (void)tirtc_session_disconnect_with_sdk_lock(hconn);
            error = TIRTC_E_BUSY;
            hconn = NULL;
        }
    }

    if (!accepted) {
        tirtc_session_set_next_connection_auto_media(true);
    }

    if (request != NULL && request->cb != NULL) {
        request->cb(error, hconn, request->user_data);
    }
    tirtc_session_free_whip_request(request);
}

static void tirtc_session_on_external_whip_connect_result(int error, tirtc_conn_t hconn, void *user_data)
{
    tirtc_session_whip_request_t *request = (tirtc_session_whip_request_t *)user_data;

    ESP_LOGI(TAG,
             "WHIP external callback: error=%s (%d) hconn=%p",
             error == 0 ? "OK" : TiRtcGetErrorStr(error),
             error,
             hconn);

    if (request != NULL && request->cb != NULL) {
        request->cb(error, hconn, request->user_data);
    }
    tirtc_session_free_whip_request(request);
}

esp_err_t tirtc_session_start_configured_peer_connect(void)
{
    tirtc_session_config_t config = {0};
    bool network_connected = false;
    bool sdk_initialized = false;
    bool sdk_started = false;
    bool sdk_prepare_in_progress = false;
    bool start_in_progress = false;
    bool stop_in_progress = false;
    bool connect_in_progress = false;
    bool current_attempt = false;
    uint32_t peer_generation = 0;
    tirtc_conn_t active_conn = NULL;
    tirtc_conn_t closing_conn = NULL;

    connect_in_progress = tirtc_connect_is_connecting();
    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_session_mode != TIRTC_SESSION_MODE_CONNECT) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        return ESP_ERR_INVALID_STATE;
    }
    network_connected = s_network_connected;
    sdk_initialized = s_sdk_initialized;
    sdk_started = s_sdk_started;
    sdk_prepare_in_progress = s_sdk_prepare_in_progress;
    start_in_progress = s_start_in_progress;
    stop_in_progress = s_stop_in_progress;
    active_conn = s_active_conn;
    closing_conn = s_closing_conn;
    if (!s_network_connected || !s_sdk_initialized || !s_sdk_started || s_sdk_prepare_in_progress ||
        s_start_in_progress || s_stop_in_progress ||
        s_active_conn != NULL || s_closing_conn != NULL) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        ESP_LOGW(TAG,
                 "rtc peer connect preflight rejected: net=%d init=%d started=%d prep=%d start=%d stop=%d active=%p closing=%p connecting=%d",
                 network_connected ? 1 : 0,
                 sdk_initialized ? 1 : 0,
                 sdk_started ? 1 : 0,
                 sdk_prepare_in_progress ? 1 : 0,
                 start_in_progress ? 1 : 0,
                 stop_in_progress ? 1 : 0,
                 active_conn,
                 closing_conn,
                 connect_in_progress ? 1 : 0);
        return ESP_ERR_INVALID_STATE;
    }
    if (connect_in_progress) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        ESP_LOGW(TAG, "rtc peer connect preflight rejected: active connect is already running");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_config.remote_device_id[0] == '\0') {
        taskEXIT_CRITICAL(&s_rtc_lock);
        tirtc_session_note_event("remote id empty");
        ESP_LOGE(TAG, "rtc peer id is empty");
        return ESP_ERR_INVALID_ARG;
    }
    config = s_config;
    peer_generation = tirtc_session_next_peer_connect_generation_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    ESP_LOGI(TAG,
             "rtc peer connect preflight ok: local_id_len=%u remote_id_len=%u state=%u",
             (unsigned)strlen(config.device_id),
             (unsigned)strlen(config.remote_device_id),
             (unsigned)tirtc_session_get_state());

    esp_err_t ret = tirtc_connect_start(&config,
                                        tirtc_session_on_peer_connect_result,
                                        (void *)(uintptr_t)peer_generation);
    if (ret != ESP_OK) {
        tirtc_session_return_to_listen_mode();
        tirtc_session_set_last_error(ret);
        tirtc_session_note_event("peer connect fail");
        return ret;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    current_attempt = s_session_mode == TIRTC_SESSION_MODE_CONNECT &&
                      s_peer_connect_generation == peer_generation;
    taskEXIT_CRITICAL(&s_rtc_lock);
    if (!current_attempt) {
        (void)tirtc_connect_cancel_pending();
        return ESP_ERR_INVALID_STATE;
    }

    tirtc_session_note_event("peer connect");
    ESP_LOGI(TAG,
             "rtc peer connect task started: remote_id_len=%u",
             (unsigned)strlen(config.remote_device_id));
    taskENTER_CRITICAL(&s_rtc_lock);
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);
    return ESP_OK;
}

static bool tirtc_session_should_tolerate_invalid_handle(tirtc_conn_t conn, uint64_t *conn_age_us)
{
    uint64_t accepted_at_us = 0;
    uint64_t now_us = 0;
    uint64_t age_us = 0;
    bool tolerate = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (conn != NULL && conn == s_active_conn && s_closing_conn == NULL && s_sdk_started &&
        !s_start_in_progress && !s_stop_in_progress) {
        accepted_at_us = s_active_conn_accepted_at_us;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (accepted_at_us != 0U) {
        now_us = esp_timer_get_time();
        age_us = now_us >= accepted_at_us ? now_us - accepted_at_us : 0U;
        tolerate = age_us <= TIRTC_SESSION_INVALID_HANDLE_GRACE_US;
    }

    if (conn_age_us != NULL) {
        *conn_age_us = age_us;
    }

    return tolerate;
}

static bool tirtc_session_should_retry_media_request_after_invalid_handle(tirtc_conn_t conn,
                                                                         const char *operation)
{
    uint64_t conn_age_us = 0;

    if (!tirtc_session_should_tolerate_invalid_handle(conn, &conn_age_us)) {
        return false;
    }

    tirtc_session_note_event("media req wait");
    ESP_LOGD(TAG,
             "%s got INVALID_HANDLE %llu us after accept; keep connection and retry media bootstrap",
             operation != NULL ? operation : "media request",
             (unsigned long long)conn_age_us);
    return true;
}

bool tirtc_session_should_retry_message_stream_after_invalid_handle(tirtc_conn_t conn, const char *operation)
{
    uint64_t conn_age_us = 0;

    if (!tirtc_session_should_tolerate_invalid_handle(conn, &conn_age_us)) {
        return false;
    }

    tirtc_session_note_event("message wait");
    ESP_LOGD(TAG,
             "%s got INVALID_HANDLE %llu us after accept; keep connection and retry time message",
             operation != NULL ? operation : "message stream",
             (unsigned long long)conn_age_us);
    return true;
}

bool tirtc_session_is_test_video_active(void)
{
    return s_hooks.is_test_video_active != NULL && s_hooks.is_test_video_active(s_hooks_ctx);
}

bool tirtc_session_is_test_audio_active(void)
{
    return s_hooks.is_test_audio_active != NULL && s_hooks.is_test_audio_active(s_hooks_ctx);
}

bool tirtc_session_is_test_media_active(void)
{
    return tirtc_session_is_test_video_active() || tirtc_session_is_test_audio_active();
}

void tirtc_session_request_test_audio_restart(void)
{
    if (s_hooks.request_test_audio_restart != NULL) {
        s_hooks.request_test_audio_restart(s_hooks_ctx);
    }
}

static bool tirtc_session_is_media_request_command(uint32_t cmdw)
{
    uint16_t cmd_id = (uint16_t)(cmdw & 0x7FFFU);

    return cmd_id == TIRTC_SESSION_CMD_REQ_VIDEO || cmd_id == TIRTC_SESSION_CMD_REQ_AUDIO;
}

static esp_err_t tirtc_session_send_time_stream_message(void)
{
#if !TIRTC_SESSION_ENABLE_TIME_STREAM_MESSAGES
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!tirtc_session_try_get_active_conn(NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t unix_time_s = tirtc_session_get_unix_time_s();
    uint8_t payload[sizeof(unix_time_s)] = {
        (uint8_t)(unix_time_s & 0xFFU),
        (uint8_t)((unix_time_s >> 8) & 0xFFU),
        (uint8_t)((unix_time_s >> 16) & 0xFFU),
        (uint8_t)((unix_time_s >> 24) & 0xFFU),
    };

    return tirtc_session_send_stream_message(payload, sizeof(payload));
#endif
}

static void tirtc_session_time_message_initial_timer_cb(void *arg)
{
    (void)arg;

    esp_err_t send_ret = tirtc_session_send_time_stream_message();

    if (send_ret == ESP_OK && s_time_message_periodic_timer != NULL && tirtc_session_try_get_active_conn(NULL)) {
        (void)esp_timer_start_periodic(s_time_message_periodic_timer,
                                       TIRTC_SESSION_TIME_MESSAGE_PERIOD_US);
    } else if (send_ret == ESP_ERR_INVALID_STATE && s_time_message_initial_timer != NULL &&
               tirtc_session_try_get_active_conn(NULL)) {
        (void)esp_timer_start_once(s_time_message_initial_timer,
                                   TIRTC_SESSION_TIME_MESSAGE_RETRY_DELAY_US);
    }
}

static void tirtc_session_time_message_periodic_timer_cb(void *arg)
{
    (void)arg;

    (void)tirtc_session_send_time_stream_message();
}

static void tirtc_session_media_bootstrap_timer_cb(void *arg)
{
    (void)arg;

    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_MEDIA_BOOTSTRAP,
    };

    if (!tirtc_session_enqueue_event(&rtc_event, TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        tirtc_session_note_event("media bootstrap drop");
        ESP_LOGW(TAG, "rtc event queue full: media bootstrap retry scheduled");
        if (s_media_bootstrap_timer != NULL) {
            (void)esp_timer_start_once(s_media_bootstrap_timer,
                                       TIRTC_SESSION_MEDIA_BOOTSTRAP_RETRY_DELAY_US);
        }
    }
}

static void tirtc_session_disconnect_watchdog_timer_cb(void *arg)
{
    tirtc_conn_t closing_conn = NULL;
    bool was_sdk_started = false;

    (void)arg;

    taskENTER_CRITICAL(&s_rtc_lock);
    closing_conn = s_closing_conn;
    was_sdk_started = s_closing_conn_was_sdk_started;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (closing_conn == NULL) {
        return;
    }

    tirtc_session_note_event("disconnect timeout");
    ESP_LOGW(TAG,
             "rtc disconnect timeout: hconn=%p, force completing teardown",
             closing_conn);
    (void)tirtc_session_complete_connection_shutdown(closing_conn, was_sdk_started);
}

static void tirtc_session_deferred_full_reset_timer_cb(void *arg)
{
    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_DEFERRED_FULL_RESET,
    };

    (void)arg;

    if (!tirtc_session_enqueue_event(&rtc_event, TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        tirtc_session_note_event("full reset drop");
        ESP_LOGW(TAG, "rtc event queue full: deferred full reset dropped");
    }
}

static void tirtc_session_deferred_start_after_full_reset_timer_cb(void *arg)
{
    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_DEFERRED_START_AFTER_FULL_RESET,
    };

    (void)arg;

    if (!tirtc_session_enqueue_event(&rtc_event, TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        tirtc_session_note_event("full start drop");
        ESP_LOGW(TAG, "rtc event queue full: deferred start after full reset dropped");
    }
}

static void tirtc_session_cancel_deferred_full_reset(void)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_deferred_full_reset_pending = false;
    s_deferred_full_reset_due_at_us = 0U;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (s_deferred_full_reset_timer != NULL) {
        (void)esp_timer_stop(s_deferred_full_reset_timer);
    }
}

static void tirtc_session_cancel_deferred_start_after_full_reset(void)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_deferred_start_after_full_reset_pending = false;
    s_deferred_start_after_full_reset_due_at_us = 0U;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (s_deferred_start_after_full_reset_timer != NULL) {
        (void)esp_timer_stop(s_deferred_start_after_full_reset_timer);
    }
}

static bool tirtc_session_start_backoff_active_locked(uint64_t now_us,
                                                       uint64_t *remaining_us,
                                                       bool *retry_timer_pending)
{
    if (remaining_us != NULL) {
        *remaining_us = 0U;
    }
    if (retry_timer_pending != NULL) {
        *retry_timer_pending = s_deferred_start_after_full_reset_pending;
    }
    if (s_next_start_allowed_us == 0U || now_us >= s_next_start_allowed_us) {
        return false;
    }
    if (remaining_us != NULL) {
        *remaining_us = s_next_start_allowed_us - now_us;
    }
    return true;
}

bool tirtc_session_schedule_deferred_full_reset(void)
{
    uint64_t due_at_us = esp_timer_get_time() + TIRTC_SESSION_DEFERRED_FULL_RESET_DELAY_US;

    if (s_deferred_full_reset_timer == NULL) {
        return false;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    s_deferred_full_reset_pending = true;
    s_deferred_full_reset_due_at_us = due_at_us;
    taskEXIT_CRITICAL(&s_rtc_lock);

    (void)esp_timer_stop(s_deferred_full_reset_timer);
    if (esp_timer_start_once(s_deferred_full_reset_timer, TIRTC_SESSION_DEFERRED_FULL_RESET_DELAY_US) != ESP_OK) {
        tirtc_session_cancel_deferred_full_reset();
        return false;
    }

    tirtc_session_note_event("rtc full reset wait");
    ESP_LOGD(TAG,
             "rtc full reset deferred: delay_us=%llu",
             (unsigned long long)TIRTC_SESSION_DEFERRED_FULL_RESET_DELAY_US);
    return true;
}

static bool tirtc_session_schedule_deferred_start_after_delay(uint64_t delay_us, const char *reason)
{
    if (s_deferred_start_after_full_reset_timer == NULL) {
        return false;
    }
    if (delay_us == 0) {
        delay_us = 1000U;
    }
    uint64_t due_at_us = esp_timer_get_time() + delay_us;

    taskENTER_CRITICAL(&s_rtc_lock);
    s_deferred_start_after_full_reset_pending = true;
    s_deferred_start_after_full_reset_due_at_us = due_at_us;
    taskEXIT_CRITICAL(&s_rtc_lock);

    (void)esp_timer_stop(s_deferred_start_after_full_reset_timer);
    if (esp_timer_start_once(s_deferred_start_after_full_reset_timer, delay_us) != ESP_OK) {
        tirtc_session_cancel_deferred_start_after_full_reset();
        return false;
    }

    tirtc_session_note_event(reason != NULL ? reason : "rtc start wait");
    ESP_LOGD(TAG,
             "rtc start deferred: reason=%s delay_us=%llu",
             reason != NULL ? reason : "start",
             (unsigned long long)delay_us);
    return true;
}

static void tirtc_session_ensure_start_backoff_timer(uint64_t remaining_us, bool retry_timer_pending)
{
    if (remaining_us == 0U || retry_timer_pending) {
        return;
    }
    if (!tirtc_session_schedule_deferred_start_after_delay(remaining_us, "rtc start backoff")) {
        ESP_LOGW(TAG, "rtc listen start backoff timer schedule failed");
    }
}

bool tirtc_session_schedule_deferred_start_after_full_reset(void)
{
    return tirtc_session_schedule_deferred_start_after_delay(TIRTC_SESSION_RESTART_AFTER_FULL_RESET_DELAY_US,
                                                            "rtc full start wait");
}

void tirtc_session_handle_deferred_full_reset(void)
{
    bool sdk_initialized = false;
    bool sdk_started = false;
    bool can_reset = false;
    bool should_restart = false;
    bool wait_stop_notice = false;
    int stop_ret = 0;

    tirtc_session_cancel_deferred_full_reset();

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_active_conn != NULL || s_closing_conn != NULL) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        tirtc_session_note_event("full reset deferred");
        ESP_LOGW(TAG, "rtc full reset deferred: connection still closing/active");
        (void)tirtc_session_schedule_deferred_full_reset();
        return;
    }

    sdk_initialized = s_sdk_initialized;
    sdk_started = s_sdk_started;
    can_reset = s_sdk_initialized || s_sdk_started || s_start_in_progress || s_sdk_prepare_in_progress;
    should_restart = s_config.enabled && s_network_connected && s_identity_ready;
    if (can_reset) {
        s_sdk_generation++;
        if (s_sdk_generation == 0U) {
            s_sdk_generation = 1U;
        }
        s_pending_stop_generation = s_sdk_generation;
        s_sdk_stop_notified = false;
        s_sdk_prepare_in_progress = false;
        s_start_in_progress = false;
        s_stop_in_progress = true;
        s_sdk_started = false;
        s_session_mode = TIRTC_SESSION_MODE_LISTEN;
        s_next_connection_auto_media = true;
        s_active_conn_auto_media = true;
        s_next_connection_defer_media = false;
        s_active_conn_defer_media = false;
        s_started_device_id[0] = '\0';
        s_started_credential_hash[0] = '\0';
        s_started_secret_len = 0;
        tirtc_session_reset_call_state_locked();
        s_local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        s_local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        tirtc_session_sync_stats_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!can_reset) {
        tirtc_session_note_event("rtc full reset idle");
        if (should_restart) {
            (void)tirtc_session_schedule_deferred_start_after_full_reset();
        }
        return;
    }

    tirtc_connect_cancel();
    tirtc_session_stop_time_stream_messages();
    tirtc_session_cancel_media_bootstrap();
    tirtc_session_flush_local_video_tx_queue();
    tirtc_session_flush_local_audio_tx_queue();
    tirtc_session_media_flush();

    ESP_LOGI(TAG,
             "rtc full reset begin: sdk_initialized=%d sdk_started=%d restart=%d",
             sdk_initialized ? 1 : 0,
             sdk_started ? 1 : 0,
             should_restart ? 1 : 0);

    if (sdk_started) {
        if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
            stop_ret = TiRtcStop();
            tirtc_session_give_sdk_api_lock();
        } else {
            stop_ret = TIRTC_E_BUSY;
        }
        if (stop_ret < 0) {
            tirtc_session_set_last_error(stop_ret);
            ESP_LOGW(TAG, "TiRtcStop during full reset failed: %s (%d)", TiRtcGetErrorStr(stop_ret), stop_ret);
        } else {
            uint32_t waited_ms = 0;
            wait_stop_notice = true;
            while (waited_ms < TIRTC_SESSION_STOP_WAIT_MS) {
                bool notified = false;

                taskENTER_CRITICAL(&s_rtc_lock);
                notified = s_sdk_stop_notified;
                taskEXIT_CRITICAL(&s_rtc_lock);
                if (notified) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                waited_ms += 20U;
            }
            if (waited_ms >= TIRTC_SESSION_STOP_WAIT_MS) {
                ESP_LOGW(TAG, "TiRtcStop notice wait timed out during full reset");
            }
        }
    }

    if (sdk_initialized) {
        if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
            TiRtcUninit();
            tirtc_session_give_sdk_api_lock();
        } else {
            ESP_LOGW(TAG, "TiRtcUninit skipped: sdk api lock busy");
        }
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    s_sdk_initialized = false;
    s_sdk_started = false;
    s_sdk_prepare_in_progress = false;
    s_start_in_progress = false;
    s_stop_in_progress = false;
    s_session_mode = TIRTC_SESSION_MODE_LISTEN;
    s_pending_stop_generation = 0U;
    s_sdk_stop_notified = true;
    s_active_conn = NULL;
    s_active_conn_accepted_at_us = 0U;
    s_active_remote_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    s_closing_conn = NULL;
    s_closing_conn_was_sdk_started = false;
    tirtc_session_reset_call_state_locked();
    s_local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    s_local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    tirtc_session_note_event("rtc full reset done");
    ESP_LOGI(TAG,
             "rtc full reset done: stop_notice=%d restart=%d",
             wait_stop_notice ? 1 : 0,
             should_restart ? 1 : 0);
    if (should_restart) {
        (void)tirtc_session_schedule_deferred_start_after_full_reset();
    }
}

void tirtc_session_handle_deferred_start_after_full_reset(void)
{
    tirtc_session_cancel_deferred_start_after_full_reset();
    tirtc_session_note_event("rtc prepare now");
    (void)tirtc_session_prepare_sdk();
}

static bool tirtc_session_schedule_disconnect_watchdog(const char *reason, uint64_t delay_us)
{
    tirtc_conn_t closing_conn = NULL;

    taskENTER_CRITICAL(&s_rtc_lock);
    closing_conn = s_closing_conn;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (closing_conn == NULL || s_disconnect_watchdog_timer == NULL) {
        return false;
    }

    (void)esp_timer_stop(s_disconnect_watchdog_timer);
    (void)esp_timer_start_once(s_disconnect_watchdog_timer, delay_us);
    ESP_LOGD(TAG,
             "schedule disconnect watchdog reason=%s hconn=%p delay_us=%llu",
             reason != NULL ? reason : "unspecified",
             closing_conn,
             (unsigned long long)delay_us);
    return true;
}

static bool tirtc_session_schedule_media_bootstrap_timer(const char *reason, uint64_t delay_us)
{
    bool should_start = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    should_start = tirtc_session_is_media_bootstrap_ready_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!should_start || s_media_bootstrap_timer == NULL) {
        return false;
    }

    (void)esp_timer_stop(s_media_bootstrap_timer);
    (void)esp_timer_start_once(s_media_bootstrap_timer, delay_us);
    tirtc_session_note_event(reason != NULL ? reason : "media bootstrap");
    ESP_LOGD(TAG,
             "rtc media bootstrap scheduled: reason=%s delay_us=%llu",
             reason != NULL ? reason : "unspecified",
             (unsigned long long)delay_us);
    return true;
}

void tirtc_session_schedule_media_bootstrap(const char *reason)
{
    bool should_start = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (tirtc_session_is_media_bootstrap_ready_locked()) {
        s_media_bootstrap_pending = true;
        should_start = true;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!should_start || s_media_bootstrap_timer == NULL) {
        return;
    }

    (void)tirtc_session_schedule_media_bootstrap_timer(reason, TIRTC_SESSION_MEDIA_BOOTSTRAP_INITIAL_DELAY_US);
}

void tirtc_session_retry_remote_media_request(bool retry_video, bool retry_audio, const char *reason)
{
    tirtc_session_retry_remote_media_request_after_delay(retry_video,
                                                        retry_audio,
                                                        reason,
                                                        TIRTC_SESSION_MEDIA_BOOTSTRAP_RETRY_DELAY_US);
}

static void tirtc_session_retry_remote_media_request_after_delay(bool retry_video,
                                                                bool retry_audio,
                                                                const char *reason,
                                                                uint64_t delay_us)
{
    bool should_start = false;

    if (!retry_video && !retry_audio) {
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (tirtc_session_is_media_bootstrap_ready_locked()) {
        if (retry_video) {
            s_remote_video_requested = false;
        }
        if (retry_audio) {
            s_remote_audio_requested = false;
            s_remote_audio_requested_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        }
        s_media_bootstrap_pending = true;
        should_start = true;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!should_start || s_media_bootstrap_timer == NULL) {
        return;
    }

    (void)tirtc_session_schedule_media_bootstrap_timer(reason, delay_us);
}

void tirtc_session_run_media_bootstrap(void)
{
    bool should_run = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_media_bootstrap_pending && tirtc_session_is_media_bootstrap_ready_locked()) {
        should_run = true;
    }
    s_media_bootstrap_pending = false;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!should_run) {
        return;
    }

    tirtc_session_note_event("media bootstrap");
    tirtc_session_apply_local_media_policy();
    tirtc_session_request_remote_media();
    if (tirtc_session_media_prepare_playback_path() != ESP_OK) {
        ESP_LOGW(TAG, "rtc media bootstrap failed: playback path not ready");
    }
}

void tirtc_session_start_time_stream_messages(void)
{
#if !TIRTC_SESSION_ENABLE_TIME_STREAM_MESSAGES
    tirtc_session_stop_time_stream_messages();
#else
    if (s_time_message_initial_timer == NULL || s_time_message_periodic_timer == NULL) {
        return;
    }

    (void)esp_timer_stop(s_time_message_initial_timer);
    (void)esp_timer_stop(s_time_message_periodic_timer);
    (void)esp_timer_start_once(s_time_message_initial_timer,
                               TIRTC_SESSION_TIME_MESSAGE_INITIAL_DELAY_US);
#endif
}

void tirtc_session_stop_time_stream_messages(void)
{
    if (s_time_message_initial_timer != NULL) {
        (void)esp_timer_stop(s_time_message_initial_timer);
    }
    if (s_time_message_periodic_timer != NULL) {
        (void)esp_timer_stop(s_time_message_periodic_timer);
    }
}

static bool tirtc_session_maybe_force_local_video_publish_locked(void)
{
    if (!s_peer_wants_video || !s_local_video_send_enabled || s_active_conn == NULL || !s_sdk_started ||
        s_start_in_progress || s_stop_in_progress || s_closing_conn != NULL || !s_call_active ||
        s_local_video_stream_id != TIRTC_SESSION_INVALID_STREAM_ID) {
        return false;
    }

    s_local_video_stream_id = TIRTC_SESSION_LOCAL_VIDEO_STREAM_ID;
    s_local_video_publish_forced = true;
    s_test_video_publish_forced = false;
    tirtc_session_sync_stats_locked();
    return true;
}

static bool tirtc_session_maybe_force_local_audio_publish_locked(void)
{
    if ((!s_peer_wants_audio && s_peer_audio_control_seen) ||
        !s_local_audio_send_enabled || s_active_conn == NULL || !s_sdk_started ||
        s_start_in_progress || s_stop_in_progress || s_closing_conn != NULL || !s_call_active ||
        s_local_audio_stream_id != TIRTC_SESSION_INVALID_STREAM_ID) {
        return false;
    }

    s_local_audio_stream_id = TIRTC_SESSION_LOCAL_AUDIO_STREAM_ID;
    s_local_audio_publish_forced = true;
    s_test_audio_publish_forced = false;
    tirtc_session_sync_stats_locked();
    return true;
}

static void tirtc_session_sync_test_media_publish_locked(bool test_video_active, bool test_audio_active)
{
    bool can_force_publish = s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
                             s_closing_conn == NULL && s_active_conn != NULL && s_call_active;
    bool changed = false;

    if (can_force_publish && test_video_active && s_local_video_send_enabled &&
        s_local_video_stream_id == TIRTC_SESSION_INVALID_STREAM_ID) {
        s_local_video_stream_id = TIRTC_SESSION_LOCAL_VIDEO_STREAM_ID;
        s_local_video_publish_forced = true;
        s_test_video_publish_forced = true;
        changed = true;
    } else if (s_test_video_publish_forced &&
               (!can_force_publish || !test_video_active || !s_local_video_send_enabled)) {
        s_local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        s_local_video_publish_forced = false;
        s_test_video_publish_forced = false;
        changed = true;
    }

    if (can_force_publish && test_audio_active && s_local_audio_send_enabled &&
        s_local_audio_stream_id == TIRTC_SESSION_INVALID_STREAM_ID) {
        s_local_audio_stream_id = TIRTC_SESSION_LOCAL_AUDIO_STREAM_ID;
        s_local_audio_publish_forced = true;
        s_test_audio_publish_forced = true;
        changed = true;
    } else if (s_test_audio_publish_forced &&
               (!can_force_publish || !test_audio_active || !s_local_audio_send_enabled)) {
        s_local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        s_local_audio_publish_forced = false;
        s_test_audio_publish_forced = false;
        changed = true;
    }

    if (changed) {
        tirtc_session_sync_stats_locked();
    }
}

static uint8_t tirtc_session_get_effective_local_video_stream_id_locked(void)
{
    if (s_local_video_stream_id != TIRTC_SESSION_INVALID_STREAM_ID) {
        return tirtc_session_normalize_local_video_stream_id(s_local_video_stream_id);
    }

    return TIRTC_SESSION_LOCAL_VIDEO_STREAM_ID;
}

static uint8_t tirtc_session_get_effective_local_audio_stream_id_locked(void)
{
    if (s_local_audio_stream_id != TIRTC_SESSION_INVALID_STREAM_ID) {
        return tirtc_session_normalize_local_audio_stream_id(s_local_audio_stream_id);
    }

    return TIRTC_SESSION_LOCAL_AUDIO_STREAM_ID;
}

static uint8_t tirtc_session_normalize_local_video_stream_id(uint8_t stream_id)
{
    return stream_id == TIRTC_SESSION_LOCAL_VIDEO_STREAM_ID ? stream_id : TIRTC_SESSION_LOCAL_VIDEO_STREAM_ID;
}

static uint8_t tirtc_session_normalize_local_audio_stream_id(uint8_t stream_id)
{
    return stream_id == TIRTC_SESSION_LOCAL_AUDIO_STREAM_ID ? stream_id : TIRTC_SESSION_LOCAL_AUDIO_STREAM_ID;
}

bool tirtc_session_should_reset_after_send_error(int error)
{
    switch (error) {
    case TIRTC_E_BUSY:
    case TIRTC_E_INVALID_PARAMETER:
    case TIRTC_E_LACK_OF_RESOURCE:
        return false;
    default:
        return error < 0;
    }
}

static bool tirtc_session_is_test_media_window_open_locked(uint64_t now_us, uint64_t retry_after_us)
{
    uint64_t accepted_at_us = s_active_conn_accepted_at_us;
    uint64_t earliest_send_us = 0U;

    if (accepted_at_us == 0U) {
        return false;
    }

    earliest_send_us = accepted_at_us + TIRTC_SESSION_TEST_MEDIA_WARMUP_US;
    if (retry_after_us > earliest_send_us) {
        earliest_send_us = retry_after_us;
    }

    return now_us >= earliest_send_us;
}

static void tirtc_session_build_local_peer_state_locked(tirtc_session_peer_state_t *state)
{
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->valid = true;
    state->call_active = s_call_active;
    state->local_video_send_enabled = s_local_video_send_enabled;
    state->local_audio_send_enabled = s_local_audio_send_enabled;
    state->video_stream_active = s_local_video_stream_id != TIRTC_SESSION_INVALID_STREAM_ID;
    state->audio_stream_active = s_local_audio_stream_id != TIRTC_SESSION_INVALID_STREAM_ID;
    memcpy(state->rgb, s_local_rgb, sizeof(state->rgb));
}

void tirtc_session_get_local_peer_state(tirtc_session_peer_state_t *state)
{
    if (state == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    tirtc_session_build_local_peer_state_locked(state);
    taskEXIT_CRITICAL(&s_rtc_lock);
}

void tirtc_session_set_last_peer_state(const tirtc_session_peer_state_t *state)
{
    if (state == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    s_last_peer_state = *state;
    taskEXIT_CRITICAL(&s_rtc_lock);
}

void tirtc_session_set_local_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_local_rgb[0] = red;
    s_local_rgb[1] = green;
    s_local_rgb[2] = blue;
    taskEXIT_CRITICAL(&s_rtc_lock);
}

void tirtc_session_set_peer_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_last_peer_state.valid = true;
    s_last_peer_state.rgb[0] = red;
    s_last_peer_state.rgb[1] = green;
    s_last_peer_state.rgb[2] = blue;
    taskEXIT_CRITICAL(&s_rtc_lock);
}

void tirtc_session_set_peer_video_requested(bool enabled)
{
    bool forced_publish = false;
    bool cleared_forced_publish = false;
    bool active = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    active = s_active_conn != NULL && s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
             s_closing_conn == NULL && s_call_active;
    if (active) {
        s_peer_wants_video = enabled;
    }
    if (active && !enabled && s_local_video_publish_forced) {
        s_local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        s_local_video_publish_forced = false;
        s_test_video_publish_forced = false;
        tirtc_session_sync_stats_locked();
        cleared_forced_publish = true;
    } else if (active && enabled) {
        forced_publish = tirtc_session_maybe_force_local_video_publish_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (forced_publish) {
        ESP_LOGD(TAG,
                 "local video fallback publish forced: peer request arrived before subscribe callback stream=%u",
                 (unsigned)TIRTC_SESSION_LOCAL_VIDEO_STREAM_ID);
    } else if (cleared_forced_publish) {
        ESP_LOGD(TAG, "local video fallback publish cleared by peer request");
    }

    tirtc_session_apply_local_media_policy();
}

void tirtc_session_set_peer_audio_requested(bool enabled)
{
    bool forced_publish = false;
    bool cleared_forced_publish = false;
    bool active = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    active = s_active_conn != NULL && s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
             s_closing_conn == NULL && s_call_active;
    if (active) {
        s_peer_wants_audio = enabled;
        s_peer_audio_control_seen = true;
    }
    if (active && !enabled && s_local_audio_publish_forced) {
        s_local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        s_local_audio_publish_forced = false;
        s_test_audio_publish_forced = false;
        tirtc_session_sync_stats_locked();
        cleared_forced_publish = true;
    } else if (active && enabled) {
        forced_publish = tirtc_session_maybe_force_local_audio_publish_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (forced_publish) {
        ESP_LOGD(TAG,
                 "local audio fallback publish forced: peer request arrived before subscribe callback stream=%u",
                 (unsigned)TIRTC_SESSION_LOCAL_AUDIO_STREAM_ID);
    } else if (cleared_forced_publish) {
        ESP_LOGD(TAG, "local audio fallback publish cleared by peer request");
    }

    tirtc_session_apply_local_media_policy();
}

void tirtc_session_get_pending_call(tirtc_conn_t *conn, uint32_t *pending_cmdw)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    if (conn != NULL) {
        *conn = (s_active_conn != NULL && s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
                 s_closing_conn == NULL) ? s_active_conn : NULL;
    }
    if (pending_cmdw != NULL) {
        *pending_cmdw = s_pending_call_cmdw;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
}

void tirtc_session_mark_incoming_call(uint32_t pending_cmdw)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_active_conn != NULL && s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
        s_closing_conn == NULL) {
        s_incoming_call_pending = true;
        s_pending_call_cmdw = pending_cmdw;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
}

static void tirtc_session_complete_call_response_internal(bool accepted, bool defer_media)
{
    bool should_bootstrap = false;
    bool was_call_active = false;
    bool builtin_media_owner = false;
    bool was_media_deferred = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    was_call_active = s_call_active;
    was_media_deferred = s_call_media_deferred;
    s_call_active = accepted;
    builtin_media_owner = s_media_profile != TIRTC_SESSION_MEDIA_PROFILE_EXTERNAL_AUDIO;
    if (accepted) {
        /*
         * Incoming IPC viewing can arrive after the UI snapshot has returned
         * the generic call controls to "idle" while no call was active.  The
         * call boundary is the one place where upload intent must be armed
         * again so microphone capture does not depend on a later UI refresh.
         * External-audio owners, such as WeChat VoIP, keep video disabled and
         * drive audio through their observer path instead of the built-in mic
         * capture path.
         */
        s_local_audio_send_enabled = true;
        if (builtin_media_owner) {
            s_local_video_send_enabled = true;
        }
        s_call_media_deferred = defer_media;
    } else {
        s_call_media_deferred = false;
    }
    s_incoming_call_pending = false;
    s_pending_call_cmdw = 0;
    tirtc_session_sync_stats_locked();
    should_bootstrap = accepted && !defer_media && (!was_call_active || was_media_deferred) &&
                       s_active_conn != NULL && s_sdk_started &&
                       !s_start_in_progress && !s_stop_in_progress && s_closing_conn == NULL;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (should_bootstrap && !tirtc_session_is_test_media_active()) {
        tirtc_session_schedule_media_bootstrap(was_media_deferred ? "deferred media allowed" :
                                                                    "call accepted");
    }
}

void tirtc_session_complete_call_response(bool accepted)
{
    tirtc_session_complete_call_response_internal(accepted, false);
}

void tirtc_session_complete_call_response_without_media(bool accepted)
{
    tirtc_session_complete_call_response_internal(accepted, true);
}

esp_err_t tirtc_session_allow_deferred_call_media(tirtc_conn_t conn)
{
    bool should_bootstrap = false;

    if (conn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (conn != s_active_conn || s_closing_conn != NULL || !s_sdk_started ||
        s_start_in_progress || s_stop_in_progress || !s_call_active) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_call_media_deferred) {
        s_call_media_deferred = false;
        s_local_audio_send_enabled = true;
        if (s_media_profile != TIRTC_SESSION_MEDIA_PROFILE_EXTERNAL_AUDIO) {
            s_local_video_send_enabled = true;
        }
        tirtc_session_sync_stats_locked();
        should_bootstrap = true;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (should_bootstrap && !tirtc_session_is_test_media_active()) {
        tirtc_session_schedule_media_bootstrap("device-call cloud and p2p ready");
    }
    tirtc_session_apply_local_media_policy();
    return ESP_OK;
}

static void tirtc_session_free_event_payload(tirtc_session_event_t *event)
{
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case TIRTC_SESSION_EVENT_REMOTE_MESSAGE:
        free(event->payload.message.data);
        event->payload.message.data = NULL;
        event->payload.message.data_len = 0;
        break;
    case TIRTC_SESSION_EVENT_REMOTE_COMMAND:
        free(event->payload.command.data);
        event->payload.command.data = NULL;
        event->payload.command.data_len = 0;
        break;
    default:
        break;
    }
}

static const char *tirtc_session_media_name(uint8_t media)
{
    switch (media) {
    case TIRTC_MEDIA_MESSAGE:
        return "message";
    case TIRTC_AUDIO_PCM:
        return "pcm";
    case TIRTC_AUDIO_AAC:
        return "aac";
    case TIRTC_VIDEO_JPEG:
        return "jpeg";
    case TIRTC_VIDEO_H264:
        return "h264";
    case TIRTC_VIDEO_H265:
        return "h265";
    default:
        break;
    }
    if (TIRTC_IS_AUDIO(media)) {
        return "audio";
    }
    if (TIRTC_IS_VIDEO(media)) {
        return "video";
    }
    return "unknown";
}

static void tirtc_session_format_payload_head(const uint8_t *data, size_t data_len, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }

    out[0] = '\0';
    if (data == NULL || data_len == 0) {
        return;
    }

    size_t preview_len = data_len < TIRTC_SESSION_MESSAGE_PREVIEW_BYTES ? data_len :
                                                                          TIRTC_SESSION_MESSAGE_PREVIEW_BYTES;
    size_t offset = 0;
    for (size_t i = 0; i < preview_len && offset < out_len; ++i) {
        int written = snprintf(out + offset,
                               out_len - offset,
                               "%s%02X",
                               i == 0 ? "" : " ",
                               data[i]);
        if (written < 0) {
            break;
        }
        if ((size_t)written >= out_len - offset) {
            out[out_len - 1] = '\0';
            break;
        }
        offset += (size_t)written;
    }
}

static void tirtc_session_handle_remote_message(const tirtc_session_event_t *event)
{
    if (event == NULL) {
        return;
    }

    const TickType_t now_tick = xTaskGetTickCount();
    const TickType_t log_interval_ticks = pdMS_TO_TICKS(TIRTC_SESSION_RX_LOG_INTERVAL_MS);
    bool log_packet = false;
    bool log_first_packet = false;
    uint32_t window_frames = 0;
    size_t window_bytes = 0;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (!s_remote_message_first_packet_logged) {
        s_remote_message_first_packet_logged = true;
        s_last_remote_message_rx_log_tick = now_tick;
        log_first_packet = true;
    }
    s_stats.rx_message_frames++;
    s_stats.rx_message_bytes += event->payload.message.data_len;
    s_remote_message_rx_window_frames++;
    s_remote_message_rx_window_bytes += event->payload.message.data_len;
    tirtc_session_set_last_event_locked("message rx");
    if (!log_first_packet &&
        (s_last_remote_message_rx_log_tick == 0 ||
         now_tick - s_last_remote_message_rx_log_tick >= log_interval_ticks)) {
        log_packet = true;
        window_frames = s_remote_message_rx_window_frames;
        window_bytes = s_remote_message_rx_window_bytes;
        s_remote_message_rx_window_frames = 0;
        s_remote_message_rx_window_bytes = 0;
        s_last_remote_message_rx_log_tick = now_tick;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (log_first_packet || log_packet) {
        char head[TIRTC_SESSION_MESSAGE_PREVIEW_TEXT_LEN] = {0};
        tirtc_session_format_payload_head(event->payload.message.data,
                                          event->payload.message.data_len,
                                          head,
                                          sizeof(head));
        ESP_LOGI(TAG,
                 "%s stream=%u media=%u(%s) flags=%u ts=%lu payload=%u%s%s",
                 log_first_packet ? "remote message first packet" : "remote message rx",
                 (unsigned)event->payload.message.stream_id,
                 (unsigned)event->payload.message.media,
                 tirtc_session_media_name(event->payload.message.media),
                 (unsigned)event->payload.message.flags,
                 (unsigned long)event->payload.message.ts,
                 (unsigned)event->payload.message.data_len,
                 head[0] != '\0' ? " head=" : "",
                 head);
        if (log_packet) {
            ESP_LOGI(TAG,
                     "remote message window frames=%lu bytes=%u",
                     (unsigned long)window_frames,
                     (unsigned)window_bytes);
        }
    }
}

static void tirtc_session_sync_stats_locked(void)
{
    s_state = tirtc_session_compute_state_locked();
    s_stats.enabled = s_config.enabled;
    s_stats.sdk_initialized = s_sdk_initialized;
    s_stats.sdk_started = s_sdk_started;
    s_stats.active_connection = (s_active_conn != NULL);
    s_stats.call_active = s_call_active;
    s_stats.incoming_call_pending = s_incoming_call_pending;
    s_stats.local_video_send_enabled = s_local_video_send_enabled;
    s_stats.local_audio_send_enabled = s_local_audio_send_enabled;
    s_stats.session_mode = s_session_mode;
    s_stats.state = s_state;
    s_stats.local_video_stream_id = s_local_video_stream_id;
    s_stats.local_audio_stream_id = s_local_audio_stream_id;
}

static uint32_t tirtc_session_next_peer_connect_generation_locked(void)
{
    s_peer_connect_generation++;
    if (s_peer_connect_generation == 0U) {
        s_peer_connect_generation = 1U;
    }
    return s_peer_connect_generation;
}

static void tirtc_session_return_to_listen_mode(void)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_session_mode = TIRTC_SESSION_MODE_LISTEN;
    s_next_connection_auto_media = true;
    s_next_connection_defer_media = false;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);
}

static void tirtc_session_set_last_event_locked(const char *event_text)
{
    strlcpy(s_stats.last_event,
            event_text != NULL ? event_text : "idle",
            sizeof(s_stats.last_event));
}

static tirtc_session_state_t tirtc_session_compute_state_locked(void)
{
    if (s_state_error_override) {
        return TIRTC_SESSION_STATE_ERROR;
    }

    if (s_stop_in_progress || s_closing_conn != NULL) {
        return TIRTC_SESSION_STATE_DISCONNECTING;
    }

    if (s_media_bootstrap_pending && s_active_conn != NULL) {
        return TIRTC_SESSION_STATE_MEDIA_BOOTSTRAPPING;
    }

    if (s_active_conn != NULL) {
        return TIRTC_SESSION_STATE_CONNECTED;
    }

    if (s_sdk_prepare_in_progress || s_start_in_progress ||
        s_deferred_full_reset_pending || s_deferred_start_after_full_reset_pending ||
        tirtc_connect_is_connecting()) {
        return TIRTC_SESSION_STATE_STARTING;
    }

    if (s_sdk_started) {
        return TIRTC_SESSION_STATE_READY;
    }

    return TIRTC_SESSION_STATE_STOPPED;
}

void tirtc_session_note_event(const char *event_text)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    tirtc_session_set_last_event_locked(event_text);
    taskEXIT_CRITICAL(&s_rtc_lock);
}

void tirtc_session_set_last_error(int error)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_stats.last_error = error;
    taskEXIT_CRITICAL(&s_rtc_lock);
}

void tirtc_session_update_state(tirtc_session_state_t state)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_state_error_override = (state == TIRTC_SESSION_STATE_ERROR);
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);
}

void tirtc_session_mark_sdk_started(void)
{
    bool notify_started = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    notify_started = !s_sdk_started || s_start_in_progress;
    if (notify_started) {
        s_sdk_generation++;
        if (s_sdk_generation == 0U) {
            s_sdk_generation = 1U;
        }
    }
    s_sdk_started = true;
    s_start_in_progress = false;
    s_stop_in_progress = false;
    s_state_error_override = false;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (notify_started) {
        tirtc_connect_on_tirtc_started();
    }
}

bool tirtc_session_mark_sdk_stopped(uint32_t generation)
{
    bool accepted = false;

    tirtc_connect_cancel();

    taskENTER_CRITICAL(&s_rtc_lock);
    accepted = generation != 0U && generation == s_sdk_generation;
    if (!accepted) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        ESP_LOGW(TAG,
                 "ignore stale rtc stopped event: event_generation=%lu current_generation=%lu",
                 (unsigned long)generation,
                 (unsigned long)s_sdk_generation);
        return false;
    }
    s_active_conn = NULL;
    s_active_conn_accepted_at_us = 0U;
    s_active_remote_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    s_closing_conn = NULL;
    s_closing_conn_was_sdk_started = false;
    s_sdk_prepare_in_progress = false;
    s_sdk_started = false;
    s_pending_stop_generation = 0U;
    s_start_in_progress = false;
    s_stop_in_progress = false;
    if (s_next_start_allowed_us == 0U ||
        (uint64_t)esp_timer_get_time() >= s_next_start_allowed_us) {
        s_next_start_allowed_us = 0U;
    }
    s_state_error_override = false;
    s_started_device_id[0] = '\0';
    s_started_credential_hash[0] = '\0';
    s_started_secret_len = 0;
    tirtc_session_reset_call_state_locked();
    s_local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    s_local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);
    return true;
}

void tirtc_session_mark_sdk_network_offline(void)
{
    tirtc_connect_cancel();

    taskENTER_CRITICAL(&s_rtc_lock);
    s_sdk_started = false;
    s_start_in_progress = false;
    s_stop_in_progress = false;
    s_next_start_allowed_us = 0U;
    s_state_error_override = false;
    s_next_connection_auto_media = true;
    s_active_conn_auto_media = true;
    s_next_connection_defer_media = false;
    s_active_conn_defer_media = false;
    s_started_device_id[0] = '\0';
    s_started_credential_hash[0] = '\0';
    s_started_secret_len = 0;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);
}

bool tirtc_session_request_runtime_restart(const char *reason)
{
    bool wait_for_disconnect = false;
    bool request_disconnect = false;
    bool request_full_reset = false;
    bool was_sdk_started = false;
    tirtc_conn_t active_conn = NULL;
    tirtc_conn_t closing_conn = NULL;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_config.enabled &&
        (s_session_mode == TIRTC_SESSION_MODE_LISTEN || s_session_mode == TIRTC_SESSION_MODE_CONNECT) &&
        s_network_connected) {
        active_conn = s_active_conn;
        closing_conn = s_closing_conn;
        if (active_conn != NULL && s_sdk_started && !s_stop_in_progress && s_closing_conn == NULL) {
            request_disconnect = true;
        } else if (closing_conn != NULL) {
            wait_for_disconnect = true;
        } else if (s_sdk_initialized || s_sdk_started || s_start_in_progress || s_sdk_prepare_in_progress) {
            request_full_reset = true;
        }
        s_restart_runtime_requested = request_disconnect || wait_for_disconnect;
        s_restart_runtime_full_requested = request_disconnect || wait_for_disconnect;
        s_force_wall_clock_sync_requested = false;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (request_disconnect) {
        tirtc_session_note_event(reason != NULL ? reason : "restart requested");
        ESP_LOGI(TAG,
                 "rtc restart requested: reason=%s action=disconnect",
                 reason != NULL ? reason : "unspecified");
        if (!tirtc_session_begin_connection_shutdown(active_conn, 0, &was_sdk_started, NULL)) {
            ESP_LOGW(TAG, "restart disconnect tracking failed hconn=%p", active_conn);
            return false;
        }

        if (!tirtc_session_enqueue_disconnect_request(active_conn, true, was_sdk_started)) {
            tirtc_session_note_event("restart disconnect drop");
            ESP_LOGW(TAG, "request disconnect for restart dropped: hconn=%p", active_conn);
            tirtc_session_complete_connection_shutdown(active_conn, was_sdk_started);
        } else {
            tirtc_session_note_event("disconnect req");
            (void)tirtc_session_schedule_disconnect_watchdog(reason, TIRTC_SESSION_DISCONNECT_TIMEOUT_US);
        }
        return true;
    }

    if (wait_for_disconnect) {
        tirtc_session_note_event(reason != NULL ? reason : "restart requested");
        ESP_LOGD(TAG,
                 "rtc restart waiting for disconnect: reason=%s",
                 reason != NULL ? reason : "unspecified");
        (void)tirtc_session_schedule_disconnect_watchdog(reason, TIRTC_SESSION_DISCONNECT_TIMEOUT_US);
        return true;
    }

    if (request_full_reset) {
        tirtc_session_note_event(reason != NULL ? reason : "restart requested");
        ESP_LOGI(TAG,
                 "rtc restart requested: reason=%s action=full-reset",
                 reason != NULL ? reason : "unspecified");
        if (!tirtc_session_schedule_deferred_full_reset()) {
            ESP_LOGW(TAG, "rtc full reset schedule failed");
            return false;
        }
        return true;
    }

    tirtc_session_note_event(reason != NULL ? reason : "restart ignored");
    ESP_LOGI(TAG,
             "rtc runtime restart collapsed to connect/disconnect lifecycle: reason=%s",
             reason != NULL ? reason : "unspecified");
    return true;
}

bool tirtc_session_consume_runtime_restart_request(bool *full_reset_requested)
{
    bool requested = false;
    bool full_reset = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    requested = s_restart_runtime_requested;
    full_reset = s_restart_runtime_full_requested;
    s_restart_runtime_requested = false;
    s_restart_runtime_full_requested = false;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (full_reset_requested != NULL) {
        *full_reset_requested = full_reset;
    }

    return requested;
}

void tirtc_session_mark_access_hijacking_detected(void)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_stats.access_hijacking_detected = true;
    taskEXIT_CRITICAL(&s_rtc_lock);
}

static bool tirtc_session_try_accept_connection_internal(tirtc_conn_t conn,
                                                         bool require_peer_attempt,
                                                         uint32_t peer_generation)
{
    bool accepted = false;
    bool newly_accepted = false;
    bool auto_media = true;
    bool defer_media = false;
    bool stop_in_progress = false;
    bool sdk_started = false;
    bool start_in_progress = false;
    tirtc_conn_t active_conn = NULL;
    tirtc_conn_t closing_conn = NULL;
    tirtc_session_mode_t session_mode = TIRTC_SESSION_MODE_LISTEN;
    uint32_t current_peer_generation = 0;

    taskENTER_CRITICAL(&s_rtc_lock);
    stop_in_progress = s_stop_in_progress;
    sdk_started = s_sdk_started;
    start_in_progress = s_start_in_progress;
    active_conn = s_active_conn;
    closing_conn = s_closing_conn;
    session_mode = s_session_mode;
    current_peer_generation = s_peer_connect_generation;
    if (conn == NULL || s_stop_in_progress || s_closing_conn != NULL || !s_sdk_started ||
        s_start_in_progress ||
        (require_peer_attempt &&
         (s_session_mode != TIRTC_SESSION_MODE_CONNECT ||
          peer_generation == 0U || peer_generation != s_peer_connect_generation))) {
        accepted = false;
    } else if (s_active_conn == NULL) {
        s_active_conn = conn;
        s_active_conn_accepted_at_us = esp_timer_get_time();
        s_active_remote_audio_stream_id = s_next_remote_audio_stream_id;
        s_active_conn_auto_media = s_next_connection_auto_media;
        s_active_conn_defer_media = s_next_connection_defer_media;
        s_remote_media_suppressed_conn = NULL;
        s_remote_media_drain_conn = NULL;
        s_next_connection_auto_media = true;
        s_next_connection_defer_media = false;
        auto_media = s_active_conn_auto_media;
        defer_media = s_active_conn_defer_media;
        accepted = true;
        newly_accepted = true;
    } else if (s_active_conn == conn) {
        auto_media = s_active_conn_auto_media;
        defer_media = s_active_conn_defer_media;
        accepted = true;
    }
    if (accepted) {
        tirtc_session_sync_stats_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!accepted) {
        ESP_LOGW(TAG,
                 "rtc connection rejected: hconn=%p active=%p closing=%p sdk_started=%d start=%d stop=%d mode=%u peer_gen=%lu/%lu",
                 conn,
                 active_conn,
                 closing_conn,
                 sdk_started,
                 start_in_progress,
                 stop_in_progress,
                 (unsigned)session_mode,
                 (unsigned long)peer_generation,
                 (unsigned long)current_peer_generation);
    } else if (newly_accepted) {
        ESP_LOGI(TAG,
                 "track accepted connection hconn=%p accepted_at_us=%llu auto_media=%d defer_media=%d",
                 conn,
                 (unsigned long long)s_active_conn_accepted_at_us,
                 auto_media ? 1 : 0,
                 defer_media ? 1 : 0);
    }

    return accepted;
}

bool tirtc_session_try_accept_connection(tirtc_conn_t conn)
{
    return tirtc_session_try_accept_connection_internal(conn, false, 0U);
}

void tirtc_session_set_next_connection_auto_media(bool enabled)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_next_connection_auto_media = enabled;
    taskEXIT_CRITICAL(&s_rtc_lock);
}

void tirtc_session_set_next_connection_defer_media(bool enabled)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_next_connection_defer_media = enabled;
    taskEXIT_CRITICAL(&s_rtc_lock);
}

bool tirtc_session_connection_auto_media_enabled(tirtc_conn_t conn)
{
    bool enabled = true;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (conn != NULL && conn == s_active_conn) {
        enabled = s_active_conn_auto_media;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
    return enabled;
}

bool tirtc_session_connection_media_deferred(tirtc_conn_t conn)
{
    bool deferred = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (conn != NULL && conn == s_active_conn) {
        deferred = s_active_conn_defer_media;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
    return deferred;
}

esp_err_t tirtc_session_track_external_connection(tirtc_conn_t conn, bool auto_media)
{
    if (conn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    tirtc_session_set_next_connection_auto_media(auto_media);
    if (!tirtc_session_try_accept_connection(conn)) {
        tirtc_session_set_next_connection_auto_media(true);
        return ESP_ERR_INVALID_STATE;
    }
    tirtc_session_set_next_connection_auto_media(true);
    tirtc_session_bind_connection_user_data(conn);

    if (!auto_media) {
        ESP_LOGI(TAG, "external connection uses external media owner: hconn=%p", conn);
    }

    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_CONN_ACCEPTED,
        .payload.conn = {
            .conn = conn,
            .error = 0,
        },
    };
    if (!tirtc_session_enqueue_event(&rtc_event, 0)) {
        tirtc_session_note_event("external accept inline");
        ESP_LOGW(TAG, "rtc event queue full: external connection handled inline");
        tirtc_session_handle_runtime_event(&rtc_event);
    }

    return ESP_OK;
}

void tirtc_session_update_local_video_subscription(tirtc_conn_t conn, uint8_t stream_id, bool subscribed)
{
    if (!APP_PRODUCT_RTC_VIDEO_ENABLED) {
        return;
    }

    uint8_t normalized_stream_id = tirtc_session_normalize_local_video_stream_id(stream_id);

    taskENTER_CRITICAL(&s_rtc_lock);
    if (conn == s_active_conn && s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
        s_closing_conn == NULL) {
        if (subscribed) {
            s_local_video_stream_id = normalized_stream_id;
            s_peer_wants_video = true;
            s_local_video_publish_forced = false;
            s_test_video_publish_forced = false;
            tirtc_session_sync_stats_locked();
        } else if (s_local_video_stream_id == normalized_stream_id) {
            s_local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
            s_peer_wants_video = false;
            s_local_video_publish_forced = false;
            s_test_video_publish_forced = false;
            tirtc_session_sync_stats_locked();
        }
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
}

void tirtc_session_update_local_audio_subscription(tirtc_conn_t conn, uint8_t stream_id, bool subscribed)
{
    uint8_t normalized_stream_id = tirtc_session_normalize_local_audio_stream_id(stream_id);

    taskENTER_CRITICAL(&s_rtc_lock);
    if (conn == s_active_conn && s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
        s_closing_conn == NULL) {
        if (subscribed) {
            s_local_audio_stream_id = normalized_stream_id;
            s_peer_wants_audio = true;
            s_peer_audio_control_seen = true;
            s_local_audio_publish_forced = false;
            s_test_audio_publish_forced = false;
            tirtc_session_sync_stats_locked();
        } else if (s_local_audio_stream_id == normalized_stream_id) {
            s_local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
            s_peer_wants_audio = false;
            s_peer_audio_control_seen = true;
            s_local_audio_publish_forced = false;
            s_test_audio_publish_forced = false;
            tirtc_session_sync_stats_locked();
        }
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
}

static bool tirtc_session_begin_connection_shutdown(tirtc_conn_t hconn,
                                                   int error,
                                                   bool *was_sdk_started,
                                                   bool *newly_detached_out)
{
    bool tracked = false;
    bool newly_detached = false;
    bool sdk_started = false;
    bool preserve_remote_media = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    sdk_started = s_sdk_started;
    if (hconn != NULL && hconn == s_active_conn) {
        s_active_conn = NULL;
        s_active_conn_accepted_at_us = 0U;
        s_active_remote_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        s_active_conn_auto_media = true;
        s_active_conn_defer_media = false;
        s_closing_conn = hconn;
        s_closing_conn_was_sdk_started = sdk_started;
        preserve_remote_media = s_remote_media_drain_conn == hconn;
        s_remote_media_drain_conn = NULL;
        s_local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        s_local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        s_local_video_publish_forced = false;
        s_local_audio_publish_forced = false;
        s_test_video_publish_forced = false;
        s_test_audio_publish_forced = false;
        tirtc_session_reset_call_state_locked();
        if (error != 0) {
            s_stats.last_error = error;
        }
        tirtc_session_sync_stats_locked();
        tracked = true;
        newly_detached = true;
    } else if (hconn != NULL && hconn == s_closing_conn) {
        if (error != 0) {
            s_stats.last_error = error;
        }
        tirtc_session_sync_stats_locked();
        tracked = true;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (was_sdk_started != NULL) {
        *was_sdk_started = sdk_started;
    }
    if (newly_detached_out != NULL) {
        *newly_detached_out = newly_detached;
    }

    if (tracked) {
        tirtc_session_cancel_media_bootstrap();
    }

    if (newly_detached) {
        tirtc_session_stop_time_stream_messages();
        tirtc_session_apply_local_media_policy();
        tirtc_session_flush_local_video_tx_queue();
        tirtc_session_flush_local_audio_tx_queue();
        if (preserve_remote_media) {
            ESP_LOGI(TAG,
                     "remote media preserved for application drain: hconn=%p",
                     hconn);
        } else {
            tirtc_session_media_flush();
        }
    }

    return tracked;
}

static bool tirtc_session_complete_connection_shutdown(tirtc_conn_t hconn, bool was_sdk_started)
{
    bool completed = false;
    bool restart_pending = false;

    tirtc_session_stop_disconnect_watchdog();

    taskENTER_CRITICAL(&s_rtc_lock);
    if (hconn != NULL && hconn == s_closing_conn) {
        s_closing_conn = NULL;
        s_closing_conn_was_sdk_started = false;
        completed = true;
    }
    if (completed && !was_sdk_started) {
        s_stop_in_progress = false;
    }
    if (completed) {
        tirtc_session_sync_stats_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!completed) {
        return false;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    restart_pending = s_restart_runtime_requested || s_restart_runtime_full_requested;
    taskEXIT_CRITICAL(&s_rtc_lock);

    /* Connection teardown returns to waiting; only explicit runtime restart crosses into SDK restart. */
    if (was_sdk_started && restart_pending &&
        tirtc_session_request_runtime_restart("restart after explicit disconnect")) {
        ESP_LOGI(TAG, "rtc restart resumed after disconnect");
        return true;
    }

    tirtc_session_return_to_listen_mode();

    ESP_LOGI(TAG,
             "rtc disconnected: next_state=%s",
             was_sdk_started ? "READY" : "STOPPED");
    return true;
}

static bool tirtc_session_is_ready_to_send_video(tirtc_conn_t *conn, uint8_t *stream_id)
{
    bool ready = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    ready = s_sdk_started && !s_start_in_progress && !s_stop_in_progress && s_closing_conn == NULL &&
            s_active_conn != NULL && s_call_active && !s_call_media_deferred &&
            s_local_video_send_enabled &&
            s_peer_wants_video && s_local_video_stream_id != TIRTC_SESSION_INVALID_STREAM_ID &&
            !s_media_bootstrap_pending;
    if (ready) {
        *conn = s_active_conn;
        *stream_id = tirtc_session_get_effective_local_video_stream_id_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
    return ready;
}

static bool tirtc_session_is_ready_to_send_audio(tirtc_conn_t *conn, uint8_t *stream_id)
{
    bool ready = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    ready = s_sdk_started && !s_start_in_progress && !s_stop_in_progress && s_closing_conn == NULL &&
            s_active_conn != NULL && s_call_active && !s_call_media_deferred &&
            s_local_audio_send_enabled &&
            s_local_audio_stream_id != TIRTC_SESSION_INVALID_STREAM_ID &&
            !s_media_bootstrap_pending;
    if (ready) {
        *conn = s_active_conn;
        *stream_id = tirtc_session_get_effective_local_audio_stream_id_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
    return ready;
}

static bool tirtc_session_is_ready_to_send_test_video(tirtc_conn_t *conn, uint8_t *stream_id)
{
    bool ready = false;
    uint64_t now_us = esp_timer_get_time();

    taskENTER_CRITICAL(&s_rtc_lock);
    ready = s_sdk_started && !s_start_in_progress && !s_stop_in_progress && s_closing_conn == NULL &&
            s_active_conn != NULL && s_call_active && !s_call_media_deferred &&
            s_local_video_send_enabled &&
            !s_media_bootstrap_pending &&
            tirtc_session_is_test_media_window_open_locked(now_us, s_test_video_retry_after_us);
    if (ready) {
        *conn = s_active_conn;
        *stream_id = tirtc_session_get_effective_local_video_stream_id_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
    return ready;
}

static bool tirtc_session_is_ready_to_send_test_audio(tirtc_conn_t *conn, uint8_t *stream_id)
{
    bool ready = false;
    uint64_t now_us = esp_timer_get_time();

    taskENTER_CRITICAL(&s_rtc_lock);
    ready = s_sdk_started && !s_start_in_progress && !s_stop_in_progress && s_closing_conn == NULL &&
            s_active_conn != NULL && s_call_active && !s_call_media_deferred &&
            s_local_audio_send_enabled &&
            !s_media_bootstrap_pending &&
            tirtc_session_is_test_media_window_open_locked(now_us, 0U);
    if (ready) {
        *conn = s_active_conn;
        *stream_id = tirtc_session_get_effective_local_audio_stream_id_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
    return ready;
}

static bool tirtc_session_is_ready_to_send_call_audio(tirtc_conn_t expected_conn,
                                                      tirtc_conn_t *conn,
                                                      uint8_t *stream_id)
{
    bool ready = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    ready = s_sdk_started && !s_start_in_progress && !s_stop_in_progress && s_closing_conn == NULL &&
            s_active_conn != NULL && (expected_conn == NULL || expected_conn == s_active_conn) &&
            s_call_active && !s_call_media_deferred && s_local_audio_send_enabled &&
            !s_media_bootstrap_pending;
    if (ready) {
        *conn = s_active_conn;
        *stream_id = tirtc_session_get_effective_local_audio_stream_id_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
    return ready;
}

static bool tirtc_session_is_ready_to_send_audio_with_gate(tirtc_session_audio_tx_gate_t gate,
                                                           tirtc_conn_t expected_conn,
                                                           tirtc_conn_t *conn,
                                                           uint8_t *stream_id)
{
    switch (gate) {
    case TIRTC_SESSION_AUDIO_TX_GATE_SUBSCRIBED:
        return tirtc_session_is_ready_to_send_audio(conn, stream_id);
    case TIRTC_SESSION_AUDIO_TX_GATE_CALL:
        return tirtc_session_is_ready_to_send_call_audio(expected_conn, conn, stream_id);
    case TIRTC_SESSION_AUDIO_TX_GATE_TEST:
    case TIRTC_SESSION_AUDIO_TX_GATE_TEST_ALAW:
        return tirtc_session_is_ready_to_send_test_audio(conn, stream_id);
    default:
        return false;
    }
}

static uint32_t tirtc_session_get_local_audio_tx_generation(void)
{
    uint32_t generation = 0;

    taskENTER_CRITICAL(&s_rtc_lock);
    generation = s_local_audio_tx_generation;
    taskEXIT_CRITICAL(&s_rtc_lock);
    return generation;
}

static uint32_t tirtc_session_get_local_video_tx_generation(void)
{
    uint32_t generation = 0;

    taskENTER_CRITICAL(&s_rtc_lock);
    generation = s_local_video_tx_generation;
    taskEXIT_CRITICAL(&s_rtc_lock);
    return generation;
}

static void tirtc_session_free_local_video_packet(tirtc_session_local_video_packet_t *packet)
{
    if (packet == NULL) {
        return;
    }

    if (packet->buffer_slot != TIRTC_SESSION_VIDEO_TX_BUFFER_SLOT_INVALID) {
        tirtc_session_release_local_video_buffer_slot(packet->buffer_slot);
    } else {
        free(packet->data);
    }
    memset(packet, 0, sizeof(*packet));
    packet->buffer_slot = TIRTC_SESSION_VIDEO_TX_BUFFER_SLOT_INVALID;
}

static void tirtc_session_free_local_audio_packet(tirtc_session_local_audio_packet_t *packet)
{
    if (packet == NULL) {
        return;
    }

    free(packet->data);
    memset(packet, 0, sizeof(*packet));
}

static void tirtc_session_drop_oldest_local_video_packet(void)
{
    tirtc_session_local_video_packet_t stale = {0};

    if (s_local_video_tx_queue != NULL && xQueueReceive(s_local_video_tx_queue, &stale, 0) == pdTRUE) {
        tirtc_session_free_local_video_packet(&stale);
    }
}

static void tirtc_session_drop_oldest_local_audio_packet(void)
{
    tirtc_session_local_audio_packet_t stale = {0};

    if (s_local_audio_tx_queue != NULL && xQueueReceive(s_local_audio_tx_queue, &stale, 0) == pdTRUE) {
        taskENTER_CRITICAL(&s_rtc_lock);
        s_stats.tx_audio_queue_pressure_drops++;
        taskEXIT_CRITICAL(&s_rtc_lock);
        tirtc_session_free_local_audio_packet(&stale);
    }
}

static void tirtc_session_note_local_audio_queue_depth(void)
{
    uint32_t depth = s_local_audio_tx_queue != NULL ?
                         (uint32_t)uxQueueMessagesWaiting(s_local_audio_tx_queue) : 0U;

    taskENTER_CRITICAL(&s_rtc_lock);
    s_stats.tx_audio_queue_depth_packets = depth;
    if (depth > s_stats.tx_audio_queue_high_water_packets) {
        s_stats.tx_audio_queue_high_water_packets = depth;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
}

static void tirtc_session_trim_local_audio_tx_queue(UBaseType_t max_packets)
{
    while (s_local_audio_tx_queue != NULL &&
           uxQueueMessagesWaiting(s_local_audio_tx_queue) >= max_packets) {
        tirtc_session_drop_oldest_local_audio_packet();
    }
}

static esp_err_t tirtc_session_acquire_local_video_buffer_slot(uint8_t *slot_out)
{
    uint8_t slot = TIRTC_SESSION_VIDEO_TX_BUFFER_SLOT_INVALID;

    if (slot_out == NULL || s_local_video_tx_free_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xQueueReceive(s_local_video_tx_free_queue, &slot, 0) != pdTRUE) {
        return ESP_ERR_NOT_FOUND;
    }

    *slot_out = slot;
    return ESP_OK;
}

static void tirtc_session_release_local_video_buffer_slot(uint8_t slot)
{
    if (slot >= TIRTC_SESSION_VIDEO_TX_BUFFER_POOL_SIZE || s_local_video_tx_free_queue == NULL) {
        return;
    }

    (void)xQueueSend(s_local_video_tx_free_queue, &slot, 0);
}

static esp_err_t tirtc_session_ensure_local_video_buffer_capacity(uint8_t slot, size_t required_size)
{
    uint8_t *new_buffer = NULL;

    if (slot >= TIRTC_SESSION_VIDEO_TX_BUFFER_POOL_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    if (required_size <= s_local_video_tx_buffer_capacities[slot]) {
        return ESP_OK;
    }

    new_buffer = tirtc_session_alloc_tx_buffer(required_size);
    ESP_RETURN_ON_FALSE(new_buffer != NULL, ESP_ERR_NO_MEM, TAG, "local video pool alloc failed");

    free(s_local_video_tx_buffers[slot]);
    s_local_video_tx_buffers[slot] = new_buffer;
    s_local_video_tx_buffer_capacities[slot] = required_size;
    return ESP_OK;
}

static esp_err_t tirtc_session_enqueue_local_video_packet(const uint8_t *data,
                                                         size_t data_len,
                                                         uint16_t width,
                                                         uint16_t height,
                                                         uint64_t pts_us,
                                                         uint8_t media,
                                                         uint8_t flags,
                                                         const TIRTCFRAMEINFO *frame_info,
                                                         bool test_frame)
{
    tirtc_session_local_video_packet_t packet = {
        .generation = tirtc_session_get_local_video_tx_generation(),
        .width = width,
        .height = height,
        .pts_us = pts_us,
        .buffer_slot = TIRTC_SESSION_VIDEO_TX_BUFFER_SLOT_INVALID,
        .data_len = data_len,
        .media = media,
        .flags = flags,
        .has_frame_info = frame_info != NULL,
        .test_frame = test_frame,
    };
    uint8_t buffer_slot = TIRTC_SESSION_VIDEO_TX_BUFFER_SLOT_INVALID;
    esp_err_t slot_ret = ESP_OK;

    if (s_local_video_tx_queue == NULL || s_local_video_tx_free_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    slot_ret = tirtc_session_acquire_local_video_buffer_slot(&buffer_slot);
    if (slot_ret != ESP_OK) {
        tirtc_session_drop_oldest_local_video_packet();
        slot_ret = tirtc_session_acquire_local_video_buffer_slot(&buffer_slot);
        if (slot_ret != ESP_OK) {
            return slot_ret;
        }
    }

    slot_ret = tirtc_session_ensure_local_video_buffer_capacity(buffer_slot, data_len);
    if (slot_ret != ESP_OK) {
        tirtc_session_release_local_video_buffer_slot(buffer_slot);
        return slot_ret;
    }

    packet.buffer_slot = buffer_slot;
    packet.data = s_local_video_tx_buffers[buffer_slot];
    if (frame_info != NULL) {
        packet.frame_info = *frame_info;
    }
    memcpy(packet.data, data, data_len);

    if (xQueueSend(s_local_video_tx_queue, &packet, 0) == pdTRUE) {
        memset(&packet, 0, sizeof(packet));
        packet.buffer_slot = TIRTC_SESSION_VIDEO_TX_BUFFER_SLOT_INVALID;
        return ESP_OK;
    }

    tirtc_session_drop_oldest_local_video_packet();
    if (xQueueSend(s_local_video_tx_queue, &packet, 0) == pdTRUE) {
        memset(&packet, 0, sizeof(packet));
        packet.buffer_slot = TIRTC_SESSION_VIDEO_TX_BUFFER_SLOT_INVALID;
        return ESP_OK;
    }

    tirtc_session_free_local_video_packet(&packet);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t tirtc_session_enqueue_local_audio_packet(const uint8_t *data,
                                                         size_t data_len,
                                                         const tirtc_session_audio_format_t *format,
                                                         uint64_t pts_us,
                                                         tirtc_session_audio_tx_gate_t gate,
                                                         tirtc_conn_t expected_conn)
{
    tirtc_session_local_audio_packet_t packet = {
        .generation = tirtc_session_get_local_audio_tx_generation(),
        .expected_conn = expected_conn,
        .pts_us = pts_us,
        .data_len = data_len,
        .gate = gate,
    };

    ESP_RETURN_ON_FALSE(format != NULL, ESP_ERR_INVALID_ARG, TAG, "local audio format required");
    packet.format = *format;

    if (s_local_audio_tx_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    packet.data = tirtc_session_alloc_tx_buffer(data_len);
    ESP_RETURN_ON_FALSE(packet.data != NULL, ESP_ERR_NO_MEM, TAG, "local audio packet alloc failed");
    memcpy(packet.data, data, data_len);

    tirtc_session_trim_local_audio_tx_queue(TIRTC_SESSION_AUDIO_TX_TARGET_BACKLOG);

    if (xQueueSend(s_local_audio_tx_queue, &packet, 0) == pdTRUE) {
        tirtc_session_note_local_audio_queue_depth();
        memset(&packet, 0, sizeof(packet));
        return ESP_OK;
    }

    tirtc_session_drop_oldest_local_audio_packet();
    if (xQueueSend(s_local_audio_tx_queue, &packet, 0) == pdTRUE) {
        tirtc_session_note_local_audio_queue_depth();
        memset(&packet, 0, sizeof(packet));
        return ESP_OK;
    }

    tirtc_session_free_local_audio_packet(&packet);
    return ESP_ERR_TIMEOUT;
}

static void tirtc_session_flush_local_video_tx_queue(void)
{
    tirtc_session_local_video_packet_t packet = {0};

    taskENTER_CRITICAL(&s_rtc_lock);
    s_local_video_tx_generation++;
    taskEXIT_CRITICAL(&s_rtc_lock);

    while (s_local_video_tx_queue != NULL && xQueueReceive(s_local_video_tx_queue, &packet, 0) == pdTRUE) {
        tirtc_session_free_local_video_packet(&packet);
    }
}

static void tirtc_session_flush_local_audio_tx_queue(void)
{
    tirtc_session_local_audio_packet_t packet = {0};

    taskENTER_CRITICAL(&s_rtc_lock);
    s_local_audio_tx_generation++;
    s_local_audio_tx_window_frames = 0;
    s_local_audio_tx_window_payload_bytes = 0;
    s_local_audio_tx_window_peak_percent = 0;
    s_last_local_audio_tx_log_tick = 0;
    taskEXIT_CRITICAL(&s_rtc_lock);

    while (s_local_audio_tx_queue != NULL && xQueueReceive(s_local_audio_tx_queue, &packet, 0) == pdTRUE) {
        tirtc_session_free_local_audio_packet(&packet);
    }
}

static bool tirtc_session_enqueue_event(const tirtc_session_event_t *event, TickType_t wait_ticks)
{
    if (s_event_queue == NULL || event == NULL) {
        return false;
    }
    return xQueueSend(s_event_queue, event, wait_ticks) == pdTRUE;
}

static bool tirtc_session_enqueue_start_if_ready(void)
{
    tirtc_session_event_t event = {
        .type = TIRTC_SESSION_EVENT_START_IF_READY,
    };

    return tirtc_session_enqueue_event(&event, TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS);
}

static bool tirtc_session_enqueue_teardown_event(const tirtc_session_event_t *event)
{
    return tirtc_session_enqueue_event(event, TIRTC_SESSION_TEARDOWN_EVENT_WAIT_TICKS);
}

static bool tirtc_session_enqueue_disconnect_request(tirtc_conn_t conn,
                                                     bool complete_shutdown,
                                                     bool was_sdk_started)
{
    tirtc_session_event_t event = {
        .type = TIRTC_SESSION_EVENT_DISCONNECT_REQUEST,
    };

    event.payload.disconnect.conn = conn;
    event.payload.disconnect.complete_shutdown = complete_shutdown;
    event.payload.disconnect.was_sdk_started = was_sdk_started;

    return tirtc_session_enqueue_teardown_event(&event);
}

static void tirtc_session_copy_config_snapshot(tirtc_session_config_t *config)
{
    if (config == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    *config = s_config;
    taskEXIT_CRITICAL(&s_rtc_lock);
}

static bool tirtc_session_extract_start_identity(const tirtc_session_config_t *config,
                                                 char *device_id,
                                                 size_t device_id_size,
                                                 char *secret_key,
                                                 size_t secret_key_size)
{
    if (config == NULL || device_id == NULL || secret_key == NULL ||
        device_id_size == 0 || secret_key_size == 0) {
        return false;
    }

    device_id[0] = '\0';
    secret_key[0] = '\0';
    strlcpy(device_id, config->device_id, device_id_size);
    strlcpy(secret_key, config->device_secret_key, secret_key_size);

    if (config->device_license[0] != '\0') {
        const char *comma = strchr(config->device_license, ',');
        if (comma != NULL) {
            if (device_id[0] == '\0') {
                size_t id_len = (size_t)(comma - config->device_license);
                if (id_len >= device_id_size) {
                    id_len = device_id_size - 1U;
                }
                memcpy(device_id, config->device_license, id_len);
                device_id[id_len] = '\0';
            }
            if (secret_key[0] == '\0') {
                strlcpy(secret_key, comma + 1, secret_key_size);
            }
        } else if (device_id[0] == '\0') {
            strlcpy(device_id, config->device_license, device_id_size);
        }
    }

    return device_id[0] != '\0' && secret_key[0] != '\0';
}

static void tirtc_session_clear_start_in_progress(void)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_start_in_progress = false;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);
}

static void tirtc_session_handle_disconnect_request(const tirtc_session_event_t *event)
{
    tirtc_conn_t conn = NULL;
    bool complete_shutdown = false;
    bool was_sdk_started = false;

    if (event == NULL) {
        return;
    }

    conn = event->payload.disconnect.conn;
    complete_shutdown = event->payload.disconnect.complete_shutdown;
    was_sdk_started = event->payload.disconnect.was_sdk_started;
    if (conn == NULL) {
        return;
    }

    int ret = tirtc_session_disconnect_with_sdk_lock(conn);
    if (ret < 0) {
        tirtc_session_set_last_error(ret);
        ESP_LOGW(TAG,
                 "rtc async disconnect failed: hconn=%p err=%s",
                 conn,
                 TiRtcGetErrorStr(ret));
        if (complete_shutdown) {
            tirtc_session_complete_connection_shutdown(conn, was_sdk_started);
        }
        return;
    }

    if (complete_shutdown) {
        tirtc_session_note_event("disconnect req");
        ESP_LOGI(TAG,
                 "rtc disconnect requested: hconn=%p ret=%d, waiting for SDK callback",
                 conn,
                 ret);
    }
}

static esp_err_t tirtc_session_copy_payload(const void *data, size_t data_len, uint8_t **out_copy)
{
    if (out_copy == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_copy = NULL;
    if (data == NULL || data_len == 0) {
        return ESP_OK;
    }

    uint8_t *copy = heap_caps_malloc(data_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == NULL) {
        copy = malloc(data_len);
    }
    ESP_RETURN_ON_FALSE(copy != NULL, ESP_ERR_NO_MEM, TAG, "payload copy alloc failed");
    memcpy(copy, data, data_len);
    *out_copy = copy;
    return ESP_OK;
}

static esp_err_t tirtc_session_validate_service_endpoint(const tirtc_session_config_t *config)
{
    if (config == NULL || strncmp(config->service_endpoint, "https://", 8) != 0) {
        ESP_LOGE(TAG, "plaintext TiRTC service endpoint rejected");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t tirtc_session_prepare_sdk_with_lock(void)
{
    int option_ret = 0;
    uint32_t max_send_buffer = TIRTC_SESSION_MAX_SEND_BUFFER;

    if (s_sdk_initialized) {
        return ESP_OK;
    }

    tirtc_session_configure_sdk_logs(true);
    ESP_LOGI(TAG, "rtc sdk version: %s build=%s", TiRtcGetVersion(), TiRtcGetBuildInfo());

    option_ret = TiRtcSetOption(TIRTC_OPT_MAX_SEND_BUFFER,
                                &max_send_buffer,
                                (uint32_t)sizeof(max_send_buffer));
    if (option_ret != 0) {
        tirtc_session_set_last_error(option_ret);
        tirtc_session_note_event("set send buffer failed");
        ESP_LOGE(TAG,
                 "TiRtcSetOption(MAX_SEND_BUFFER) failed: %s (%d)",
                 TiRtcGetErrorStr(option_ret),
                 option_ret);
        tirtc_session_update_state(TIRTC_SESSION_STATE_ERROR);
        return ESP_FAIL;
    }

    if (s_config.service_endpoint[0] != '\0') {
        option_ret = TiRtcSetOption(TIRTC_OPT_SERVICE_ENDPOINT,
                                    s_config.service_endpoint,
                                    (uint32_t)(strlen(s_config.service_endpoint) + 1U));
        if (option_ret != 0) {
            tirtc_session_set_last_error(option_ret);
            tirtc_session_note_event("set endpoint failed");
            ESP_LOGE(TAG, "TiRtcSetOption(SERVICE_ENDPOINT) failed: %s (%d)", TiRtcGetErrorStr(option_ret), option_ret);
            tirtc_session_update_state(TIRTC_SESSION_STATE_ERROR);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "rtc sdk option set: service_endpoint=%s", s_config.service_endpoint);
    }

    ESP_LOGI(TAG, "rtc sdk init stage: TiRtcInit begin");
    int init_ret = TiRtcInit();
    if (init_ret < 0) {
        tirtc_session_set_last_error(init_ret);
        tirtc_session_note_event("init failed");
        ESP_LOGE(TAG, "TiRtcInit failed: %s", TiRtcGetErrorStr(init_ret));
        tirtc_session_update_state(TIRTC_SESSION_STATE_ERROR);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "rtc sdk init stage: TiRtcInit done");

    taskENTER_CRITICAL(&s_rtc_lock);
    s_sdk_initialized = true;
    s_sdk_started = false;
    s_start_in_progress = false;
    s_stop_in_progress = false;
    s_sdk_stop_notified = true;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    ESP_LOGI(TAG, "rtc sdk init ready: TiRtcInit complete, listen start can be queued");
    return ESP_OK;
}

static const char *tirtc_session_start_error_name(int error)
{
    if (error == TIRTC_SESSION_SERVICE_CODE_CLIENT_ID_CONFLICT) {
        return "TIRTC_SERVICE_CLIENT_ID_CONFLICT";
    }
    return TiRtcGetErrorStr(error);
}

static esp_err_t tirtc_session_start_sdk_from_worker(void)
{
    tirtc_session_config_t config = {0};
    char device_id[TIRTC_SESSION_DEVICE_ID_MAX_LEN] = {0};
    char secret_key[TIRTC_SESSION_SECRET_KEY_MAX_LEN] = {0};
    char client_id[TIRTC_SESSION_CLIENT_ID_MAX_LEN] = {0};
    char credential_material[TIRTC_SESSION_DEVICE_LICENSE_MAX_LEN] = {0};
    bool already_started = false;
    bool can_start = false;
    bool identity_ready = false;
    bool start_backoff_active = false;
    bool retry_timer_pending = false;
    uint64_t retry_remaining_us = 0U;
    int option_ret = 0;
    int start_ret = 0;
    uint32_t sys_started_cb_before = 0;
    uint32_t sys_started_cb_after = 0;
    int64_t start_begin_us = 0;
    int64_t start_elapsed_ms = 0;

    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "rtc not initialized");

    tirtc_session_copy_config_snapshot(&config);
    if (!config.enabled) {
        tirtc_session_note_event("rtc disabled");
        return ESP_OK;
    }

    if (!tirtc_session_extract_start_identity(&config,
                                              device_id,
                                              sizeof(device_id),
                                              secret_key,
                                              sizeof(secret_key))) {
        tirtc_session_note_event("start identity empty");
        ESP_LOGE(TAG, "rtc listen start failed: device_id/device_secret_key is empty");
        return ESP_ERR_INVALID_ARG;
    }

    if (config.client_id[0] == '\0') {
        tirtc_session_note_event("start client id empty");
        ESP_LOGE(TAG, "rtc listen start failed: client_id is empty");
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(client_id, config.client_id, sizeof(client_id));

    uint64_t now_us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_rtc_lock);
    identity_ready = s_identity_ready;
    start_backoff_active = tirtc_session_start_backoff_active_locked(now_us,
                                                                     &retry_remaining_us,
                                                                     &retry_timer_pending);
    already_started = identity_ready && s_network_connected && s_sdk_initialized && s_sdk_started &&
                      !s_start_in_progress && !s_stop_in_progress;
    can_start = identity_ready && s_network_connected && s_sdk_initialized && !s_sdk_started &&
                 !s_sdk_prepare_in_progress && !s_start_in_progress && !s_stop_in_progress &&
                 s_active_conn == NULL && s_closing_conn == NULL && !start_backoff_active;
    if (can_start) {
        s_start_in_progress = true;
        tirtc_session_sync_stats_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (already_started) {
        return ESP_OK;
    }
    if (!can_start) {
        if (!identity_ready) {
            tirtc_session_note_event("identity not ready");
            ESP_LOGD(TAG, "rtc listen start waits for verified device identity");
        } else if (start_backoff_active) {
            tirtc_session_note_event("start backoff");
            tirtc_session_ensure_start_backoff_timer(retry_remaining_us, retry_timer_pending);
            ESP_LOGD(TAG,
                     "rtc listen start event ignored during backoff: retry_in_ms=%llu",
                     (unsigned long long)((retry_remaining_us + 999ULL) / 1000ULL));
        }
        return ESP_ERR_INVALID_STATE;
    }

    int written = snprintf(credential_material,
                           sizeof(credential_material),
                           "%s,%s",
                           device_id,
                           secret_key);
    if (written < 0 || written >= (int)sizeof(credential_material)) {
        tirtc_session_clear_start_in_progress();
        tirtc_session_note_event("start credential too long");
        ESP_LOGE(TAG, "rtc listen start failed: device credential buffer too small");
        return ESP_ERR_INVALID_SIZE;
    }

    char credential_hash[TIRTC_SESSION_SHA256_HEX_LEN] = {0};
    if (!tirtc_session_sha256_hex(credential_material, credential_hash, sizeof(credential_hash))) {
        strlcpy(credential_hash, "hash-failed", sizeof(credential_hash));
    }

    if (!tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        tirtc_session_clear_start_in_progress();
        tirtc_session_note_event("sdk lock failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "rtc listen start stage: TiRtcStart begin endpoint=%s start_arg=device_id device_id=%s client_id=%s secret_len=%u",
             config.service_endpoint,
             device_id,
             client_id,
             (unsigned)strlen(secret_key));
    option_ret = TiRtcSetOption(TIRTC_OPT_DEVICE_SECRET_KEY,
                                secret_key,
                                (uint32_t)strlen(secret_key) + 1U);
    if (option_ret != 0) {
        tirtc_session_give_sdk_api_lock();
        tirtc_session_clear_start_in_progress();
        tirtc_session_set_last_error(option_ret);
        tirtc_session_note_event("set secret failed");
        ESP_LOGE(TAG, "TiRtcSetOption(DEVICE_SECRET_KEY) failed: %s (%d)",
                 TiRtcGetErrorStr(option_ret),
                 option_ret);
        tirtc_session_update_state(TIRTC_SESSION_STATE_ERROR);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG,
             "rtc listen start option set: device_secret_key length=%u",
             (unsigned)strlen(secret_key));

    option_ret = TiRtcSetOption(TIRTC_OPT_CLIENT_ID,
                                client_id,
                                (uint32_t)strlen(client_id) + 1U);
    if (option_ret != 0) {
        tirtc_session_give_sdk_api_lock();
        tirtc_session_clear_start_in_progress();
        tirtc_session_set_last_error(option_ret);
        tirtc_session_note_event("set client id failed");
        ESP_LOGE(TAG, "TiRtcSetOption(CLIENT_ID) failed: %s (%d)",
                 TiRtcGetErrorStr(option_ret),
                 option_ret);
        tirtc_session_update_state(TIRTC_SESSION_STATE_ERROR);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG,
             "rtc listen start option set: client_id_len=%u",
             (unsigned)strlen(client_id));

    taskENTER_CRITICAL(&s_rtc_lock);
    sys_started_cb_before = s_sys_started_callback_count;
    taskEXIT_CRITICAL(&s_rtc_lock);

    tirtc_session_log_start_resources("before-TiRtcStart");
    start_begin_us = esp_timer_get_time();
    start_ret = TiRtcStart(device_id, &s_tirtc_callbacks);
    start_elapsed_ms = (esp_timer_get_time() - start_begin_us) / 1000;
    tirtc_session_log_start_resources("after-TiRtcStart");
    taskENTER_CRITICAL(&s_rtc_lock);
    sys_started_cb_after = s_sys_started_callback_count;
    taskEXIT_CRITICAL(&s_rtc_lock);

    ESP_LOGI(TAG,
             "rtc listen start stage: TiRtcStart returned ret=%d elapsed_ms=%lld sys_started_cb=%d cb_before=%lu cb_after=%lu",
             start_ret,
             (long long)start_elapsed_ms,
             sys_started_cb_after != sys_started_cb_before ? 1 : 0,
             (unsigned long)sys_started_cb_before,
             (unsigned long)sys_started_cb_after);

    if (start_ret != 0) {
        /*
         * 40305 means this device_id belongs to another client_id. Retry the
         * same stable physical client_id after backoff; never replace it with
         * device_id because one physical client_id may serve successive IDs.
         */
        uint64_t retry_delay_us = start_ret == TIRTC_SESSION_SERVICE_CODE_CLIENT_ID_CONFLICT ?
                                  TIRTC_SESSION_CLIENT_ID_CONFLICT_RETRY_DELAY_US :
                                  TIRTC_SESSION_START_RETRY_DELAY_US;

        /*
         * Keep the next attempt aligned with the official example lifecycle:
         * a failed TiRtcStart() must not leave the SDK in an initialized but
         * non-started half-state.  Retry from TiRtcInit() after the backoff.
         */
        TiRtcUninit();
        tirtc_session_give_sdk_api_lock();

        tirtc_session_clear_start_in_progress();
        tirtc_session_set_last_error(start_ret);
        tirtc_session_note_event("start failed");
        taskENTER_CRITICAL(&s_rtc_lock);
        s_sdk_initialized = false;
        s_sdk_started = false;
        s_sdk_prepare_in_progress = false;
        s_stop_in_progress = false;
        s_sdk_stop_notified = true;
        s_next_start_allowed_us = esp_timer_get_time() + retry_delay_us;
        tirtc_session_sync_stats_locked();
        taskEXIT_CRITICAL(&s_rtc_lock);
        ESP_LOGE(TAG,
                 "TiRtcStart failed: %s (%d) device_id=%s client_id=%s retry_in_ms=%llu",
                 tirtc_session_start_error_name(start_ret),
                 start_ret,
                 device_id,
                 client_id,
                 (unsigned long long)(retry_delay_us / 1000ULL));
        tirtc_session_update_state(TIRTC_SESSION_STATE_ERROR);
        tirtc_session_notify_start_error(start_ret, device_id, client_id);
        if (!tirtc_session_schedule_deferred_start_after_delay(retry_delay_us, "rtc start retry")) {
            ESP_LOGW(TAG, "rtc listen start retry schedule failed");
        }
        return ESP_FAIL;
    }

    tirtc_session_give_sdk_api_lock();

    taskENTER_CRITICAL(&s_rtc_lock);
    strlcpy(s_started_device_id, device_id, sizeof(s_started_device_id));
    strlcpy(s_started_credential_hash, credential_hash, sizeof(s_started_credential_hash));
    s_started_secret_len = (uint32_t)strlen(secret_key);
    s_next_start_allowed_us = 0U;
    taskEXIT_CRITICAL(&s_rtc_lock);

    /*
     * v2.2.0 TiRtcStart() performs /v1/start + listen synchronously and returns
     * after the listener is ready. Some integrations also emit SYS_STARTED from
     * inside TiRtcStart(); make the state transition idempotent so the device is
     * not left in start_in_progress if that callback is delayed or swallowed.
     */
    if (sys_started_cb_after == sys_started_cb_before) {
        ESP_LOGW(TAG,
                 "rtc listen start returned OK but SYS_STARTED callback was not observed; using return-path fallback");
    }
    tirtc_session_mark_sdk_started();
    tirtc_session_note_event("rtc started");
    ESP_LOGI(TAG, "rtc system started");

    tirtc_session_note_event("listen start req");
    ESP_LOGI(TAG, "rtc listen start stage: TiRtcStart accepted client_id=%s", client_id);
    return ESP_OK;
}

esp_err_t tirtc_session_send_command_raw_wait(tirtc_conn_t conn,
                                              uint32_t cmdw,
                                              const void *data,
                                              size_t data_len,
                                              TickType_t wait_ticks)
{
    if (!tirtc_session_is_connection_usable(conn)) {
        ESP_LOGW(TAG, "rtc command skipped: inactive connection cmd=0x%08lx", (unsigned long)cmdw);
        return ESP_ERR_INVALID_STATE;
    }

    int ret = TIRTC_E_BUSY;
    if (tirtc_session_take_sdk_api_lock(wait_ticks)) {
        if (!tirtc_session_is_connection_usable(conn)) {
            tirtc_session_give_sdk_api_lock();
            ESP_LOGW(TAG, "rtc command skipped after state changed: cmd=0x%08lx", (unsigned long)cmdw);
            return ESP_ERR_INVALID_STATE;
        }
        ret = TiRtcSendCommand(conn, cmdw, data, (uint32_t)data_len);
        tirtc_session_give_sdk_api_lock();
    }
    if (ret >= 0) {
        ESP_LOGI(TAG,
                 "rtc command sent: hconn=%p cmd=0x%08lx len=%lu ret=%d",
                 conn,
                 (unsigned long)cmdw,
                 (unsigned long)data_len,
                 ret);
        return ESP_OK;
    }

    if (ret == TIRTC_E_INVALID_HANDLE) {
        if (tirtc_session_is_media_request_command(cmdw) &&
            tirtc_session_should_retry_media_request_after_invalid_handle(conn, "send media request command")) {
            ESP_LOGD(TAG,
                     "send command 0x%08lx delayed for media retry after early INVALID_HANDLE",
                     (unsigned long)cmdw);
            return ESP_FAIL;
        }

        tirtc_session_set_last_error(ret);
        tirtc_session_handle_connection_loss(conn, ret);
    } else {
        tirtc_session_set_last_error(ret);
    }
    ESP_LOGW(TAG, "rtc command send failed: cmd=0x%08lx err=%s", (unsigned long)cmdw, TiRtcGetErrorStr(ret));
    return ESP_FAIL;
}

esp_err_t tirtc_session_send_command_raw(tirtc_conn_t conn, uint32_t cmdw, const void *data, size_t data_len)
{
    return tirtc_session_send_command_raw_wait(conn,
                                               cmdw,
                                               data,
                                               data_len,
                                               TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS);
}

esp_err_t tirtc_session_subscribe_audio(tirtc_conn_t conn, uint8_t stream_id)
{
    if (!tirtc_session_is_connection_usable(conn)) {
        return ESP_ERR_INVALID_STATE;
    }

    int ret = TIRTC_E_BUSY;
    if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        if (!tirtc_session_is_connection_usable(conn)) {
            tirtc_session_give_sdk_api_lock();
            return ESP_ERR_INVALID_STATE;
        }
        ret = TiRtcSubscribeAudio(conn, stream_id);
        tirtc_session_give_sdk_api_lock();
    }

    if (ret >= 0) {
        return ESP_OK;
    }
    if (ret == TIRTC_E_INVALID_HANDLE) {
        tirtc_session_handle_connection_loss(conn, ret);
    } else {
        tirtc_session_set_last_error(ret);
    }
    ESP_LOGW(TAG, "subscribe audio failed stream=%u err=%s", stream_id, TiRtcGetErrorStr(ret));
    return ESP_FAIL;
}

esp_err_t tirtc_session_unsubscribe_audio(tirtc_conn_t conn, uint8_t stream_id)
{
    if (!tirtc_session_is_connection_usable(conn)) {
        return ESP_ERR_INVALID_STATE;
    }

    int ret = TIRTC_E_BUSY;
    if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        if (!tirtc_session_is_connection_usable(conn)) {
            tirtc_session_give_sdk_api_lock();
            return ESP_ERR_INVALID_STATE;
        }
        ret = TiRtcUnsubscribeAudio(conn, stream_id);
        tirtc_session_give_sdk_api_lock();
    }

    if (ret >= 0) {
        return ESP_OK;
    }
    if (ret == TIRTC_E_INVALID_HANDLE) {
        tirtc_session_handle_connection_loss(conn, ret);
    } else {
        tirtc_session_set_last_error(ret);
    }
    ESP_LOGW(TAG, "unsubscribe audio failed stream=%u err=%s", stream_id, TiRtcGetErrorStr(ret));
    return ESP_FAIL;
}

static esp_err_t tirtc_session_request_remote_audio(tirtc_conn_t conn)
{
    uint8_t stream_id = TIRTC_SESSION_REMOTE_AUDIO_STREAM_ID;

    if (!tirtc_session_is_connection_usable(conn)) {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    stream_id = s_active_remote_audio_stream_id != TIRTC_SESSION_INVALID_STREAM_ID ?
                s_active_remote_audio_stream_id : s_next_remote_audio_stream_id;
    taskEXIT_CRITICAL(&s_rtc_lock);

    int ret = TIRTC_E_BUSY;
    if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        if (!tirtc_session_is_connection_usable(conn)) {
            tirtc_session_give_sdk_api_lock();
            return ESP_ERR_INVALID_STATE;
        }
        ret = TiRtcSubscribeAudio(conn, stream_id);
        tirtc_session_give_sdk_api_lock();
    }
    if (ret < 0) {
        if (ret == TIRTC_E_INVALID_HANDLE &&
            tirtc_session_should_retry_media_request_after_invalid_handle(conn, "subscribe remote audio")) {
            return ESP_FAIL;
        }

        if (ret == TIRTC_E_INVALID_HANDLE) {
            tirtc_session_handle_connection_loss(conn, ret);
        } else {
            tirtc_session_set_last_error(ret);
            ESP_LOGW(TAG, "request remote audio failed: %s", TiRtcGetErrorStr(ret));
        }
        return ESP_FAIL;
    }

    esp_err_t cmd_ret = tirtc_session_send_media_toggle_request(TIRTC_SESSION_CMD_REQ_AUDIO, true);
    if (cmd_ret != ESP_OK) {
        ESP_LOGW(TAG, "remote audio request command failed: %s", esp_err_to_name(cmd_ret));
        (void)tirtc_session_unsubscribe_audio(conn, stream_id);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "remote audio requested: stream=%u", (unsigned)stream_id);

    taskENTER_CRITICAL(&s_rtc_lock);
    if (conn == s_active_conn && s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
        s_closing_conn == NULL) {
        s_remote_audio_requested = true;
        s_remote_audio_requested_stream_id = stream_id;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    tirtc_session_note_event("remote audio req");
    return ESP_OK;
}

static esp_err_t tirtc_session_request_remote_video(tirtc_conn_t conn)
{
    if (!tirtc_session_is_connection_usable(conn)) {
        return ESP_ERR_INVALID_STATE;
    }

    int ret = TIRTC_E_BUSY;
    if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        if (!tirtc_session_is_connection_usable(conn)) {
            tirtc_session_give_sdk_api_lock();
            return ESP_ERR_INVALID_STATE;
        }
        ret = TiRtcSubscribeVideo(conn, TIRTC_SESSION_REMOTE_JPEG_STREAM_ID);
        tirtc_session_give_sdk_api_lock();
    }
    if (ret < 0) {
        if (ret == TIRTC_E_INVALID_HANDLE &&
            tirtc_session_should_retry_media_request_after_invalid_handle(conn, "subscribe remote video")) {
            return ESP_FAIL;
        }

        if (ret == TIRTC_E_INVALID_HANDLE) {
            tirtc_session_handle_connection_loss(conn, ret);
        } else {
            tirtc_session_set_last_error(ret);
            ESP_LOGW(TAG, "request remote video failed: %s", TiRtcGetErrorStr(ret));
        }
        return ESP_FAIL;
    }

    esp_err_t cmd_ret = tirtc_session_send_media_toggle_request(TIRTC_SESSION_CMD_REQ_VIDEO, true);
    if (cmd_ret != ESP_OK) {
        ESP_LOGW(TAG, "remote video request command failed: %s", esp_err_to_name(cmd_ret));
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "remote video requested: stream=%u", (unsigned)TIRTC_SESSION_REMOTE_JPEG_STREAM_ID);
    (void)tirtc_session_request_remote_key_frame(conn,
                                                TIRTC_SESSION_REMOTE_JPEG_STREAM_ID,
                                                "remote video requested");

    taskENTER_CRITICAL(&s_rtc_lock);
    if (conn == s_active_conn && s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
        s_closing_conn == NULL) {
        s_remote_video_requested = true;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    tirtc_session_note_event("remote video req");
    return ESP_OK;
}

void tirtc_session_request_remote_media(void)
{
    tirtc_conn_t conn = NULL;
    bool request_video = false;
    bool request_audio = false;
    bool retry_video = false;
    bool retry_audio = false;

    if (tirtc_session_is_test_media_active()) {
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_sdk_started && !s_start_in_progress && !s_stop_in_progress && s_closing_conn == NULL &&
        s_active_conn != NULL && s_call_active) {
        conn = s_active_conn;
        request_video = tirtc_session_media_profile_allows_remote_video_locked() &&
                        !s_remote_video_requested;
        /* External audio owners, such as WeChat VoIP, use their own call signaling. */
        request_audio = s_media_profile != TIRTC_SESSION_MEDIA_PROFILE_EXTERNAL_AUDIO &&
                        !s_remote_audio_requested;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (conn == NULL) {
        return;
    }

    if (request_video) {
        if (tirtc_session_request_remote_video(conn) != ESP_OK) {
            retry_video = true;
        } else if (request_audio) {
            tirtc_session_retry_remote_media_request_after_delay(false,
                                                                true,
                                                                "audio after video req",
                                                                TIRTC_SESSION_MEDIA_AUDIO_FOLLOWUP_DELAY_US);
        }
    } else if (request_audio && tirtc_session_request_remote_audio(conn) != ESP_OK) {
        retry_audio = true;
    }

    if (retry_video || retry_audio) {
        tirtc_session_retry_remote_media_request(retry_video, retry_audio, "media req retry");
    }
}

static void tirtc_session_release_remote_media(void)
{
    tirtc_conn_t conn = NULL;
    bool release_video = false;
    bool release_audio = false;
    uint8_t audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_active_conn != NULL && s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
        s_closing_conn == NULL) {
        conn = s_active_conn;
        release_video = s_remote_video_requested;
        release_audio = s_remote_audio_requested;
        audio_stream_id = s_remote_audio_requested_stream_id;
    }
    s_remote_video_requested = false;
    s_remote_audio_requested = false;
    s_remote_audio_requested_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (conn == NULL) {
        return;
    }
    if ((release_video || release_audio) && !tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        ESP_LOGW(TAG, "rtc sdk api lock unavailable while releasing remote media");
        return;
    }
    if (release_video) {
        int ret = TiRtcUnsubscribeVideo(conn, TIRTC_SESSION_REMOTE_JPEG_STREAM_ID);
        if (ret < 0) {
            tirtc_session_set_last_error(ret);
            ESP_LOGW(TAG, "release remote video failed: %s", TiRtcGetErrorStr(ret));
        }
    }
    if (release_audio && audio_stream_id != TIRTC_SESSION_INVALID_STREAM_ID) {
        int ret = TiRtcUnsubscribeAudio(conn, audio_stream_id);
        if (ret < 0) {
            tirtc_session_set_last_error(ret);
            ESP_LOGW(TAG, "release remote audio failed: %s", TiRtcGetErrorStr(ret));
        }
    } else if (release_audio) {
        ESP_LOGW(TAG, "release remote audio skipped: requested stream is invalid");
    }
    if (release_video || release_audio) {
        tirtc_session_give_sdk_api_lock();
    }
}

static uint32_t tirtc_session_pcm_level_percent(const uint8_t *data, size_t data_len)
{
    if (data == NULL || data_len < sizeof(int16_t)) {
        return 0;
    }

    const int16_t *samples = (const int16_t *)data;
    size_t sample_count = data_len / sizeof(int16_t);
    uint32_t peak = 0;

    for (size_t index = 0; index < sample_count; ++index) {
        int32_t sample = samples[index];
        uint32_t abs_value = (uint32_t)(sample < 0 ? -sample : sample);
        if (abs_value > peak) {
            peak = abs_value;
        }
    }

    uint32_t level = (peak * 100U) / 32767U;
    return level > 100U ? 100U : level;
}

static bool tirtc_session_audio_format_to_pcm_flags(const tirtc_session_audio_format_t *format, uint8_t *flags)
{
    if (format == NULL || flags == NULL || format->bits_per_sample != 16U) {
        return false;
    }

    if (format->sample_rate_hz == 8000U && format->channels == 1U) {
        *flags = TIRTC_AUDIOSAMPLE_8K16B1C;
        return true;
    }
    if (format->sample_rate_hz == 16000U && format->channels == 1U) {
        *flags = TIRTC_AUDIOSAMPLE_16K16B1C;
        return true;
    }
    if (format->sample_rate_hz == 8000U && format->channels == 2U) {
        *flags = TIRTC_AUDIOSAMPLE_8K16B2C;
        return true;
    }
    if (format->sample_rate_hz == 16000U && format->channels == 2U) {
        *flags = TIRTC_AUDIOSAMPLE_16K16B2C;
        return true;
    }

    return false;
}

static esp_err_t tirtc_session_encode_builtin_audio_alaw(const uint8_t *data,
                                                         size_t data_len,
                                                         const tirtc_session_audio_format_t *format,
                                                         audio_alaw_stream_encoder_t *encoder,
                                                         uint8_t *encoded_data,
                                                         size_t encoded_capacity,
                                                         size_t *encoded_len)
{
    if (data == NULL || data_len == 0U || format == NULL || encoded_data == NULL || encoded_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *encoded_len = 0;

    if (format->bits_per_sample != 16U || format->channels != 1U) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (format->sample_rate_hz == TIRTC_SESSION_ALAW_SAMPLE_RATE_HZ) {
        return audio_alaw_encode_to(data,
                                    data_len,
                                    encoded_data,
                                    encoded_capacity,
                                    encoded_len);
    }

    if (format->sample_rate_hz != (TIRTC_SESSION_ALAW_SAMPLE_RATE_HZ * 2U) ||
        (data_len & 0x3U) != 0U) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    return audio_alaw_stream_encode_16k_mono_to_8k(encoder,
                                                   data,
                                                   data_len,
                                                   encoded_data,
                                                   encoded_capacity,
                                                   encoded_len);
}

static void tirtc_session_send_local_audio_packet(const uint8_t *data,
                                                 size_t data_len,
                                                 const tirtc_session_audio_format_t *format,
                                                 uint64_t pts_us,
                                                 tirtc_session_audio_tx_gate_t gate,
                                                 tirtc_conn_t expected_conn,
                                                 audio_alaw_stream_encoder_t *alaw_encoder)
{
    tirtc_conn_t conn = NULL;
    uint8_t stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    uint8_t media = TIRTC_AUDIO_PCM;
    uint8_t flags = 0;
    uint8_t alaw_stack[TIRTC_SESSION_BUILTIN_AUDIO_STACK_ALAW_BYTES];
    const uint8_t *send_data = data;
    size_t send_data_len = data_len;
    uint32_t input_level = 0;
    tirtc_session_builtin_audio_format_t builtin_audio_format =
        TIRTC_SESSION_BUILTIN_AUDIO_FORMAT_ALAW_8K;
    if (data == NULL || data_len == 0U || data_len > (size_t)UINT32_MAX) {
        return;
    }

    input_level = tirtc_session_pcm_level_percent(data, data_len);
    if (!tirtc_session_is_ready_to_send_audio_with_gate(gate, expected_conn, &conn, &stream_id)) {
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    builtin_audio_format = s_builtin_audio_format;
    taskEXIT_CRITICAL(&s_rtc_lock);

    /*
     * SUBSCRIBED is the built-in microphone path used by IPC and the current
     * device-call owner. CALL is the explicit captured-audio API. Keep both on
     * the same wire codec as WeChat VoIP so either route cannot silently fall
     * back to PCM when the application selected A-law.
     */
    if ((gate == TIRTC_SESSION_AUDIO_TX_GATE_SUBSCRIBED ||
         gate == TIRTC_SESSION_AUDIO_TX_GATE_CALL ||
         gate == TIRTC_SESSION_AUDIO_TX_GATE_TEST_ALAW) &&
        builtin_audio_format == TIRTC_SESSION_BUILTIN_AUDIO_FORMAT_ALAW_8K) {
        esp_err_t encode_ret =
            tirtc_session_encode_builtin_audio_alaw(data,
                                                    data_len,
                                                    format,
                                                    alaw_encoder,
                                                    alaw_stack,
                                                    sizeof(alaw_stack),
                                                    &send_data_len);
        if (encode_ret != ESP_OK) {
            if (format != NULL) {
                ESP_LOGW(TAG,
                         "unsupported built-in A-law input: %lu Hz %u bit %u ch len=%u ret=%s",
                         (unsigned long)format->sample_rate_hz,
                         format->bits_per_sample,
                         format->channels,
                         (unsigned)data_len,
                         esp_err_to_name(encode_ret));
            } else {
                ESP_LOGW(TAG, "missing built-in A-law input format");
            }
            return;
        }
        send_data = alaw_stack;
        media = TIRTC_AUDIO_ALAW;
        flags = TIRTC_AUDIOSAMPLE_8K16B1C;
    } else if (!tirtc_session_audio_format_to_pcm_flags(format, &flags)) {
        if (format != NULL) {
            ESP_LOGW(TAG,
                     "unsupported local pcm format: %lu Hz %u bit %u ch",
                     (unsigned long)format->sample_rate_hz,
                     format->bits_per_sample,
                     format->channels);
        } else {
            ESP_LOGW(TAG, "missing local pcm format");
        }
        return;
    }

    TIRTCFRAMEINFO frame_info = {
        .stream_id = stream_id,
        .media = media,
        .flags = flags,
        .reserved = 0,
        .ts = (uint32_t)(pts_us / 1000ULL),
        .length = (uint32_t)send_data_len,
    };

    int send_ret = TIRTC_E_BUSY;
    uint64_t lock_wait_started_us = esp_timer_get_time();
    bool sdk_lock_acquired = tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS);
    uint32_t sdk_lock_wait_us = (uint32_t)(esp_timer_get_time() - lock_wait_started_us);

    taskENTER_CRITICAL(&s_rtc_lock);
    s_stats.tx_audio_sdk_lock_wait_last_us = sdk_lock_wait_us;
    if (sdk_lock_wait_us > s_stats.tx_audio_sdk_lock_wait_max_us) {
        s_stats.tx_audio_sdk_lock_wait_max_us = sdk_lock_wait_us;
    }
    if (!sdk_lock_acquired) {
        s_stats.tx_audio_sdk_lock_failures++;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (sdk_lock_acquired) {
        if (!tirtc_session_is_ready_to_send_audio_with_gate(gate, expected_conn, &conn, &stream_id)) {
            tirtc_session_give_sdk_api_lock();
            return;
        }
        frame_info.stream_id = stream_id;
        if (!tirtc_session_check_send_buffer(conn, "audio", false)) {
            tirtc_session_give_sdk_api_lock();
            return;
        }

        taskENTER_CRITICAL(&s_rtc_lock);
        s_stats.tx_attempts++;
        taskEXIT_CRITICAL(&s_rtc_lock);

        uint64_t sdk_send_started_us = esp_timer_get_time();
        send_ret = TiRtcSendAudioStream(conn, &frame_info, send_data);
        uint32_t sdk_send_us = (uint32_t)(esp_timer_get_time() - sdk_send_started_us);

        taskENTER_CRITICAL(&s_rtc_lock);
        s_stats.tx_audio_sdk_send_last_us = sdk_send_us;
        if (sdk_send_us > s_stats.tx_audio_sdk_send_max_us) {
            s_stats.tx_audio_sdk_send_max_us = sdk_send_us;
        }
        taskEXIT_CRITICAL(&s_rtc_lock);
        tirtc_session_give_sdk_api_lock();
    }
    if (send_ret >= 0) {
        bool log_first_packet = false;
        bool log_window = false;
        bool peer_wants_audio = false;
        bool forced_publish = false;
        uint32_t window_frames = 0;
        size_t window_payload_bytes = 0;
        uint32_t window_peak_percent = 0;
        const TickType_t now_tick = xTaskGetTickCount();
        const TickType_t log_interval_ticks = pdMS_TO_TICKS(TIRTC_SESSION_TX_LOG_INTERVAL_MS);

        taskENTER_CRITICAL(&s_rtc_lock);
        if (!s_local_audio_first_packet_logged) {
            s_local_audio_first_packet_logged = true;
            s_last_local_audio_tx_log_tick = now_tick;
            log_first_packet = true;
        }
        s_stats.tx_audio_frames++;
        s_stats.tx_audio_bytes += send_data_len;
        s_local_audio_tx_window_frames++;
        s_local_audio_tx_window_payload_bytes += send_data_len;
        if (input_level > s_local_audio_tx_window_peak_percent) {
            s_local_audio_tx_window_peak_percent = input_level;
        }
        peer_wants_audio = s_peer_wants_audio;
        forced_publish = s_local_audio_publish_forced;
        if (!log_first_packet &&
            (s_last_local_audio_tx_log_tick == 0 || now_tick - s_last_local_audio_tx_log_tick >= log_interval_ticks)) {
            log_window = true;
            window_frames = s_local_audio_tx_window_frames;
            window_payload_bytes = s_local_audio_tx_window_payload_bytes;
            window_peak_percent = s_local_audio_tx_window_peak_percent;
            s_local_audio_tx_window_frames = 0;
            s_local_audio_tx_window_payload_bytes = 0;
            s_local_audio_tx_window_peak_percent = 0;
            s_last_local_audio_tx_log_tick = now_tick;
        }
        tirtc_session_set_last_event_locked("audio tx");
        taskEXIT_CRITICAL(&s_rtc_lock);
        if (log_first_packet) {
            ESP_LOGI(TAG,
                     "local audio first packet stream=%u media=%u(%s) flags=%u payload=%u input=%lu peer_audio=%d forced=%d",
                     stream_id,
                     media,
                     tirtc_session_media_name(media),
                     flags,
                     (unsigned)send_data_len,
                     (unsigned long)input_level,
                     peer_wants_audio,
                     forced_publish);
        } else if (log_window) {
            ESP_LOGD(TAG,
                     "local audio tx frames=%lu payload=%u peak=%lu peer_audio=%d forced=%d",
                     (unsigned long)window_frames,
                     (unsigned)window_payload_bytes,
                     (unsigned long)window_peak_percent,
                     peer_wants_audio,
                     forced_publish);
        }
    } else {
        uint64_t conn_age_us = 0;

        taskENTER_CRITICAL(&s_rtc_lock);
        s_stats.tx_failures++;
        taskEXIT_CRITICAL(&s_rtc_lock);
        if (send_ret == TIRTC_E_INVALID_HANDLE &&
            tirtc_session_should_tolerate_invalid_handle(conn, &conn_age_us)) {
            tirtc_session_note_event("audio tx wait");
            ESP_LOGD(TAG,
                     "send audio got INVALID_HANDLE %llu us after accept; keep connection and retry next packet",
                     (unsigned long long)conn_age_us);
            return;
        }
        tirtc_session_set_last_error(send_ret);
        tirtc_session_note_event("audio tx error");
        if (tirtc_session_should_reset_after_send_error(send_ret)) {
            tirtc_session_handle_connection_loss(conn, send_ret);
        }
        ESP_LOGW(TAG, "send audio failed: %s (%d)", TiRtcGetErrorStr(send_ret), send_ret);
    }
}

static void tirtc_session_send_local_video_packet(const uint8_t *data,
                                                 size_t data_len,
                                                 uint16_t width,
                                                 uint16_t height,
                                                 uint64_t pts_us,
                                                 uint8_t media,
                                                 uint8_t flags,
                                                 const TIRTCFRAMEINFO *input_frame_info,
                                                 bool test_frame)
{
    (void)width;
    (void)height;

    tirtc_conn_t conn = NULL;
    uint8_t stream_id = TIRTC_SESSION_INVALID_STREAM_ID;

    uint64_t now_ms = pts_us / 1000ULL;
    TIRTCFRAMEINFO frame_info = {0};

    if (input_frame_info != NULL) {
        frame_info = *input_frame_info;
        frame_info.length = (uint32_t)data_len;
    } else {
        frame_info.media = media;
        frame_info.flags = flags;
        frame_info.reserved = 0;
        frame_info.ts = (uint32_t)now_ms;
        frame_info.length = (uint32_t)data_len;
    }

    bool video_stream_started = false;
    taskENTER_CRITICAL(&s_rtc_lock);
    video_stream_started = s_stats.tx_video_frames > 0U;
    taskEXIT_CRITICAL(&s_rtc_lock);

    bool key_frame = (frame_info.flags & TIRTC_FRAME_FLAG_KEY_FRAME) != 0;
    bool can_drop_frame = video_stream_started &&
                          (frame_info.media == TIRTC_VIDEO_MJPEG || !key_frame);
    int send_ret = TIRTC_E_BUSY;
    if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        bool ready = test_frame ? tirtc_session_is_ready_to_send_test_video(&conn, &stream_id)
                                : tirtc_session_is_ready_to_send_video(&conn, &stream_id);
        if (!ready) {
            tirtc_session_give_sdk_api_lock();
            return;
        }
        frame_info.stream_id = stream_id;
        if (!tirtc_session_check_send_buffer(conn,
                                            test_frame ? "test video" : "video",
                                            can_drop_frame)) {
            tirtc_session_give_sdk_api_lock();
            return;
        }

        taskENTER_CRITICAL(&s_rtc_lock);
        s_stats.tx_attempts++;
        taskEXIT_CRITICAL(&s_rtc_lock);

        send_ret = TiRtcSendVideoStream(conn, &frame_info, data);
        tirtc_session_give_sdk_api_lock();
    }
    if (send_ret >= 0) {
        taskENTER_CRITICAL(&s_rtc_lock);
        s_stats.tx_video_frames++;
        s_stats.tx_video_bytes += data_len;
        tirtc_session_set_last_event_locked(test_frame ? "test video tx" : "video tx");
        taskEXIT_CRITICAL(&s_rtc_lock);
    } else {
        uint64_t conn_age_us = 0;

        taskENTER_CRITICAL(&s_rtc_lock);
        s_stats.tx_failures++;
        taskEXIT_CRITICAL(&s_rtc_lock);

        if (send_ret == TIRTC_E_BUSY) {
            return;
        }
        if (send_ret == TIRTC_E_INVALID_HANDLE &&
            tirtc_session_should_tolerate_invalid_handle(conn, &conn_age_us)) {
            if (test_frame) {
                uint64_t retry_after_us = esp_timer_get_time() + TIRTC_SESSION_TEST_MEDIA_RETRY_DELAY_US;

                taskENTER_CRITICAL(&s_rtc_lock);
                if (retry_after_us > s_test_video_retry_after_us) {
                    s_test_video_retry_after_us = retry_after_us;
                }
                taskEXIT_CRITICAL(&s_rtc_lock);
            }
            tirtc_session_note_event(test_frame ? "test video tx wait" : "video tx wait");
            ESP_LOGD(TAG,
                     "send %svideo got INVALID_HANDLE %llu us after accept; keep connection and retry next packet",
                     test_frame ? "test " : "",
                     (unsigned long long)conn_age_us);
            return;
        }

        tirtc_session_set_last_error(send_ret);
        tirtc_session_note_event(test_frame ? "test video tx error" : "video tx error");
        if (tirtc_session_should_reset_after_send_error(send_ret)) {
            tirtc_session_handle_connection_loss(conn, send_ret);
        }
        ESP_LOGW(TAG,
                 "%svideo send failed: err=%s (%d)",
                 test_frame ? "test " : "",
                 TiRtcGetErrorStr(send_ret),
                 send_ret);
    }
}

esp_err_t tirtc_session_send_test_video_frame(const TIRTCFRAMEINFO *frame_info, const uint8_t *data)
{
    tirtc_conn_t conn = NULL;
    uint8_t stream_id = TIRTC_SESSION_INVALID_STREAM_ID;

    if (!APP_PRODUCT_RTC_VIDEO_ENABLED) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (frame_info == NULL || data == NULL || frame_info->length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tirtc_session_is_ready_to_send_test_video(&conn, &stream_id)) {
        return ESP_ERR_INVALID_STATE;
    }

    return tirtc_session_enqueue_local_video_packet(data,
                                                   frame_info->length,
                                                   0,
                                                   0,
                                                   (uint64_t)frame_info->ts * 1000ULL,
                                                   frame_info->media,
                                                   frame_info->flags,
                                                   frame_info,
                                                   true);
}

esp_err_t tirtc_session_send_test_audio_pcm_frame(const uint8_t *data,
                                                  size_t data_len,
                                                  const tirtc_session_audio_format_t *format,
                                                  uint64_t pts_us)
{
    tirtc_conn_t conn = NULL;
    uint8_t stream_id = TIRTC_SESSION_INVALID_STREAM_ID;

    if (data == NULL || data_len == 0U || format == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tirtc_session_is_ready_to_send_test_audio(&conn, &stream_id)) {
        return ESP_ERR_INVALID_STATE;
    }

    return tirtc_session_enqueue_local_audio_packet(data,
                                                   data_len,
                                                   format,
                                                   pts_us,
                                                   TIRTC_SESSION_AUDIO_TX_GATE_TEST,
                                                   NULL);
}

esp_err_t tirtc_session_send_audio_frame(tirtc_conn_t conn, const TIRTCFRAMEINFO *frame_info, const void *data)
{
    TIRTCFRAMEINFO send_info = {0};
    int send_ret = TIRTC_E_BUSY;

    if (conn == NULL || frame_info == NULL || data == NULL || frame_info->length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tirtc_session_is_connection_usable(conn)) {
        return ESP_ERR_INVALID_STATE;
    }

    send_info = *frame_info;
    if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        if (!tirtc_session_is_connection_usable(conn)) {
            tirtc_session_give_sdk_api_lock();
            return ESP_ERR_INVALID_STATE;
        }
        if (!tirtc_session_check_send_buffer(conn, "audio", true)) {
            tirtc_session_give_sdk_api_lock();
            return ESP_ERR_TIMEOUT;
        }

        taskENTER_CRITICAL(&s_rtc_lock);
        s_stats.tx_attempts++;
        taskEXIT_CRITICAL(&s_rtc_lock);

        send_ret = TiRtcSendAudioStream(conn, &send_info, data);
        tirtc_session_give_sdk_api_lock();
    } else {
        return ESP_ERR_TIMEOUT;
    }

    if (send_ret >= 0) {
        taskENTER_CRITICAL(&s_rtc_lock);
        s_stats.tx_audio_frames++;
        s_stats.tx_audio_bytes += frame_info->length;
        tirtc_session_set_last_event_locked("audio tx");
        taskEXIT_CRITICAL(&s_rtc_lock);
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    s_stats.tx_failures++;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (send_ret == TIRTC_E_BUSY) {
        tirtc_session_note_event("audio tx busy");
        return ESP_ERR_TIMEOUT;
    }
    if (send_ret == TIRTC_E_INVALID_HANDLE) {
        uint64_t conn_age_us = 0;

        if (tirtc_session_should_tolerate_invalid_handle(conn, &conn_age_us)) {
            tirtc_session_note_event("audio tx wait");
            return ESP_ERR_INVALID_STATE;
        }
    }

    tirtc_session_set_last_error(send_ret);
    tirtc_session_note_event("audio tx error");
    if (tirtc_session_should_reset_after_send_error(send_ret)) {
        tirtc_session_handle_connection_loss(conn, send_ret);
    }

    ESP_LOGW(TAG,
             "send audio frame failed hconn=%p stream=%u len=%u ts=%lu err=%s (%d)",
             conn,
             send_info.stream_id,
             (unsigned)send_info.length,
             (unsigned long)send_info.ts,
             TiRtcGetErrorStr(send_ret),
             send_ret);
    return ESP_FAIL;
}

esp_err_t tirtc_session_get_send_buffer_used(tirtc_conn_t conn, size_t *used)
{
    esp_err_t ret = ESP_OK;

    if (used == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *used = 0;
    if (conn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tirtc_session_is_connection_usable(conn)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!tirtc_session_take_sdk_api_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }
    if (tirtc_session_is_connection_usable(conn)) {
        *used = TiRtcGetSendBufferUsed(conn);
    } else {
        ret = ESP_ERR_INVALID_STATE;
    }
    tirtc_session_give_sdk_api_lock();
    return ret;
}
esp_err_t tirtc_session_get_active_send_buffer_used(size_t *used)
{
    tirtc_conn_t conn = NULL;

    if (used == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *used = 0;
    if (!tirtc_session_try_get_active_conn(&conn)) {
        return ESP_ERR_INVALID_STATE;
    }
    return tirtc_session_get_send_buffer_used(conn, used);
}


esp_err_t tirtc_session_send_captured_audio_frame(tirtc_conn_t conn,
                                                  const uint8_t *data,
                                                  size_t data_len,
                                                  const tirtc_session_audio_format_t *format,
                                                  uint64_t pts_us)
{
    tirtc_conn_t active_conn = NULL;
    uint8_t stream_id = TIRTC_SESSION_INVALID_STREAM_ID;

    if (conn == NULL || data == NULL || data_len == 0U || format == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tirtc_session_is_ready_to_send_call_audio(conn, &active_conn, &stream_id)) {
        return ESP_ERR_INVALID_STATE;
    }

    return tirtc_session_enqueue_local_audio_packet(data,
                                                   data_len,
                                                   format,
                                                   pts_us,
                                                   TIRTC_SESSION_AUDIO_TX_GATE_CALL,
                                                   conn);
}

static void tirtc_session_local_audio_tx_task(void *ctx)
{
    (void)ctx;
    tirtc_session_local_audio_packet_t packet = {0};
    audio_alaw_stream_encoder_t alaw_encoder = {0};
    uint32_t alaw_encoder_generation = UINT32_MAX;

    while (xQueueReceive(s_local_audio_tx_queue, &packet, portMAX_DELAY) == pdTRUE) {
        uint64_t now_us = esp_timer_get_time();
        uint32_t current_generation = tirtc_session_get_local_audio_tx_generation();
        bool packet_is_fresh = packet.pts_us == 0 || now_us <= packet.pts_us ||
                               now_us - packet.pts_us <= TIRTC_SESSION_AUDIO_TX_MAX_AGE_US;

        if (packet.generation == current_generation && packet_is_fresh) {
            if (alaw_encoder_generation != packet.generation) {
                audio_alaw_stream_encoder_reset(&alaw_encoder);
                alaw_encoder_generation = packet.generation;
            }
            tirtc_session_send_local_audio_packet(packet.data,
                                                 packet.data_len,
                                                 &packet.format,
                                                 packet.pts_us != 0 ? packet.pts_us : now_us,
                                                 packet.gate,
                                                 packet.expected_conn,
                                                 &alaw_encoder);
        } else {
            uint32_t age_ms = 0;
            if (packet.pts_us != 0 && now_us > packet.pts_us) {
                age_ms = (uint32_t)((now_us - packet.pts_us) / 1000ULL);
            }
            taskENTER_CRITICAL(&s_rtc_lock);
            if (packet.generation != current_generation) {
                s_stats.tx_audio_queue_generation_drops++;
            } else {
                s_stats.tx_audio_queue_stale_drops++;
            }
            taskEXIT_CRITICAL(&s_rtc_lock);
            ESP_LOGD(TAG,
                     "queued audio dropped: len=%u pts=%lu gen=%lu current=%lu fresh=%d age_ms=%lu",
                     (unsigned)packet.data_len,
                     (unsigned long)(packet.pts_us / 1000ULL),
                     (unsigned long)packet.generation,
                     (unsigned long)current_generation,
                     packet_is_fresh,
                     (unsigned long)age_ms);
        }
        tirtc_session_free_local_audio_packet(&packet);
    }
}

static void tirtc_session_local_video_tx_task(void *ctx)
{
    (void)ctx;
    tirtc_session_local_video_packet_t packet = {0};

    while (xQueueReceive(s_local_video_tx_queue, &packet, portMAX_DELAY) == pdTRUE) {
        if (packet.generation == tirtc_session_get_local_video_tx_generation()) {
            tirtc_session_send_local_video_packet(packet.data,
                                                 packet.data_len,
                                                 packet.width,
                                                 packet.height,
                                                 packet.pts_us,
                                                 packet.media,
                                                 packet.flags,
                                                 packet.has_frame_info ? &packet.frame_info : NULL,
                                                 packet.test_frame);
        }
        tirtc_session_free_local_video_packet(&packet);
    }
}

static void tirtc_session_local_audio_cb(const uint8_t *data,
                                        size_t data_len,
                                        const tirtc_session_audio_format_t *format,
                                        void *ctx)
{
    (void)ctx;

    tirtc_conn_t conn = NULL;
    uint8_t stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    if (!tirtc_session_is_ready_to_send_audio(&conn, &stream_id)) {
        return;
    }

    esp_err_t enqueue_ret = tirtc_session_enqueue_local_audio_packet(data,
                                                                    data_len,
                                                                    format,
                                                                    esp_timer_get_time(),
                                                                    TIRTC_SESSION_AUDIO_TX_GATE_SUBSCRIBED,
                                                                    NULL);
    if (enqueue_ret != ESP_OK) {
        tirtc_session_note_event("audio tx drop");
        TickType_t now = xTaskGetTickCount();
        if (s_last_local_audio_queue_fail_log_tick == 0 ||
            now - s_last_local_audio_queue_fail_log_tick >= pdMS_TO_TICKS(1000)) {
            s_last_local_audio_queue_fail_log_tick = now;
            ESP_LOGW(TAG, "local audio queue pressure: %s", esp_err_to_name(enqueue_ret));
        }
    }
}

static void tirtc_session_on_event(int event, const void *data, int len)
{
    (void)data;
    (void)len;

    tirtc_session_event_t rtc_event = {0};

    ESP_LOGD(TAG, "rtc system event: event=%d len=%d", event, len);

    switch (event) {
    case TIRTC_EVENT_SYS_STARTED:
        taskENTER_CRITICAL(&s_rtc_lock);
        s_sys_started_callback_count++;
        s_last_sys_started_callback_us = (uint64_t)esp_timer_get_time();
        taskEXIT_CRITICAL(&s_rtc_lock);
        rtc_event.type = TIRTC_SESSION_EVENT_SYS_STARTED;
        break;
    case TIRTC_EVENT_SYS_STOPPED:
        taskENTER_CRITICAL(&s_rtc_lock);
        s_sdk_stop_notified = true;
        rtc_event.payload.system.generation =
            s_pending_stop_generation != 0U ? s_pending_stop_generation : s_sdk_generation;
        taskEXIT_CRITICAL(&s_rtc_lock);
        rtc_event.type = TIRTC_SESSION_EVENT_SYS_STOPPED;
        break;
    case TIRTC_EVENT_ACCESS_HIJACKING:
        rtc_event.type = TIRTC_SESSION_EVENT_ACCESS_HIJACKING;
        break;
    default:
        ESP_LOGW(TAG, "rtc system event ignored: event=%d", event);
        return;
    }

    if (!tirtc_session_enqueue_event(&rtc_event, TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        tirtc_session_note_event("sys event drop");
        ESP_LOGW(TAG, "rtc event queue full: system event dropped event=%d", event);
    }
}

static void tirtc_session_on_conn_accepted(tirtc_conn_t hconn)
{
    bool accepted = tirtc_session_try_accept_connection(hconn);
    tirtc_session_mode_t mode = TIRTC_SESSION_MODE_LISTEN;
    tirtc_session_state_t state = TIRTC_SESSION_STATE_STOPPED;
    bool sdk_started = false;
    bool start_in_progress = false;
    bool stop_in_progress = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    mode = s_session_mode;
    state = s_state;
    sdk_started = s_sdk_started;
    start_in_progress = s_start_in_progress;
    stop_in_progress = s_stop_in_progress;
    taskEXIT_CRITICAL(&s_rtc_lock);

    ESP_LOGI(TAG,
             "rtc conn accepted callback: hconn=%p accepted=%d mode=%u state=%u sdk_started=%d start=%d stop=%d",
             hconn,
             accepted,
             (unsigned)mode,
             (unsigned)state,
             sdk_started ? 1 : 0,
             start_in_progress ? 1 : 0,
             stop_in_progress ? 1 : 0);

    if (!accepted) {
        ESP_LOGW(TAG, "rtc connection rejected: hconn=%p", hconn);
        (void)tirtc_session_disconnect_with_sdk_lock(hconn);
        return;
    }

    tirtc_session_bind_connection_user_data(hconn);

    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_CONN_ACCEPTED,
        .payload.conn = {
            .conn = hconn,
            .error = 0,
        },
    };
    if (!tirtc_session_enqueue_event(&rtc_event, 0)) {
        tirtc_session_note_event("conn accept inline");
        ESP_LOGW(TAG, "rtc event queue full: connection accept handled inline");
        tirtc_session_handle_runtime_event(&rtc_event);
    }
}

static void tirtc_session_on_conn_error(tirtc_conn_t hconn, int error)
{
    tirtc_session_log_connection_user_data("connection error", hconn);
    tirtc_session_log_connection_time_stats(hconn, "connection-error");
    ESP_LOGW(TAG,
             "rtc connection error: hconn=%p err=%s",
             hconn,
             TiRtcGetErrorStr(error));
    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_CONN_ERROR,
        .payload.conn = {
            .conn = hconn,
            .error = error,
        },
    };
    if (!tirtc_session_enqueue_teardown_event(&rtc_event)) {
        tirtc_session_note_event("conn err inline");
        ESP_LOGE(TAG, "rtc teardown queue failed after connection error: err=%d", error);
        tirtc_session_handle_connection_loss(hconn, error);
    }
}

static void tirtc_session_on_disconnected(tirtc_conn_t hconn)
{
    tirtc_session_log_connection_user_data("disconnected callback", hconn);
    tirtc_session_log_connection_time_stats(hconn, "disconnected");
    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_DISCONNECTED,
        .payload.conn = {
            .conn = hconn,
            .error = 0,
        },
    };
    if (!tirtc_session_enqueue_teardown_event(&rtc_event)) {
        tirtc_session_note_event("disconnect inline");
        ESP_LOGE(TAG, "rtc teardown queue failed after disconnect");
        tirtc_session_handle_connection_loss(hconn, 0);
    }
}

static void tirtc_session_log_bad_remote_audio_frame(tirtc_conn_t hconn,
                                                    const TIRTCFRAMEINFO *frame_info)
{
    TickType_t now_tick = xTaskGetTickCount();
    const TickType_t log_interval_ticks = pdMS_TO_TICKS(TIRTC_SESSION_BAD_REMOTE_AUDIO_LOG_INTERVAL_MS);

    if (s_last_bad_remote_audio_log_tick != 0 &&
        now_tick - s_last_bad_remote_audio_log_tick < log_interval_ticks) {
        return;
    }
    s_last_bad_remote_audio_log_tick = now_tick;

    ESP_LOGW(TAG,
             "remote audio dropped: invalid length hconn=%p stream=%u media=%u(%s) flags=%u len=%lu max=%u",
             hconn,
             (unsigned)frame_info->stream_id,
             (unsigned)frame_info->media,
             tirtc_session_media_name(frame_info->media),
             (unsigned)frame_info->flags,
             (unsigned long)frame_info->length,
             TIRTC_SESSION_REMOTE_AUDIO_MAX_PAYLOAD);
}

static void tirtc_session_on_audio(tirtc_conn_t hconn, const TIRTCFRAMEINFO *frame_info, void *data)
{
    size_t playback_data_len = 0;
    bool log_first_packet = false;
    bool log_window = false;
    uint32_t window_frames = 0;
    size_t window_payload_bytes = 0;
    size_t window_playback_bytes = 0;

    if (!tirtc_session_remote_media_allowed(hconn)) {
        return;
    }

    if (frame_info == NULL || data == NULL || frame_info->length == 0) {
        return;
    }

    if (frame_info->length > TIRTC_SESSION_REMOTE_AUDIO_MAX_PAYLOAD) {
        tirtc_session_log_bad_remote_audio_frame(hconn, frame_info);
        tirtc_session_note_event("bad audio len");
        return;
    }

    if (tirtc_session_media_submit_remote_audio(frame_info->media,
                                                frame_info->flags,
                                                frame_info->ts,
                                                (const uint8_t *)data,
                                                frame_info->length,
                                                &playback_data_len) != ESP_OK) {
        tirtc_session_note_event("remote audio drop");
        return;
    }

    const TickType_t now_tick = xTaskGetTickCount();
    const TickType_t log_interval_ticks = pdMS_TO_TICKS(TIRTC_SESSION_RX_LOG_INTERVAL_MS);

    taskENTER_CRITICAL(&s_rtc_lock);
    if (!s_remote_audio_first_packet_logged) {
        s_remote_audio_first_packet_logged = true;
        s_last_remote_audio_rx_log_tick = now_tick;
        log_first_packet = true;
    }
    s_stats.rx_audio_frames++;
    s_stats.rx_audio_bytes += playback_data_len;
    s_remote_audio_rx_window_frames++;
    s_remote_audio_rx_window_payload_bytes += frame_info->length;
    s_remote_audio_rx_window_playback_bytes += playback_data_len;
    tirtc_session_set_last_event_locked("audio rx");
    if (!log_first_packet &&
        (s_last_remote_audio_rx_log_tick == 0 || now_tick - s_last_remote_audio_rx_log_tick >= log_interval_ticks)) {
        log_window = true;
        window_frames = s_remote_audio_rx_window_frames;
        window_payload_bytes = s_remote_audio_rx_window_payload_bytes;
        window_playback_bytes = s_remote_audio_rx_window_playback_bytes;
        s_remote_audio_rx_window_frames = 0;
        s_remote_audio_rx_window_payload_bytes = 0;
        s_remote_audio_rx_window_playback_bytes = 0;
        s_last_remote_audio_rx_log_tick = now_tick;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (log_first_packet) {
        ESP_LOGI(TAG,
                 "remote audio first packet stream=%u media=%u(%s) flags=%u ts=%lu payload=%u playback=%u",
                 (unsigned)frame_info->stream_id,
                 (unsigned)frame_info->media,
                 tirtc_session_media_name(frame_info->media),
                 (unsigned)frame_info->flags,
                 (unsigned long)frame_info->ts,
                 (unsigned)frame_info->length,
                 (unsigned)playback_data_len);
    } else if (log_window) {
        ESP_LOGD(TAG,
                 "remote audio rx frames=%lu payload=%u playback=%u media=%u(%s) flags=%u",
                 (unsigned long)window_frames,
                 (unsigned)window_payload_bytes,
                 (unsigned)window_playback_bytes,
                 (unsigned)frame_info->media,
                 tirtc_session_media_name(frame_info->media),
                 (unsigned)frame_info->flags);
    }
}

static void tirtc_session_on_video(tirtc_conn_t hconn, const TIRTCFRAMEINFO *frame_info, void *data)
{
    bool log_first_packet = false;

    if (!APP_PRODUCT_RTC_VIDEO_ENABLED) {
        return;
    }

    if (!tirtc_session_remote_media_allowed(hconn)) {
        return;
    }

    if (frame_info == NULL || data == NULL || frame_info->length == 0) {
        return;
    }

    if (frame_info->media != TIRTC_VIDEO_MJPEG) {
        tirtc_session_note_event("bad remote video");
        ESP_LOGW(TAG, "unsupported remote video media: %u", frame_info->media);
        return;
    }

    if (tirtc_session_media_submit_remote_video_jpeg((const uint8_t *)data, frame_info->length) != ESP_OK) {
        tirtc_session_note_event("remote video drop");
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (!s_remote_video_first_packet_logged) {
        s_remote_video_first_packet_logged = true;
        log_first_packet = true;
    }
    s_stats.rx_video_frames++;
    s_stats.rx_video_bytes += frame_info->length;
    tirtc_session_set_last_event_locked("video rx");
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (log_first_packet) {
        ESP_LOGI(TAG,
                 "remote video first packet media=%u flags=%u payload=%u",
                 frame_info->media,
                 frame_info->flags,
                 (unsigned)frame_info->length);
        tirtc_session_retry_remote_media_request_after_delay(false,
                                                            true,
                                                            "audio after first video",
                                                            TIRTC_SESSION_MEDIA_AUDIO_FOLLOWUP_DELAY_US);
    }
}

static void tirtc_session_on_message(tirtc_conn_t hconn, const TIRTCFRAMEINFO *frame_info, void *data)
{
    tirtc_conn_t active_conn = NULL;

    if (!tirtc_session_try_get_active_conn(&active_conn) || hconn != active_conn) {
        return;
    }

    if (frame_info == NULL) {
        return;
    }

    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_REMOTE_MESSAGE,
        .payload.message = {
            .conn = hconn,
            .media = frame_info->media,
            .stream_id = frame_info->stream_id,
            .flags = frame_info->flags,
            .ts = frame_info->ts,
            .data_len = frame_info->length,
        },
    };

    if (tirtc_session_copy_payload(data, frame_info->length, &rtc_event.payload.message.data) != ESP_OK ||
        !tirtc_session_enqueue_event(&rtc_event, 0)) {
        tirtc_session_note_event("remote msg drop");
        tirtc_session_free_event_payload(&rtc_event);
    }
}

static void tirtc_session_on_command(tirtc_conn_t hconn, uint32_t cmdw, const void *data, uint32_t len)
{
    uint16_t cmd = (uint16_t)(cmdw & ~TIRTC_SESSION_CMD_RESP_BIT);
    bool control_cmd = cmd == TIRTC_SESSION_CMD_CALL ||
                       cmd == TIRTC_SESSION_CMD_HANGUP ||
                       cmd == TIRTC_SESSION_CMD_DEVICE_CALL_CONNECTED ||
                       cmd == TIRTC_SESSION_CMD_DEVICE_CALL_HANGUP;

    ESP_LOGI(TAG,
             "rtc command callback: hconn=%p cmd=0x%04x resp=%d len=%lu",
             hconn,
             (unsigned)cmd,
             (cmdw & TIRTC_SESSION_CMD_RESP_BIT) != 0,
             (unsigned long)len);

    if (tirtc_session_try_handle_reserved_control_command(hconn, cmdw, data, len)) {
        return;
    }

    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_REMOTE_COMMAND,
        .payload.command = {
            .conn = hconn,
            .cmdw = cmdw,
            .data_len = len,
        },
    };

    if (tirtc_session_copy_payload(data, len, &rtc_event.payload.command.data) != ESP_OK) {
        tirtc_session_note_event("remote cmd drop");
        ESP_LOGW(TAG, "remote command dropped: payload alloc failed cmd=0x%08lx", (unsigned long)cmdw);
        tirtc_session_free_event_payload(&rtc_event);
        return;
    }

    if (!tirtc_session_enqueue_event(&rtc_event, control_cmd ? 0 : TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        if (control_cmd) {
            tirtc_session_note_event("remote cmd inline");
            ESP_LOGW(TAG, "rtc event queue full: handling control command inline cmd=0x%08lx", (unsigned long)cmdw);
            tirtc_session_handle_remote_command(&rtc_event);
        } else {
            tirtc_session_note_event("remote cmd drop");
            ESP_LOGW(TAG, "rtc event queue full: remote command dropped cmd=0x%08lx", (unsigned long)cmdw);
        }
        tirtc_session_free_event_payload(&rtc_event);
    }
}

static bool tirtc_session_try_handle_reserved_control_command(tirtc_conn_t hconn,
                                                              uint32_t cmdw,
                                                              const void *data,
                                                              uint32_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t wire_cmdw = cmdw;
    uint8_t stream_id = TIRTC_SESSION_INVALID_STREAM_ID;

    if (bytes != NULL && len == 5U) {
        wire_cmdw = tirtc_session_load_le32(bytes);
        stream_id = bytes[4];
    } else if (bytes != NULL && len == 1U) {
        stream_id = bytes[0];
    } else {
        return false;
    }

    switch (wire_cmdw) {
    case TIRTC_RESERVED_CMD_REQUEST_KEY_FRAME_NEW:
    case TIRTC_RESERVED_CMD_REQUEST_KEY_FRAME_OLD:
        ESP_LOGI(TAG,
                 "reserved media control fallback: request key frame stream=%u cmdw=0x%04lx",
                 (unsigned)stream_id,
                 (unsigned long)wire_cmdw);
        tirtc_session_on_request_key_frame(hconn, stream_id);
        return true;
    case TIRTC_RESERVED_CMD_SUBSCRIBE_VIDEO_NEW:
    case TIRTC_RESERVED_CMD_SUBSCRIBE_VIDEO_OLD:
        ESP_LOGI(TAG,
                 "reserved media control fallback: subscribe video stream=%u cmdw=0x%04lx",
                 (unsigned)stream_id,
                 (unsigned long)wire_cmdw);
        (void)tirtc_session_on_subscribe_video(hconn, stream_id);
        return true;
    case TIRTC_RESERVED_CMD_UNSUBSCRIBE_VIDEO_NEW:
    case TIRTC_RESERVED_CMD_UNSUBSCRIBE_VIDEO_OLD:
        ESP_LOGI(TAG,
                 "reserved media control fallback: unsubscribe video stream=%u cmdw=0x%04lx",
                 (unsigned)stream_id,
                 (unsigned long)wire_cmdw);
        tirtc_session_on_unsubscribe_video(hconn, stream_id);
        return true;
    case TIRTC_RESERVED_CMD_SUBSCRIBE_AUDIO_NEW:
    case TIRTC_RESERVED_CMD_SUBSCRIBE_AUDIO_OLD:
        ESP_LOGI(TAG,
                 "reserved media control fallback: subscribe audio stream=%u cmdw=0x%04lx",
                 (unsigned)stream_id,
                 (unsigned long)wire_cmdw);
        (void)tirtc_session_on_subscribe_audio(hconn, stream_id);
        return true;
    case TIRTC_RESERVED_CMD_UNSUBSCRIBE_AUDIO_NEW:
    case TIRTC_RESERVED_CMD_UNSUBSCRIBE_AUDIO_OLD:
        ESP_LOGI(TAG,
                 "reserved media control fallback: unsubscribe audio stream=%u cmdw=0x%04lx",
                 (unsigned)stream_id,
                 (unsigned long)wire_cmdw);
        tirtc_session_on_unsubscribe_audio(hconn, stream_id);
        return true;
    default:
        return false;
    }
}

static int tirtc_session_on_subscribe_video(tirtc_conn_t hconn, uint8_t stream_id)
{
    if (!APP_PRODUCT_RTC_VIDEO_ENABLED) {
        (void)hconn;
        (void)stream_id;
        return -1;
    }

    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_SUBSCRIBE_VIDEO,
        .payload.subscribe = {
            .conn = hconn,
            .stream_id = stream_id,
        },
    };
    if (!tirtc_session_enqueue_event(&rtc_event, TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        tirtc_session_note_event("sub video drop");
        ESP_LOGW(TAG, "rtc event queue full: subscribe video dropped stream=%u", stream_id);
    }
    return 0;
}

static void tirtc_session_on_unsubscribe_video(tirtc_conn_t hconn, uint8_t stream_id)
{
    if (!APP_PRODUCT_RTC_VIDEO_ENABLED) {
        (void)hconn;
        (void)stream_id;
        return;
    }

    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_UNSUBSCRIBE_VIDEO,
        .payload.subscribe = {
            .conn = hconn,
            .stream_id = stream_id,
        },
    };
    if (!tirtc_session_enqueue_event(&rtc_event, TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        tirtc_session_note_event("unsub video drop");
        ESP_LOGW(TAG, "rtc event queue full: unsubscribe video dropped stream=%u", stream_id);
    }
}

static int tirtc_session_on_subscribe_audio(tirtc_conn_t hconn, uint8_t stream_id)
{
    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_SUBSCRIBE_AUDIO,
        .payload.subscribe = {
            .conn = hconn,
            .stream_id = stream_id,
        },
    };
    if (!tirtc_session_enqueue_event(&rtc_event, TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        tirtc_session_note_event("sub audio drop");
        ESP_LOGW(TAG, "rtc event queue full: subscribe audio dropped stream=%u", stream_id);
    }
    return 0;
}

static void tirtc_session_on_unsubscribe_audio(tirtc_conn_t hconn, uint8_t stream_id)
{
    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_UNSUBSCRIBE_AUDIO,
        .payload.subscribe = {
            .conn = hconn,
            .stream_id = stream_id,
        },
    };
    if (!tirtc_session_enqueue_event(&rtc_event, TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        tirtc_session_note_event("unsub audio drop");
        ESP_LOGW(TAG, "rtc event queue full: unsubscribe audio dropped stream=%u", stream_id);
    }
}

static void tirtc_session_sdk_log_cb(const char *log, uint32_t length)
{
    if (log == NULL || length == 0) {
        return;
    }

    uint32_t offset = 0;
    while (offset < length) {
        uint32_t raw_chunk_len = length - offset;
        if (raw_chunk_len > TIRTC_SESSION_SDK_LOG_CHUNK_LEN) {
            raw_chunk_len = TIRTC_SESSION_SDK_LOG_CHUNK_LEN;
        }

        uint32_t chunk_len = raw_chunk_len;
        while (chunk_len > 0) {
            char last = log[offset + chunk_len - 1];
            if (last != '\n' && last != '\r') {
                break;
            }
            --chunk_len;
        }

        if (chunk_len > 0) {
            char chunk[TIRTC_SESSION_SDK_LOG_CHUNK_LEN + 1];
            memcpy(chunk, log + offset, chunk_len);
            chunk[chunk_len] = '\0';
            if (strstr(chunk, "signal_token") != NULL ||
                strstr(chunk, "signal_tok") != NULL ||
                strstr(chunk, "X-Tg-Signature") != NULL) {
                ESP_LOGI(TIRTC_SDK_LOG_TAG, "[redacted sensitive SDK log line]");
            } else {
                ESP_LOGI(TIRTC_SDK_LOG_TAG, "%s", chunk);
            }
        }

        offset += raw_chunk_len;
    }
}

esp_err_t tirtc_session_send_test_audio_alaw_frame(const uint8_t *data,
                                                   size_t data_len,
                                                   const tirtc_session_audio_format_t *format,
                                                   uint64_t pts_us)
{
    tirtc_conn_t conn = NULL;
    uint8_t stream_id = TIRTC_SESSION_INVALID_STREAM_ID;

    if (data == NULL || data_len == 0U || format == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tirtc_session_is_ready_to_send_test_audio(&conn, &stream_id)) {
        return ESP_ERR_INVALID_STATE;
    }

    return tirtc_session_enqueue_local_audio_packet(data,
                                                    data_len,
                                                    format,
                                                    pts_us,
                                                    TIRTC_SESSION_AUDIO_TX_GATE_TEST_ALAW,
                                                    NULL);
}

static int tirtc_session_tgtrp_log_level(int level)
{
    int tgtrp_level = level & 0x07;
    if ((level & 0x08) != 0) {
        tgtrp_level |= TGTRP_LOG_FLAG_STAT;
    }
    return tgtrp_level;
}

static void tirtc_session_configure_sdk_logs(bool announce)
{
    TiRtcLogSetCallback(tirtc_session_sdk_log_cb);
    TiRtcLogSetLevel(s_sdk_log_level);
    tgtrp_set_log_level(tirtc_session_tgtrp_log_level(s_sdk_log_level));

    if (announce) {
        ESP_LOGI(TAG,
                 "tirtc logs enabled: sdk_callback=esp_log sdk_level=%d",
                 s_sdk_log_level);
    }
}

esp_err_t tirtc_session_set_sdk_log_level(int level)
{
    if (level < 0 || level > 15) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    s_sdk_log_level = level;
    taskEXIT_CRITICAL(&s_rtc_lock);
    TiRtcLogSetLevel(level);
    tgtrp_set_log_level(tirtc_session_tgtrp_log_level(level));
    ESP_LOGI(TAG, "rtc sdk log level changed: level=%d", level);
    return ESP_OK;
}

int tirtc_session_get_sdk_log_level(void)
{
    int level = 0;

    taskENTER_CRITICAL(&s_rtc_lock);
    level = s_sdk_log_level;
    taskEXIT_CRITICAL(&s_rtc_lock);
    return level;
}

esp_err_t tirtc_session_set_link_mode(tirtc_session_link_mode_t mode)
{
    if (mode != TIRTC_SESSION_LINK_MODE_DEFAULT &&
        mode != TIRTC_SESSION_LINK_MODE_DIRECT_ONLY &&
        mode != TIRTC_SESSION_LINK_MODE_RELAY_ONLY) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        return ESP_ERR_TIMEOUT;
    }

    bool connection_busy = false;
    taskENTER_CRITICAL(&s_rtc_lock);
    connection_busy = s_active_conn != NULL || s_closing_conn != NULL ||
                      s_call_active || s_incoming_call_pending;
    taskEXIT_CRITICAL(&s_rtc_lock);
    if (connection_busy || tirtc_connect_is_connecting()) {
        tirtc_session_give_sdk_api_lock();
        return ESP_ERR_INVALID_STATE;
    }

    TiRtcSetConnFlag((int)mode);
    taskENTER_CRITICAL(&s_rtc_lock);
    s_link_mode = mode;
    taskEXIT_CRITICAL(&s_rtc_lock);
    tirtc_session_give_sdk_api_lock();

    ESP_LOGI(TAG, "rtc link mode changed: mode=%d", (int)mode);
    return ESP_OK;
}

tirtc_session_link_mode_t tirtc_session_get_link_mode(void)
{
    tirtc_session_link_mode_t mode = TIRTC_SESSION_LINK_MODE_DEFAULT;

    taskENTER_CRITICAL(&s_rtc_lock);
    mode = s_link_mode;
    taskEXIT_CRITICAL(&s_rtc_lock);
    return mode;
}

static void tirtc_session_on_request_key_frame(tirtc_conn_t hconn, uint8_t stream_id)
{
    if (!APP_PRODUCT_RTC_VIDEO_ENABLED) {
        (void)hconn;
        (void)stream_id;
        return;
    }

    bool active = false;
    bool forced_publish = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    active = hconn != NULL && hconn == s_active_conn && s_sdk_started && !s_start_in_progress &&
             !s_stop_in_progress && s_closing_conn == NULL && s_call_active;
    if (active && stream_id == TIRTC_SESSION_LOCAL_VIDEO_STREAM_ID) {
        s_peer_wants_video = true;
        forced_publish = tirtc_session_maybe_force_local_video_publish_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!active) {
        ESP_LOGD(TAG, "ignore key frame request for inactive connection hconn=%p stream=%u", hconn, stream_id);
        return;
    }

    if (stream_id != TIRTC_SESSION_LOCAL_VIDEO_STREAM_ID) {
        ESP_LOGD(TAG, "ignore key frame request for unsupported stream=%u", stream_id);
        return;
    }

    tirtc_session_note_event("key frame asked");
    ESP_LOGI(TAG,
             "remote requested local key frame: hconn=%p stream=%u forced_publish=%d",
             hconn,
             (unsigned)stream_id,
             forced_publish);

    if (forced_publish) {
        tirtc_session_apply_local_media_policy();
    }
}

void tirtc_session_apply_hangup_local_state(void)
{
    tirtc_session_release_remote_media();

    taskENTER_CRITICAL(&s_rtc_lock);
    s_call_active = false;
    s_incoming_call_pending = false;
    s_pending_call_cmdw = 0;
    s_media_bootstrap_pending = false;
    s_local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    s_local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    tirtc_session_cancel_media_bootstrap();

    tirtc_session_apply_local_media_policy();
    tirtc_session_flush_local_video_tx_queue();
    tirtc_session_flush_local_audio_tx_queue();
    tirtc_session_media_flush();

    tirtc_session_note_event("hangup local");
}

esp_err_t tirtc_session_set_external_audio_call_active(tirtc_conn_t conn, bool active)
{
    tirtc_conn_t active_conn = NULL;

    ESP_RETURN_ON_FALSE(conn != NULL, ESP_ERR_INVALID_ARG, TAG, "external call connection is null");
    ESP_RETURN_ON_FALSE(tirtc_session_try_get_active_conn(&active_conn) && active_conn == conn,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "external call connection is inactive");

    if (active) {
        taskENTER_CRITICAL(&s_rtc_lock);
        s_media_profile = TIRTC_SESSION_MEDIA_PROFILE_EXTERNAL_AUDIO;
        s_local_video_send_enabled = false;
        s_peer_wants_video = false;
        s_remote_video_requested = false;
        tirtc_session_sync_stats_locked();
        taskEXIT_CRITICAL(&s_rtc_lock);

        tirtc_session_complete_call_response(true);
        (void)tirtc_session_set_local_audio_send_enabled(true);
        tirtc_session_apply_local_media_policy();
    } else {
        tirtc_session_apply_hangup_local_state();
    }
    return ESP_OK;
}

esp_err_t tirtc_session_use_builtin_media(void)
{
    bool profile_changed = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_media_profile != TIRTC_SESSION_MEDIA_PROFILE_AV) {
        s_media_profile = TIRTC_SESSION_MEDIA_PROFILE_AV;
        profile_changed = true;
    }
    /*
     * AI Chat and WeChat VoIP deliberately bypass the built-in TiRTC capture
     * owner: AI sends command/audio frames by itself, and WeChat uses an
     * external WHIP audio path.  IPC viewing is the opposite: it relies on the
     * TiRTC media bridge owning the primary microphone callback and publishing
     * the local audio stream.  Re-arm that owner whenever IPC is entered so a
     * previous app cannot leave the session in "external audio" mode.
     */
    s_next_connection_auto_media = true;
    s_active_conn_auto_media = true;
    s_next_connection_defer_media = false;
    s_active_conn_defer_media = false;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    tirtc_session_restore_builtin_capture_callback();
    if (profile_changed) {
        ESP_LOGI(TAG, "rtc media owner restored: builtin");
    }
    tirtc_session_apply_local_media_policy();
    /*
     * IPC viewing is the only app that uses the built-in AV media bridge for
     * both directions. When returning from AI Chat or WeChat VoIP, the RTC
     * connection can already be active while the previous app left remote audio
     * subscription/playback bootstrap outside the normal "call accepted" edge.
     * Re-run the bootstrap here so IPC always re-requests remote audio/video and
     * prepares the speaker path after media ownership is restored.
     */
    tirtc_session_schedule_media_bootstrap("builtin media restored");
    return ESP_OK;
}

esp_err_t tirtc_session_set_builtin_audio_format(tirtc_session_builtin_audio_format_t format)
{
    bool changed = false;

    if (format != TIRTC_SESSION_BUILTIN_AUDIO_FORMAT_ALAW_8K &&
        format != TIRTC_SESSION_BUILTIN_AUDIO_FORMAT_PCM_16K) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_builtin_audio_format != format) {
        s_builtin_audio_format = format;
        changed = true;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (changed) {
        ESP_LOGI(TAG,
                 "builtin audio wire format: %s",
                 format == TIRTC_SESSION_BUILTIN_AUDIO_FORMAT_PCM_16K ? "pcm-16k" : "alaw-8k");
    }
    return ESP_OK;
}

void tirtc_session_apply_local_media_policy(void)
{
    bool enable_audio = false;
    bool audio_publish_forced = false;
    bool builtin_capture_allowed = false;
    bool capture_changed = false;
    bool peer_wants_audio = false;
    uint8_t local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    bool test_video_active = tirtc_session_is_test_video_active();
    bool test_audio_active = tirtc_session_is_test_audio_active();
    bool test_media_active = test_video_active || test_audio_active;

    taskENTER_CRITICAL(&s_rtc_lock);
    tirtc_session_sync_test_media_publish_locked(test_video_active, test_audio_active);
    builtin_capture_allowed = tirtc_session_media_profile_uses_builtin_capture_locked();
    audio_publish_forced = tirtc_session_maybe_force_local_audio_publish_locked();
    peer_wants_audio = s_peer_wants_audio;
    local_audio_stream_id = s_local_audio_stream_id;
    enable_audio = s_sdk_started && !s_start_in_progress && !s_stop_in_progress && s_closing_conn == NULL &&
                   s_active_conn != NULL && s_call_active && s_local_audio_send_enabled &&
                   builtin_capture_allowed &&
                   s_local_audio_stream_id != TIRTC_SESSION_INVALID_STREAM_ID &&
                   !s_media_bootstrap_pending;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (test_media_active) {
        enable_audio = false;
    }
    if (builtin_capture_allowed) {
        tirtc_session_restore_builtin_capture_callback();
    }

    if (audio_publish_forced) {
        ESP_LOGI(TAG,
                 "local audio publish fallback: stream=%u peer_audio=%d",
                 (unsigned)TIRTC_SESSION_LOCAL_AUDIO_STREAM_ID,
                 peer_wants_audio);
    }

    esp_err_t capture_ret = tirtc_session_media_set_capture_enabled(enable_audio);
    if (capture_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "audio capture policy failed: enable=%d peer_audio=%d stream=%u builtin=%d ret=%s",
                 enable_audio,
                 peer_wants_audio,
                 (unsigned)local_audio_stream_id,
                 builtin_capture_allowed,
                 esp_err_to_name(capture_ret));
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_builtin_capture_enabled != enable_audio) {
        s_builtin_capture_enabled = enable_audio;
        capture_changed = true;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (capture_changed) {
        ESP_LOGI(TAG,
                 "rtc microphone capture %s: stream=%u peer_audio=%d owner=%s",
                 enable_audio ? "enabled" : "disabled",
                 (unsigned)local_audio_stream_id,
                 peer_wants_audio,
                 builtin_capture_allowed ? "tirtc" : "external");
    }
}

void tirtc_session_refresh_media_policy(void)
{
    tirtc_session_apply_local_media_policy();
}

void tirtc_session_handle_connection_loss(tirtc_conn_t hconn, int error)
{
    bool tracked = false;
    bool was_sdk_started = false;
    bool newly_detached = false;
    bool wait_for_disconnect = false;

    tracked = tirtc_session_begin_connection_shutdown(hconn,
                                                     error,
                                                     &was_sdk_started,
                                                     &newly_detached);
    if (!tracked) {
        if (error != 0 && hconn != NULL) {
            /*
             * The SDK contract requires every handle delivered through
             * on_conn_error() to be released with TiRtcDisconnect().  An
             * inbound attempt can fail before on_conn_accepted(), so it is
             * intentionally absent from s_active_conn even though the SDK
             * still owns transport sockets for it.
             */
            if (!tirtc_session_drain_unowned_connection(hconn)) {
                int disconnect_ret = tirtc_session_disconnect_with_sdk_lock(hconn);
                if (disconnect_ret < 0) {
                    ESP_LOGW(TAG,
                             "unowned error connection release failed: hconn=%p error=%d ret=%s",
                             hconn,
                             error,
                             TiRtcGetErrorStr(disconnect_ret));
                } else {
                    ESP_LOGI(TAG,
                             "unowned error connection release requested: hconn=%p error=%d",
                             hconn,
                             error);
                }
            }
        } else {
            ESP_LOGD(TAG,
                     "ignore connection loss for inactive hconn=%p error=%d",
                     hconn,
                     error);
        }
        return;
    }

    if (error != 0) {
        tirtc_session_set_last_error(error);
        tirtc_session_note_event("conn error");
        ESP_LOGW(TAG, "rtc connection error: %s", TiRtcGetErrorStr(error));

        if (newly_detached) {
            if (error == TIRTC_E_CONN_REMOTECLOSE) {
                int disconnect_ret = tirtc_session_disconnect_with_sdk_lock(hconn);

                if (disconnect_ret >= 0) {
                    /*
                     * Remote close means the peer has already torn down its side.
                     * TiRtcDisconnect() only requests destruction; the handle remains
                     * owned by the SDK until on_disconnected() arrives. Keep the
                     * closing tombstone until that callback so a late error/callback
                     * cannot classify the same handle as unowned and destroy it twice.
                     * The disconnect watchdog bounds this wait if the SDK omits the
                     * callback.
                     */
                    ESP_LOGI(TAG,
                             "remote close cleanup requested: hconn=%p ret=%d, waiting for SDK callback",
                             hconn,
                             disconnect_ret);
                    wait_for_disconnect = true;
                } else {
                    ESP_LOGW(TAG,
                             "disconnect after remote close failed hconn=%p ret=%s; completing teardown inline",
                             hconn,
                             TiRtcGetErrorStr(disconnect_ret));
                }
            } else if (error == TIRTC_E_INVALID_HANDLE) {
                int disconnect_ret = tirtc_session_disconnect_with_sdk_lock(hconn);

                if (disconnect_ret >= 0) {
                    ESP_LOGD(TAG,
                             "request disconnect after invalid handle hconn=%p ret=%d",
                             hconn,
                             disconnect_ret);
                    wait_for_disconnect = true;
                } else {
                    ESP_LOGW(TAG,
                             "disconnect after invalid handle failed hconn=%p ret=%s; completing teardown inline",
                             hconn,
                             TiRtcGetErrorStr(disconnect_ret));
                }
            } else {
                int disconnect_ret = tirtc_session_disconnect_with_sdk_lock(hconn);
                if (disconnect_ret >= 0) {
                    wait_for_disconnect = true;
                } else {
                    tirtc_session_set_last_error(disconnect_ret);
                    ESP_LOGW(TAG,
                             "request disconnect during error handling failed: %s",
                             TiRtcGetErrorStr(disconnect_ret));
                }
            }
        } else {
            wait_for_disconnect = true;
        }

        if (wait_for_disconnect) {
            (void)tirtc_session_schedule_disconnect_watchdog("connection loss", TIRTC_SESSION_DISCONNECT_TIMEOUT_US);
            return;
        }
    } else {
        tirtc_session_note_event("disconnect done");
        ESP_LOGI(TAG, "rtc disconnected: hconn=%p", hconn);
    }

    tirtc_session_complete_connection_shutdown(hconn, was_sdk_started);
}

static void tirtc_session_worker_task(void *ctx)
{
    (void)ctx;
    tirtc_session_event_t event = {0};

    while (true) {
        if (xQueueReceive(s_event_queue, &event, pdMS_TO_TICKS(TIRTC_SESSION_WORKER_POLL_MS)) != pdTRUE) {
            uint64_t now_us = esp_timer_get_time();
            bool run_deferred_full_reset = false;
            bool run_deferred_start = false;

            taskENTER_CRITICAL(&s_rtc_lock);
            run_deferred_full_reset = s_deferred_full_reset_pending &&
                                      s_deferred_full_reset_due_at_us != 0U &&
                                      now_us >= s_deferred_full_reset_due_at_us;
            run_deferred_start = s_deferred_start_after_full_reset_pending &&
                                 s_deferred_start_after_full_reset_due_at_us != 0U &&
                                 now_us >= s_deferred_start_after_full_reset_due_at_us;
            taskEXIT_CRITICAL(&s_rtc_lock);

            if (run_deferred_full_reset) {
                tirtc_session_handle_deferred_full_reset();
            }
            if (run_deferred_start) {
                tirtc_session_handle_deferred_start_after_full_reset();
            }
            continue;
        }

        switch (event.type) {
        case TIRTC_SESSION_EVENT_START_IF_READY:
            (void)tirtc_session_start_sdk_from_worker();
            break;
        case TIRTC_SESSION_EVENT_REMOTE_MESSAGE:
            (void)tirtc_session_notify_message(event.payload.message.conn,
                                               event.payload.message.media,
                                               event.payload.message.stream_id,
                                               event.payload.message.flags,
                                               event.payload.message.data,
                                               (uint32_t)event.payload.message.data_len);
            tirtc_session_handle_remote_message(&event);
            break;
        case TIRTC_SESSION_EVENT_REMOTE_COMMAND:
            if (!tirtc_session_notify_command(event.payload.command.conn,
                                              event.payload.command.cmdw,
                                              event.payload.command.data,
                                              (uint32_t)event.payload.command.data_len)) {
                tirtc_session_handle_remote_command(&event);
            }
            break;
        case TIRTC_SESSION_EVENT_CONN_ERROR:
            tirtc_session_notify_connection_error(event.payload.conn.conn, event.payload.conn.error);
            tirtc_session_handle_runtime_event(&event);
            break;
        case TIRTC_SESSION_EVENT_DISCONNECTED:
            tirtc_session_notify_disconnected(event.payload.conn.conn);
            tirtc_session_handle_runtime_event(&event);
            break;
        case TIRTC_SESSION_EVENT_DISCONNECT_REQUEST:
            tirtc_session_handle_disconnect_request(&event);
            break;
        default:
            tirtc_session_handle_runtime_event(&event);
            break;
        }

        tirtc_session_free_event_payload(&event);
    }
}

esp_err_t tirtc_session_configure(const tirtc_session_config_t *config)
{
    tirtc_session_config_t normalized_config;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(tirtc_session_validate_service_endpoint(config),
                        TAG,
                        "rtc service endpoint validation failed");

    normalized_config = *config;

    if (tirtc_connect_is_connecting()) {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_sdk_started || s_sdk_prepare_in_progress || s_start_in_progress || s_stop_in_progress ||
        s_active_conn != NULL || s_closing_conn != NULL) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_sdk_initialized && strcmp(s_config.service_endpoint, normalized_config.service_endpoint) != 0) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        ESP_LOGW(TAG, "rtc endpoint changed after SDK init; reboot or explicit full reset is required");
        return ESP_ERR_INVALID_STATE;
    }

    s_config = normalized_config;
    s_session_mode = normalized_config.default_session_mode;
    s_next_start_allowed_us = 0U;
    s_state_error_override = false;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);
    return ESP_OK;
}

esp_err_t tirtc_session_set_media_bridge(const tirtc_session_media_ops_t *ops, void *ctx)
{
    if (ops == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_initialized || s_sdk_started || s_sdk_prepare_in_progress || s_start_in_progress || s_stop_in_progress) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_media_ops = *ops;
    s_media_ctx = ctx;
    taskEXIT_CRITICAL(&s_rtc_lock);
    return ESP_OK;
}

void tirtc_session_set_hooks(const tirtc_session_hooks_t *hooks, void *ctx)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    if (hooks != NULL) {
        s_hooks = *hooks;
        s_hooks_ctx = ctx;
    } else {
        memset(&s_hooks, 0, sizeof(s_hooks));
        s_hooks_ctx = NULL;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
}

void tirtc_session_set_control_ops(const tirtc_session_control_ops_t *ops, void *ctx)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    if (ops != NULL) {
        s_control_ops = *ops;
        s_control_ctx = ctx;
    } else {
        memset(&s_control_ops, 0, sizeof(s_control_ops));
        s_control_ctx = NULL;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
}

static bool tirtc_session_copy_control_ops(tirtc_session_control_ops_t *ops, void **ctx)
{
    bool configured = false;

    if (ops == NULL || ctx == NULL) {
        return false;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    *ops = s_control_ops;
    *ctx = s_control_ctx;
    configured = ops->set_speaker_volume != NULL || ops->set_door_open != NULL;
    taskEXIT_CRITICAL(&s_rtc_lock);
    return configured;
}

esp_err_t tirtc_session_apply_remote_volume_command(uint8_t percent)
{
    tirtc_session_control_ops_t ops = {0};
    void *ctx = NULL;

    if (!tirtc_session_copy_control_ops(&ops, &ctx) || ops.set_speaker_volume == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ops.set_speaker_volume(percent, ctx);
}

esp_err_t tirtc_session_apply_remote_door_command(bool open)
{
    tirtc_session_control_ops_t ops = {0};
    void *ctx = NULL;

    if (!tirtc_session_copy_control_ops(&ops, &ctx) || ops.set_door_open == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ops.set_door_open(open, ctx);
}

esp_err_t tirtc_session_register_observer(const tirtc_session_observer_t *observer, void *ctx)
{
    if (observer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_observer_lock);
    for (size_t index = 0; index < TIRTC_SESSION_OBSERVER_MAX; ++index) {
        if (s_observers[index].used &&
            s_observers[index].observer.on_command == observer->on_command &&
            s_observers[index].observer.on_message == observer->on_message &&
            s_observers[index].observer.on_connection_accepted == observer->on_connection_accepted &&
            s_observers[index].observer.on_connection_error == observer->on_connection_error &&
            s_observers[index].observer.on_disconnected == observer->on_disconnected &&
            s_observers[index].observer.on_start_error == observer->on_start_error &&
            s_observers[index].ctx == ctx) {
            taskEXIT_CRITICAL(&s_observer_lock);
            return ESP_OK;
        }
    }
    for (size_t index = 0; index < TIRTC_SESSION_OBSERVER_MAX; ++index) {
        if (!s_observers[index].used) {
            s_observers[index].observer = *observer;
            s_observers[index].ctx = ctx;
            s_observers[index].used = true;
            taskEXIT_CRITICAL(&s_observer_lock);
            return ESP_OK;
        }
    }
    taskEXIT_CRITICAL(&s_observer_lock);

    return ESP_ERR_NO_MEM;
}

esp_err_t tirtc_session_init(const tirtc_session_config_t *config)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(tirtc_session_has_media_bridge(),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "rtc media bridge not configured");
    ESP_RETURN_ON_ERROR(tirtc_session_configure(config), TAG, "configure rtc failed");
    tirtc_session_init_stats();
    ESP_RETURN_ON_ERROR(tirtc_session_create_runtime_resources(), TAG, "rtc runtime init failed");
    tirtc_session_configure_runtime_callbacks();

    s_initialized = true;
    return ESP_OK;
}

const char *tirtc_session_error_string(int error)
{
    const char *message = TiRtcGetErrorStr(error);

    return message != NULL ? message : "unknown TiRTC error";
}

void tirtc_session_set_identity_ready(bool ready)
{
    bool changed = false;
    bool reset_required = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    changed = s_identity_ready != ready;
    s_identity_ready = ready;
    if (!ready) {
        reset_required = s_initialized &&
                         (s_sdk_initialized || s_sdk_started || s_sdk_prepare_in_progress ||
                          s_start_in_progress || s_stop_in_progress);
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!changed) {
        return;
    }

    ESP_LOGI(TAG, "rtc identity gate changed: ready=%d", ready ? 1 : 0);
    if (!ready && reset_required && !tirtc_session_schedule_deferred_full_reset()) {
        ESP_LOGW(TAG, "rtc identity gate could not schedule sdk reset");
    }
}

esp_err_t tirtc_session_prepare_sdk(void)
{
    bool network_connected = false;
    bool identity_ready = false;
    bool prepare_in_progress = false;
    bool request_start = false;
    bool start_backoff_active = false;
    bool retry_timer_pending = false;
    uint64_t next_start_allowed_us = 0U;
    uint64_t retry_remaining_us = 0U;
    uint64_t now_us = 0U;
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "rtc not initialized");

    if (!s_config.enabled) {
        tirtc_session_note_event("rtc disabled");
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    identity_ready = s_identity_ready;
    taskEXIT_CRITICAL(&s_rtc_lock);
    if (!identity_ready) {
        tirtc_session_note_event("identity not ready");
        return ESP_ERR_INVALID_STATE;
    }
    if (!system_time_has_valid_time()) {
        tirtc_session_note_event("waiting time");
        ESP_LOGI(TAG, "rtc sdk init waits for valid system time");
        return ESP_ERR_INVALID_STATE;
    }
    if (strlen(s_config.service_endpoint) == 0) {
        tirtc_session_note_event("waiting endpoint");
        ESP_LOGI(TAG, "rtc sdk init waits for endpoint");
        return ESP_ERR_INVALID_STATE;
    }

    now_us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_rtc_lock);
    network_connected = s_network_connected;
    identity_ready = s_identity_ready;
    if (!identity_ready) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        tirtc_session_note_event("identity not ready");
        return ESP_ERR_INVALID_STATE;
    }
    start_backoff_active = !s_sdk_started &&
                           tirtc_session_start_backoff_active_locked(now_us,
                                                                    &retry_remaining_us,
                                                                    &retry_timer_pending);
    if (start_backoff_active) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        tirtc_session_note_event("start backoff");
        tirtc_session_ensure_start_backoff_timer(retry_remaining_us, retry_timer_pending);
        ESP_LOGD(TAG,
                 "rtc sdk prepare blocked by start backoff: retry_in_ms=%llu",
                 (unsigned long long)((retry_remaining_us + 999ULL) / 1000ULL));
        return ESP_ERR_INVALID_STATE;
    }
    if (s_sdk_initialized) {
        next_start_allowed_us = s_next_start_allowed_us;
        if (identity_ready && network_connected && !s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
            (next_start_allowed_us == 0U || now_us >= next_start_allowed_us)) {
            request_start = true;
        }
        bool sdk_ready = identity_ready && network_connected && !s_stop_in_progress &&
                         (s_sdk_started || s_start_in_progress || request_start);
        taskEXIT_CRITICAL(&s_rtc_lock);
        if (network_connected && !request_start && !sdk_ready &&
            next_start_allowed_us != 0U && now_us < next_start_allowed_us) {
            tirtc_session_note_event("start backoff");
            ESP_LOGD(TAG,
                     "rtc listen start delayed after previous failure: retry_in_ms=%llu",
                     (unsigned long long)((next_start_allowed_us - now_us + 999ULL) / 1000ULL));
        }
        if (request_start && !tirtc_session_enqueue_start_if_ready()) {
            tirtc_session_note_event("start evt drop");
            ESP_LOGW(TAG, "rtc listen start event dropped");
            return ESP_ERR_TIMEOUT;
        }
        return sdk_ready ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    prepare_in_progress = s_sdk_prepare_in_progress;
    if (!prepare_in_progress &&
        (s_sdk_started || s_start_in_progress || s_stop_in_progress ||
         s_active_conn != NULL || s_closing_conn != NULL)) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!prepare_in_progress) {
        s_sdk_prepare_in_progress = true;
        tirtc_session_sync_stats_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!network_connected) {
        if (!prepare_in_progress) {
            taskENTER_CRITICAL(&s_rtc_lock);
            s_sdk_prepare_in_progress = false;
            tirtc_session_sync_stats_locked();
            taskEXIT_CRITICAL(&s_rtc_lock);
        }
        tirtc_session_note_event("waiting network");
        return ESP_ERR_INVALID_STATE;
    }

    if (prepare_in_progress) {
        return ESP_OK;
    }

    if (!tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        tirtc_session_note_event("sdk lock failed");
        ret = ESP_FAIL;
    } else {
        ret = tirtc_session_prepare_sdk_with_lock();
        tirtc_session_give_sdk_api_lock();
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    s_sdk_prepare_in_progress = false;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (ret != ESP_OK) {
        return ret;
    }

    if (!tirtc_session_enqueue_start_if_ready()) {
        tirtc_session_note_event("start evt drop");
        ESP_LOGW(TAG, "rtc listen start event dropped after init");
        return ESP_ERR_TIMEOUT;
    }

    tirtc_session_note_event("sdk initialized");
    return ESP_OK;
}

esp_err_t tirtc_session_start_if_ready(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "rtc not initialized");
    return tirtc_session_prepare_sdk();
}

int tirtc_session_whip_connect(const char *service_desc,
                               const char *token,
                               TIRTCCONNECTCALLBACK cb,
                               void *user_data)
{
    tirtc_session_whip_request_t *request = NULL;
    size_t service_desc_len = 0;
    size_t token_len = 0;
    int ret = TIRTC_E_BUSY;

    if (service_desc == NULL || service_desc[0] == '\0' ||
        token == NULL || token[0] == '\0' || cb == NULL) {
        return TIRTC_E_INVALID_PARAMETER;
    }
    service_desc_len = strlen(service_desc);
    token_len = strlen(token);
    if (!s_initialized) {
        return TIRTC_E_BUSY;
    }
    taskENTER_CRITICAL(&s_rtc_lock);
    bool sdk_ready = s_network_connected && s_sdk_initialized && s_sdk_started &&
                     !s_sdk_prepare_in_progress && !s_start_in_progress &&
                     !s_stop_in_progress && s_active_conn == NULL && s_closing_conn == NULL;
    tirtc_conn_t active_conn = s_active_conn;
    tirtc_conn_t closing_conn = s_closing_conn;
    taskEXIT_CRITICAL(&s_rtc_lock);
    if (!sdk_ready) {
        ESP_LOGW(TAG,
                 "WHIP submit rejected before SDK call: active=%p closing=%p",
                 active_conn,
                 closing_conn);
        return TIRTC_E_BUSY;
    }

    request = tirtc_session_alloc_whip_request(service_desc, token, cb, user_data);
    if (request == NULL) {
        return TIRTC_E_LACK_OF_RESOURCE;
    }

    ESP_LOGI(TAG,
             "WHIP submit begin: service_desc_len=%u token_len=%u",
             (unsigned)service_desc_len,
             (unsigned)token_len);
    if (!tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        tirtc_session_free_whip_request(request);
        tirtc_session_set_next_connection_auto_media(true);
        return TIRTC_E_BUSY;
    }
    ret = TiRtcWhipConnect(request->service_desc,
                           request->token,
                           tirtc_session_on_whip_connect_result,
                           request);
    tirtc_session_give_sdk_api_lock();

    if (ret != 0) {
        ESP_LOGE(TAG,
                 "WHIP submit rejected: ret=%d %s service_desc_len=%u token_len=%u",
                 ret,
                 TiRtcGetErrorStr(ret),
                 (unsigned)service_desc_len,
                 (unsigned)token_len);
        tirtc_session_free_whip_request(request);
        tirtc_session_set_next_connection_auto_media(true);
    }
    return ret;
}

int tirtc_session_whip_connect_external(const char *service_desc,
                                        const char *token,
                                        TIRTCCONNECTCALLBACK cb,
                                        void *user_data)
{
    tirtc_session_whip_request_t *request = NULL;
    size_t service_desc_len = 0;
    size_t token_len = 0;
    int ret = TIRTC_E_BUSY;

    if (service_desc == NULL || service_desc[0] == '\0' ||
        token == NULL || token[0] == '\0' || cb == NULL) {
        return TIRTC_E_INVALID_PARAMETER;
    }
    service_desc_len = strlen(service_desc);
    token_len = strlen(token);
    if (!s_initialized) {
        return TIRTC_E_BUSY;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    bool sdk_ready = s_network_connected && s_sdk_initialized && s_sdk_started &&
                     !s_sdk_prepare_in_progress && !s_start_in_progress &&
                     !s_stop_in_progress && s_active_conn == NULL && s_closing_conn == NULL;
    tirtc_conn_t active_conn = s_active_conn;
    tirtc_conn_t closing_conn = s_closing_conn;
    taskEXIT_CRITICAL(&s_rtc_lock);
    if (!sdk_ready) {
        ESP_LOGW(TAG,
                 "WHIP external submit rejected before SDK call: active=%p closing=%p",
                 active_conn,
                 closing_conn);
        return TIRTC_E_BUSY;
    }

    ESP_LOGI(TAG,
             "WHIP external submit begin: service_desc_len=%u token_len=%u",
             (unsigned)service_desc_len,
             (unsigned)token_len);
    request = tirtc_session_alloc_whip_request(service_desc, token, cb, user_data);
    if (request == NULL) {
        return TIRTC_E_LACK_OF_RESOURCE;
    }

    if (!tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        tirtc_session_free_whip_request(request);
        return TIRTC_E_BUSY;
    }
    ret = TiRtcWhipConnect(request->service_desc,
                           request->token,
                           tirtc_session_on_external_whip_connect_result,
                           request);
    tirtc_session_give_sdk_api_lock();

    if (ret != 0) {
        ESP_LOGE(TAG,
                 "WHIP external submit rejected: ret=%d %s service_desc_len=%u token_len=%u",
                 ret,
                 TiRtcGetErrorStr(ret),
                 (unsigned)service_desc_len,
                 (unsigned)token_len);
        tirtc_session_free_whip_request(request);
    }
    return ret;
}

int tirtc_session_service_request(const char *path,
                                  const char *json_body,
                                  const char *token,
                                  TIRTCSERVICEREQUESTCALLBACK cb,
                                  void *user_data)
{
    int ret = TIRTC_E_BUSY;

    if (path == NULL || path[0] == '\0') {
        return TIRTC_E_INVALID_PARAMETER;
    }
    if (!s_initialized) {
        return TIRTC_E_NOT_INITIALIZED;
    }

    if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        ret = TiRtcServiceRequest(path, json_body, token, cb, user_data);
        tirtc_session_give_sdk_api_lock();
    }

    return ret;
}

esp_err_t tirtc_session_connect_peer(const char *remote_device_id,
                                     const char *remote_device_secret_key)
{
    bool sdk_started = false;
    bool start_in_progress = false;
    bool connect_in_progress = false;
    tirtc_session_event_t event = {0};

    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "rtc not initialized");
    ESP_RETURN_ON_FALSE(s_event_queue != NULL, ESP_ERR_INVALID_STATE, TAG, "rtc event queue not ready");
    ESP_RETURN_ON_FALSE(remote_device_id != NULL && remote_device_id[0] != '\0',
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "remote device id is empty");
    ESP_RETURN_ON_FALSE(strlen(remote_device_id) < sizeof(s_config.remote_device_id),
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "remote device id is too long");
    if (remote_device_secret_key != NULL && remote_device_secret_key[0] != '\0') {
        ESP_RETURN_ON_FALSE(strlen(remote_device_secret_key) < sizeof(s_config.remote_device_secret_key),
                            ESP_ERR_INVALID_SIZE,
                            TAG,
                            "remote device secret is too long");
    }
    connect_in_progress = tirtc_connect_is_connecting();
    if (connect_in_progress) {
        ESP_LOGW(TAG, "rtc peer connect rejected: active connect is already running");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(tirtc_session_prepare_sdk(), TAG, "prepare rtc sdk failed");

    taskENTER_CRITICAL(&s_rtc_lock);
    if (!s_network_connected || !s_sdk_initialized || s_sdk_prepare_in_progress ||
        s_stop_in_progress ||
        s_active_conn != NULL || s_closing_conn != NULL) {
        bool network_connected = s_network_connected;
        bool sdk_initialized = s_sdk_initialized;
        bool sdk_prepare_in_progress = s_sdk_prepare_in_progress;
        bool stop_in_progress = s_stop_in_progress;
        tirtc_conn_t active_conn = s_active_conn;
        tirtc_conn_t closing_conn = s_closing_conn;
        taskEXIT_CRITICAL(&s_rtc_lock);
        ESP_LOGW(TAG,
                 "rtc peer connect rejected: net=%d init=%d prep=%d stop=%d active=%p closing=%p connecting=%d",
                 network_connected ? 1 : 0,
                 sdk_initialized ? 1 : 0,
                 sdk_prepare_in_progress ? 1 : 0,
                 stop_in_progress ? 1 : 0,
                 active_conn,
                 closing_conn,
                 connect_in_progress ? 1 : 0);
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_sdk_started && !s_start_in_progress) {
        bool sdk_initialized = s_sdk_initialized;
        bool sdk_prepare_in_progress = s_sdk_prepare_in_progress;
        taskEXIT_CRITICAL(&s_rtc_lock);
        ESP_LOGW(TAG,
                 "rtc peer connect rejected: sdk not started init=%d prep=%d",
                 sdk_initialized ? 1 : 0,
                 sdk_prepare_in_progress ? 1 : 0);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_config.device_id[0] != '\0' && strcmp(remote_device_id, s_config.device_id) == 0) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        tirtc_session_note_event("self call blocked");
        ESP_LOGW(TAG, "rtc peer connect rejected: remote_id is local device_id");
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_config.remote_device_id, remote_device_id, sizeof(s_config.remote_device_id));
    if (remote_device_secret_key != NULL && remote_device_secret_key[0] != '\0') {
        strlcpy(s_config.remote_device_secret_key,
                remote_device_secret_key,
                sizeof(s_config.remote_device_secret_key));
    }
    if (s_config.remote_device_secret_key[0] == '\0') {
        taskEXIT_CRITICAL(&s_rtc_lock);
        return ESP_ERR_INVALID_ARG;
    }

    s_session_mode = TIRTC_SESSION_MODE_CONNECT;
    s_next_connection_auto_media = false;
    s_next_connection_defer_media = false;
    sdk_started = s_sdk_started;
    start_in_progress = s_start_in_progress;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    event.type = sdk_started ? TIRTC_SESSION_EVENT_CONNECT_PEER : TIRTC_SESSION_EVENT_START_IF_READY;
    if (!tirtc_session_enqueue_event(&event, TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        tirtc_session_return_to_listen_mode();
        tirtc_session_note_event("peer connect evt drop");
        return ESP_FAIL;
    }

    tirtc_session_note_event(sdk_started ? "peer connect req" : "peer wait start");
    ESP_LOGI(TAG,
             "rtc peer connect requested: remote_id_len=%u sdk_started=%d start_in_progress=%d",
             (unsigned)strlen(remote_device_id),
             sdk_started,
             start_in_progress);
    return ESP_OK;
}

esp_err_t tirtc_session_connect_peer_with_token(const char *remote_device_id,
                                                const char *connect_token)
{
    bool connect_in_progress = false;
    bool current_attempt = false;
    uint32_t peer_generation = 0;
    tirtc_session_config_t config = {0};

    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "rtc not initialized");
    ESP_RETURN_ON_FALSE(s_event_queue != NULL, ESP_ERR_INVALID_STATE, TAG, "rtc event queue not ready");
    ESP_RETURN_ON_FALSE(remote_device_id != NULL && remote_device_id[0] != '\0',
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "remote device id is empty");
    ESP_RETURN_ON_FALSE(connect_token != NULL && connect_token[0] != '\0',
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "connect token is empty");
    ESP_RETURN_ON_FALSE(strlen(remote_device_id) < sizeof(s_config.remote_device_id),
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "remote device id is too long");
    ESP_RETURN_ON_FALSE(strlen(connect_token) < TIRTC_CONNECT_TOKEN_MAX_LEN,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "connect token is too long");

    connect_in_progress = tirtc_connect_is_connecting();
    if (connect_in_progress) {
        ESP_LOGW(TAG, "rtc token connect rejected: active connect is already running");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(tirtc_session_prepare_sdk(), TAG, "prepare rtc sdk failed");

    taskENTER_CRITICAL(&s_rtc_lock);
    if (!s_network_connected || !s_sdk_initialized || !s_sdk_started ||
        s_sdk_prepare_in_progress || s_start_in_progress || s_stop_in_progress ||
        s_active_conn != NULL || s_closing_conn != NULL) {
        bool network_connected = s_network_connected;
        bool sdk_initialized = s_sdk_initialized;
        bool sdk_started = s_sdk_started;
        bool sdk_prepare_in_progress = s_sdk_prepare_in_progress;
        bool start_in_progress = s_start_in_progress;
        bool stop_in_progress = s_stop_in_progress;
        tirtc_conn_t active_conn = s_active_conn;
        tirtc_conn_t closing_conn = s_closing_conn;
        taskEXIT_CRITICAL(&s_rtc_lock);
        ESP_LOGW(TAG,
                 "rtc token connect rejected: net=%d init=%d started=%d prep=%d start=%d stop=%d active=%p closing=%p",
                 network_connected ? 1 : 0,
                 sdk_initialized ? 1 : 0,
                 sdk_started ? 1 : 0,
                 sdk_prepare_in_progress ? 1 : 0,
                 start_in_progress ? 1 : 0,
                 stop_in_progress ? 1 : 0,
                 active_conn,
                 closing_conn);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_config.device_id[0] != '\0' && strcmp(remote_device_id, s_config.device_id) == 0) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        tirtc_session_note_event("self call blocked");
        ESP_LOGW(TAG, "rtc token connect rejected: remote_id is local device_id");
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_config.remote_device_id, remote_device_id, sizeof(s_config.remote_device_id));
    s_session_mode = TIRTC_SESSION_MODE_CONNECT;
    s_next_connection_auto_media = true;
    s_next_connection_defer_media = false;
    peer_generation = tirtc_session_next_peer_connect_generation_locked();
    config = s_config;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    esp_err_t ret = tirtc_connect_start_with_token(config.remote_device_id,
                                                   connect_token,
                                                   tirtc_session_on_peer_connect_result,
                                                   (void *)(uintptr_t)peer_generation);
    if (ret != ESP_OK) {
        tirtc_session_return_to_listen_mode();
        tirtc_session_set_last_error(ret);
        tirtc_session_note_event("token connect fail");
        return ret;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    current_attempt = s_session_mode == TIRTC_SESSION_MODE_CONNECT &&
                      s_peer_connect_generation == peer_generation;
    taskEXIT_CRITICAL(&s_rtc_lock);
    if (!current_attempt) {
        (void)tirtc_connect_cancel_pending();
        return ESP_ERR_INVALID_STATE;
    }

    tirtc_session_note_event("token connect");
    ESP_LOGI(TAG,
             "rtc token peer connect task started: remote_id_len=%u token_len=%u",
             (unsigned)strlen(config.remote_device_id),
             (unsigned)strlen(connect_token));
    return ESP_OK;
}

esp_err_t tirtc_session_restart(void)
{
    tirtc_session_runtime_snapshot_t snapshot = {0};
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "rtc not initialized");

    tirtc_session_get_runtime_snapshot(&snapshot);

    if (!snapshot.enabled) {
        return ESP_OK;
    }

    if (snapshot.active_conn != NULL || snapshot.closing_conn != NULL) {
        tirtc_session_note_event("manual restart");
        ret = tirtc_session_disconnect();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            return ret;
        }
    }

    return tirtc_session_prepare_sdk();
}

esp_err_t tirtc_session_stop(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }
    return tirtc_session_disconnect();
}

esp_err_t tirtc_session_disconnect(void)
{
    tirtc_conn_t conn = NULL;
    bool was_sdk_started = false;
    bool connect_draining = false;
    bool newly_detached = false;

    /*
     * Disconnect is a lifecycle release operation. Identity reset and app
     * teardown may legitimately reach it before the lazy RTC runtime init.
     */
    if (!s_initialized) {
        return ESP_OK;
    }

    /* A business hangup can race TiRtcConnect before the SDK has produced an
     * hconn. Invalidate the session attempt first, then cancel the adapter
     * intent. A submitted handle-less connect remains busy until its result
     * callback drains it; starting another TiRtcConnect on top would corrupt
     * the following call lifecycle. */
    taskENTER_CRITICAL(&s_rtc_lock);
    (void)tirtc_session_next_peer_connect_generation_locked();
    s_session_mode = TIRTC_SESSION_MODE_LISTEN;
    s_next_connection_auto_media = true;
    s_next_connection_defer_media = false;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    connect_draining = tirtc_connect_cancel_pending();

    if (!tirtc_session_try_get_active_conn(&conn)) {
        tirtc_session_note_event(connect_draining ? "connect draining" : "disconnect idle");
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(tirtc_session_begin_connection_shutdown(conn,
                                                                0,
                                                                &was_sdk_started,
                                                                &newly_detached),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "rtc connection shutdown not tracked");

    /* try_get_active_conn() and begin_connection_shutdown() are deliberately
     * separate operations because the latter also owns media teardown. Two
     * callers can therefore observe the same active handle before either one
     * detaches it. Only the caller that performs the active -> closing
     * transition may enqueue TiRtcDisconnect(); later callers join the same
     * teardown instead of asking the SDK to destroy one handle twice. */
    if (!newly_detached) {
        tirtc_session_note_event("disconnect joined");
        return ESP_OK;
    }

    if (!tirtc_session_enqueue_disconnect_request(conn, true, was_sdk_started)) {
        tirtc_session_note_event("disconnect drop");
        ESP_LOGW(TAG, "rtc disconnect request dropped: hconn=%p", conn);
        tirtc_session_complete_connection_shutdown(conn, was_sdk_started);
        return ESP_ERR_TIMEOUT;
    }

    tirtc_session_note_event("disconnect req");
    (void)tirtc_session_schedule_disconnect_watchdog("explicit disconnect",
                                                     TIRTC_SESSION_DISCONNECT_TIMEOUT_US);
    return ESP_OK;
}

bool tirtc_session_get_active_connection(tirtc_conn_t *conn)
{
    return tirtc_session_try_get_active_conn(conn);
}

int tirtc_session_disconnect_connection(tirtc_conn_t conn)
{
    if (conn == NULL) {
        return TIRTC_E_INVALID_PARAMETER;
    }
    if (!tirtc_session_enqueue_disconnect_request(conn, false, false)) {
        return TIRTC_E_BUSY;
    }
    return 0;
}

bool tirtc_session_drain_unowned_connection(tirtc_conn_t conn)
{
    bool tracked = false;
    bool was_sdk_started = false;

    if (conn == NULL) {
        return false;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_active_conn == NULL && s_closing_conn == NULL &&
        s_sdk_started && !s_stop_in_progress) {
        s_closing_conn = conn;
        s_closing_conn_was_sdk_started = true;
        was_sdk_started = true;
        tirtc_session_sync_stats_locked();
        tracked = true;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!tracked) {
        return false;
    }

    tirtc_session_bind_connection_user_data(conn);
    ESP_LOGI(TAG, "drain canceled peer connection: hconn=%p", conn);
    if (!tirtc_session_enqueue_disconnect_request(conn, true, was_sdk_started)) {
        ESP_LOGW(TAG, "canceled peer disconnect request dropped: hconn=%p", conn);
        (void)tirtc_session_complete_connection_shutdown(conn, was_sdk_started);
        return false;
    }
    (void)tirtc_session_schedule_disconnect_watchdog("canceled peer connect",
                                                     TIRTC_SESSION_DISCONNECT_TIMEOUT_US);
    return true;
}

bool tirtc_session_get_last_peer_state(tirtc_session_peer_state_t *state)
{
    if (state == NULL) {
        return false;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    *state = s_last_peer_state;
    taskEXIT_CRITICAL(&s_rtc_lock);
    return state->valid;
}

void tirtc_session_on_network_state_changed(const tirtc_session_network_state_t *state)
{
    if (state == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    s_network_connected = state->connected;
    taskEXIT_CRITICAL(&s_rtc_lock);

    tirtc_session_event_t event = {
        .type = TIRTC_SESSION_EVENT_NETWORK_CHANGED,
        .payload.network = {
            .state = *state,
        },
    };
    if (!tirtc_session_enqueue_event(&event, TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        tirtc_session_note_event("network evt drop");
        ESP_LOGW(TAG, "rtc event queue full: network change dropped connected=%d", state->connected);
    }
}

esp_err_t tirtc_session_set_local_video_send_enabled(bool enabled)
{
    bool forced_publish = false;
    bool cleared_forced_publish = false;

    if (enabled && !APP_PRODUCT_RTC_VIDEO_ENABLED) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    s_local_video_send_enabled = enabled;
    if (!enabled && s_local_video_publish_forced) {
        s_local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        s_local_video_publish_forced = false;
        s_test_video_publish_forced = false;
        cleared_forced_publish = true;
    } else if (enabled) {
        forced_publish = tirtc_session_maybe_force_local_video_publish_locked();
    }
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (forced_publish) {
        ESP_LOGD(TAG,
                 "local video fallback publish forced: send enabled after peer request stream=%u",
                 (unsigned)TIRTC_SESSION_LOCAL_VIDEO_STREAM_ID);
    } else if (cleared_forced_publish) {
        ESP_LOGD(TAG, "local video fallback publish cleared");
    }

    tirtc_session_note_event(enabled ? "video send on" : "video send off");
    tirtc_session_apply_local_media_policy();
    return ESP_OK;
}

esp_err_t tirtc_session_set_local_audio_send_enabled(bool enabled)
{
    bool forced_publish = false;
    bool cleared_forced_publish = false;
    bool flush_audio_queue = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    flush_audio_queue = !enabled && s_local_audio_send_enabled;
    s_local_audio_send_enabled = enabled;
    if (!enabled && s_local_audio_publish_forced) {
        s_local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        s_local_audio_publish_forced = false;
        s_test_audio_publish_forced = false;
        cleared_forced_publish = true;
    } else if (enabled) {
        forced_publish = tirtc_session_maybe_force_local_audio_publish_locked();
    }
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (forced_publish) {
        ESP_LOGD(TAG,
                 "local audio fallback publish forced: send enabled after peer request stream=%u",
                 (unsigned)TIRTC_SESSION_LOCAL_AUDIO_STREAM_ID);
    } else if (cleared_forced_publish) {
        ESP_LOGD(TAG, "local audio fallback publish cleared");
    }

    if (flush_audio_queue) {
        tirtc_session_flush_local_audio_tx_queue();
    }
    tirtc_session_note_event(enabled ? "audio send on" : "audio send off");
    tirtc_session_apply_local_media_policy();
    return ESP_OK;
}

esp_err_t tirtc_session_set_remote_audio_stream_id(uint8_t stream_id)
{
    if (stream_id == TIRTC_SESSION_INVALID_STREAM_ID) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (stream_id == s_next_remote_audio_stream_id) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        return ESP_OK;
    }
    s_next_remote_audio_stream_id = stream_id;
    taskEXIT_CRITICAL(&s_rtc_lock);

    ESP_LOGI(TAG, "next connection remote audio stream configured: stream=%u", (unsigned)stream_id);
    return ESP_OK;
}

esp_err_t tirtc_session_set_session_mode(tirtc_session_mode_t session_mode)
{
    if (session_mode != TIRTC_SESSION_MODE_LISTEN &&
        session_mode != TIRTC_SESSION_MODE_CONNECT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (tirtc_connect_is_connecting()) {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_sdk_started || s_sdk_prepare_in_progress || s_start_in_progress || s_stop_in_progress ||
        s_active_conn != NULL || s_closing_conn != NULL) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_session_mode = session_mode;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);
    tirtc_session_note_event(session_mode == TIRTC_SESSION_MODE_LISTEN ? "mode listen" : "mode connect");
    return ESP_OK;
}

tirtc_session_mode_t tirtc_session_get_session_mode(void)
{
    tirtc_session_mode_t session_mode = TIRTC_SESSION_MODE_LISTEN;

    taskENTER_CRITICAL(&s_rtc_lock);
    session_mode = s_session_mode;
    taskEXIT_CRITICAL(&s_rtc_lock);
    return session_mode;
}

tirtc_session_state_t tirtc_session_get_state(void)
{
    tirtc_session_state_t state = TIRTC_SESSION_STATE_STOPPED;

    taskENTER_CRITICAL(&s_rtc_lock);
    state = s_state;
    taskEXIT_CRITICAL(&s_rtc_lock);
    return state;
}

void tirtc_session_get_config(tirtc_session_config_t *config)
{
    if (config == NULL) {
        return;
    }
    *config = s_config;
}

bool tirtc_session_get_started_auth_debug(char *device_id,
                                          size_t device_id_size,
                                          char *credential_hash,
                                          size_t credential_hash_size,
                                          uint32_t *secret_len)
{
    bool available = false;

    if ((device_id == NULL || device_id_size == 0) &&
        (credential_hash == NULL || credential_hash_size == 0) &&
        secret_len == NULL) {
        return false;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    available = s_started_device_id[0] != '\0' && s_started_credential_hash[0] != '\0';
    if (device_id != NULL && device_id_size > 0) {
        strlcpy(device_id, s_started_device_id, device_id_size);
    }
    if (credential_hash != NULL && credential_hash_size > 0) {
        strlcpy(credential_hash, s_started_credential_hash, credential_hash_size);
    }
    if (secret_len != NULL) {
        *secret_len = s_started_secret_len;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
    return available;
}

void tirtc_session_get_stats(tirtc_session_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    uint32_t audio_queue_depth = s_local_audio_tx_queue != NULL ?
                                     (uint32_t)uxQueueMessagesWaiting(s_local_audio_tx_queue) : 0U;

    taskENTER_CRITICAL(&s_rtc_lock);
    tirtc_session_sync_stats_locked();
    s_stats.tx_audio_queue_depth_packets = audio_queue_depth;
    *stats = s_stats;
    taskEXIT_CRITICAL(&s_rtc_lock);
}
