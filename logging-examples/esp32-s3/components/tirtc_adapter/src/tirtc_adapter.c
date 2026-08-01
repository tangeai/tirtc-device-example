#include "tirtc_adapter.h"

#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tiRTC.h"

#define TIRTC_DISCONNECT_QUEUE_DEPTH 8U
#define TIRTC_DISCONNECT_OVERFLOW_SLOTS 4U
#define TIRTC_DISCONNECT_CAPACITY \
    (TIRTC_DISCONNECT_QUEUE_DEPTH + TIRTC_DISCONNECT_OVERFLOW_SLOTS)
#define TIRTC_DISCONNECT_TASK_STACK_BYTES 4096U
#define TIRTC_DISCONNECT_CALLBACK_DRAIN_TICKS 1U
#define TIRTC_DISCONNECT_DRAIN_TIMEOUT_MS 500U
#define TIRTC_DISCONNECT_RETRY_COUNT 5U
#define TIRTC_DISCONNECT_RETRY_DELAY_MS 20U
#define TIRTC_SDK_DEFAULT_ENDPOINT "http://ep-tirtc.tange365.com"
#define TIRTC_SDK_DEFAULT_ENDPOINT_HTTPS_ALIAS "https://ep-tirtc.tange365.com"

#ifndef TIRTC_SDK_STATIC_SEMAPHORE_SIZE
#error "TiRTC SDK build contract did not define StaticSemaphore_t size"
#endif

_Static_assert(sizeof(StaticSemaphore_t) == TIRTC_SDK_STATIC_SEMAPHORE_SIZE,
               "FreeRTOS StaticSemaphore_t does not match the TiRTC SDK build contract");

static const char *TAG = "tirtc_adapter";
static atomic_int s_state = TIRTC_ADAPTER_IDLE;
static atomic_bool s_restart_required;
static bool s_initialized;
static int s_last_start_error;
static const char *s_last_start_stage = "idle";
static atomic_uint_fast32_t s_audio_rx_frames;
static atomic_uint_fast32_t s_video_rx_frames;
static atomic_uint_fast32_t s_audio_rx_bytes;
static atomic_uint_fast32_t s_video_rx_bytes;
static atomic_uint_fast32_t s_audio_tx_frames;
static atomic_uint_fast32_t s_video_tx_frames;
static atomic_uint_fast32_t s_audio_tx_bytes;
static atomic_uint_fast32_t s_video_tx_bytes;
static atomic_uint_fast32_t s_send_errors;
static atomic_uint_fast32_t s_first_tx_ms;
static atomic_uint_fast32_t s_last_tx_ms;
static atomic_uint_fast32_t s_first_rx_ms;
static atomic_uint_fast32_t s_last_rx_ms;
static atomic_int s_measured_profile;
static atomic_uint_fast32_t s_measured_session_generation;
static atomic_uintptr_t s_active_connection;
static portMUX_TYPE s_connection_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_connection_users;
static atomic_uint_fast32_t s_connection_generation;
static atomic_bool s_connection_incoming;
static atomic_uint_fast32_t s_connect_request_generation;
static atomic_uint_fast32_t s_connect_callback_generation;
static atomic_uint_fast32_t s_connect_session_generation;
static uintptr_t s_failed_connect_connection;
static uint32_t s_failed_connect_generation;
static uint32_t s_incoming_session_generation;
static atomic_uint_fast32_t s_connect_submissions;
static atomic_bool s_connect_request_pending;
static atomic_bool s_connect_callback_pending;
static atomic_uint_fast32_t s_accept_callbacks_pending;
static atomic_int s_media_profile;
static atomic_uint_fast32_t s_media_session_generation;
static atomic_bool s_downlink_video_enabled;
static tirtc_adapter_event_handlers_t s_event_handlers;
static QueueHandle_t s_disconnect_queue;
static TaskHandle_t s_disconnect_task;
static atomic_uintptr_t
    s_disconnect_overflow[TIRTC_DISCONNECT_OVERFLOW_SLOTS];
static atomic_uint_fast32_t s_disconnects_pending;
static uintptr_t s_disconnect_owned[TIRTC_DISCONNECT_CAPACITY];

typedef enum {
    DISCONNECT_RESERVATION_NEW = 0,
    DISCONNECT_RESERVATION_ALREADY_OWNED,
    DISCONNECT_RESERVATION_FULL,
} disconnect_reservation_result_t;

static uint32_t monotonic_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void mark_first_timestamp(atomic_uint_fast32_t *destination,
                                 uint32_t timestamp_ms)
{
    uint_fast32_t expected = 0U;
    (void)atomic_compare_exchange_strong_explicit(destination,
                                                  &expected,
                                                  timestamp_ms,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed);
}

static void reset_media_metrics(tirtc_adapter_media_profile_t profile,
                                uint32_t session_generation)
{
    atomic_store_explicit(&s_audio_rx_frames, 0, memory_order_relaxed);
    atomic_store_explicit(&s_video_rx_frames, 0, memory_order_relaxed);
    atomic_store_explicit(&s_audio_rx_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&s_video_rx_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&s_audio_tx_frames, 0, memory_order_relaxed);
    atomic_store_explicit(&s_video_tx_frames, 0, memory_order_relaxed);
    atomic_store_explicit(&s_audio_tx_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&s_video_tx_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&s_send_errors, 0, memory_order_relaxed);
    atomic_store_explicit(&s_first_tx_ms, 0, memory_order_relaxed);
    atomic_store_explicit(&s_last_tx_ms, 0, memory_order_relaxed);
    atomic_store_explicit(&s_first_rx_ms, 0, memory_order_relaxed);
    atomic_store_explicit(&s_last_rx_ms, 0, memory_order_relaxed);
    atomic_store_explicit(&s_measured_profile, profile, memory_order_relaxed);
    atomic_store_explicit(&s_measured_session_generation,
                          session_generation,
                          memory_order_release);
}

static int record_send_error(int error)
{
    if (error != 0) {
        (void)atomic_fetch_add_explicit(&s_send_errors,
                                        1,
                                        memory_order_relaxed);
    }
    return error;
}

static int normalize_send_result(int result)
{
    /* TiRtcSendCommand/AudioStream/VideoStream return the accepted byte count
     * on success. The adapter contract deliberately exposes 0/-error so
     * session and media owners do not mistake a positive byte count for an
     * error code. */
    return result >= 0 ? 0 : result;
}

static bool is_terminal_connection_send_result(int error)
{
    return error == TIRTC_E_INVALID_HANDLE ||
           error == TIRTC_E_CONN_TIMEOUTCLOSE ||
           error == TIRTC_E_CONN_REMOTECLOSE ||
           error == TIRTC_E_CONN_OTHER_ERROR;
}

static int record_media_send_result(
    int result,
    tirtc_conn_t connection,
    tirtc_adapter_media_profile_t profile,
    uint32_t session_generation)
{
    int normalized = normalize_send_result(result);
    if (normalized == 0) {
        return 0;
    }
    /*
     * Connection teardown is reported by the lifecycle callback. Preserve the
     * terminal result for the producer, but do not classify it as a media-path
     * fault when the callback races with the final scheduled send.
     */
    if (is_terminal_connection_send_result(normalized)) {
        return normalized;
    }
    /*
     * A remote close can invalidate the SDK handle while a producer that
     * already acquired it finishes its current scheduling slice. Report the
     * error to that producer, but do not charge it to the now-ended media
     * generation. Errors on the still-current route remain observable.
     */
    bool route_is_current =
        (tirtc_conn_t)atomic_load_explicit(&s_active_connection,
                                           memory_order_acquire) ==
            connection &&
        tirtc_adapter_media_profile() == profile &&
        tirtc_adapter_media_session_generation() == session_generation;
    return route_is_current ? record_send_error(normalized) : normalized;
}

static tirtc_adapter_state_t adapter_state_load(void)
{
    return (tirtc_adapter_state_t)atomic_load_explicit(
        &s_state,
        memory_order_acquire);
}

static void adapter_state_store(tirtc_adapter_state_t state)
{
    atomic_store_explicit(&s_state, state, memory_order_release);
}

/* Every non-NULL SDK handle must have exactly one owner until
 * on_disconnected confirms that the SDK has released it. */
static disconnect_reservation_result_t reserve_disconnect_locked(
    tirtc_conn_t connection)
{
    if (connection == NULL) {
        return DISCONNECT_RESERVATION_FULL;
    }
    for (size_t index = 0; index < TIRTC_DISCONNECT_CAPACITY; ++index) {
        if (s_disconnect_owned[index] == (uintptr_t)connection) {
            return DISCONNECT_RESERVATION_ALREADY_OWNED;
        }
    }
    for (size_t index = 0; index < TIRTC_DISCONNECT_CAPACITY; ++index) {
        if (s_disconnect_owned[index] == 0U) {
            s_disconnect_owned[index] = (uintptr_t)connection;
            (void)atomic_fetch_add_explicit(&s_disconnects_pending,
                                            1,
                                            memory_order_relaxed);
            return DISCONNECT_RESERVATION_NEW;
        }
    }
    return DISCONNECT_RESERVATION_FULL;
}

static bool complete_disconnect_reservation(tirtc_conn_t connection)
{
    bool completed = false;
    portENTER_CRITICAL(&s_connection_lock);
    for (size_t index = 0; index < TIRTC_DISCONNECT_CAPACITY; ++index) {
        if (s_disconnect_owned[index] == (uintptr_t)connection) {
            s_disconnect_owned[index] = 0U;
            (void)atomic_fetch_sub_explicit(&s_disconnects_pending,
                                            1,
                                            memory_order_release);
            completed = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_connection_lock);
    return completed;
}

static uint32_t advance_connection_generation_locked(void)
{
    uint32_t generation =
        (uint32_t)atomic_load_explicit(&s_connection_generation,
                                       memory_order_relaxed) +
        1U;
    if (generation == 0U) {
        generation = 1U;
    }
    atomic_store_explicit(&s_connection_generation,
                          generation,
                          memory_order_release);
    return generation;
}

static void prepare_connection_event_locked(
    tirtc_adapter_connection_event_t *event,
    bool connected,
    bool incoming,
    int error)
{
    uint32_t connection_generation =
        advance_connection_generation_locked();
    if (event == NULL) {
        return;
    }
    *event = (tirtc_adapter_connection_event_t) {
        .connected = connected,
        .incoming = incoming,
        .error = error,
        .session_generation = (uint32_t)atomic_load_explicit(
            &s_media_session_generation, memory_order_relaxed),
        .connection_generation = connection_generation,
    };
}

static bool install_active_connection(
    tirtc_conn_t connection,
    bool incoming,
    tirtc_adapter_connection_event_t *event)
{
    bool installed = false;
    portENTER_CRITICAL(&s_connection_lock);
    uint32_t media_session_generation =
        (uint32_t)atomic_load_explicit(&s_media_session_generation,
                                       memory_order_relaxed);
    if (connection != NULL && event != NULL && s_connection_users == 0U &&
        (!incoming ||
         (!atomic_load_explicit(&s_connect_request_pending,
                                memory_order_relaxed) &&
          !atomic_load_explicit(&s_connect_callback_pending,
                                memory_order_relaxed) &&
          media_session_generation != 0U &&
          s_incoming_session_generation ==
              media_session_generation)) &&
        atomic_load_explicit(&s_disconnects_pending,
                             memory_order_relaxed) == 0U &&
        atomic_load_explicit(&s_active_connection,
                             memory_order_relaxed) == 0U) {
        atomic_store_explicit(&s_active_connection,
                              (uintptr_t)connection,
                              memory_order_release);
        atomic_store_explicit(&s_connection_incoming,
                              incoming,
                              memory_order_relaxed);
        if (incoming) {
            s_incoming_session_generation = 0U;
        }
        prepare_connection_event_locked(event, true, incoming, 0);
        installed = true;
    }
    portEXIT_CRITICAL(&s_connection_lock);
    return installed;
}

static bool detach_expected_connection_transition(
    tirtc_conn_t connection,
    bool reserve_disconnect,
    int error,
    tirtc_adapter_connection_event_t *event,
    bool *cleanup_reserved)
{
    bool detached = false;
    disconnect_reservation_result_t reservation =
        DISCONNECT_RESERVATION_ALREADY_OWNED;
    if (cleanup_reserved != NULL) {
        *cleanup_reserved = false;
    }
    portENTER_CRITICAL(&s_connection_lock);
    if (connection != NULL &&
        atomic_load_explicit(&s_active_connection,
                             memory_order_relaxed) ==
            (uintptr_t)connection) {
        bool incoming = atomic_load_explicit(&s_connection_incoming,
                                             memory_order_relaxed);
        atomic_store_explicit(&s_active_connection, 0, memory_order_release);
        atomic_store_explicit(&s_connection_incoming,
                              false,
                              memory_order_relaxed);
        if (reserve_disconnect) {
            reservation = reserve_disconnect_locked(connection);
        }
        prepare_connection_event_locked(event, false, incoming, error);
        detached = true;
    }
    portEXIT_CRITICAL(&s_connection_lock);
    if (cleanup_reserved != NULL &&
        reservation == DISCONNECT_RESERVATION_NEW) {
        *cleanup_reserved = true;
    }
    if (reservation == DISCONNECT_RESERVATION_FULL) {
        ESP_LOGE(TAG,
                 "disconnect ownership capacity violated while detaching handle=%p",
                 (void *)connection);
        adapter_state_store(TIRTC_ADAPTER_ERROR);
    }
    return detached;
}

static tirtc_conn_t detach_active_connection_for_disconnect(
    tirtc_adapter_connection_event_t *event,
    bool *cleanup_reserved)
{
    disconnect_reservation_result_t reservation =
        DISCONNECT_RESERVATION_ALREADY_OWNED;
    if (cleanup_reserved != NULL) {
        *cleanup_reserved = false;
    }
    portENTER_CRITICAL(&s_connection_lock);
    /* Closing a session revokes its one-shot incoming permit in the same
     * critical section that detaches any connection which won the race. */
    s_incoming_session_generation = 0U;
    tirtc_conn_t connection = (tirtc_conn_t)atomic_load_explicit(
        &s_active_connection,
        memory_order_relaxed);
    if (connection != NULL) {
        bool incoming = atomic_load_explicit(&s_connection_incoming,
                                             memory_order_relaxed);
        atomic_store_explicit(&s_active_connection, 0, memory_order_release);
        atomic_store_explicit(&s_connection_incoming,
                              false,
                              memory_order_relaxed);
        reservation = reserve_disconnect_locked(connection);
        prepare_connection_event_locked(event, false, incoming, 0);
    }
    portEXIT_CRITICAL(&s_connection_lock);
    if (cleanup_reserved != NULL &&
        reservation == DISCONNECT_RESERVATION_NEW) {
        *cleanup_reserved = true;
    }
    if (reservation == DISCONNECT_RESERVATION_FULL) {
        ESP_LOGE(TAG,
                 "disconnect ownership capacity violated for active handle=%p",
                 (void *)connection);
        adapter_state_store(TIRTC_ADAPTER_ERROR);
    }
    return connection;
}

static bool reserve_connect_request(uint32_t session_generation,
                                    uint32_t *request_generation)
{
    bool reserved = false;
    portENTER_CRITICAL(&s_connection_lock);
    if (request_generation != NULL &&
        adapter_state_load() == TIRTC_ADAPTER_RUNNING &&
        session_generation != 0U &&
        session_generation ==
            atomic_load_explicit(&s_media_session_generation,
                                 memory_order_relaxed) &&
        s_connection_users == 0U &&
        atomic_load_explicit(&s_active_connection,
                             memory_order_relaxed) == 0U &&
        atomic_load_explicit(&s_disconnects_pending,
                             memory_order_relaxed) == 0U &&
        !atomic_load_explicit(&s_connect_request_pending,
                              memory_order_relaxed) &&
        !atomic_load_explicit(&s_connect_callback_pending,
                              memory_order_relaxed)) {
        uint32_t generation =
            (uint32_t)atomic_load_explicit(&s_connect_request_generation,
                                           memory_order_relaxed) +
            1U;
        if (generation == 0U) {
            generation = 1U;
        }
        atomic_store_explicit(&s_connect_request_generation,
                              generation,
                              memory_order_relaxed);
        atomic_store_explicit(&s_connect_session_generation,
                              session_generation,
                              memory_order_relaxed);
        atomic_store_explicit(&s_connect_request_pending,
                              true,
                              memory_order_relaxed);
        atomic_store_explicit(&s_connect_callback_generation,
                              generation,
                              memory_order_relaxed);
        atomic_store_explicit(&s_connect_callback_pending,
                              true,
                              memory_order_release);
        *request_generation = generation;
        reserved = true;
    }
    portEXIT_CRITICAL(&s_connection_lock);
    return reserved;
}

static void abort_connect_submission(uint32_t request_generation)
{
    portENTER_CRITICAL(&s_connection_lock);
    if (request_generation ==
        atomic_load_explicit(&s_connect_request_generation,
                             memory_order_relaxed)) {
        atomic_store_explicit(&s_connect_request_pending,
                              false,
                              memory_order_relaxed);
        atomic_store_explicit(&s_connect_session_generation,
                              0,
                              memory_order_relaxed);
    }
    if (request_generation != 0U &&
        request_generation ==
            atomic_load_explicit(&s_connect_callback_generation,
                                 memory_order_relaxed)) {
        atomic_store_explicit(&s_connect_callback_pending,
                              false,
                              memory_order_release);
        atomic_store_explicit(&s_connect_callback_generation,
                              0,
                              memory_order_relaxed);
        if (s_failed_connect_generation == request_generation) {
            s_failed_connect_connection = 0U;
            s_failed_connect_generation = 0U;
        }
    }
    portEXIT_CRITICAL(&s_connection_lock);
}

static tirtc_conn_t acquire_active_connection(void)
{
    portENTER_CRITICAL(&s_connection_lock);
    tirtc_conn_t connection = (tirtc_conn_t)atomic_load_explicit(
        &s_active_connection,
        memory_order_relaxed);
    if (connection != NULL) {
        s_connection_users++;
    }
    portEXIT_CRITICAL(&s_connection_lock);
    return connection;
}

static void release_active_connection(tirtc_conn_t connection)
{
    if (connection == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_connection_lock);
    if (s_connection_users > 0U) {
        s_connection_users--;
    }
    portEXIT_CRITICAL(&s_connection_lock);
}

static uint32_t active_connection_users(void)
{
    portENTER_CRITICAL(&s_connection_lock);
    uint32_t users = s_connection_users;
    portEXIT_CRITICAL(&s_connection_lock);
    return users;
}

static void run_reserved_disconnect(tirtc_conn_t connection,
                                    const char *queue_name)
{
    vTaskDelay(TIRTC_DISCONNECT_CALLBACK_DRAIN_TICKS);
    while (active_connection_users() != 0U) {
        vTaskDelay(1);
    }

    int rc = TIRTC_E_INTERNAL_ERROR;
    for (unsigned attempt = 1U;
         attempt <= TIRTC_DISCONNECT_RETRY_COUNT;
         ++attempt) {
        rc = TiRtcDisconnect(connection);
        if (rc == 0) {
            ESP_LOGI(TAG,
                     "%s disconnect submitted handle=%p rc=%d attempt=%u",
                     queue_name,
                     (void *)connection,
                     rc,
                     attempt);
            /* on_disconnected is the SDK's resource-release completion
             * boundary. Keep this reservation asserted until that callback. */
            return;
        }
        if (rc == TIRTC_E_INVALID_HANDLE) {
            ESP_LOGI(TAG,
                     "%s disconnect already complete handle=%p rc=%d attempt=%u",
                     queue_name,
                     (void *)connection,
                     rc,
                     attempt);
            (void)complete_disconnect_reservation(connection);
            return;
        }
        ESP_LOGW(TAG,
                 "%s disconnect handle=%p rc=%d attempt=%u",
                 queue_name,
                 (void *)connection,
                 rc,
                 attempt);
        vTaskDelay(pdMS_TO_TICKS(TIRTC_DISCONNECT_RETRY_DELAY_MS));
    }

    /* Keep the pending reservation asserted. A handle that the SDK did not
     * release must block all later sessions until an explicit device restart. */
    ESP_LOGE(TAG,
             "%s disconnect handle=%p failed permanently rc=%d",
             queue_name,
             (void *)connection,
             rc);
    adapter_state_store(TIRTC_ADAPTER_ERROR);
}

static void disconnect_worker(void *argument)
{
    (void)argument;
    for (;;) {
        tirtc_conn_t connection = NULL;
        if (xQueueReceive(s_disconnect_queue,
                          &connection,
                          pdMS_TO_TICKS(100)) == pdTRUE &&
            connection != NULL) {
            /* A disconnect can be requested while the session task is handling
             * an event copied from an SDK callback. Keep the SDK call on this
             * worker and one scheduler tick away from the originating callback
             * stack. */
            run_reserved_disconnect(connection, "async");
        }
        for (size_t index = 0;
             index < TIRTC_DISCONNECT_OVERFLOW_SLOTS;
             ++index) {
            connection = (tirtc_conn_t)atomic_exchange_explicit(
                &s_disconnect_overflow[index], 0, memory_order_acq_rel);
            if (connection != NULL) {
                run_reserved_disconnect(connection, "overflow");
            }
        }
    }
}

static int ensure_disconnect_worker(void)
{
    if (s_disconnect_task != NULL) {
        return 0;
    }
    if (s_disconnect_queue == NULL) {
        s_disconnect_queue = xQueueCreate(TIRTC_DISCONNECT_QUEUE_DEPTH,
                                           sizeof(tirtc_conn_t));
    }
    if (s_disconnect_queue == NULL) {
        return TIRTC_E_INTERNAL_ERROR;
    }
    BaseType_t created = xTaskCreate(disconnect_worker,
                                     "tirtc_cleanup",
                                     TIRTC_DISCONNECT_TASK_STACK_BYTES,
                                     NULL,
                                     tskIDLE_PRIORITY + 1,
                                     &s_disconnect_task);
    if (created != pdPASS) {
        vQueueDelete(s_disconnect_queue);
        s_disconnect_queue = NULL;
        return TIRTC_E_INTERNAL_ERROR;
    }
    return 0;
}

static bool queue_reserved_disconnect(tirtc_conn_t connection,
                                      const char *reason)
{
    if (connection == NULL) {
        return false;
    }
    if (s_disconnect_queue != NULL &&
        xQueueSend(s_disconnect_queue, &connection, 0) == pdTRUE) {
        return true;
    }
    for (size_t index = 0;
         index < TIRTC_DISCONNECT_OVERFLOW_SLOTS;
         ++index) {
        uintptr_t expected = 0;
        if (atomic_compare_exchange_strong_explicit(
                &s_disconnect_overflow[index],
                &expected,
                (uintptr_t)connection,
                memory_order_acq_rel,
                memory_order_acquire)) {
            return true;
        }
    }
    /* The configured SDK connection ceiling cannot produce more unique live
     * handles than this fixed backlog can own. Saturation therefore signals a
     * broken SDK/callback contract. Fail closed without blocking an SDK
     * callback or allowing a later session to overlap the unowned handle. */
    ESP_LOGE(TAG,
             "disconnect ownership capacity violated handle=%p reason=%s",
             (void *)connection,
             reason == NULL ? "" : reason);
    adapter_state_store(TIRTC_ADAPTER_ERROR);
    return false;
}

static bool defer_disconnect(tirtc_conn_t connection, const char *reason)
{
    if (connection == NULL) {
        return false;
    }
    disconnect_reservation_result_t reservation;
    portENTER_CRITICAL(&s_connection_lock);
    reservation = reserve_disconnect_locked(connection);
    portEXIT_CRITICAL(&s_connection_lock);
    if (reservation == DISCONNECT_RESERVATION_ALREADY_OWNED) {
        return true;
    }
    if (reservation == DISCONNECT_RESERVATION_FULL) {
        ESP_LOGE(TAG,
                 "disconnect reservation full handle=%p reason=%s",
                 (void *)connection,
                 reason == NULL ? "" : reason);
        adapter_state_store(TIRTC_ADAPTER_ERROR);
        return false;
    }
    return queue_reserved_disconnect(connection, reason);
}

static const char *media_name(uint8_t media)
{
    switch (media) {
    case TIRTC_AUDIO_PCM: return "pcm";
    case TIRTC_AUDIO_ALAW: return "g711a";
    case TIRTC_AUDIO_AAC: return "aac";
    case TIRTC_AUDIO_OPUS: return "opus";
    case TIRTC_AUDIO_AMR: return "amr";
    case TIRTC_VIDEO_JPEG: return "mjpeg";
    case TIRTC_VIDEO_H264: return "h264";
    case TIRTC_VIDEO_H265: return "h265";
    default: return "unknown";
    }
}

static bool profile_streams(tirtc_adapter_media_profile_t profile,
                            uint8_t *audio_stream,
                            uint8_t *video_stream)
{
    switch (profile) {
    case TIRTC_ADAPTER_MEDIA_AI:
        if (audio_stream != NULL) {
            *audio_stream = 1U;
        }
        if (video_stream != NULL) {
            *video_stream = 0U;
        }
        return true;
    case TIRTC_ADAPTER_MEDIA_CALL:
        if (audio_stream != NULL) {
            *audio_stream = 10U;
        }
        if (video_stream != NULL) {
            *video_stream = 11U;
        }
        return true;
    case TIRTC_ADAPTER_MEDIA_NONE:
    default:
        return false;
    }
}

static void emit_connection_event(
    const tirtc_adapter_connection_event_t *event)
{
    if (event == NULL || s_event_handlers.on_connection_changed == NULL) {
        return;
    }
    s_event_handlers.on_connection_changed(event, s_event_handlers.user_data);
}

static void sdk_log(const char *log, uint32_t length)
{
    if (log != NULL && length > 0) {
        ESP_LOGI("TiRTC", "%.*s", (int)length, log);
    }
}

static void on_event(int event, const void *data, int len)
{
    (void)data;
    (void)len;

    switch (event) {
    case TIRTC_EVENT_SYS_STARTED:
        {
            int expected = TIRTC_ADAPTER_STARTING;
            if (atomic_compare_exchange_strong_explicit(
                    &s_state,
                    &expected,
                    TIRTC_ADAPTER_RUNNING,
                    memory_order_acq_rel,
                    memory_order_acquire)) {
                ESP_LOGI(TAG, "SDK started");
            } else {
                ESP_LOGW(TAG,
                         "ignoring late SDK started event while state=%d",
                         expected);
            }
        }
        break;
    case TIRTC_EVENT_SYS_STOPPED:
        adapter_state_store(TIRTC_ADAPTER_STOPPED);
        ESP_LOGI(TAG, "SDK stopped");
        break;
    default:
        ESP_LOGI(TAG, "SDK event=%d", event);
        break;
    }
}

static void on_conn_accepted(tirtc_conn_t connection)
{
    tirtc_adapter_connection_event_t connected_event;
    (void)atomic_fetch_add_explicit(&s_accept_callbacks_pending,
                                    1,
                                    memory_order_acq_rel);
    tirtc_adapter_media_profile_t profile =
        (tirtc_adapter_media_profile_t)atomic_load_explicit(
            &s_media_profile, memory_order_acquire);
    uint32_t session_generation = (uint32_t)atomic_load_explicit(
        &s_media_session_generation, memory_order_acquire);
    if (adapter_state_load() != TIRTC_ADAPTER_RUNNING ||
        atomic_load_explicit(&s_disconnects_pending,
                             memory_order_acquire) != 0U ||
        profile == TIRTC_ADAPTER_MEDIA_NONE || session_generation == 0U) {
        ESP_LOGW(TAG, "rejecting unowned incoming connection");
        defer_disconnect(connection, "unowned-incoming");
        goto done;
    }
    if (!install_active_connection(connection, true, &connected_event)) {
        ESP_LOGW(TAG, "rejecting additional connection while another session is active");
        defer_disconnect(connection, "additional-incoming");
        goto done;
    }
    if (adapter_state_load() != TIRTC_ADAPTER_RUNNING ||
        atomic_load_explicit(&s_disconnects_pending,
                             memory_order_acquire) != 0U ||
        profile != (tirtc_adapter_media_profile_t)atomic_load_explicit(
                       &s_media_profile, memory_order_acquire) ||
        session_generation != atomic_load_explicit(
                                  &s_media_session_generation,
                                  memory_order_acquire)) {
        bool cleanup_reserved = false;
        if (detach_expected_connection_transition(connection,
                                                  true,
                                                  0,
                                                  NULL,
                                                  &cleanup_reserved) &&
            cleanup_reserved) {
            (void)queue_reserved_disconnect(connection,
                                             "incoming-owner-changed");
        }
        goto done;
    }
    ESP_LOGI(TAG, "incoming connection accepted handle=%p", (void *)connection);
    emit_connection_event(&connected_event);

done:
    (void)atomic_fetch_sub_explicit(&s_accept_callbacks_pending,
                                    1,
                                    memory_order_release);
}

static void on_conn_error(tirtc_conn_t connection, int error)
{
    tirtc_adapter_connection_event_t disconnected_event;
    bool cleanup_reserved = false;
    bool emit_pending_failure = false;
    ESP_LOGW(TAG, "connection error=%d (%s)", error, TiRtcGetErrorStr(error));
    if (detach_expected_connection_transition(connection,
                                              true,
                                              error,
                                              &disconnected_event,
                                              &cleanup_reserved)) {
        bool cleanup_queued =
            !cleanup_reserved ||
            queue_reserved_disconnect(connection, "connection-error");
        emit_connection_event(&disconnected_event);
        if (!cleanup_queued) {
            ESP_LOGE(TAG,
                     "connection error handle=%p could not be queued for cleanup",
                     (void *)connection);
        }
    } else if (connection != NULL) {
        disconnect_reservation_result_t reservation;
        bool abandoned_outgoing = false;
        portENTER_CRITICAL(&s_connection_lock);
        reservation = reserve_disconnect_locked(connection);
        if (reservation != DISCONNECT_RESERVATION_FULL &&
            atomic_load_explicit(&s_active_connection,
                                 memory_order_relaxed) == 0U &&
            atomic_load_explicit(&s_connect_request_pending,
                                 memory_order_relaxed) &&
            atomic_load_explicit(&s_connect_callback_pending,
                                 memory_order_relaxed)) {
            uint32_t callback_generation =
                (uint32_t)atomic_load_explicit(
                    &s_connect_callback_generation,
                    memory_order_relaxed);
            atomic_store_explicit(&s_connect_request_pending,
                                  false,
                                  memory_order_relaxed);
            atomic_store_explicit(&s_connect_session_generation,
                                  0,
                                  memory_order_relaxed);
            s_failed_connect_connection = (uintptr_t)connection;
            s_failed_connect_generation = callback_generation;
            prepare_connection_event_locked(&disconnected_event,
                                            false,
                                            false,
                                            error);
            emit_pending_failure = true;
            abandoned_outgoing = true;
        }
        portEXIT_CRITICAL(&s_connection_lock);

        if (reservation == DISCONNECT_RESERVATION_NEW) {
            bool queued =
                queue_reserved_disconnect(connection,
                                           abandoned_outgoing
                                               ? "failed-pending-outgoing"
                                               : "unowned-connection-error");
            ESP_LOGW(TAG,
                     "connection error cleanup handle=%p pending_outgoing=%d queued=%d",
                     (void *)connection,
                     abandoned_outgoing,
                     queued);
        } else if (reservation == DISCONNECT_RESERVATION_ALREADY_OWNED) {
            ESP_LOGI(TAG,
                     "connection error handle=%p cleanup already owned",
                     (void *)connection);
        } else {
            ESP_LOGE(TAG,
                     "connection error handle=%p has no cleanup capacity",
                     (void *)connection);
            adapter_state_store(TIRTC_ADAPTER_ERROR);
        }
        if (emit_pending_failure) {
            emit_connection_event(&disconnected_event);
        }
    }
}

static void on_disconnected(tirtc_conn_t connection)
{
    tirtc_adapter_connection_event_t disconnected_event;
    if (detach_expected_connection_transition(connection,
                                              false,
                                              0,
                                              &disconnected_event,
                                              NULL)) {
        emit_connection_event(&disconnected_event);
    }
    bool cleanup_completed = complete_disconnect_reservation(connection);
    ESP_LOGI(TAG,
             "connection disconnected handle=%p cleanup_completed=%d",
             (void *)connection,
             cleanup_completed);
}

static void on_connect_result(int error, tirtc_conn_t connection, void *user_data)
{
    tirtc_adapter_connection_event_t connection_event;
    uint32_t request_generation = (uint32_t)(uintptr_t)user_data;
    bool emit_event = false;
    bool cleanup_reserved = false;
    bool installed = false;
    bool stale = false;
    bool recycle_after_failed_connect = false;
    bool failed_connection_callback = false;
    disconnect_reservation_result_t reservation =
        DISCONNECT_RESERVATION_ALREADY_OWNED;
    int failure =
        error != 0 ? error
                   : connection == NULL ? TIRTC_E_INVALID_HANDLE : 0;

    portENTER_CRITICAL(&s_connection_lock);
    bool matching_request =
        request_generation != 0U &&
        request_generation ==
            atomic_load_explicit(&s_connect_request_generation,
                                 memory_order_relaxed);
    bool matching_callback =
        request_generation != 0U &&
        atomic_load_explicit(&s_connect_callback_pending,
                             memory_order_relaxed) &&
        request_generation ==
            atomic_load_explicit(&s_connect_callback_generation,
                                 memory_order_relaxed);
    failed_connection_callback =
        matching_callback &&
        request_generation == s_failed_connect_generation &&
        connection != NULL &&
        (uintptr_t)connection == s_failed_connect_connection;
    bool current_request =
        matching_request &&
        adapter_state_load() == TIRTC_ADAPTER_RUNNING &&
        atomic_load_explicit(&s_connect_request_pending,
                             memory_order_relaxed) &&
        atomic_load_explicit(&s_connect_session_generation,
                             memory_order_relaxed) ==
            atomic_load_explicit(&s_media_session_generation,
                                 memory_order_relaxed);

    if (!current_request) {
        stale = true;
        if (matching_request) {
            atomic_store_explicit(&s_connect_request_pending,
                                  false,
                                  memory_order_relaxed);
            atomic_store_explicit(&s_connect_session_generation,
                                  0,
                                  memory_order_relaxed);
        }
        if (connection != NULL && !failed_connection_callback) {
            reservation = reserve_disconnect_locked(connection);
            cleanup_reserved =
                reservation == DISCONNECT_RESERVATION_NEW;
        }
    } else if (failure != 0) {
        atomic_store_explicit(&s_connect_request_pending,
                              false,
                              memory_order_relaxed);
        atomic_store_explicit(&s_connect_session_generation,
                              0,
                              memory_order_relaxed);
        prepare_connection_event_locked(&connection_event,
                                        false,
                                        false,
                                        failure);
        emit_event = true;
        if (connection != NULL) {
            reservation = reserve_disconnect_locked(connection);
            cleanup_reserved =
                reservation == DISCONNECT_RESERVATION_NEW;
        }
    } else if (s_connection_users == 0U &&
               atomic_load_explicit(&s_disconnects_pending,
                                    memory_order_relaxed) == 0U &&
               atomic_load_explicit(&s_active_connection,
                                    memory_order_relaxed) == 0U) {
        atomic_store_explicit(&s_active_connection,
                              (uintptr_t)connection,
                              memory_order_release);
        atomic_store_explicit(&s_connection_incoming,
                              false,
                              memory_order_relaxed);
        prepare_connection_event_locked(&connection_event,
                                        true,
                                        false,
                                        0);
        atomic_store_explicit(&s_connect_request_pending,
                              false,
                              memory_order_relaxed);
        atomic_store_explicit(&s_connect_session_generation,
                              0,
                              memory_order_relaxed);
        emit_event = true;
        installed = true;
    } else {
        atomic_store_explicit(&s_connect_request_pending,
                              false,
                              memory_order_relaxed);
        atomic_store_explicit(&s_connect_session_generation,
                              0,
                              memory_order_relaxed);
        reservation = reserve_disconnect_locked(connection);
        cleanup_reserved = reservation == DISCONNECT_RESERVATION_NEW;
    }
    recycle_after_failed_connect =
        matching_callback && failure != 0 && connection == NULL;
    portEXIT_CRITICAL(&s_connection_lock);

    if (reservation == DISCONNECT_RESERVATION_FULL) {
        ESP_LOGE(TAG,
                 "outgoing connection handle=%p has no cleanup capacity",
                 (void *)connection);
        adapter_state_store(TIRTC_ADAPTER_ERROR);
    }
    if (cleanup_reserved) {
        (void)queue_reserved_disconnect(
            connection,
            stale ? "stale-outgoing"
                  : failure != 0 ? "failed-outgoing"
                                 : "outgoing-conflict");
    }
    if (failure != 0 && !stale) {
        ESP_LOGE(TAG,
                 "outgoing connection failed: %d (%s)",
                 failure,
                 TiRtcGetErrorStr(failure));
    } else if (stale && failure != 0) {
        ESP_LOGI(TAG,
                 "stale outgoing result failed error=%d handle=%p",
                 failure,
                 (void *)connection);
    } else if (stale && connection != NULL) {
        ESP_LOGI(TAG,
                 "closing stale outgoing connection handle=%p",
                 (void *)connection);
    } else if (installed) {
        ESP_LOGI(TAG,
                 "outgoing connection established handle=%p",
                 (void *)connection);
    }
    if (emit_event) {
        emit_connection_event(&connection_event);
    }
    if (recycle_after_failed_connect) {
        /*
         * The SDK reports asynchronous connect failures without a public
         * handle. There is no public resource handle to release, and repeated
         * in-process Stop/Uninit cycles have left transport sockets behind on
         * this SDK build. Require a controlled device restart after the
         * terminal callback so the network stack owns the cleanup boundary.
         */
        ESP_LOGW(TAG,
                 "device restart required after failed connect terminal error=%d",
                 failure);
        atomic_store_explicit(&s_restart_required,
                              true,
                              memory_order_release);
        int expected = TIRTC_ADAPTER_RUNNING;
        (void)atomic_compare_exchange_strong_explicit(
            &s_state,
            &expected,
            TIRTC_ADAPTER_ERROR,
            memory_order_acq_rel,
            memory_order_acquire);
    }
    /*
     * This is the callback's completion boundary. SDK stop/deinit waits for
     * this flag, so publish all state and events before clearing it.
     */
    if (matching_callback) {
        portENTER_CRITICAL(&s_connection_lock);
        if (request_generation ==
                atomic_load_explicit(&s_connect_callback_generation,
                                     memory_order_relaxed) &&
            atomic_load_explicit(&s_connect_callback_pending,
                                 memory_order_relaxed)) {
            atomic_store_explicit(&s_connect_callback_generation,
                                  0,
                                  memory_order_relaxed);
            if (s_failed_connect_generation == request_generation) {
                s_failed_connect_connection = 0U;
                s_failed_connect_generation = 0U;
            }
            atomic_store_explicit(&s_connect_callback_pending,
                                  false,
                                  memory_order_release);
        }
        portEXIT_CRITICAL(&s_connection_lock);
    }
}

static void on_command(tirtc_conn_t connection,
                       uint32_t command,
                       const void *data,
                       uint32_t length)
{
    tirtc_adapter_command_event_t event = {
        .command = command,
        .data = data,
        .length = length,
    };
    bool current = false;
    portENTER_CRITICAL(&s_connection_lock);
    if (atomic_load_explicit(&s_active_connection,
                             memory_order_relaxed) ==
        (uintptr_t)connection) {
        event.session_generation = (uint32_t)atomic_load_explicit(
            &s_media_session_generation, memory_order_relaxed);
        event.connection_generation = (uint32_t)atomic_load_explicit(
            &s_connection_generation, memory_order_relaxed);
        current = true;
    }
    portEXIT_CRITICAL(&s_connection_lock);
    if (!current || s_event_handlers.on_command == NULL) {
        return;
    }
    s_event_handlers.on_command(&event, s_event_handlers.user_data);
}

static void on_request_key_frame(tirtc_conn_t connection, uint8_t stream_id)
{
    (void)connection;
    ESP_LOGI(TAG, "key-frame requested stream=%u", stream_id);
}

static int on_subscribe_video(tirtc_conn_t connection, uint8_t stream_id)
{
    (void)connection;
    uint8_t expected_stream = 0;
    tirtc_adapter_media_profile_t profile =
        (tirtc_adapter_media_profile_t)atomic_load_explicit(
            &s_media_profile, memory_order_acquire);
    bool enabled = atomic_load_explicit(&s_downlink_video_enabled,
                                        memory_order_acquire);
    ESP_LOGI(TAG, "video subscription stream=%u %s",
             stream_id,
             enabled ? "accepted" : "rejected by product/session policy");
    return enabled && profile_streams(profile, NULL, &expected_stream) &&
                   stream_id == expected_stream
               ? 0
               : -1;
}

static int on_subscribe_audio(tirtc_conn_t connection, uint8_t stream_id)
{
    (void)connection;
    uint8_t expected_stream = 0;
    tirtc_adapter_media_profile_t profile =
        (tirtc_adapter_media_profile_t)atomic_load_explicit(
            &s_media_profile, memory_order_acquire);
    ESP_LOGI(TAG, "audio subscribed stream=%u", stream_id);
    return profile_streams(profile, &expected_stream, NULL) &&
                   stream_id == expected_stream
               ? 0
               : -1;
}

static void on_unsubscribe(tirtc_conn_t connection, uint8_t stream_id)
{
    (void)connection;
    ESP_LOGI(TAG, "media unsubscribed stream=%u", stream_id);
}

static void on_audio(tirtc_conn_t connection, const TIRTCFRAMEINFO *frame, void *data)
{
    (void)data;
    if (frame == NULL ||
        connection != (tirtc_conn_t)atomic_load_explicit(
                          &s_active_connection, memory_order_acquire)) {
        return;
    }

    uint32_t count = (uint32_t)atomic_fetch_add_explicit(
                         &s_audio_rx_frames, 1, memory_order_relaxed) + 1U;
    (void)atomic_fetch_add_explicit(&s_audio_rx_bytes,
                                    frame->length,
                                    memory_order_relaxed);
    uint32_t now_ms = monotonic_ms();
    mark_first_timestamp(&s_first_rx_ms, now_ms);
    atomic_store_explicit(&s_last_rx_ms, now_ms, memory_order_relaxed);
    if (count <= 3U || count % 500U == 0U) {
        ESP_LOGI(TAG,
                 "RX audio #%lu stream=%u codec=%s len=%lu ts=%lu; no speaker sink, dropped",
                 (unsigned long)count,
                 frame->stream_id,
                 media_name(frame->media),
                 (unsigned long)frame->length,
                 (unsigned long)frame->ts);
    }

    /* This protocol/log example intentionally has no speaker sink. A product
     * port replaces this drop point with a non-blocking decoder queue. */
}

static void on_video(tirtc_conn_t connection, const TIRTCFRAMEINFO *frame, void *data)
{
    (void)data;
    if (frame == NULL ||
        connection != (tirtc_conn_t)atomic_load_explicit(
                          &s_active_connection, memory_order_acquire)) {
        return;
    }

    uint32_t count = (uint32_t)atomic_fetch_add_explicit(
                         &s_video_rx_frames, 1, memory_order_relaxed) + 1U;
    (void)atomic_fetch_add_explicit(&s_video_rx_bytes,
                                    frame->length,
                                    memory_order_relaxed);
    uint32_t now_ms = monotonic_ms();
    mark_first_timestamp(&s_first_rx_ms, now_ms);
    atomic_store_explicit(&s_last_rx_ms, now_ms, memory_order_relaxed);
    if (count <= 3U || count % 100U == 0U) {
        ESP_LOGI(TAG,
                 "RX video #%lu stream=%u codec=%s len=%lu ts=%lu key=%d; no display sink, dropped",
                 (unsigned long)count,
                 frame->stream_id,
                 media_name(frame->media),
                 (unsigned long)frame->length,
                 (unsigned long)frame->ts,
                 (frame->flags & TIRTC_FRAME_FLAG_KEY_FRAME) != 0);
    }

    /* This protocol/log example intentionally has no display sink. A product
     * port replaces this drop point with a non-blocking decoder queue. */
}

/* The callback table must outlive TiRtcStart/TiRtcStop. */
static const TIRTCCALLBACKS s_callbacks = {
    .on_event = on_event,
    .on_conn_accepted = on_conn_accepted,
    .on_conn_error = on_conn_error,
    .on_disconnected = on_disconnected,
    .on_audio = on_audio,
    .on_video = on_video,
    .on_command = on_command,
    .on_request_key_frame = on_request_key_frame,
    .on_subscribe_video = on_subscribe_video,
    .on_unsubscribe_video = on_unsubscribe,
    .on_subscribe_audio = on_subscribe_audio,
    .on_unsubscribe_audio = on_unsubscribe,
};

static int set_string_option(TIRTCOPTION option, const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    return TiRtcSetOption(option, value, (uint32_t)strlen(value));
}

const char *tirtc_adapter_version(void)
{
    return TiRtcGetVersion();
}

const char *tirtc_adapter_build_info(void)
{
    return TiRtcGetBuildInfo();
}

tirtc_adapter_state_t tirtc_adapter_state(void)
{
    return adapter_state_load();
}

int tirtc_adapter_last_start_error(void)
{
    return s_last_start_error;
}

const char *tirtc_adapter_last_start_stage(void)
{
    return s_last_start_stage;
}

bool tirtc_adapter_restart_required(void)
{
    return atomic_load_explicit(&s_restart_required, memory_order_acquire);
}

int tirtc_adapter_start(const tirtc_adapter_config_t *config)
{
    s_last_start_stage = "validate";
    s_last_start_error = 0;
    if (config == NULL || config->device_id == NULL || config->device_id[0] == '\0' ||
        config->device_secret == NULL || config->device_secret[0] == '\0' ||
        config->max_connections <= 0 ||
        config->max_connections > (int)TIRTC_DISCONNECT_CAPACITY) {
        s_last_start_error = TIRTC_E_INVALID_PARAMETER;
        return TIRTC_E_INVALID_PARAMETER;
    }
    if (adapter_state_load() != TIRTC_ADAPTER_IDLE) {
        s_last_start_stage = "state";
        s_last_start_error = TIRTC_E_BUSY;
        return TIRTC_E_BUSY;
    }
    s_last_start_stage = "disconnect-worker";
    int rc = ensure_disconnect_worker();
    if (rc != 0) {
        adapter_state_store(TIRTC_ADAPTER_ERROR);
        s_last_start_error = rc;
        return rc;
    }
    portENTER_CRITICAL(&s_connection_lock);
    atomic_store_explicit(&s_active_connection, 0, memory_order_release);
    atomic_store_explicit(&s_connection_incoming, false, memory_order_relaxed);
    s_incoming_session_generation = 0U;
    s_connection_users = 0U;
    s_failed_connect_connection = 0U;
    s_failed_connect_generation = 0U;
    portEXIT_CRITICAL(&s_connection_lock);
    atomic_store_explicit(&s_connect_submissions, 0, memory_order_release);
    atomic_store_explicit(&s_connect_request_pending, false, memory_order_release);
    atomic_store_explicit(&s_connect_callback_pending, false, memory_order_release);
    atomic_store_explicit(&s_connect_callback_generation, 0, memory_order_release);
    atomic_store_explicit(&s_accept_callbacks_pending, 0, memory_order_release);
    atomic_store_explicit(&s_connect_session_generation, 0, memory_order_release);
    atomic_store_explicit(&s_media_profile,
                          TIRTC_ADAPTER_MEDIA_NONE,
                          memory_order_release);
    atomic_store_explicit(&s_media_session_generation, 0, memory_order_release);
    atomic_store_explicit(&s_downlink_video_enabled, false, memory_order_release);
    (void)atomic_fetch_add_explicit(
        &s_connect_request_generation, 1, memory_order_release);

    TiRtcLogSetCallback(sdk_log);
    TiRtcLogSetLevel(config->log_level > 0 ? config->log_level : 3);

    if (config->max_send_buffer_bytes > 0) {
        s_last_start_stage = "max-send-buffer";
        rc = TiRtcSetOption(TIRTC_OPT_MAX_SEND_BUFFER,
                            &config->max_send_buffer_bytes,
                            sizeof(config->max_send_buffer_bytes));
        if (rc != 0) {
            goto fail;
        }
    }

    s_last_start_stage = "init";
    rc = TiRtcInit();
    if (rc != 0) {
        goto fail;
    }
    s_initialized = true;

    if (config->max_connections > 0) {
        s_last_start_stage = "max-connections";
        rc = TiRtcSetOption(TIRTC_OPT_MAX_CONNECTIONS,
                            &config->max_connections,
                            sizeof(config->max_connections));
        if (rc != 0) {
            goto fail;
        }
    }
    int network_type = TIRTC_NETCONN_WIFI;
    s_last_start_stage = "network-type";
    rc = TiRtcSetOption(TIRTC_OPT_NETWORK_TYPE,
                        &network_type,
                        sizeof(network_type));
    if (rc != 0) {
        goto fail;
    }
    s_last_start_stage = "device-secret";
    rc = set_string_option(TIRTC_OPT_DEVICE_SECRET_KEY, config->device_secret);
    if (rc != 0) {
        goto fail;
    }
    s_last_start_stage = "client-id";
    rc = set_string_option(TIRTC_OPT_CLIENT_ID, config->client_id);
    if (rc != 0) {
        goto fail;
    }
    /* The platform advertises an HTTPS alias for the SDK's compiled HTTP endpoint. */
    if (config->service_endpoint != NULL &&
        config->service_endpoint[0] != '\0' &&
        strcmp(config->service_endpoint, TIRTC_SDK_DEFAULT_ENDPOINT) != 0 &&
        strcmp(config->service_endpoint,
               TIRTC_SDK_DEFAULT_ENDPOINT_HTTPS_ALIAS) != 0) {
        s_last_start_stage = "service-endpoint";
        rc = set_string_option(TIRTC_OPT_SERVICE_ENDPOINT,
                               config->service_endpoint);
        if (rc != 0) {
            goto fail;
        }
    }

    adapter_state_store(TIRTC_ADAPTER_STARTING);
    atomic_store_explicit(&s_restart_required, false, memory_order_release);
    s_last_start_stage = "start";
    rc = TiRtcStart(config->device_id, &s_callbacks);
    if (rc != 0) {
        goto fail;
    }

    ESP_LOGI(TAG, "TiRTC start submitted for device_id=%s", config->device_id);
    s_last_start_stage = "submitted";
    s_last_start_error = 0;
    return 0;

fail:
    if (s_initialized) {
        TiRtcUninit();
        s_initialized = false;
    }
    adapter_state_store(TIRTC_ADAPTER_ERROR);
    s_last_start_error = rc;
    ESP_LOGE(TAG, "TiRTC start failed: %d (%s)", rc, TiRtcGetErrorStr(rc));
    return rc;
}

int tirtc_adapter_request_stop(void)
{
    tirtc_adapter_state_t previous_state = TIRTC_ADAPTER_STARTING;
    int expected = TIRTC_ADAPTER_STARTING;
    if (!atomic_compare_exchange_strong_explicit(
            &s_state,
            &expected,
            TIRTC_ADAPTER_STOPPING,
            memory_order_acq_rel,
            memory_order_acquire)) {
        expected = TIRTC_ADAPTER_RUNNING;
        if (!atomic_compare_exchange_strong_explicit(
                &s_state,
                &expected,
                TIRTC_ADAPTER_STOPPING,
                memory_order_acq_rel,
                memory_order_acquire)) {
            expected = TIRTC_ADAPTER_ERROR;
            if (!atomic_compare_exchange_strong_explicit(
                    &s_state,
                    &expected,
                    TIRTC_ADAPTER_STOPPING,
                    memory_order_acq_rel,
                    memory_order_acquire)) {
                return TIRTC_E_NOT_INITIALIZED;
            }
            previous_state = TIRTC_ADAPTER_ERROR;
        } else {
            previous_state = TIRTC_ADAPTER_RUNNING;
        }
    }
    TickType_t submission_started = xTaskGetTickCount();
    while (atomic_load_explicit(&s_connect_submissions,
                                memory_order_acquire) != 0U &&
           xTaskGetTickCount() - submission_started <
               pdMS_TO_TICKS(TIRTC_DISCONNECT_DRAIN_TIMEOUT_MS)) {
        vTaskDelay(1);
    }
    if (atomic_load_explicit(&s_connect_submissions,
                             memory_order_acquire) != 0U) {
        ESP_LOGW(TAG, "connection submission did not return before SDK stop");
        int stopping = TIRTC_ADAPTER_STOPPING;
        (void)atomic_compare_exchange_strong_explicit(
            &s_state,
            &stopping,
            previous_state,
            memory_order_acq_rel,
            memory_order_acquire);
        return TIRTC_E_BUSY;
    }
    (void)tirtc_adapter_disconnect();
    TickType_t drain_started = xTaskGetTickCount();
    while ((atomic_load_explicit(&s_connect_submissions,
                                 memory_order_acquire) != 0U ||
            atomic_load_explicit(&s_connect_callback_pending,
                                 memory_order_acquire) ||
            atomic_load_explicit(&s_accept_callbacks_pending,
                                 memory_order_acquire) != 0U ||
            atomic_load_explicit(&s_disconnects_pending,
                                 memory_order_acquire) != 0U ||
            active_connection_users() != 0U) &&
           xTaskGetTickCount() - drain_started <
               pdMS_TO_TICKS(TIRTC_DISCONNECT_DRAIN_TIMEOUT_MS)) {
        vTaskDelay(1);
    }
    if (atomic_load_explicit(&s_connect_submissions,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&s_connect_callback_pending,
                             memory_order_acquire) ||
        atomic_load_explicit(&s_accept_callbacks_pending,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&s_disconnects_pending,
                             memory_order_acquire) != 0U ||
        active_connection_users() != 0U) {
        ESP_LOGW(TAG, "connection cleanup did not drain before SDK stop");
        int stopping = TIRTC_ADAPTER_STOPPING;
        (void)atomic_compare_exchange_strong_explicit(
            &s_state,
            &stopping,
            previous_state,
            memory_order_acq_rel,
            memory_order_acquire);
        return TIRTC_E_BUSY;
    }
    int rc = TiRtcStop();
    if (rc != 0) {
        int stopping = TIRTC_ADAPTER_STOPPING;
        (void)atomic_compare_exchange_strong_explicit(
            &s_state,
            &stopping,
            previous_state,
            memory_order_acq_rel,
            memory_order_acquire);
    }
    return rc;
}

int tirtc_adapter_deinit(void)
{
    tirtc_adapter_state_t state = adapter_state_load();
    if (state != TIRTC_ADAPTER_STOPPED && state != TIRTC_ADAPTER_ERROR) {
        return TIRTC_E_BUSY;
    }
    if (atomic_load_explicit(&s_disconnects_pending,
                             memory_order_acquire) != 0U) {
        return TIRTC_E_BUSY;
    }
    if (atomic_load_explicit(&s_connect_callback_pending,
                             memory_order_acquire)) {
        return TIRTC_E_BUSY;
    }
    if (atomic_load_explicit(&s_connect_submissions,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&s_accept_callbacks_pending,
                             memory_order_acquire) != 0U ||
        active_connection_users() != 0U ||
        tirtc_adapter_has_connection()) {
        return TIRTC_E_BUSY;
    }
    if (state == TIRTC_ADAPTER_ERROR && s_initialized) {
        return TIRTC_E_BUSY;
    }
    if (s_initialized) {
        TiRtcUninit();
        s_initialized = false;
    }
    adapter_state_store(TIRTC_ADAPTER_IDLE);
    return 0;
}

bool tirtc_adapter_has_connection(void)
{
    return atomic_load_explicit(&s_active_connection, memory_order_acquire) != 0;
}

uint32_t tirtc_adapter_connection_generation(void)
{
    return (uint32_t)atomic_load_explicit(&s_connection_generation, memory_order_acquire);
}

int tirtc_adapter_set_media_profile(tirtc_adapter_media_profile_t profile,
                                    uint32_t session_generation,
                                    bool downlink_video_enabled)
{
    if ((profile == TIRTC_ADAPTER_MEDIA_NONE && session_generation != 0U) ||
        (profile != TIRTC_ADAPTER_MEDIA_NONE && session_generation == 0U) ||
        (profile != TIRTC_ADAPTER_MEDIA_NONE &&
         profile != TIRTC_ADAPTER_MEDIA_AI &&
         profile != TIRTC_ADAPTER_MEDIA_CALL)) {
        return TIRTC_E_INVALID_PARAMETER;
    }
    if (profile != TIRTC_ADAPTER_MEDIA_NONE &&
        (adapter_state_load() != TIRTC_ADAPTER_RUNNING ||
         tirtc_adapter_has_connection() ||
         atomic_load_explicit(&s_connect_request_pending, memory_order_acquire) ||
         atomic_load_explicit(&s_connect_callback_pending,
                              memory_order_acquire) ||
         atomic_load_explicit(&s_connect_submissions,
                              memory_order_acquire) != 0U ||
         atomic_load_explicit(&s_disconnects_pending,
                              memory_order_acquire) != 0U)) {
        return TIRTC_E_BUSY;
    }
    portENTER_CRITICAL(&s_connection_lock);
    s_incoming_session_generation = 0U;
    portEXIT_CRITICAL(&s_connection_lock);
    atomic_store_explicit(&s_downlink_video_enabled,
                          downlink_video_enabled,
                          memory_order_release);
    atomic_store_explicit(&s_media_session_generation,
                          session_generation,
                          memory_order_release);
    atomic_store_explicit(&s_media_profile, profile, memory_order_release);
    if (profile != TIRTC_ADAPTER_MEDIA_NONE) {
        reset_media_metrics(profile, session_generation);
    }
    return 0;
}

int tirtc_adapter_expect_incoming(uint32_t session_generation)
{
    int rc = TIRTC_E_BUSY;
    portENTER_CRITICAL(&s_connection_lock);
    if (adapter_state_load() == TIRTC_ADAPTER_RUNNING &&
        session_generation != 0U &&
        session_generation ==
            atomic_load_explicit(&s_media_session_generation,
                                 memory_order_relaxed) &&
        atomic_load_explicit(&s_media_profile,
                             memory_order_relaxed) ==
            TIRTC_ADAPTER_MEDIA_CALL &&
        s_connection_users == 0U &&
        atomic_load_explicit(&s_active_connection,
                             memory_order_relaxed) == 0U &&
        atomic_load_explicit(&s_disconnects_pending,
                             memory_order_relaxed) == 0U &&
        !atomic_load_explicit(&s_connect_request_pending,
                              memory_order_relaxed) &&
        !atomic_load_explicit(&s_connect_callback_pending,
                              memory_order_relaxed)) {
        s_incoming_session_generation = session_generation;
        rc = 0;
    }
    portEXIT_CRITICAL(&s_connection_lock);
    return rc;
}

tirtc_adapter_media_profile_t tirtc_adapter_media_profile(void)
{
    return (tirtc_adapter_media_profile_t)atomic_load_explicit(
        &s_media_profile, memory_order_acquire);
}

uint32_t tirtc_adapter_media_session_generation(void)
{
    return (uint32_t)atomic_load_explicit(
        &s_media_session_generation, memory_order_acquire);
}

void tirtc_adapter_get_metrics(tirtc_adapter_metrics_t *metrics)
{
    if (metrics == NULL) {
        return;
    }
    uint32_t connection_users = 0U;
    bool incoming_armed = false;
    portENTER_CRITICAL(&s_connection_lock);
    connection_users = s_connection_users;
    incoming_armed = s_incoming_session_generation != 0U;
    portEXIT_CRITICAL(&s_connection_lock);
    *metrics = (tirtc_adapter_metrics_t) {
        .connected = tirtc_adapter_has_connection(),
        .adapter_state = adapter_state_load(),
        .active_profile = tirtc_adapter_media_profile(),
        .measured_profile =
            (tirtc_adapter_media_profile_t)atomic_load_explicit(
                &s_measured_profile, memory_order_acquire),
        .active_session_generation =
            tirtc_adapter_media_session_generation(),
        .measured_session_generation =
            (uint32_t)atomic_load_explicit(
                &s_measured_session_generation, memory_order_acquire),
        .connection_generation = tirtc_adapter_connection_generation(),
        .tx_audio_frames =
            (uint32_t)atomic_load_explicit(
                &s_audio_tx_frames, memory_order_relaxed),
        .tx_audio_bytes =
            (uint32_t)atomic_load_explicit(
                &s_audio_tx_bytes, memory_order_relaxed),
        .tx_video_frames =
            (uint32_t)atomic_load_explicit(
                &s_video_tx_frames, memory_order_relaxed),
        .tx_video_bytes =
            (uint32_t)atomic_load_explicit(
                &s_video_tx_bytes, memory_order_relaxed),
        .rx_audio_frames =
            (uint32_t)atomic_load_explicit(
                &s_audio_rx_frames, memory_order_relaxed),
        .rx_audio_bytes =
            (uint32_t)atomic_load_explicit(
                &s_audio_rx_bytes, memory_order_relaxed),
        .rx_video_frames =
            (uint32_t)atomic_load_explicit(
                &s_video_rx_frames, memory_order_relaxed),
        .rx_video_bytes =
            (uint32_t)atomic_load_explicit(
                &s_video_rx_bytes, memory_order_relaxed),
        .send_errors =
            (uint32_t)atomic_load_explicit(
                &s_send_errors, memory_order_relaxed),
        .first_tx_ms =
            (uint32_t)atomic_load_explicit(
                &s_first_tx_ms, memory_order_relaxed),
        .last_tx_ms =
            (uint32_t)atomic_load_explicit(
                &s_last_tx_ms, memory_order_relaxed),
        .first_rx_ms =
            (uint32_t)atomic_load_explicit(
                &s_first_rx_ms, memory_order_relaxed),
        .last_rx_ms =
            (uint32_t)atomic_load_explicit(
                &s_last_rx_ms, memory_order_relaxed),
        .connect_request_pending =
            atomic_load_explicit(&s_connect_request_pending,
                                 memory_order_acquire),
        .connect_callback_pending =
            atomic_load_explicit(&s_connect_callback_pending,
                                 memory_order_acquire),
        .accept_callbacks_pending =
            (uint32_t)atomic_load_explicit(
                &s_accept_callbacks_pending, memory_order_acquire),
        .disconnects_pending =
            (uint32_t)atomic_load_explicit(
                &s_disconnects_pending, memory_order_acquire),
        .connection_users = connection_users,
        .incoming_armed = incoming_armed,
    };
}

int tirtc_adapter_connect(const char *remote_id,
                          const char *token,
                          uint32_t session_generation)
{
    (void)atomic_fetch_add_explicit(&s_connect_submissions,
                                    1,
                                    memory_order_acq_rel);
    int rc = TIRTC_E_INVALID_PARAMETER;
    if (adapter_state_load() != TIRTC_ADAPTER_RUNNING || remote_id == NULL ||
        remote_id[0] == '\0' || token == NULL || token[0] == '\0' ||
        session_generation == 0U ||
        session_generation != tirtc_adapter_media_session_generation()) {
        goto done;
    }
    uint32_t request_generation = 0U;
    if (!reserve_connect_request(session_generation,
                                 &request_generation)) {
        rc = TIRTC_E_BUSY;
        goto done;
    }
    rc = TiRtcConnect(remote_id,
                      token,
                      on_connect_result,
                      (void *)(uintptr_t)request_generation);
    if (rc != 0) {
        abort_connect_submission(request_generation);
    }

done:
    (void)atomic_fetch_sub_explicit(&s_connect_submissions,
                                    1,
                                    memory_order_release);
    return rc;
}

int tirtc_adapter_disconnect(void)
{
    tirtc_adapter_connection_event_t disconnected_event;
    bool cleanup_reserved = false;
    (void)tirtc_adapter_cancel_connect();
    tirtc_conn_t connection =
        detach_active_connection_for_disconnect(&disconnected_event,
                                                &cleanup_reserved);
    if (connection == NULL) {
        return TIRTC_E_INVALID_HANDLE;
    }
    emit_connection_event(&disconnected_event);
    return (!cleanup_reserved ||
            queue_reserved_disconnect(connection, "application-request"))
               ? 0
               : TIRTC_E_LACK_OF_RESOURCE;
}

int tirtc_adapter_cancel_connect(void)
{
    portENTER_CRITICAL(&s_connection_lock);
    bool pending = atomic_load_explicit(&s_connect_request_pending,
                                        memory_order_relaxed);
    atomic_store_explicit(&s_connect_request_pending,
                          false,
                          memory_order_relaxed);
    atomic_store_explicit(&s_connect_session_generation,
                          0,
                          memory_order_relaxed);
    uint32_t generation =
        (uint32_t)atomic_load_explicit(&s_connect_request_generation,
                                       memory_order_relaxed) +
        1U;
    if (generation == 0U) {
        generation = 1U;
    }
    atomic_store_explicit(&s_connect_request_generation,
                          generation,
                          memory_order_release);
    portEXIT_CRITICAL(&s_connection_lock);
    return pending ? 0 : TIRTC_E_INVALID_HANDLE;
}

int tirtc_adapter_whip_connect(const char *service_description,
                               const char *token,
                               uint32_t session_generation)
{
    (void)atomic_fetch_add_explicit(&s_connect_submissions,
                                    1,
                                    memory_order_acq_rel);
    int rc = TIRTC_E_INVALID_PARAMETER;
    if (adapter_state_load() != TIRTC_ADAPTER_RUNNING ||
        service_description == NULL ||
        service_description[0] == '\0' || token == NULL || token[0] == '\0' ||
        session_generation == 0U ||
        session_generation != tirtc_adapter_media_session_generation()) {
        goto done;
    }
    uint32_t request_generation = 0U;
    if (!reserve_connect_request(session_generation,
                                 &request_generation)) {
        rc = TIRTC_E_BUSY;
        goto done;
    }
    rc = TiRtcWhipConnect(service_description,
                          token,
                          on_connect_result,
                          (void *)(uintptr_t)request_generation);
    if (rc != 0) {
        abort_connect_submission(request_generation);
    }

done:
    (void)atomic_fetch_sub_explicit(&s_connect_submissions,
                                    1,
                                    memory_order_release);
    return rc;
}

int tirtc_adapter_send_command(uint32_t session_generation,
                               uint32_t command,
                               const void *data,
                               uint32_t length)
{
    if (session_generation == 0U ||
        session_generation != tirtc_adapter_media_session_generation()) {
        return TIRTC_E_INVALID_HANDLE;
    }
    tirtc_conn_t connection = acquire_active_connection();
    if (connection == NULL) {
        return TIRTC_E_INVALID_HANDLE;
    }
    int rc = normalize_send_result(
        TiRtcSendCommand(connection, command, data, length));
    release_active_connection(connection);
    return rc;
}

int tirtc_adapter_get_send_buffer_used(uint32_t session_generation,
                                       size_t *used_bytes)
{
    if (used_bytes == NULL || session_generation == 0U ||
        session_generation != tirtc_adapter_media_session_generation()) {
        return TIRTC_E_INVALID_PARAMETER;
    }
    tirtc_conn_t connection = acquire_active_connection();
    if (connection == NULL) {
        return TIRTC_E_INVALID_HANDLE;
    }
    *used_bytes = TiRtcGetSendBufferUsed(connection);
    release_active_connection(connection);
    return 0;
}

int tirtc_adapter_subscribe_downlink(uint32_t session_generation,
                                     bool audio,
                                     bool video)
{
    tirtc_adapter_media_profile_t profile = tirtc_adapter_media_profile();
    if ((!audio && !video) || session_generation == 0U ||
        session_generation != tirtc_adapter_media_session_generation()) {
        return TIRTC_E_INVALID_PARAMETER;
    }
    if (video &&
        !atomic_load_explicit(&s_downlink_video_enabled, memory_order_acquire)) {
        return TIRTC_E_INVALID_PARAMETER;
    }
    uint8_t audio_stream = 0;
    uint8_t video_stream = 0;
    if (!profile_streams(profile, &audio_stream, &video_stream)) {
        return TIRTC_E_INVALID_HANDLE;
    }
    tirtc_conn_t connection = acquire_active_connection();
    if (connection == NULL) {
        return TIRTC_E_INVALID_HANDLE;
    }

    int rc = 0;
    if (audio) {
        rc = TiRtcSubscribeAudio(connection, audio_stream);
        if (rc < 0) {
            release_active_connection(connection);
            return rc;
        }
    }
    if (video) {
        rc = TiRtcSubscribeVideo(connection, video_stream);
        if (rc < 0 && audio) {
            (void)TiRtcUnsubscribeAudio(connection, audio_stream);
        }
    }
    if (rc >= 0) {
        ESP_LOGI(TAG,
                 "downlink subscribed profile=%d audio=%d video=%d streams=%u/%u",
                 (int)profile,
                 audio,
                 video,
                 audio_stream,
                 video_stream);
    }
    release_active_connection(connection);
    return normalize_send_result(rc);
}

int tirtc_adapter_service_request(const char *path,
                                  const char *json_body,
                                  const char *token,
                                  tirtc_adapter_service_callback_t callback,
                                  void *user_data)
{
    if (adapter_state_load() != TIRTC_ADAPTER_RUNNING ||
        path == NULL || path[0] == '\0') {
        return TIRTC_E_NOT_INITIALIZED;
    }
    return TiRtcServiceRequest(path,
                               json_body,
                               token,
                               (TIRTCSERVICEREQUESTCALLBACK)callback,
                               user_data);
}

void tirtc_adapter_set_event_handlers(const tirtc_adapter_event_handlers_t *handlers)
{
    if (handlers == NULL) {
        memset(&s_event_handlers, 0, sizeof(s_event_handlers));
    } else {
        s_event_handlers = *handlers;
    }
}

static uint8_t audio_media(device_audio_codec_t codec)
{
    switch (codec) {
    case DEVICE_AUDIO_CODEC_G711A: return TIRTC_AUDIO_ALAW;
    case DEVICE_AUDIO_CODEC_AMR_NB:
    case DEVICE_AUDIO_CODEC_AMR_WB: return TIRTC_AUDIO_AMR;
    case DEVICE_AUDIO_CODEC_OPUS: return TIRTC_AUDIO_OPUS;
    default: return 0;
    }
}

int tirtc_adapter_send_audio(tirtc_adapter_media_profile_t profile,
                             uint32_t session_generation,
                             const device_audio_config_t *config,
                             uint32_t timestamp_ms,
                             const void *data,
                             uint32_t length)
{
    uint8_t stream_id = 0;
    if (profile != tirtc_adapter_media_profile() ||
        session_generation == 0U ||
        session_generation != tirtc_adapter_media_session_generation() ||
        !profile_streams(profile, &stream_id, NULL)) {
        /* A producer can observe the old generation for one scheduling slice
         * while an intentional session teardown clears adapter ownership. */
        return TIRTC_E_INVALID_HANDLE;
    }
    if (config == NULL || data == NULL || length == 0) {
        return record_send_error(TIRTC_E_INVALID_PARAMETER);
    }

    uint8_t media = audio_media(config->codec);
    if (media == 0 || config->channels != 1 ||
        (config->sample_rate_hz != 8000 && config->sample_rate_hz != 16000)) {
        return record_send_error(TIRTC_E_INVALID_PARAMETER);
    }
    tirtc_conn_t connection = acquire_active_connection();
    if (connection == NULL) {
        return TIRTC_E_INVALID_HANDLE;
    }

    TIRTCFRAMEINFO frame = {
        .stream_id = stream_id,
        .media = media,
        .flags = config->sample_rate_hz == 8000
                     ? TIRTC_AUDIOSAMPLE_8K16B1C
                     : TIRTC_AUDIOSAMPLE_16K16B1C,
        .reserved = 0,
        .ts = timestamp_ms,
        .length = length,
    };
    int result = TiRtcSendAudioStream(connection, &frame, data);
    release_active_connection(connection);
    if (result >= 0) {
        (void)atomic_fetch_add_explicit(&s_audio_tx_frames,
                                        1,
                                        memory_order_relaxed);
        (void)atomic_fetch_add_explicit(&s_audio_tx_bytes,
                                        length,
                                        memory_order_relaxed);
        uint32_t now_ms = monotonic_ms();
        mark_first_timestamp(&s_first_tx_ms, now_ms);
        atomic_store_explicit(&s_last_tx_ms, now_ms, memory_order_relaxed);
    }
    return record_media_send_result(result,
                                    connection,
                                    profile,
                                    session_generation);
}

int tirtc_adapter_send_video(tirtc_adapter_media_profile_t profile,
                             uint32_t session_generation,
                             const device_video_config_t *config,
                             uint32_t timestamp_ms,
                             bool key_frame,
                             const void *data,
                             uint32_t length)
{
    uint8_t stream_id = 0;
    if (profile != tirtc_adapter_media_profile() ||
        session_generation == 0U ||
        session_generation != tirtc_adapter_media_session_generation() ||
        !profile_streams(profile, NULL, &stream_id)) {
        return TIRTC_E_INVALID_HANDLE;
    }
    if (config == NULL || data == NULL || length == 0) {
        return record_send_error(TIRTC_E_INVALID_PARAMETER);
    }

    uint8_t media;
    switch (config->codec) {
    case DEVICE_VIDEO_CODEC_MJPEG: media = TIRTC_VIDEO_JPEG; break;
    case DEVICE_VIDEO_CODEC_H264: media = TIRTC_VIDEO_H264; break;
    default: return record_send_error(TIRTC_E_INVALID_PARAMETER);
    }
    tirtc_conn_t connection = acquire_active_connection();
    if (connection == NULL) {
        return TIRTC_E_INVALID_HANDLE;
    }

    TIRTCFRAMEINFO frame = {
        .stream_id = stream_id,
        .media = media,
        .flags = key_frame ? TIRTC_FRAME_FLAG_KEY_FRAME : 0,
        .reserved = 0,
        .ts = timestamp_ms,
        .length = length,
    };
    int result = TiRtcSendVideoStream(connection, &frame, data);
    release_active_connection(connection);
    if (result >= 0) {
        (void)atomic_fetch_add_explicit(&s_video_tx_frames,
                                        1,
                                        memory_order_relaxed);
        (void)atomic_fetch_add_explicit(&s_video_tx_bytes,
                                        length,
                                        memory_order_relaxed);
        uint32_t now_ms = monotonic_ms();
        mark_first_timestamp(&s_first_tx_ms, now_ms);
        atomic_store_explicit(&s_last_tx_ms, now_ms, memory_order_relaxed);
    }
    return record_media_send_result(result,
                                    connection,
                                    profile,
                                    session_generation);
}
