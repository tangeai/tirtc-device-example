#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <driver/systick.h>
#include <os/mutex.h>
#include <os/thread.h>
#include <spinlock.h>

#include "tiRTC.h"
#include "tirtc_link.h"
#include "tirtc_link_config.h"
#include "tirtc_link_defaults.h"
#include "tirtc_link_platform.h"
#include "tirtc_sample_avi.h"
#include "tirtc_sample_media.h"
#include "tirtc_test_log.h"

#define LINK_WORK_SYSTEM_EVENT (1U << 0)
#define LINK_WORK_ACCEPTED (1U << 1)
#define LINK_WORK_CONNECT_RESULT (1U << 2)
#define LINK_WORK_CONN_ERROR (1U << 3)
#define LINK_WORK_DISCONNECTED (1U << 4)
#define LINK_WORK_CONNECT (1U << 5)
#define LINK_WORK_DISCONNECT (1U << 6)
#define LINK_WORK_CONTROL_EVENT (1U << 7)

#define LINK_CLIENT_ID_BYTES 13U
#define LINK_CONTROL_EVENT_DEPTH 8U

typedef enum {
    LINK_CONTROL_COMMAND,
    LINK_CONTROL_VIDEO_SUBSCRIBE,
    LINK_CONTROL_VIDEO_UNSUBSCRIBE,
    LINK_CONTROL_AUDIO_SUBSCRIBE,
    LINK_CONTROL_AUDIO_UNSUBSCRIBE,
} link_control_event_type_t;

typedef struct {
    link_control_event_type_t type;
    uint8_t stream_id;
    int result;
    uint32_t command;
    uint32_t length;
} link_control_event_t;

/*
 * Ownership rules:
 * - g_link_lock protects connection/state/event fields touched by callbacks.
 * - g_sdk_api_lock serializes media sends, buffer queries and disconnect.
 * - only link_worker starts/stops the SDK and accepts/rejects connections.
 */
static DEFINE_SPINLOCK(g_link_lock);
static DEFINE_MUTEX(g_sdk_api_lock);
static thread_ptr_t g_worker;
static uint32_t g_work;
static bool g_sdk_initialized;
static bool g_sdk_starting;
static bool g_sdk_started;
static bool g_connect_pending;
static bool g_connect_inflight;
static bool g_auto_connect_queued;
static tirtc_conn_t g_connection;
static tirtc_conn_t g_pending_connection;
static int g_pending_connection_error;
static bool g_pending_connection_incoming;
static int g_pending_system_event = -1;
static int g_pending_conn_error;
static tirtc_conn_t g_disconnected_connection;
static tirtc_conn_t g_disconnect_queue[TIRTC_LINK_DISCONNECT_QUEUE_DEPTH];
static uint8_t g_disconnect_read;
static uint8_t g_disconnect_write;
static uint8_t g_disconnect_count;
static link_control_event_t g_control_events[LINK_CONTROL_EVENT_DEPTH];
static uint8_t g_control_read;
static uint8_t g_control_write;
static uint8_t g_control_count;
static uint32_t g_control_overflow_count;
static char g_remote_device_id[TIRTC_LINK_REMOTE_DEVICE_ID_MAX];
static char g_remote_token[TIRTC_LINK_REMOTE_TOKEN_MAX];
static tirtc_link_status_t g_status = {
    .state = TIRTC_LINK_STATE_IDLE,
};

static void link_lock(unsigned long *flags);
static void link_unlock(unsigned long flags);

/* TiRTC send bridge used by the independent sample-media worker. */
static bool link_connection_is_active(tirtc_conn_t connection)
{
    bool active;
    unsigned long flags;

    link_lock(&flags);
    active = connection != NULL && connection == g_connection;
    link_unlock(flags);
    return active;
}

static int link_sample_send_video(tirtc_conn_t connection,
                                  const TIRTCFRAMEINFO *frame,
                                  const void *data)
{
    int result;

    mutex_lock(&g_sdk_api_lock);
    result = link_connection_is_active(connection) ?
             TiRtcSendVideoStream(connection, frame, (void *)data) :
             TIRTC_E_INVALID_HANDLE;
    mutex_unlock(&g_sdk_api_lock);
    return result;
}

static int link_sample_send_audio(tirtc_conn_t connection,
                                  const TIRTCFRAMEINFO *frame,
                                  const void *data)
{
    int result;

    mutex_lock(&g_sdk_api_lock);
    result = link_connection_is_active(connection) ?
             TiRtcSendAudioStream(connection, frame, (void *)data) :
             TIRTC_E_INVALID_HANDLE;
    mutex_unlock(&g_sdk_api_lock);
    return result;
}

static size_t link_sample_send_buffer_used(tirtc_conn_t connection)
{
    size_t used = 0U;

    mutex_lock(&g_sdk_api_lock);
    if (link_connection_is_active(connection)) {
        used = TiRtcGetSendBufferUsed(connection);
    }
    mutex_unlock(&g_sdk_api_lock);
    return used;
}

static int link_sdk_disconnect(tirtc_conn_t connection)
{
    int result;

    mutex_lock(&g_sdk_api_lock);
    result = TiRtcDisconnect(connection);
    mutex_unlock(&g_sdk_api_lock);
    return result;
}

static const tirtc_sample_media_ops_t g_sample_media_ops = {
    .send_video = link_sample_send_video,
    .send_audio = link_sample_send_audio,
    .send_buffer_used = link_sample_send_buffer_used,
};

static void link_lock(unsigned long *flags)
{
    spin_lock_irqsave(&g_link_lock, *flags);
}

static void link_unlock(unsigned long flags)
{
    spin_unlock_irqrestore(&g_link_lock, flags);
}

static void link_copy(char *destination, size_t capacity, const char *source)
{
    if (destination == NULL || capacity == 0U) {
        return;
    }
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    strncpy(destination, source, capacity - 1U);
    destination[capacity - 1U] = '\0';
}

static void link_set_state(tirtc_link_state_t state, int error)
{
    tirtc_link_state_t previous;
    int previous_error;
    unsigned long flags;

    link_lock(&flags);
    previous = g_status.state;
    previous_error = g_status.last_error;
    g_status.state = state;
    g_status.last_error = error;
    link_unlock(flags);
    if (previous != state || previous_error != error) {
        if (error != 0) {
            tirtc_test_log_failure(tirtc_test_state_description(state), error);
        } else if (state == TIRTC_LINK_STATE_WAIT_NETWORK) {
            if (previous >= TIRTC_LINK_STATE_SYNC_TIME) {
                tirtc_test_log_failure("WiFi联网",
                                       TIRTC_LINK_E_WIFI_DISCONNECTED);
            } else {
                printf("[TEST][STEP] WiFi联网 | 正在连接热点\n");
            }
        } else if (state == TIRTC_LINK_STATE_SYNC_TIME) {
            if (previous == TIRTC_LINK_STATE_WAIT_NETWORK) {
                printf("[TEST][PASS] WiFi联网 | 网络已就绪\n");
            }
            printf("[TEST][STEP] 时间校准 | 正在访问NTP服务器\n");
        }
    }
}

static void link_schedule(uint32_t work)
{
    thread_ptr_t worker;
    unsigned long flags;

    link_lock(&flags);
    g_work |= work;
    worker = g_worker;
    link_unlock(flags);
    if (worker != NULL) {
        thread_wakeup(worker);
    }
}

static uint32_t link_take_work(void)
{
    uint32_t work;
    unsigned long flags;

    link_lock(&flags);
    work = g_work;
    g_work = 0U;
    link_unlock(flags);
    return work;
}

/* Callbacks enqueue diagnostics so serial I/O never blocks an SDK thread. */
static void link_queue_control_event(const link_control_event_t *event)
{
    unsigned long flags;

    link_lock(&flags);
    if (g_control_count < LINK_CONTROL_EVENT_DEPTH) {
        g_control_events[g_control_write] = *event;
        g_control_write =
            (uint8_t)((g_control_write + 1U) % LINK_CONTROL_EVENT_DEPTH);
        ++g_control_count;
    } else {
        ++g_control_overflow_count;
    }
    link_unlock(flags);
    link_schedule(LINK_WORK_CONTROL_EVENT);
}

static bool link_pop_control_event(link_control_event_t *event)
{
    unsigned long flags;

    link_lock(&flags);
    if (g_control_count == 0U) {
        link_unlock(flags);
        return false;
    }
    *event = g_control_events[g_control_read];
    g_control_read =
        (uint8_t)((g_control_read + 1U) % LINK_CONTROL_EVENT_DEPTH);
    --g_control_count;
    link_unlock(flags);
    return true;
}

static bool link_queue_disconnect_locked(tirtc_conn_t connection)
{
    uint8_t index;

    if (connection == NULL ||
        g_disconnect_count >= TIRTC_LINK_DISCONNECT_QUEUE_DEPTH) {
        return false;
    }
    for (index = 0U; index < g_disconnect_count; ++index) {
        uint8_t position = (uint8_t)((g_disconnect_read + index) %
                                     TIRTC_LINK_DISCONNECT_QUEUE_DEPTH);
        if (g_disconnect_queue[position] == connection) {
            return true;
        }
    }
    g_disconnect_queue[g_disconnect_write] = connection;
    g_disconnect_write = (uint8_t)((g_disconnect_write + 1U) %
                                   TIRTC_LINK_DISCONNECT_QUEUE_DEPTH);
    ++g_disconnect_count;
    return true;
}

static bool link_pop_disconnect(tirtc_conn_t *connection)
{
    unsigned long flags;

    link_lock(&flags);
    if (g_disconnect_count == 0U) {
        link_unlock(flags);
        return false;
    }
    *connection = g_disconnect_queue[g_disconnect_read];
    g_disconnect_queue[g_disconnect_read] = NULL;
    g_disconnect_read = (uint8_t)((g_disconnect_read + 1U) %
                                  TIRTC_LINK_DISCONNECT_QUEUE_DEPTH);
    --g_disconnect_count;
    link_unlock(flags);
    return true;
}

static bool link_tirtc_configured(void)
{
    return TIRTC_LINK_SERVICE_ENDPOINT[0] != '\0' &&
           TIRTC_LINK_DEVICE_ID[0] != '\0' &&
           TIRTC_LINK_DEVICE_SECRET[0] != '\0';
}

static void link_on_event(int event, const void *data, int len)
{
    unsigned long flags;

    (void)data;
    (void)len;
    link_lock(&flags);
    g_pending_system_event = event;
    link_unlock(flags);
    link_schedule(LINK_WORK_SYSTEM_EVENT);
}

static void link_on_accepted(tirtc_conn_t connection)
{
    unsigned long flags;

    link_lock(&flags);
    if (g_pending_connection == NULL) {
        g_pending_connection = connection;
        g_pending_connection_error = 0;
        g_pending_connection_incoming = true;
    } else if (!link_queue_disconnect_locked(connection)) {
        g_status.last_error = TIRTC_E_LACK_OF_RESOURCE;
    }
    link_unlock(flags);
    link_schedule(LINK_WORK_ACCEPTED);
}

static void link_on_conn_error(tirtc_conn_t connection, int error)
{
    unsigned long flags;

    link_lock(&flags);
    g_pending_conn_error = error;
    if (!link_queue_disconnect_locked(connection)) {
        g_status.last_error = TIRTC_E_LACK_OF_RESOURCE;
    }
    link_unlock(flags);
    link_schedule(LINK_WORK_CONN_ERROR);
}

static void link_on_disconnected(tirtc_conn_t connection)
{
    unsigned long flags;

    link_lock(&flags);
    g_disconnected_connection = connection;
    link_unlock(flags);
    link_schedule(LINK_WORK_DISCONNECTED);
}

static void link_on_audio(tirtc_conn_t connection,
                          const TIRTCFRAMEINFO *frame, void *data)
{
    /* This output-only sample intentionally discards incoming media. */
    (void)connection;
    (void)frame;
    (void)data;
}

static void link_on_video(tirtc_conn_t connection,
                          const TIRTCFRAMEINFO *frame, void *data)
{
    /* This output-only sample intentionally discards incoming media. */
    (void)connection;
    (void)frame;
    (void)data;
}

static void link_on_message(tirtc_conn_t connection,
                            const TIRTCFRAMEINFO *frame, void *data)
{
    (void)connection;
    (void)frame;
    (void)data;
}

static void link_on_command(tirtc_conn_t connection, uint32_t command,
                            const void *data, uint32_t length)
{
    link_control_event_t event = {
        .type = LINK_CONTROL_COMMAND,
        .command = command,
        .length = length,
    };
    unsigned long flags;

    (void)connection;
    (void)data;
    link_lock(&flags);
    ++g_status.received_commands;
    link_unlock(flags);
    link_queue_control_event(&event);
}

static void link_on_key_frame(tirtc_conn_t connection, uint8_t stream_id)
{
    /* Every MJPEG frame is independently decodable; keep the current cadence. */
    (void)connection;
    (void)stream_id;
}

static int link_on_subscribe_video(tirtc_conn_t connection, uint8_t stream_id)
{
    link_control_event_t event = {
        .type = LINK_CONTROL_VIDEO_SUBSCRIBE,
        .stream_id = stream_id,
        .result = TIRTC_E_INVALID_PARAMETER,
    };
    int result;

    if (TIRTC_LINK_ENABLE_SAMPLE_MEDIA) {
        result = tirtc_sample_media_subscribe_video(connection, stream_id);
        event.result = result;
        link_queue_control_event(&event);
        return result;
    }
    link_queue_control_event(&event);
    return TIRTC_E_INVALID_PARAMETER;
}

static void link_on_unsubscribe_video(tirtc_conn_t connection,
                                      uint8_t stream_id)
{
    link_control_event_t event = {
        .type = LINK_CONTROL_VIDEO_UNSUBSCRIBE,
        .stream_id = stream_id,
    };

    if (TIRTC_LINK_ENABLE_SAMPLE_MEDIA) {
        tirtc_sample_media_unsubscribe_video(connection, stream_id);
        link_queue_control_event(&event);
    }
}

static int link_on_subscribe_audio(tirtc_conn_t connection, uint8_t stream_id)
{
    link_control_event_t event = {
        .type = LINK_CONTROL_AUDIO_SUBSCRIBE,
        .stream_id = stream_id,
        .result = TIRTC_E_INVALID_PARAMETER,
    };
    int result;

    if (TIRTC_LINK_ENABLE_SAMPLE_MEDIA) {
        result = tirtc_sample_media_subscribe_audio(connection, stream_id);
        event.result = result;
        link_queue_control_event(&event);
        return result;
    }
    link_queue_control_event(&event);
    return TIRTC_E_INVALID_PARAMETER;
}

static void link_on_unsubscribe_audio(tirtc_conn_t connection,
                                      uint8_t stream_id)
{
    link_control_event_t event = {
        .type = LINK_CONTROL_AUDIO_UNSUBSCRIBE,
        .stream_id = stream_id,
    };

    if (TIRTC_LINK_ENABLE_SAMPLE_MEDIA) {
        tirtc_sample_media_unsubscribe_audio(connection, stream_id);
        link_queue_control_event(&event);
    }
}

static const TIRTCCALLBACKS g_callbacks = {
    .on_event = link_on_event,
    .on_conn_accepted = link_on_accepted,
    .on_conn_error = link_on_conn_error,
    .on_disconnected = link_on_disconnected,
    .on_audio = link_on_audio,
    .on_video = link_on_video,
    .on_message = link_on_message,
    .on_command = link_on_command,
    .on_request_key_frame = link_on_key_frame,
    .on_subscribe_video = link_on_subscribe_video,
    .on_unsubscribe_video = link_on_unsubscribe_video,
    .on_subscribe_audio = link_on_subscribe_audio,
    .on_unsubscribe_audio = link_on_unsubscribe_audio,
};

static void link_connect_result(int error, tirtc_conn_t connection,
                                void *user_data)
{
    unsigned long flags;

    (void)user_data;
    link_lock(&flags);
    if (g_pending_connection == NULL) {
        g_pending_connection = connection;
        g_pending_connection_error = error;
        g_pending_connection_incoming = false;
    } else if (connection != NULL &&
               !link_queue_disconnect_locked(connection)) {
        g_status.last_error = TIRTC_E_LACK_OF_RESOURCE;
    }
    link_unlock(flags);
    link_schedule(LINK_WORK_CONNECT_RESULT);
}

static int link_prepare_sdk(const char *client_id)
{
    uint32_t max_send_buffer = TIRTC_LINK_MAX_SEND_BUFFER;
    int max_connections = 1;
    int network_type = TIRTC_NETCONN_WIFI;
    int result;

    /* The send-buffer option is consumed while TiRtcInit creates runtime. */
    TiRtcLogSetLevel(TIRTC_LINK_SDK_LOG_LEVEL);
    result = TiRtcSetOption(TIRTC_OPT_MAX_SEND_BUFFER, &max_send_buffer,
                            sizeof(max_send_buffer));
    if (result != 0) {
        return result;
    }
    result = TiRtcInit();
    if (result != 0) {
        return result;
    }
    g_sdk_initialized = true;

    result = TiRtcSetOption(TIRTC_OPT_SERVICE_ENDPOINT,
                            TIRTC_LINK_SERVICE_ENDPOINT,
                            sizeof(TIRTC_LINK_SERVICE_ENDPOINT));
    if (result == 0) {
        result = TiRtcSetOption(TIRTC_OPT_DEVICE_SECRET_KEY,
                                TIRTC_LINK_DEVICE_SECRET,
                                sizeof(TIRTC_LINK_DEVICE_SECRET));
    }
    if (result == 0) {
        result = TiRtcSetOption(TIRTC_OPT_CLIENT_ID, client_id,
                                (uint32_t)strlen(client_id) + 1U);
    }
    if (result == 0) {
        result = TiRtcSetOption(TIRTC_OPT_MAX_CONNECTIONS, &max_connections,
                                sizeof(max_connections));
    }
    if (result == 0) {
        result = TiRtcSetOption(TIRTC_OPT_NETWORK_TYPE, &network_type,
                                sizeof(network_type));
    }
    return result;
}

static int link_start_sdk(void)
{
    char client_id[LINK_CLIENT_ID_BYTES];
    uint64_t begin_ms;
    int result;
    unsigned long flags;

    if (!link_tirtc_configured()) {
        link_set_state(TIRTC_LINK_STATE_WAIT_TIRTC_CONFIG,
                       TIRTC_LINK_E_IDENTITY_MISSING);
        return TIRTC_LINK_E_IDENTITY_MISSING;
    }
    if (tirtc_link_platform_get_client_id(client_id, sizeof(client_id)) != 0) {
        link_set_state(TIRTC_LINK_STATE_ERROR, TIRTC_LINK_E_CLIENT_ID);
        return TIRTC_LINK_E_CLIENT_ID;
    }

    result = link_prepare_sdk(client_id);
    if (result == 0) {
        link_lock(&flags);
        g_sdk_starting = true;
        g_status.state = TIRTC_LINK_STATE_STARTING;
        g_status.last_error = 0;
        link_unlock(flags);
        printf("[TEST][STEP] TiRTC上线 | sdk=%s buffer=%luKB\n",
               TiRtcGetVersion(),
               (unsigned long)(TIRTC_LINK_MAX_SEND_BUFFER / 1024U));
        begin_ms = systick_get_time_ms();
        result = TiRtcStart(TIRTC_LINK_DEVICE_ID, &g_callbacks);
        if (result == 0) {
            printf("[TEST][INFO] TiRTC上线 | 启动请求已提交 elapsed=%lums\n",
                   (unsigned long)(systick_get_time_ms() - begin_ms));
        }
    }
    if (result != 0) {
        link_lock(&flags);
        g_sdk_starting = false;
        link_unlock(flags);
        if (g_sdk_initialized) {
            TiRtcUninit();
            g_sdk_initialized = false;
        }
        link_set_state(TIRTC_LINK_STATE_ERROR, result);
    }
    memset(client_id, 0, sizeof(client_id));
    return result;
}

static void link_process_system_event(void)
{
    int event;
    bool clear_sample = false;
    bool uninit_sdk = false;
    unsigned long flags;

    link_lock(&flags);
    event = g_pending_system_event;
    g_pending_system_event = -1;
    if (event == TIRTC_EVENT_SYS_STARTED) {
        g_sdk_starting = false;
        g_sdk_started = true;
        g_status.sdk_ready = true;
        g_status.state = TIRTC_LINK_STATE_LISTENING;
        g_status.last_error = 0;
    } else if (event == TIRTC_EVENT_SYS_STOPPED) {
        g_sdk_starting = false;
        g_sdk_started = false;
        g_status.sdk_ready = false;
        g_status.peer_connected = false;
        g_status.state = TIRTC_LINK_STATE_WAIT_NETWORK;
        g_connection = NULL;
        g_connect_pending = false;
        g_connect_inflight = false;
        g_auto_connect_queued = false;
        g_pending_connection = NULL;
        g_pending_connection_error = 0;
        g_pending_conn_error = 0;
        g_disconnected_connection = NULL;
        g_disconnect_read = 0U;
        g_disconnect_write = 0U;
        g_disconnect_count = 0U;
        clear_sample = true;
        uninit_sdk = g_sdk_initialized;
    }
    link_unlock(flags);
    if (clear_sample && TIRTC_LINK_ENABLE_SAMPLE_MEDIA) {
        tirtc_sample_media_set_connection(NULL);
    }
    if (uninit_sdk) {
        mutex_lock(&g_sdk_api_lock);
        TiRtcUninit();
        g_sdk_initialized = false;
        mutex_unlock(&g_sdk_api_lock);
    }
    if (event == TIRTC_EVENT_SYS_STARTED) {
        printf("[TEST][PASS] TiRTC上线 | 设备在线，等待网页连接\n");
    } else if (event == TIRTC_EVENT_SYS_STOPPED) {
        printf("[TEST][INFO] TiRTC上线 | SDK已停止，等待自动恢复\n");
    }
}

static void link_process_connection_result(void)
{
    tirtc_conn_t connection;
    tirtc_conn_t reject = NULL;
    bool incoming;
    bool accepted = false;
    int error;
    unsigned long flags;

    link_lock(&flags);
    connection = g_pending_connection;
    error = g_pending_connection_error;
    incoming = g_pending_connection_incoming;
    g_pending_connection = NULL;
    g_pending_connection_error = 0;
    g_connect_inflight = false;
    if (error == 0 && connection != NULL && g_connection == NULL &&
        g_sdk_started) {
        g_connection = connection;
        g_status.peer_connected = true;
        g_status.state = TIRTC_LINK_STATE_CONNECTED;
        g_status.last_error = 0;
        ++g_status.accepted_connections;
        accepted = true;
    } else {
        reject = connection;
        g_status.last_error = error != 0 ? error : TIRTC_E_BUSY;
        g_status.state = g_sdk_started ? TIRTC_LINK_STATE_LISTENING :
                                         TIRTC_LINK_STATE_ERROR;
        if (reject != NULL) {
            (void)link_queue_disconnect_locked(reject);
        }
    }
    link_unlock(flags);
    if (accepted && TIRTC_LINK_ENABLE_SAMPLE_MEDIA) {
        tirtc_sample_media_set_connection(connection);
    } else if (reject != NULL && TIRTC_LINK_ENABLE_SAMPLE_MEDIA) {
        tirtc_sample_media_clear_connection(reject);
    }
    if (accepted) {
        printf("[TEST][PASS] 对端连接 | mode=%s，准备输出演示音视频\n",
               incoming ? "网页呼入" : "设备呼出");
    } else {
        tirtc_test_log_failure("对端连接",
                               error != 0 ? error : TIRTC_E_BUSY);
        link_lock(&flags);
        if (g_status.state == TIRTC_LINK_STATE_LISTENING) {
            g_status.last_error = 0;
        }
        link_unlock(flags);
    }
}

static void link_submit_connect(void)
{
    char remote_id[TIRTC_LINK_REMOTE_DEVICE_ID_MAX];
    char token[TIRTC_LINK_REMOTE_TOKEN_MAX];
    bool ready;
    int result;
    unsigned long flags;

    link_lock(&flags);
    ready = g_connect_pending && g_sdk_started && !g_connect_inflight &&
            g_connection == NULL;
    if (!ready) {
        link_unlock(flags);
        return;
    }
    link_copy(remote_id, sizeof(remote_id), g_remote_device_id);
    link_copy(token, sizeof(token), g_remote_token);
    g_connect_pending = false;
    g_connect_inflight = true;
    g_status.state = TIRTC_LINK_STATE_CONNECTING;
    link_unlock(flags);

    printf("[TEST][STEP] 对端连接 | 正在主动连接\n");
    result = TiRtcConnect(remote_id, token[0] != '\0' ? token : NULL,
                          link_connect_result, NULL);
    memset(remote_id, 0, sizeof(remote_id));
    memset(token, 0, sizeof(token));
    if (result != 0) {
        link_connect_result(result, NULL, NULL);
    }
}

static void link_process_disconnects(void)
{
    tirtc_conn_t connection;
    bool active;
    bool sdk_initialized;
    int result;
    int connection_error;
    unsigned long flags;

    while (link_pop_disconnect(&connection)) {
        link_lock(&flags);
        active = connection == g_connection;
        sdk_initialized = g_sdk_initialized;
        connection_error = g_pending_conn_error;
        if (active) {
            g_connection = NULL;
            g_status.peer_connected = false;
            g_status.state = g_sdk_started ? TIRTC_LINK_STATE_LISTENING :
                                             TIRTC_LINK_STATE_ERROR;
            if (connection_error != 0) {
                g_status.last_error = connection_error;
            }
        }
        link_unlock(flags);
        if (active && TIRTC_LINK_ENABLE_SAMPLE_MEDIA) {
            tirtc_sample_media_set_connection(NULL);
        }
        result = sdk_initialized ? link_sdk_disconnect(connection) : 0;
        if (connection_error != 0) {
            tirtc_test_log_failure("对端连接", connection_error);
        }
        if (result != 0) {
            tirtc_test_log_failure("断开连接", result);
        } else if (active) {
            printf("[TEST][INFO] 对端连接 | 断开请求已提交\n");
        }
        link_lock(&flags);
        if (g_status.state == TIRTC_LINK_STATE_LISTENING) {
            g_status.last_error = 0;
        }
        link_unlock(flags);
    }
    link_lock(&flags);
    g_pending_conn_error = 0;
    link_unlock(flags);
}

static void link_process_disconnected(void)
{
    tirtc_conn_t connection;
    bool active;
    unsigned long flags;

    link_lock(&flags);
    connection = g_disconnected_connection;
    g_disconnected_connection = NULL;
    active = connection != NULL && connection == g_connection;
    if (active) {
        g_connection = NULL;
        g_status.peer_connected = false;
        g_status.state = g_sdk_started ? TIRTC_LINK_STATE_LISTENING :
                                         TIRTC_LINK_STATE_ERROR;
    }
    link_unlock(flags);
    if (active && TIRTC_LINK_ENABLE_SAMPLE_MEDIA) {
        tirtc_sample_media_set_connection(NULL);
    }
    if (active) {
        printf("[TEST][INFO] 对端连接 | 已断开，继续等待网页连接\n");
    }
}

static void link_process_control_events(void)
{
    link_control_event_t event;
    uint32_t overflow_count;
    unsigned long flags;

    link_lock(&flags);
    overflow_count = g_control_overflow_count;
    g_control_overflow_count = 0U;
    link_unlock(flags);
    if (overflow_count != 0U) {
        tirtc_test_log_failure_detail(
            "控制事件队列", TIRTC_E_LACK_OF_RESOURCE,
            "SDK回调事件产生速度超过工作线程处理速度",
            "检查回调风暴或缩短TIRTC_LINK_WORKER_POLL_MS");
    }

    while (link_pop_control_event(&event)) {
        switch (event.type) {
        case LINK_CONTROL_COMMAND:
            printf("[TEST][INFO] 控制命令 | command=0x%08lx bytes=%lu\n",
                   (unsigned long)event.command,
                   (unsigned long)event.length);
            break;
        case LINK_CONTROL_VIDEO_SUBSCRIBE:
            if (event.result == 0) {
                printf("[TEST][PASS] 视频订阅 | stream=%u codec=MJPEG %ux%u@%ufps\n",
                       (unsigned)event.stream_id,
                       (unsigned)TIRTC_SAMPLE_AVI_VIDEO_WIDTH,
                       (unsigned)TIRTC_SAMPLE_AVI_VIDEO_HEIGHT,
                       (unsigned)TIRTC_SAMPLE_AVI_VIDEO_FPS);
            } else {
                tirtc_test_log_failure("视频订阅", event.result);
            }
            break;
        case LINK_CONTROL_VIDEO_UNSUBSCRIBE:
            printf("[TEST][INFO] 视频订阅 | stream=%u 已取消\n",
                   (unsigned)event.stream_id);
            break;
        case LINK_CONTROL_AUDIO_SUBSCRIBE:
            if (event.result == 0) {
                printf("[TEST][PASS] 音频订阅 | stream=%u codec=PCMA 8kHz mono %ums\n",
                       (unsigned)event.stream_id,
                       (unsigned)TIRTC_LINK_SAMPLE_AUDIO_PACKET_MS);
            } else {
                tirtc_test_log_failure("音频订阅", event.result);
            }
            break;
        case LINK_CONTROL_AUDIO_UNSUBSCRIBE:
            printf("[TEST][INFO] 音频订阅 | stream=%u 已取消\n",
                   (unsigned)event.stream_id);
            break;
        default:
            break;
        }
    }
}

static void link_log_status(void)
{
    tirtc_link_status_t status;
    tirtc_sample_media_status_t sample = {0};
    tirtc_conn_t connection;
    unsigned long flags;

    tirtc_link_get_status(&status);
    link_lock(&flags);
    connection = g_connection;
    link_unlock(flags);
    if (TIRTC_LINK_ENABLE_SAMPLE_MEDIA) {
        tirtc_sample_media_get_status(&sample);
    }
    if (status.last_error != 0) {
        tirtc_test_log_failure("运行状态", status.last_error);
    } else if (status.peer_connected) {
        printf("[TEST][PASS] 运行健康 | video=%lu帧 audio=%lu包 dropped=%lu buffer=%u/%u\n",
               (unsigned long)sample.sent_video_frames,
               (unsigned long)sample.sent_audio_packets,
               (unsigned long)sample.dropped_packets,
               (unsigned)link_sample_send_buffer_used(connection),
               (unsigned)TIRTC_LINK_MAX_SEND_BUFFER);
        if (sample.last_error != 0) {
            tirtc_test_log_failure("音视频发送", sample.last_error);
        }
    } else {
        printf("[TEST][WAIT] 运行状态 | %s | WiFi=%s 时钟=%s TiRTC=%s\n",
               tirtc_test_state_description(status.state),
               status.network_ready ? "OK" : "等待",
               status.clock_ready ? "OK" : "等待",
               status.sdk_ready ? "OK" : "等待");
    }
}

static void link_worker(void *data)
{
    uint64_t next_start_ms = 0U;
    uint64_t next_ntp_ms = 0U;
    uint64_t next_status_ms = 0U;

    (void)data;
    for (;;) {
        uint32_t work = link_take_work();
        uint64_t now_ms;
        bool network_ready;
        bool clock_ready;
        bool sdk_started;
        bool sdk_starting;
        bool peer_connected;
        unsigned long flags;

        tirtc_link_platform_poll();
        if ((work & LINK_WORK_SYSTEM_EVENT) != 0U) {
            link_process_system_event();
        }
        if ((work & (LINK_WORK_ACCEPTED | LINK_WORK_CONNECT_RESULT)) != 0U) {
            link_process_connection_result();
        }
        if ((work & LINK_WORK_DISCONNECT) != 0U) {
            link_lock(&flags);
            (void)link_queue_disconnect_locked(g_connection);
            link_unlock(flags);
        }
        link_process_disconnects();
        if ((work & LINK_WORK_DISCONNECTED) != 0U) {
            link_process_disconnected();
        }
        if ((work & LINK_WORK_CONTROL_EVENT) != 0U) {
            link_process_control_events();
        }

        now_ms = systick_get_time_ms();
        network_ready = tirtc_link_platform_network_ready();
        clock_ready = tirtc_link_platform_time_ready();
        link_lock(&flags);
        g_status.network_ready = network_ready;
        g_status.clock_ready = clock_ready;
        sdk_started = g_sdk_started;
        sdk_starting = g_sdk_starting;
        peer_connected = g_connection != NULL;
        link_unlock(flags);

        if (!tirtc_link_platform_wifi_configured()) {
            link_set_state(TIRTC_LINK_STATE_WAIT_WIFI_CONFIG,
                           TIRTC_LINK_E_WIFI_NOT_CONFIGURED);
        } else if (!network_ready) {
            link_set_state(TIRTC_LINK_STATE_WAIT_NETWORK, 0);
        } else if (!clock_ready) {
            if (now_ms >= next_ntp_ms) {
                link_set_state(TIRTC_LINK_STATE_SYNC_TIME, 0);
                {
                    int sync_result = tirtc_link_platform_sync_time();
                    if (sync_result != 0) {
                        link_set_state(TIRTC_LINK_STATE_SYNC_TIME,
                                       sync_result);
                    }
                }
            next_ntp_ms = systick_get_time_ms() + TIRTC_LINK_NTP_RETRY_MS;
            }
        } else if (!link_tirtc_configured()) {
            link_set_state(TIRTC_LINK_STATE_WAIT_TIRTC_CONFIG,
                           TIRTC_LINK_E_IDENTITY_MISSING);
        } else if (!sdk_started && !sdk_starting && now_ms >= next_start_ms) {
            (void)link_start_sdk();
            next_start_ms = systick_get_time_ms() + TIRTC_LINK_START_RETRY_MS;
        } else if (sdk_started && !peer_connected) {
            link_lock(&flags);
            if (!g_connect_inflight && !g_connect_pending) {
                g_status.state = TIRTC_LINK_STATE_LISTENING;
            }
            link_unlock(flags);
        }

        link_lock(&flags);
        sdk_started = g_sdk_started;
        if (sdk_started && TIRTC_LINK_AUTO_CONNECT && !g_auto_connect_queued &&
            TIRTC_LINK_REMOTE_DEVICE_ID[0] != '\0') {
            g_auto_connect_queued = true;
            g_connect_pending = true;
            link_copy(g_remote_device_id, sizeof(g_remote_device_id),
                      TIRTC_LINK_REMOTE_DEVICE_ID);
            link_copy(g_remote_token, sizeof(g_remote_token),
                      TIRTC_LINK_REMOTE_TOKEN);
        }
        link_unlock(flags);
        link_submit_connect();
        if (TIRTC_LINK_STATUS_LOG_INTERVAL_MS != 0U &&
            now_ms >= next_status_ms) {
            link_log_status();
            next_status_ms = now_ms + TIRTC_LINK_STATUS_LOG_INTERVAL_MS;
        }
        (void)thread_wait_timeout(TIRTC_LINK_WORKER_POLL_MS);
    }
}

void tirtc_link_service_init(void)
{
    int sample_result;
    thread_ptr_t worker;
    unsigned long flags;

    if (tirtc_link_platform_init() != 0) {
        link_set_state(TIRTC_LINK_STATE_ERROR, TIRTC_E_INTERNAL_ERROR);
        return;
    }
    if (TIRTC_LINK_ENABLE_SAMPLE_MEDIA) {
        sample_result = tirtc_sample_media_init(&g_sample_media_ops);
        if (sample_result != 0) {
            link_set_state(TIRTC_LINK_STATE_ERROR, sample_result);
            return;
        }
    }
    link_lock(&flags);
    worker = g_worker;
    link_unlock(flags);
    if (worker != NULL) {
        return;
    }
    worker = thread_create("tirtc_link", TIRTC_LINK_WORKER_STACK_BYTES,
                           link_worker, NULL);
    if (worker == NULL) {
        link_set_state(TIRTC_LINK_STATE_ERROR, TIRTC_E_LACK_OF_RESOURCE);
        return;
    }
    link_lock(&flags);
    if (g_worker == NULL) {
        g_worker = worker;
        worker = NULL;
    }
    link_unlock(flags);
    if (worker != NULL) {
        thread_delete(worker);
    }
    link_schedule(0U);
}

int tirtc_link_connect(const char *remote_device_id, const char *token)
{
    unsigned long flags;

    if (remote_device_id == NULL || remote_device_id[0] == '\0' ||
        strlen(remote_device_id) >= sizeof(g_remote_device_id) ||
        (token != NULL && strlen(token) >= sizeof(g_remote_token))) {
        return TIRTC_E_INVALID_PARAMETER;
    }
    link_lock(&flags);
    if (g_connection != NULL || g_connect_pending || g_connect_inflight) {
        link_unlock(flags);
        return TIRTC_E_BUSY;
    }
    link_copy(g_remote_device_id, sizeof(g_remote_device_id), remote_device_id);
    link_copy(g_remote_token, sizeof(g_remote_token), token);
    g_connect_pending = true;
    link_unlock(flags);
    link_schedule(LINK_WORK_CONNECT);
    return 0;
}

int tirtc_link_disconnect(void)
{
    bool connected;
    unsigned long flags;

    link_lock(&flags);
    connected = g_connection != NULL;
    link_unlock(flags);
    if (!connected) {
        return TIRTC_E_INVALID_HANDLE;
    }
    link_schedule(LINK_WORK_DISCONNECT);
    return 0;
}

void tirtc_link_get_status(tirtc_link_status_t *status)
{
    unsigned long flags;

    if (status == NULL) {
        return;
    }
    link_lock(&flags);
    *status = g_status;
    link_unlock(flags);
}

const char *tirtc_link_state_name(tirtc_link_state_t state)
{
    switch (state) {
    case TIRTC_LINK_STATE_IDLE:
        return "idle";
    case TIRTC_LINK_STATE_WAIT_WIFI_CONFIG:
        return "wait-wifi-config";
    case TIRTC_LINK_STATE_WAIT_NETWORK:
        return "wait-network";
    case TIRTC_LINK_STATE_SYNC_TIME:
        return "sync-time";
    case TIRTC_LINK_STATE_WAIT_TIRTC_CONFIG:
        return "wait-tirtc-config";
    case TIRTC_LINK_STATE_STARTING:
        return "starting";
    case TIRTC_LINK_STATE_LISTENING:
        return "listening";
    case TIRTC_LINK_STATE_CONNECTING:
        return "connecting";
    case TIRTC_LINK_STATE_CONNECTED:
        return "connected";
    case TIRTC_LINK_STATE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}
