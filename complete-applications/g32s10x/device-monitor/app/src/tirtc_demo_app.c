#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <dfs_posix.h>
#include <FreeRTOS.h>
#include <os/thread.h>
#include <spinlock.h>
#include <task.h>

#include "application/com_services/audio/common/include/audio_dev.h"
#include "cJSON.h"
#include "driver/network/wireless.h"
#include "driver/systick.h"
#include "tirtc_demo_app.h"
#include "tirtc_demo_cloud.h"
#include "tirtc_demo_media.h"
#include "tirtc_demo_sdk_gate.h"

#ifdef CONFIG_TIRTC
#include "tiRTC.h"
#endif

#define TIRTC_DEMO_MAX_CONTACTS 5
#define TIRTC_DEMO_CONFIG_DIR "/data/tirtc"
#define TIRTC_DEMO_CONFIG_TMP_PATH "/data/tirtc/config.json.tmp"
#define TIRTC_DEMO_CONFIG_BACKUP_PATH "/data/tirtc/config.json.bak"
#define TIRTC_DEMO_CONFIG_FILE_MAX 1024
#define TIRTC_DEMO_WORKER_STACK_SIZE 24576
#define TIRTC_DEMO_WORKER_POLL_MS 50
#define TIRTC_DEMO_NETWORK_POLL_MS 1000U
#define TIRTC_DEMO_SDK_RETRY_MS 1000U
#define TIRTC_DEMO_AI_START_DELAY_MS 300U
#define TIRTC_DEMO_AI_START_TIMEOUT_MS 30000U
#define TIRTC_DEMO_SESSION_READY_TIMEOUT_MS 30000U
#define TIRTC_DEMO_REMOTE_AUDIO_RETRY_MS 1000U
#define TIRTC_DEMO_COMMAND_QUEUE_DEPTH 4U
#define TIRTC_DEMO_COMMAND_DATA_MAX 1024U
#define TIRTC_DEMO_AI_START_REQUEST_ID "start-session-001"
#define TIRTC_DEMO_SESSION_QUEUE_DEPTH 3U
#define TIRTC_DEMO_DISCONNECT_QUEUE_DEPTH 4U

#define TIRTC_DEMO_WORK_START (1U << 0)
#define TIRTC_DEMO_WORK_RESTART (1U << 1)
#define TIRTC_DEMO_WORK_DISCONNECT (1U << 2)
#define TIRTC_DEMO_WORK_SESSION_READY (1U << 3)
#define TIRTC_DEMO_WORK_CONNECT_RESULT (1U << 4)
#define TIRTC_DEMO_WORK_COMMAND (1U << 5)
#define TIRTC_DEMO_WORK_AUDIO_SUBSCRIBE (1U << 6)
#define TIRTC_DEMO_WORK_AUDIO_UNSUBSCRIBE (1U << 7)
#define TIRTC_DEMO_WORK_VIDEO_SUBSCRIBE (1U << 8)
#define TIRTC_DEMO_WORK_VIDEO_UNSUBSCRIBE (1U << 9)
#define TIRTC_DEMO_WORK_LOCAL_HANGUP (1U << 10)
#define TIRTC_DEMO_WORK_AI_START_TX (1U << 11)
#define TIRTC_DEMO_WORK_AI_STOP_TX (1U << 12)
#define TIRTC_DEMO_WORK_AI_INTERRUPT (1U << 13)
#define TIRTC_DEMO_WORK_WECHAT_REJECT (1U << 14)
#define TIRTC_DEMO_WORK_DIAGNOSTICS (1U << 15)
#define TIRTC_DEMO_WORK_WEB_CALL_ACCEPT (1U << 16)
#define TIRTC_DEMO_WORK_WEB_CALL_REJECT (1U << 17)

#define TIRTC_DEMO_CMD_CALL 0x1101U
#define TIRTC_DEMO_CMD_VOLUME 0x1102U
#define TIRTC_DEMO_CMD_DOOR 0x1103U
#define TIRTC_DEMO_CMD_WEB_HANGUP 0x1104U
#define TIRTC_DEMO_CMD_REQUEST_VIDEO 0x1105U
#define TIRTC_DEMO_CMD_REQUEST_AUDIO 0x1106U
#define TIRTC_DEMO_CMD_SET_SEND_VIDEO 0x1107U
#define TIRTC_DEMO_CMD_SET_SEND_AUDIO 0x1108U
#define TIRTC_DEMO_CMD_STATE 0x1113U
#define TIRTC_DEMO_CMD_TIME_QUERY 0x1f11U
#define TIRTC_DEMO_CMD_CALL_CONNECTED 0x2000U
#define TIRTC_DEMO_CMD_CALL_HANGUP 0x2001U
#define TIRTC_DEMO_CMD_AI_SIGNALING 0x2100U
#define TIRTC_DEMO_CMD_DIRECT_RESPONSE_BIT 0x8000U

#define TIRTC_DEMO_WEB_REPLY_ALLOW "ALLOW"
#define TIRTC_DEMO_WEB_REPLY_REJECT "REJECT"
#define TIRTC_DEMO_WEB_REPLY_BUSY "BUSY"
#define TIRTC_DEMO_WEB_REPLY_OK "OK"
#define TIRTC_DEMO_WEB_REPLY_ERROR "ERR"
#define TIRTC_DEMO_WEB_REPLY_NO_CALL "NO_CALL"

typedef enum {
    TIRTC_DEMO_SESSION_NONE = 0,
    TIRTC_DEMO_SESSION_MONITOR,
    TIRTC_DEMO_SESSION_DEVICE,
    TIRTC_DEMO_SESSION_WECHAT,
    TIRTC_DEMO_SESSION_AI,
} tirtc_demo_session_type_t;

typedef struct {
    tirtc_conn_t connection;
    uint32_t command;
    uint16_t length;
    char data[TIRTC_DEMO_COMMAND_DATA_MAX];
} tirtc_demo_command_packet_t;

typedef struct {
    tirtc_conn_t connection;
    bool already_closed;
} tirtc_demo_disconnect_request_t;

typedef struct {
    char name[TIRTC_DEMO_CLOUD_CONTACT_NAME_MAX];
    char device_id[TIRTC_DEMO_DEVICE_ID_MAX];
    char last_call[32];
} tirtc_demo_contact_storage_t;

typedef struct {
    char name[TIRTC_DEMO_CLOUD_CONTACT_NAME_MAX];
    char open_id[TIRTC_DEMO_WECHAT_OPENID_MAX];
    char app_id[TIRTC_DEMO_WECHAT_APP_ID_MAX];
    char model_id[TIRTC_DEMO_WECHAT_MODEL_ID_MAX];
} tirtc_demo_wechat_contact_storage_t;

typedef struct {
    uint8_t call_active;
    uint8_t local_video_send_enabled;
    uint8_t local_audio_send_enabled;
    uint8_t video_stream_active;
    uint8_t audio_stream_active;
    uint8_t rgb[3];
} tirtc_demo_web_state_t;

static DEFINE_SPINLOCK(g_tirtc_demo_lock);

static tirtc_demo_contact_t g_contacts[TIRTC_DEMO_MAX_CONTACTS] = {{0}};
static tirtc_demo_contact_storage_t g_contact_storage[TIRTC_DEMO_MAX_CONTACTS];
static size_t g_contact_count;
static tirtc_demo_wechat_contact_t
    g_wechat_contacts[TIRTC_DEMO_MAX_CONTACTS] = {{0}};
static tirtc_demo_wechat_contact_storage_t
    g_wechat_contact_storage[TIRTC_DEMO_MAX_CONTACTS];
static size_t g_wechat_contact_count;
static tirtc_demo_config_t g_config = {
    .endpoint = TIRTC_DEMO_DEFAULT_ENDPOINT,
};
static bool g_config_loaded;
static bool g_cloud_initialized;
static bool g_external_media_active;
static uint32_t g_cloud_status_generation;
static char g_cloud_stage[32];
static char g_cloud_message[160];
static char g_cloud_peer_id[TIRTC_DEMO_DEVICE_ID_MAX];
static char g_cloud_binding_code[TIRTC_DEMO_CLOUD_BINDING_CODE_MAX];
static char g_session_type_text[24] = "none";
static char g_ai_caption[TIRTC_DEMO_AI_CAPTION_MAX];
static tirtc_demo_ai_message_t g_ai_messages[TIRTC_DEMO_AI_MESSAGE_MAX];
static size_t g_ai_message_count;
static char g_diagnostic_summary[TIRTC_DEMO_DIAGNOSTIC_SUMMARY_MAX] =
    "尚未运行链路自检。";

static tirtc_demo_status_t g_status = {
    .state = TIRTC_DEMO_STATE_IDLE,
    .stage = "未启动",
    .message = "TiRTC 服务尚未启动。",
};
static tirtc_demo_runtime_t g_runtime = {
    .peer_id = TIRTC_DEMO_UNCONFIGURED_TEXT,
    .call_phase = "idle",
    .binding_code = "",
    .session_type = g_session_type_text,
    .ai_caption = g_ai_caption,
    .diagnostic_summary = g_diagnostic_summary,
};
static int g_rx_volume = 62;
static int g_tx_volume = 48;
static int g_rx_restore_volume = 62;
static int g_tx_restore_volume = 48;
static bool g_rx_muted;
static bool g_tx_muted;
static uint64_t g_call_started_at_ms;

#ifdef CONFIG_TIRTC
static thread_ptr_t g_worker_thread;
static volatile uint32_t g_worker_events;
static bool g_sdk_initialized;
static bool g_sdk_starting;
static bool g_sdk_started;
static bool g_sdk_stopping;
static bool g_restart_pending;
static tirtc_conn_t g_active_conn;
static tirtc_demo_disconnect_request_t
    g_disconnect_queue[TIRTC_DEMO_DISCONNECT_QUEUE_DEPTH];
static uint8_t g_disconnect_read;
static uint8_t g_disconnect_write;
static uint8_t g_disconnect_count;
static tirtc_demo_session_type_t g_session_type;
static tirtc_demo_cloud_session_t g_session;
static tirtc_demo_cloud_session_t
    *g_pending_cloud_sessions[TIRTC_DEMO_SESSION_QUEUE_DEPTH];
static uint8_t g_pending_session_read;
static uint8_t g_pending_session_write;
static uint8_t g_pending_session_count;
static uint32_t g_session_generation;
static bool g_connect_inflight;
static bool g_session_connect_allowed;
static tirtc_conn_t g_connect_result_conn;
static int g_connect_result_error;
static uint32_t g_connect_result_generation;
static bool g_session_media_started;
static bool g_session_waiting_ready;
static bool g_video_subscribed;
static bool g_monitor_audio_subscribed;
static uint32_t g_monitor_audio_subscribe_attempts;
static uint64_t g_monitor_audio_retry_at_ms;
static bool g_ai_start_pending;
static bool g_ai_start_waiting;
static uint32_t g_pending_web_call_command;
static bool g_web_call_active;
static uint64_t g_ai_start_at_ms;
static uint64_t g_ai_start_deadline_ms;
static uint64_t g_ai_heartbeat_at_ms;
static uint64_t g_session_ready_deadline_ms;
static tirtc_demo_command_packet_t
    g_command_queue[TIRTC_DEMO_COMMAND_QUEUE_DEPTH];
static uint8_t g_command_read;
static uint8_t g_command_write;
static uint8_t g_command_count;
static void tirtc_demo_schedule_work(uint32_t events);
static bool tirtc_demo_queue_disconnect_locked(tirtc_conn_t connection,
                                                bool already_closed);
#endif

static int tirtc_demo_ensure_cloud(void);
static void tirtc_demo_sync_cloud_state(void);
static int tirtc_demo_set_volume_now(tirtc_demo_volume_t volume, int value,
                                     bool report_status);

static void tirtc_demo_lock(unsigned long *flags)
{
    spin_lock_irqsave(&g_tirtc_demo_lock, *flags);
}

static void tirtc_demo_unlock(unsigned long flags)
{
    spin_unlock_irqrestore(&g_tirtc_demo_lock, flags);
}

#ifdef CONFIG_TIRTC
static int tirtc_demo_sdk_send_command(tirtc_conn_t connection,
                                       uint32_t command, const void *data,
                                       uint32_t length)
{
    int result;

    if (!tirtc_demo_sdk_gate_take(TIRTC_DEMO_SDK_CONTROL_WAIT_MS)) {
        return TIRTC_E_BUSY;
    }
    result = TiRtcSendCommand(connection, command, data, length);
    tirtc_demo_sdk_gate_give();
    return result;
}

static int tirtc_demo_sdk_subscribe_audio(tirtc_conn_t connection,
                                          uint8_t stream_id)
{
    int result;

    if (!tirtc_demo_sdk_gate_take(TIRTC_DEMO_SDK_CONTROL_WAIT_MS)) {
        return TIRTC_E_BUSY;
    }
    result = TiRtcSubscribeAudio(connection, stream_id);
    tirtc_demo_sdk_gate_give();
    return result;
}

static int tirtc_demo_sdk_unsubscribe_audio(tirtc_conn_t connection,
                                            uint8_t stream_id)
{
    int result;

    if (!tirtc_demo_sdk_gate_take(TIRTC_DEMO_SDK_CONTROL_WAIT_MS)) {
        return TIRTC_E_BUSY;
    }
    result = TiRtcUnsubscribeAudio(connection, stream_id);
    tirtc_demo_sdk_gate_give();
    return result;
}

static int tirtc_demo_sdk_disconnect(tirtc_conn_t connection)
{
    int result;

    if (!tirtc_demo_sdk_gate_take(TIRTC_DEMO_SDK_CONTROL_WAIT_MS)) {
        return TIRTC_E_BUSY;
    }
    result = TiRtcDisconnect(connection);
    tirtc_demo_sdk_gate_give();
    return result;
}
#endif

static void tirtc_demo_copy_text(char *dest, size_t dest_size, const char *source)
{
    if (dest == NULL || dest_size == 0) {
        return;
    }

    if (source == NULL) {
        source = "";
    }

    snprintf(dest, dest_size, "%s", source);
}

static void tirtc_demo_copy_utf8(char *dest, size_t dest_size,
                                 const char *source)
{
    size_t input = 0U;
    size_t output = 0U;

    if (dest == NULL || dest_size == 0U) {
        return;
    }
    if (source == NULL) {
        source = "";
    }
    while (source[input] != '\0' && output + 1U < dest_size) {
        const unsigned char lead = (unsigned char)source[input];
        size_t sequence = 1U;
        bool valid = true;

        if (lead < 0x80U) {
            sequence = 1U;
        } else if (lead >= 0xc2U && lead <= 0xdfU) {
            sequence = 2U;
        } else if (lead >= 0xe0U && lead <= 0xefU) {
            sequence = 3U;
        } else if (lead >= 0xf0U && lead <= 0xf4U) {
            sequence = 4U;
        } else {
            valid = false;
        }
        if (valid) {
            for (size_t i = 1U; i < sequence; ++i) {
                const unsigned char continuation =
                    (unsigned char)source[input + i];

                if (continuation == 0U ||
                    (continuation & 0xc0U) != 0x80U) {
                    valid = false;
                    break;
                }
            }
        }
        if (!valid) {
            dest[output++] = '?';
            ++input;
            continue;
        }
        if (output + sequence >= dest_size) {
            break;
        }
        memcpy(dest + output, source + input, sequence);
        output += sequence;
        input += sequence;
    }
    dest[output] = '\0';
}

static void tirtc_demo_set_status(tirtc_demo_state_t state, int err,
                                  const char *stage, const char *message)
{
    unsigned long flags;

    tirtc_demo_lock(&flags);
    g_status.state = state;
    g_status.last_error = err;
    g_status.stage = stage;
    g_status.message = message;
    tirtc_demo_unlock(flags);
}

static int tirtc_demo_service_pending(const char *stage, const char *message)
{
    tirtc_demo_set_status(TIRTC_DEMO_STATE_UNCONFIGURED,
                          TIRTC_DEMO_ERR_SERVICE_PENDING,
                          stage, message);
    return TIRTC_DEMO_ERR_SERVICE_PENDING;
}

static bool tirtc_demo_endpoint_valid(const char *endpoint)
{
    size_t prefix_length;

    if (endpoint == NULL || strchr(endpoint, ' ') != NULL) {
        return false;
    }
    prefix_length = strncmp(endpoint, "https://", 8) == 0 ? 8U :
                    strncmp(endpoint, "http://", 7) == 0 ? 7U : 0U;
    return prefix_length != 0U && endpoint[prefix_length] != '\0';
}

static bool tirtc_demo_config_valid(const tirtc_demo_config_t *config)
{
    if (config == NULL ||
        config->endpoint[sizeof(config->endpoint) - 1] != '\0' ||
        config->device_id[sizeof(config->device_id) - 1] != '\0' ||
        config->device_secret[sizeof(config->device_secret) - 1] != '\0') {
        return false;
    }

    return tirtc_demo_endpoint_valid(config->endpoint) &&
           config->device_id[0] != '\0' &&
           config->device_secret[0] != '\0';
}

static bool tirtc_demo_json_string(const cJSON *root, const char *key,
                                   char *output, size_t output_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    size_t length;

    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }

    length = strlen(item->valuestring);
    if (length == 0 || length >= output_size) {
        return false;
    }

    memcpy(output, item->valuestring, length + 1);
    return true;
}

static int tirtc_demo_read_file(char *buffer, size_t buffer_size)
{
    int fd;
    int total = 0;
    int result;

    fd = open(TIRTC_DEMO_CONFIG_PATH, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    while ((size_t)total < buffer_size - 1) {
        result = read(fd, buffer + total, buffer_size - 1 - (size_t)total);
        if (result < 0) {
            close(fd);
            return -1;
        }
        if (result == 0) {
            break;
        }
        total += result;
    }

    close(fd);
    buffer[total] = '\0';
    return total;
}

static int tirtc_demo_load_config_file(tirtc_demo_config_t *config)
{
    char buffer[TIRTC_DEMO_CONFIG_FILE_MAX];
    cJSON *root;
    bool endpoint_loaded;
    bool id_loaded;
    bool secret_loaded;
    const cJSON *avatar;
    int size;

    memset(config, 0, sizeof(*config));
    tirtc_demo_copy_text(config->endpoint, sizeof(config->endpoint),
                          TIRTC_DEMO_DEFAULT_ENDPOINT);

    size = tirtc_demo_read_file(buffer, sizeof(buffer));
    if (size < 0) {
        return -1;
    }
    if (size == 0 || size >= (int)sizeof(buffer) - 1) {
        return -2;
    }

    root = cJSON_Parse(buffer);
    if (root == NULL) {
        return -2;
    }

    endpoint_loaded = tirtc_demo_json_string(root, "service_endpoint",
                                              config->endpoint,
                                              sizeof(config->endpoint));
    if (!endpoint_loaded) {
        (void)tirtc_demo_json_string(root, "endpoint", config->endpoint,
                                     sizeof(config->endpoint));
    }
    id_loaded = tirtc_demo_json_string(root, "device_id", config->device_id,
                                       sizeof(config->device_id));
    secret_loaded = tirtc_demo_json_string(root, "device_secret_key",
                                           config->device_secret,
                                           sizeof(config->device_secret));
    if (!secret_loaded) {
        secret_loaded = tirtc_demo_json_string(root, "device_secret",
                                               config->device_secret,
                                               sizeof(config->device_secret));
    }
    avatar = cJSON_GetObjectItemCaseSensitive(root, "ai_avatar");
    config->ai_avatar = cJSON_IsNumber(avatar) && avatar->valueint >= 0 &&
                                avatar->valueint < TIRTC_DEMO_AI_AVATAR_COUNT ?
                            (uint8_t)avatar->valueint : 0U;
    cJSON_Delete(root);

    if (!tirtc_demo_endpoint_valid(config->endpoint)) {
        tirtc_demo_copy_text(config->endpoint, sizeof(config->endpoint),
                              TIRTC_DEMO_DEFAULT_ENDPOINT);
    }
    if (!id_loaded || !secret_loaded) {
        config->device_id[0] = '\0';
        config->device_secret[0] = '\0';
    }

    return tirtc_demo_config_valid(config) ? 0 : -3;
}

static int tirtc_demo_write_all(int fd, const char *buffer, size_t length)
{
    size_t total = 0;

    while (total < length) {
        int result = write(fd, buffer + total, length - total);

        if (result <= 0) {
            return -1;
        }
        total += (size_t)result;
    }

    return 0;
}

static int tirtc_demo_save_config_file(const tirtc_demo_config_t *config)
{
    cJSON *root;
    char *payload;
    size_t payload_length;
    bool old_config_moved = false;
    int fd;
    int result = -1;

    root = cJSON_CreateObject();
    if (root == NULL ||
        cJSON_AddStringToObject(root, "service_endpoint", config->endpoint) == NULL ||
        cJSON_AddStringToObject(root, "device_id", config->device_id) == NULL ||
        cJSON_AddStringToObject(root, "device_secret_key", config->device_secret) == NULL ||
        cJSON_AddNumberToObject(root, "ai_avatar", config->ai_avatar) == NULL) {
        cJSON_Delete(root);
        return -1;
    }

    payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return -1;
    }
    payload_length = strlen(payload);

    (void)mkdir(TIRTC_DEMO_CONFIG_DIR, 0700);
    fd = open(TIRTC_DEMO_CONFIG_TMP_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        if (tirtc_demo_write_all(fd, payload, payload_length) == 0 &&
            fsync(fd) == 0) {
            result = 0;
        }
        close(fd);
    }

    memset(payload, 0, payload_length);
    cJSON_free(payload);
    if (result != 0) {
        (void)unlink(TIRTC_DEMO_CONFIG_TMP_PATH);
        return -1;
    }

    if (rename(TIRTC_DEMO_CONFIG_TMP_PATH, TIRTC_DEMO_CONFIG_PATH) != 0) {
        (void)unlink(TIRTC_DEMO_CONFIG_BACKUP_PATH);
        old_config_moved =
            rename(TIRTC_DEMO_CONFIG_PATH,
                   TIRTC_DEMO_CONFIG_BACKUP_PATH) == 0;
        if (rename(TIRTC_DEMO_CONFIG_TMP_PATH, TIRTC_DEMO_CONFIG_PATH) != 0) {
            if (old_config_moved) {
                (void)rename(TIRTC_DEMO_CONFIG_BACKUP_PATH,
                             TIRTC_DEMO_CONFIG_PATH);
            }
            (void)unlink(TIRTC_DEMO_CONFIG_TMP_PATH);
            return -1;
        }
        if (old_config_moved) {
            (void)unlink(TIRTC_DEMO_CONFIG_BACKUP_PATH);
        }
    }
    (void)unlink(TIRTC_DEMO_CONFIG_BACKUP_PATH);

    return 0;
}

static void tirtc_demo_apply_config(const tirtc_demo_config_t *config)
{
    unsigned long flags;
    bool ready = tirtc_demo_config_valid(config);

    tirtc_demo_lock(&flags);
    g_config = *config;
    g_runtime.identity_ready = ready;
    g_runtime.peer_id = ready ? g_config.device_id : TIRTC_DEMO_UNCONFIGURED_TEXT;
    g_runtime.ai_avatar = config->ai_avatar < TIRTC_DEMO_AI_AVATAR_COUNT ?
                              config->ai_avatar : 0U;
    tirtc_demo_unlock(flags);
}

static void tirtc_demo_load_config_once(void)
{
    tirtc_demo_config_t loaded;
    unsigned long flags;
    bool already_loaded;
    int result;

    tirtc_demo_lock(&flags);
    already_loaded = g_config_loaded;
    tirtc_demo_unlock(flags);
    if (already_loaded) {
        return;
    }

    result = tirtc_demo_load_config_file(&loaded);
    tirtc_demo_apply_config(&loaded);

    tirtc_demo_lock(&flags);
    g_config_loaded = true;
    tirtc_demo_unlock(flags);

    if (result == 0) {
        printf("[tirtc_demo] config loaded device_id_len=%u\n",
               (unsigned int)strlen(loaded.device_id));
    } else if (result != -1) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_UNCONFIGURED,
                              TIRTC_DEMO_ERR_CONFIG_REQUIRED,
                              "配置", "配置文件无效，请重新保存设备凭据。");
    }
}

static void tirtc_demo_refresh_network_state(void)
{
    unsigned long flags;
    bool ready = wifi_status_get() == JZ_WIFI_STATUS_AVAILABLE;
    int rssi = ready ? wifi_get_rssi() : 0;

    tirtc_demo_lock(&flags);
    g_runtime.network_ready = ready;
    g_runtime.network_rssi = rssi;
    tirtc_demo_unlock(flags);
}

#ifdef CONFIG_TIRTC
static void tirtc_demo_schedule_work(uint32_t events)
{
    thread_ptr_t worker;
    unsigned long flags;

    tirtc_demo_lock(&flags);
    g_worker_events |= events;
    worker = g_worker_thread;
    tirtc_demo_unlock(flags);

    if (worker != NULL) {
        thread_wakeup(worker);
    }
}

static uint32_t tirtc_demo_take_work(void)
{
    uint32_t events;
    unsigned long flags;

    tirtc_demo_lock(&flags);
    events = g_worker_events;
    g_worker_events = 0;
    tirtc_demo_unlock(flags);
    return events;
}

static bool tirtc_demo_work_pending(void)
{
    bool pending;
    unsigned long flags;

    tirtc_demo_lock(&flags);
    pending = g_worker_events != 0U;
    tirtc_demo_unlock(flags);
    return pending;
}

static bool tirtc_demo_queue_disconnect_locked(tirtc_conn_t connection,
                                                bool already_closed)
{
    uint8_t index;

    if (connection == NULL) {
        return false;
    }
    index = g_disconnect_read;
    for (uint8_t i = 0U; i < g_disconnect_count; ++i) {
        if (g_disconnect_queue[index].connection == connection) {
            g_disconnect_queue[index].already_closed |= already_closed;
            g_worker_events |= TIRTC_DEMO_WORK_DISCONNECT;
            return true;
        }
        index = (uint8_t)((index + 1U) % TIRTC_DEMO_DISCONNECT_QUEUE_DEPTH);
    }
    if (g_disconnect_count >= TIRTC_DEMO_DISCONNECT_QUEUE_DEPTH) {
        g_restart_pending = true;
        g_worker_events |= TIRTC_DEMO_WORK_RESTART;
        g_status.state = TIRTC_DEMO_STATE_ERROR;
        g_status.last_error = TIRTC_E_LACK_OF_RESOURCE;
        g_status.stage = "连接回收";
        g_status.message = "连接回收队列已满，将重启 TiRTC 服务统一释放资源。";
        return false;
    }
    g_disconnect_queue[g_disconnect_write].connection = connection;
    g_disconnect_queue[g_disconnect_write].already_closed = already_closed;
    g_disconnect_write = (uint8_t)((g_disconnect_write + 1U) %
                                   TIRTC_DEMO_DISCONNECT_QUEUE_DEPTH);
    ++g_disconnect_count;
    g_worker_events |= TIRTC_DEMO_WORK_DISCONNECT;
    return true;
}

static bool tirtc_demo_take_disconnect(
    tirtc_demo_disconnect_request_t *request)
{
    unsigned long flags;

    if (request == NULL) {
        return false;
    }
    tirtc_demo_lock(&flags);
    if (g_disconnect_count == 0U) {
        tirtc_demo_unlock(flags);
        return false;
    }
    *request = g_disconnect_queue[g_disconnect_read];
    memset(&g_disconnect_queue[g_disconnect_read], 0,
           sizeof(g_disconnect_queue[g_disconnect_read]));
    g_disconnect_read = (uint8_t)((g_disconnect_read + 1U) %
                                  TIRTC_DEMO_DISCONNECT_QUEUE_DEPTH);
    --g_disconnect_count;
    if (g_disconnect_count > 0U) {
        g_worker_events |= TIRTC_DEMO_WORK_DISCONNECT;
    }
    tirtc_demo_unlock(flags);
    return true;
}

static tirtc_demo_session_type_t tirtc_demo_session_from_cloud(
    tirtc_demo_cloud_session_type_t type)
{
    switch (type) {
    case TIRTC_DEMO_CLOUD_SESSION_DEVICE:
        return TIRTC_DEMO_SESSION_DEVICE;
    case TIRTC_DEMO_CLOUD_SESSION_WECHAT:
        return TIRTC_DEMO_SESSION_WECHAT;
    case TIRTC_DEMO_CLOUD_SESSION_AI:
        return TIRTC_DEMO_SESSION_AI;
    case TIRTC_DEMO_CLOUD_SESSION_NONE:
    default:
        return TIRTC_DEMO_SESSION_NONE;
    }
}

static tirtc_demo_media_mode_t tirtc_demo_media_mode(
    tirtc_demo_session_type_t type)
{
    switch (type) {
    case TIRTC_DEMO_SESSION_MONITOR:
        return TIRTC_DEMO_MEDIA_MONITOR;
    case TIRTC_DEMO_SESSION_DEVICE:
        return TIRTC_DEMO_MEDIA_DEVICE_CALL;
    case TIRTC_DEMO_SESSION_WECHAT:
        return TIRTC_DEMO_MEDIA_WECHAT;
    case TIRTC_DEMO_SESSION_AI:
        return TIRTC_DEMO_MEDIA_AI;
    case TIRTC_DEMO_SESSION_NONE:
    default:
        return TIRTC_DEMO_MEDIA_NONE;
    }
}

static tirtc_demo_cloud_session_type_t tirtc_demo_cloud_session_type(
    tirtc_demo_session_type_t type)
{
    switch (type) {
    case TIRTC_DEMO_SESSION_DEVICE:
        return TIRTC_DEMO_CLOUD_SESSION_DEVICE;
    case TIRTC_DEMO_SESSION_WECHAT:
        return TIRTC_DEMO_CLOUD_SESSION_WECHAT;
    case TIRTC_DEMO_SESSION_AI:
        return TIRTC_DEMO_CLOUD_SESSION_AI;
    case TIRTC_DEMO_SESSION_MONITOR:
    case TIRTC_DEMO_SESSION_NONE:
    default:
        return TIRTC_DEMO_CLOUD_SESSION_NONE;
    }
}

static void tirtc_demo_cloud_session_ready(
    const tirtc_demo_cloud_session_t *session, void *context)
{
    tirtc_demo_cloud_session_t *copy;
    thread_ptr_t worker = NULL;
    unsigned long flags;

    (void)context;
    if (session == NULL || session->type == TIRTC_DEMO_CLOUD_SESSION_NONE) {
        return;
    }
    copy = malloc(sizeof(*copy));
    if (copy == NULL) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR,
                              TIRTC_E_LACK_OF_RESOURCE,
                              "会话", "会话参数内存不足。");
        tirtc_demo_cloud_notify_session_ended(session->type,
                                              "设备内存不足，未建立会话。");
        return;
    }
    memcpy(copy, session, sizeof(*copy));
    tirtc_demo_lock(&flags);
    if (g_pending_session_count < TIRTC_DEMO_SESSION_QUEUE_DEPTH) {
        g_pending_cloud_sessions[g_pending_session_write] = copy;
        g_pending_session_write = (uint8_t)((g_pending_session_write + 1U) %
                                            TIRTC_DEMO_SESSION_QUEUE_DEPTH);
        ++g_pending_session_count;
        g_worker_events |= TIRTC_DEMO_WORK_SESSION_READY;
        worker = g_worker_thread;
        copy = NULL;
    }
    tirtc_demo_unlock(flags);
    if (copy != NULL) {
        tirtc_demo_cloud_session_type_t type = copy->type;

        memset(copy, 0, sizeof(*copy));
        free(copy);
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR,
                              TIRTC_DEMO_CLOUD_ERR_BUSY,
                              "会话", "会话队列已满，请稍后重试。");
        tirtc_demo_cloud_notify_session_ended(type,
                                              "设备会话队列已满。");
    } else if (worker != NULL) {
        thread_wakeup(worker);
    }
}

static tirtc_demo_cloud_session_t *tirtc_demo_take_cloud_session(void)
{
    tirtc_demo_cloud_session_t *session = NULL;
    unsigned long flags;

    tirtc_demo_lock(&flags);
    if (g_pending_session_count > 0U) {
        session = g_pending_cloud_sessions[g_pending_session_read];
        g_pending_cloud_sessions[g_pending_session_read] = NULL;
        g_pending_session_read = (uint8_t)((g_pending_session_read + 1U) %
                                           TIRTC_DEMO_SESSION_QUEUE_DEPTH);
        --g_pending_session_count;
        if (g_pending_session_count > 0U) {
            g_worker_events |= TIRTC_DEMO_WORK_SESSION_READY;
        }
    }
    tirtc_demo_unlock(flags);
    return session;
}

static void tirtc_demo_drop_pending_cloud_sessions(void)
{
    tirtc_demo_cloud_session_t *pending[TIRTC_DEMO_SESSION_QUEUE_DEPTH] = {0};
    uint8_t count = 0U;
    unsigned long flags;

    tirtc_demo_lock(&flags);
    while (g_pending_session_count > 0U &&
           count < TIRTC_DEMO_SESSION_QUEUE_DEPTH) {
        pending[count++] = g_pending_cloud_sessions[g_pending_session_read];
        g_pending_cloud_sessions[g_pending_session_read] = NULL;
        g_pending_session_read = (uint8_t)((g_pending_session_read + 1U) %
                                           TIRTC_DEMO_SESSION_QUEUE_DEPTH);
        --g_pending_session_count;
    }
    g_pending_session_read = 0U;
    g_pending_session_write = 0U;
    g_worker_events &= ~TIRTC_DEMO_WORK_SESSION_READY;
    tirtc_demo_unlock(flags);

    for (uint8_t i = 0U; i < count; ++i) {
        if (pending[i] != NULL) {
            tirtc_demo_cloud_notify_session_ended(
                pending[i]->type, "TiRTC 服务正在重启，会话已取消。");
            memset(pending[i], 0, sizeof(*pending[i]));
            free(pending[i]);
        }
    }
}

static bool tirtc_demo_showcase_command(uint16_t command)
{
    switch (command) {
    case TIRTC_DEMO_CMD_CALL:
    case TIRTC_DEMO_CMD_VOLUME:
    case TIRTC_DEMO_CMD_DOOR:
    case TIRTC_DEMO_CMD_WEB_HANGUP:
    case TIRTC_DEMO_CMD_REQUEST_VIDEO:
    case TIRTC_DEMO_CMD_REQUEST_AUDIO:
    case TIRTC_DEMO_CMD_SET_SEND_VIDEO:
    case TIRTC_DEMO_CMD_SET_SEND_AUDIO:
    case TIRTC_DEMO_CMD_STATE:
    case TIRTC_DEMO_CMD_TIME_QUERY:
    case TIRTC_DEMO_CMD_CALL_CONNECTED:
    case TIRTC_DEMO_CMD_CALL_HANGUP:
        return true;
    default:
        return false;
    }
}

static uint16_t tirtc_demo_command_id(uint32_t command)
{
    uint16_t low = (uint16_t)(command & 0xffffU);
    uint16_t direct = (uint16_t)(low & ~TIRTC_DEMO_CMD_DIRECT_RESPONSE_BIT);

    if (tirtc_demo_showcase_command(direct)) {
        return direct;
    }
    if (command == TIRTC_DEMO_CMD_AI_SIGNALING ||
        (command & ~RESPONSE_BIT) == TIRTC_DEMO_CMD_AI_SIGNALING) {
        return TIRTC_DEMO_CMD_AI_SIGNALING;
    }
    return (uint16_t)GET_CMD(command);
}

static bool tirtc_demo_command_is_response(uint32_t command)
{
    uint16_t low = (uint16_t)(command & 0xffffU);

    if (tirtc_demo_showcase_command(
            (uint16_t)(low & ~TIRTC_DEMO_CMD_DIRECT_RESPONSE_BIT))) {
        return (low & TIRTC_DEMO_CMD_DIRECT_RESPONSE_BIT) != 0U;
    }
    return (command & RESPONSE_BIT) != 0U;
}

static bool tirtc_demo_command_matches(uint32_t command, uint16_t expected)
{
    return tirtc_demo_command_id(command) == expected;
}

static uint32_t tirtc_demo_response_command(uint32_t request)
{
    uint16_t low = (uint16_t)(request & 0xffffU);

    if (tirtc_demo_showcase_command(
            (uint16_t)(low & ~TIRTC_DEMO_CMD_DIRECT_RESPONSE_BIT))) {
        return request | TIRTC_DEMO_CMD_DIRECT_RESPONSE_BIT;
    }
    return request | RESPONSE_BIT;
}

static uint32_t tirtc_demo_load_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void tirtc_demo_normalize_command(tirtc_demo_command_packet_t *packet)
{
    uint32_t embedded;
    uint16_t embedded_id;
    uint16_t current_id;

    if (packet == NULL || packet->length < sizeof(uint32_t)) {
        return;
    }
    embedded = tirtc_demo_load_le32((const uint8_t *)packet->data);
    embedded_id = tirtc_demo_command_id(embedded);
    current_id = tirtc_demo_command_id(packet->command);
    if (!tirtc_demo_showcase_command(embedded_id) ||
        (tirtc_demo_showcase_command(current_id) && embedded_id != current_id)) {
        return;
    }
    packet->command = embedded;
    packet->length = (uint16_t)(packet->length - sizeof(uint32_t));
    memmove(packet->data, packet->data + sizeof(uint32_t), packet->length);
    packet->data[packet->length] = '\0';
}

static int tirtc_demo_send_command_response(tirtc_conn_t connection,
                                             uint32_t request,
                                             const void *data,
                                             size_t length)
{
    if (connection == NULL || length > UINT32_MAX) {
        return TIRTC_E_INVALID_PARAMETER;
    }
    return tirtc_demo_sdk_send_command(
        connection, tirtc_demo_response_command(request), data,
        (uint32_t)length);
}

static int tirtc_demo_send_text_response(tirtc_conn_t connection,
                                          uint32_t request,
                                          const char *text)
{
    return tirtc_demo_send_command_response(connection, request, text,
                                             text != NULL ? strlen(text) : 0U);
}

static bool tirtc_demo_parse_assignment(const char *data, size_t length,
                                         char key, int maximum, int *value)
{
    char text[TIRTC_DEMO_COMMAND_DATA_MAX];
    const char *number;
    char *end = NULL;
    long parsed;

    if (data == NULL || value == NULL || length == 0U ||
        length >= sizeof(text)) {
        return false;
    }
    if (length == 1U && (uint8_t)data[0] <= 1U) {
        *value = data[0];
        return true;
    }
    memcpy(text, data, length);
    text[length] = '\0';
    if (text[0] == '{') {
        char name[2] = {key, '\0'};
        cJSON *root = cJSON_Parse(text);
        const cJSON *item = root != NULL ?
            cJSON_GetObjectItemCaseSensitive(root, name) : NULL;

        if (cJSON_IsBool(item)) {
            *value = cJSON_IsTrue(item) ? 1 : 0;
            cJSON_Delete(root);
            return true;
        }
        if (cJSON_IsNumber(item) && item->valueint >= 0 &&
            item->valueint <= maximum) {
            *value = item->valueint;
            cJSON_Delete(root);
            return true;
        }
        cJSON_Delete(root);
        return false;
    }
    number = text;
    if (text[0] == key && text[1] == '=') {
        number += 2;
    } else if (strcmp(text, "true") == 0 || strcmp(text, "on") == 0) {
        *value = 1;
        return true;
    } else if (strcmp(text, "false") == 0 || strcmp(text, "off") == 0) {
        *value = 0;
        return true;
    }
    parsed = strtol(number, &end, 10);
    if (end == number || parsed < 0 || parsed > maximum) {
        return false;
    }
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
        ++end;
    }
    if (*end != '\0') {
        return false;
    }
    *value = (int)parsed;
    return true;
}

static void tirtc_demo_sdk_event(int event, const void *data, int len)
{
    unsigned long flags;
    bool starting;
    bool started;
    bool stopping;
    bool restart_pending;
    const char *event_name = "unknown";

    (void)data;
    (void)len;

    tirtc_demo_lock(&flags);
    if (event == TIRTC_EVENT_SYS_STARTED) {
        event_name = "started";
        g_sdk_starting = false;
        g_sdk_started = true;
        g_runtime.sdk_ready = true;
        g_runtime.service_starting = false;
        g_status.state = TIRTC_DEMO_STATE_READY;
        g_status.last_error = 0;
        g_status.stage = "在线";
        g_status.message = "TiRTC 设备服务已上线，正在等待对端连接。";
    } else if (event == TIRTC_EVENT_SYS_STOPPED) {
        event_name = "stopped";
        g_sdk_starting = false;
        g_sdk_started = false;
        g_sdk_stopping = false;
        g_runtime.sdk_ready = false;
        g_runtime.service_starting = false;
        g_status.state = TIRTC_DEMO_STATE_READY;
        g_status.last_error = 0;
        g_status.stage = "重启";
        g_status.message = "TiRTC 服务已停止，正在应用新配置。";
    } else if (event == TIRTC_EVENT_ACCESS_HIJACKING) {
        event_name = "access-hijacking";
        g_status.state = TIRTC_DEMO_STATE_ERROR;
        g_status.last_error = TIRTC_E_UNEXPECTED_RESPONSE;
        g_status.stage = "网络安全";
        g_status.message = "服务请求发生重定向，请检查当前网络。";
    }
    starting = g_sdk_starting;
    started = g_sdk_started;
    stopping = g_sdk_stopping;
    restart_pending = g_restart_pending;
    tirtc_demo_unlock(flags);

    printf("[tirtc_demo] sdk event=%d name=%s starting=%d started=%d "
           "stopping=%d restart_pending=%d\n",
           event, event_name, starting ? 1 : 0, started ? 1 : 0,
           stopping ? 1 : 0, restart_pending ? 1 : 0);

    if (event == TIRTC_EVENT_SYS_STOPPED) {
        tirtc_demo_schedule_work(TIRTC_DEMO_WORK_RESTART);
    }
}

static void tirtc_demo_conn_accepted(tirtc_conn_t connection)
{
    tirtc_conn_t duplicate = NULL;
    thread_ptr_t worker = NULL;
    unsigned long flags;

    tirtc_demo_lock(&flags);
    if (g_external_media_active || g_active_conn != NULL || g_connect_inflight ||
        g_session_type != TIRTC_DEMO_SESSION_NONE) {
        duplicate = connection;
        (void)tirtc_demo_queue_disconnect_locked(duplicate, false);
    } else {
        ++g_session_generation;
        if (g_session_generation == 0U) {
            g_session_generation = 1U;
        }
        g_session_type = TIRTC_DEMO_SESSION_MONITOR;
        tirtc_demo_copy_text(g_session_type_text,
                             sizeof(g_session_type_text), "monitor");
        g_runtime.call_phase = "connecting";
        memset(&g_session, 0, sizeof(g_session));
        g_connect_result_conn = connection;
        g_connect_result_error = 0;
        g_connect_result_generation = g_session_generation;
        g_worker_events |= TIRTC_DEMO_WORK_CONNECT_RESULT;
        worker = g_worker_thread;
    }
    tirtc_demo_unlock(flags);

    if (duplicate != NULL) {
        tirtc_demo_schedule_work(TIRTC_DEMO_WORK_DISCONNECT);
    } else if (worker != NULL) {
        thread_wakeup(worker);
    }
}

static void tirtc_demo_connect_result(int error, tirtc_conn_t connection,
                                      void *user_data)
{
    uint32_t generation = (uint32_t)(uintptr_t)user_data;
    tirtc_conn_t stale_connection = NULL;
    thread_ptr_t worker = NULL;
    unsigned long flags;

    tirtc_demo_lock(&flags);
    if (generation != g_session_generation || !g_connect_inflight) {
        stale_connection = connection;
        if (stale_connection != NULL) {
            (void)tirtc_demo_queue_disconnect_locked(stale_connection, false);
        }
    } else {
        g_connect_result_conn = connection;
        g_connect_result_error = error;
        g_connect_result_generation = generation;
        g_worker_events |= TIRTC_DEMO_WORK_CONNECT_RESULT;
    }
    worker = g_worker_thread;
    tirtc_demo_unlock(flags);
    if (worker != NULL) {
        thread_wakeup(worker);
    }
}

static void tirtc_demo_conn_error(tirtc_conn_t connection, int error)
{
    unsigned long flags;

    tirtc_demo_lock(&flags);
    (void)tirtc_demo_queue_disconnect_locked(connection, false);
    g_status.state = TIRTC_DEMO_STATE_ERROR;
    g_status.last_error = error;
    g_status.stage = "连接结束";
    g_status.message = "对端连接已结束，设备监听服务保持在线。";
    tirtc_demo_unlock(flags);

    tirtc_demo_schedule_work(TIRTC_DEMO_WORK_DISCONNECT);
}

static void tirtc_demo_disconnected(tirtc_conn_t connection)
{
    unsigned long flags;

    tirtc_demo_lock(&flags);
    (void)tirtc_demo_queue_disconnect_locked(connection, true);
    tirtc_demo_unlock(flags);

    tirtc_demo_schedule_work(TIRTC_DEMO_WORK_DISCONNECT);
}

static void tirtc_demo_audio_received(tirtc_conn_t connection,
                                      const TIRTCFRAMEINFO *frame, void *data)
{
    (void)tirtc_demo_media_submit_remote_audio(connection, frame, data);
}

static void tirtc_demo_video_received(tirtc_conn_t connection,
                                      const TIRTCFRAMEINFO *frame, void *data)
{
    (void)tirtc_demo_media_submit_remote_video(connection, frame, data);
}

static void tirtc_demo_message_received(tirtc_conn_t connection,
                                        const TIRTCFRAMEINFO *frame, void *data)
{
    (void)connection;
    (void)frame;
    (void)data;
}

static void tirtc_demo_command_received(tirtc_conn_t connection, uint32_t command,
                                        const void *data, uint32_t len)
{
    tirtc_demo_command_packet_t *packet;
    thread_ptr_t worker = NULL;
    bool critical;
    unsigned long flags;

    if (len >= TIRTC_DEMO_COMMAND_DATA_MAX || (len > 0U && data == NULL)) {
        return;
    }
    tirtc_demo_lock(&flags);
    if (connection == g_active_conn &&
        !tirtc_demo_command_is_response(command) &&
        (tirtc_demo_command_matches(command, TIRTC_DEMO_CMD_CALL_HANGUP) ||
         tirtc_demo_command_matches(command, TIRTC_DEMO_CMD_WEB_HANGUP))) {
        (void)tirtc_demo_queue_disconnect_locked(connection, false);
        worker = g_worker_thread;
    } else if (connection == g_active_conn) {
        critical = tirtc_demo_command_matches(
                       command, TIRTC_DEMO_CMD_CALL_CONNECTED) ||
                   tirtc_demo_command_matches(command,
                                              TIRTC_DEMO_CMD_CALL) ||
                   (g_session_type == TIRTC_DEMO_SESSION_AI &&
                    g_ai_start_waiting &&
                    tirtc_demo_command_matches(
                        command, TIRTC_DEMO_CMD_AI_SIGNALING));
        if (critical &&
            g_command_count >= TIRTC_DEMO_COMMAND_QUEUE_DEPTH) {
            g_command_read = 0U;
            g_command_write = 0U;
            g_command_count = 0U;
        }
    }
    if (connection == g_active_conn &&
        !(!tirtc_demo_command_is_response(command) &&
          (tirtc_demo_command_matches(command, TIRTC_DEMO_CMD_CALL_HANGUP) ||
           tirtc_demo_command_matches(command, TIRTC_DEMO_CMD_WEB_HANGUP))) &&
        g_command_count < TIRTC_DEMO_COMMAND_QUEUE_DEPTH) {
        packet = &g_command_queue[g_command_write];
        packet->connection = connection;
        packet->command = command;
        packet->length = (uint16_t)len;
        if (len > 0U) {
            memcpy(packet->data, data, len);
        }
        packet->data[len] = '\0';
        g_command_write = (uint8_t)((g_command_write + 1U) %
                                    TIRTC_DEMO_COMMAND_QUEUE_DEPTH);
        ++g_command_count;
        g_worker_events |= TIRTC_DEMO_WORK_COMMAND;
        worker = g_worker_thread;
    }
    tirtc_demo_unlock(flags);
    if (worker != NULL) {
        thread_wakeup(worker);
    }
}

static void tirtc_demo_key_frame_requested(tirtc_conn_t connection, uint8_t stream_id)
{
    (void)connection;
    (void)stream_id;
    tirtc_demo_media_request_key_frame();
}

static int tirtc_demo_video_subscribe(tirtc_conn_t connection, uint8_t stream_id)
{
    tirtc_demo_session_type_t type;
    unsigned long flags;

    tirtc_demo_lock(&flags);
    type = g_session_type;
    if (connection != g_active_conn ||
        (type != TIRTC_DEMO_SESSION_MONITOR &&
         type != TIRTC_DEMO_SESSION_DEVICE) ||
        stream_id != TIRTC_DEMO_DEVICE_VIDEO_STREAM_ID) {
        tirtc_demo_unlock(flags);
        return -1;
    }
    g_video_subscribed = true;
    tirtc_demo_unlock(flags);
    tirtc_demo_schedule_work(TIRTC_DEMO_WORK_VIDEO_SUBSCRIBE);
    return 0;
}

static void tirtc_demo_video_unsubscribe(tirtc_conn_t connection, uint8_t stream_id)
{
    bool valid;
    unsigned long flags;

    tirtc_demo_lock(&flags);
    valid = connection == g_active_conn &&
            stream_id == TIRTC_DEMO_DEVICE_VIDEO_STREAM_ID;
    if (valid) {
        g_video_subscribed = false;
    }
    tirtc_demo_unlock(flags);
    if (valid) {
        tirtc_demo_schedule_work(TIRTC_DEMO_WORK_VIDEO_UNSUBSCRIBE);
    }
}

static int tirtc_demo_audio_subscribe(tirtc_conn_t connection, uint8_t stream_id)
{
    tirtc_demo_session_type_t type;
    uint8_t expected_stream;
    unsigned long flags;

    tirtc_demo_lock(&flags);
    type = g_session_type;
    expected_stream = type == TIRTC_DEMO_SESSION_AI ?
                          TIRTC_DEMO_AI_AUDIO_STREAM_ID :
                          TIRTC_DEMO_DEVICE_AUDIO_STREAM_ID;
    if (connection != g_active_conn || type == TIRTC_DEMO_SESSION_NONE ||
        stream_id != expected_stream) {
        tirtc_demo_unlock(flags);
        return -1;
    }
    tirtc_demo_unlock(flags);
    tirtc_demo_schedule_work(TIRTC_DEMO_WORK_AUDIO_SUBSCRIBE);
    return 0;
}

static void tirtc_demo_audio_unsubscribe(tirtc_conn_t connection, uint8_t stream_id)
{
    tirtc_demo_session_type_t type;
    uint8_t expected_stream;
    bool valid;
    unsigned long flags;

    tirtc_demo_lock(&flags);
    type = g_session_type;
    expected_stream = type == TIRTC_DEMO_SESSION_AI ?
                          TIRTC_DEMO_AI_AUDIO_STREAM_ID :
                          TIRTC_DEMO_DEVICE_AUDIO_STREAM_ID;
    valid = connection == g_active_conn && stream_id == expected_stream;
    tirtc_demo_unlock(flags);
    if (valid) {
        tirtc_demo_schedule_work(TIRTC_DEMO_WORK_AUDIO_UNSUBSCRIBE);
    }
}

static const TIRTCCALLBACKS g_tirtc_callbacks = {
    .on_event = tirtc_demo_sdk_event,
    .on_conn_accepted = tirtc_demo_conn_accepted,
    .on_conn_error = tirtc_demo_conn_error,
    .on_disconnected = tirtc_demo_disconnected,
    .on_audio = tirtc_demo_audio_received,
    .on_video = tirtc_demo_video_received,
    .on_message = tirtc_demo_message_received,
    .on_command = tirtc_demo_command_received,
    .on_request_key_frame = tirtc_demo_key_frame_requested,
    .on_subscribe_video = tirtc_demo_video_subscribe,
    .on_unsubscribe_video = tirtc_demo_video_unsubscribe,
    .on_subscribe_audio = tirtc_demo_audio_subscribe,
    .on_unsubscribe_audio = tirtc_demo_audio_unsubscribe,
};

static int tirtc_demo_start_sdk_now(void)
{
    tirtc_demo_config_t config;
    char client_id[13] = {0};
    unsigned long flags;
    int max_connections = 1;
    int network_type = TIRTC_NETCONN_WIFI;
    uint32_t max_send_buffer = 768U * 1024U;
    uint64_t start_begin_ms;
    int result;
    bool initialized_here = false;

    tirtc_demo_refresh_network_state();
    tirtc_demo_app_get_config(&config);

    tirtc_demo_lock(&flags);
    if (g_sdk_started || g_sdk_starting || g_sdk_stopping) {
        tirtc_demo_unlock(flags);
        return 0;
    }
    if (!tirtc_demo_config_valid(&config)) {
        g_runtime.identity_ready = false;
        g_runtime.sdk_ready = false;
        g_runtime.service_starting = false;
        tirtc_demo_unlock(flags);
        tirtc_demo_set_status(TIRTC_DEMO_STATE_UNCONFIGURED,
                              TIRTC_DEMO_ERR_CONFIG_REQUIRED,
                              "配置", "请填写设备 ID 和设备密钥后保存。" );
        return TIRTC_DEMO_ERR_CONFIG_REQUIRED;
    }
    if (!g_runtime.network_ready) {
        g_runtime.sdk_ready = false;
        g_runtime.service_starting = false;
        tirtc_demo_unlock(flags);
        tirtc_demo_set_status(TIRTC_DEMO_STATE_READY,
                              TIRTC_DEMO_ERR_NETWORK_PENDING,
                              "等待网络", "请先通过君正系统设置连接 WiFi。" );
        return TIRTC_DEMO_ERR_NETWORK_PENDING;
    }
    tirtc_demo_unlock(flags);

    if (tirtc_demo_cloud_get_physical_client_id(client_id,
                                                 sizeof(client_id)) != 0) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR,
                              TIRTC_DEMO_ERR_CONFIG_REQUIRED,
                              "配置", "无法读取稳定的设备物理标识。" );
        printf("[tirtc_demo] start blocked physical client id unavailable\n");
        return TIRTC_DEMO_ERR_CONFIG_REQUIRED;
    }

    tirtc_demo_lock(&flags);
    if (!g_sdk_initialized) {
        tirtc_demo_unlock(flags);
        result = TiRtcSetOption(TIRTC_OPT_MAX_SEND_BUFFER, &max_send_buffer,
                                sizeof(max_send_buffer));
        if (result == 0) {
            result = TiRtcInit();
        }
        if (result < 0) {
            tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, result,
                                  "初始化", "TiRTC SDK 初始化失败。" );
            return result;
        }
        initialized_here = true;
        tirtc_demo_lock(&flags);
        g_sdk_initialized = true;
    }
    tirtc_demo_unlock(flags);

    result = TiRtcSetOption(TIRTC_OPT_SERVICE_ENDPOINT, config.endpoint,
                            (uint32_t)strlen(config.endpoint) + 1U);
    if (result == 0) {
        result = TiRtcSetOption(TIRTC_OPT_DEVICE_SECRET_KEY, config.device_secret,
                                (uint32_t)strlen(config.device_secret) + 1U);
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
    if (result < 0) {
        if (initialized_here) {
            TiRtcUninit();
            tirtc_demo_lock(&flags);
            g_sdk_initialized = false;
            tirtc_demo_unlock(flags);
        }
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, result,
                              "配置", "TiRTC SDK 配置项应用失败。" );
        return result;
    }

    tirtc_demo_lock(&flags);
    g_sdk_starting = true;
    g_runtime.service_starting = true;
    g_runtime.sdk_ready = false;
    g_status.state = TIRTC_DEMO_STATE_READY;
    g_status.last_error = 0;
    g_status.stage = "连接服务";
    g_status.message = "正在启动 TiRTC 设备监听服务。";
    tirtc_demo_unlock(flags);

    printf("[tirtc_demo] start begin sdk=%s build=%s device_id_len=%u "
           "client_id=%s secret_len=%u\n",
           TiRtcGetVersion(), TiRtcGetBuildInfo(),
           (unsigned int)strlen(config.device_id), client_id,
           (unsigned int)strlen(config.device_secret));
    start_begin_ms = systick_get_time_ms();
    result = TiRtcStart(config.device_id, &g_tirtc_callbacks);
    printf("[tirtc_demo] start returned result=%d elapsed_ms=%llu\n",
           result,
           (unsigned long long)(systick_get_time_ms() - start_begin_ms));
    if (result != 0) {
        tirtc_demo_lock(&flags);
        g_sdk_starting = false;
        g_runtime.service_starting = false;
        tirtc_demo_unlock(flags);
        TiRtcUninit();
        tirtc_demo_lock(&flags);
        g_sdk_initialized = false;
        tirtc_demo_unlock(flags);
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, result,
                              "启动", "TiRTC 设备服务启动失败。" );
        return result;
    }

    printf("[tirtc_demo] start requested sdk=%s device_id_len=%u "
           "client_id_len=%u\n",
           TiRtcGetVersion(), (unsigned int)strlen(config.device_id),
           (unsigned int)strlen(client_id));
    return 0;
}

static void tirtc_demo_reset_session_locked(void)
{
    memset(&g_session, 0, sizeof(g_session));
    g_session_type = TIRTC_DEMO_SESSION_NONE;
    g_session_connect_allowed = false;
    g_connect_inflight = false;
    g_connect_result_conn = NULL;
    g_connect_result_error = 0;
    g_session_media_started = false;
    g_session_waiting_ready = false;
    g_video_subscribed = false;
    g_monitor_audio_subscribed = false;
    g_monitor_audio_subscribe_attempts = 0U;
    g_monitor_audio_retry_at_ms = 0U;
    g_call_started_at_ms = 0U;
    g_ai_start_pending = false;
    g_ai_start_waiting = false;
    g_ai_start_at_ms = 0U;
    g_ai_start_deadline_ms = 0U;
    g_ai_heartbeat_at_ms = 0U;
    g_session_ready_deadline_ms = 0U;
    g_pending_web_call_command = 0U;
    g_web_call_active = false;
    g_runtime.peer_connected = false;
    g_runtime.call_active = false;
    g_runtime.incoming_call = false;
    g_runtime.wechat_incoming = false;
    g_runtime.audio_tx_active = false;
    g_runtime.audio_rx_active = false;
    g_runtime.video_tx_active = false;
    g_runtime.video_rx_active = false;
    g_runtime.ai_ready = false;
    g_runtime.ai_talking = false;
    g_runtime.call_phase = "idle";
    tirtc_demo_copy_text(g_session_type_text, sizeof(g_session_type_text), "none");
}

static int tirtc_demo_start_session_connect(void)
{
    tirtc_demo_session_type_t type;
    uint32_t generation;
    size_t peer_id_length;
    size_t token_length;
    size_t credentials_length;
    char *credentials;
    char *peer_id;
    char *token;
    unsigned long flags;
    int result;

    tirtc_demo_lock(&flags);
    type = g_session_type;
    if (!g_sdk_started || !g_session_connect_allowed || g_connect_inflight ||
        g_active_conn != NULL || type == TIRTC_DEMO_SESSION_NONE ||
        type == TIRTC_DEMO_SESSION_MONITOR) {
        tirtc_demo_unlock(flags);
        return TIRTC_DEMO_ERR_SERVICE_PENDING;
    }
    generation = g_session_generation;
    peer_id_length = strlen(g_session.peer_id) + 1U;
    token_length = strlen(g_session.token) + 1U;
    if (peer_id_length <= 1U || token_length <= 1U ||
        peer_id_length > TIRTC_DEMO_CLOUD_PEER_ID_MAX ||
        token_length > TIRTC_DEMO_CLOUD_CONNECT_TOKEN_MAX ||
        peer_id_length > SIZE_MAX - token_length) {
        tirtc_demo_unlock(flags);
        return TIRTC_E_INVALID_PARAMETER;
    }
    credentials_length = peer_id_length + token_length;
    g_connect_inflight = true;
    g_runtime.call_phase = "connecting";
    g_status.state = type == TIRTC_DEMO_SESSION_AI ?
                         TIRTC_DEMO_STATE_AI_TALK :
                         TIRTC_DEMO_STATE_CALLING;
    g_status.last_error = 0;
    g_status.stage = "建立会话";
    g_status.message = "正在建立 TiRTC 媒体连接。";
    tirtc_demo_unlock(flags);

    credentials = malloc(credentials_length);
    if (credentials == NULL) {
        tirtc_demo_lock(&flags);
        if (generation == g_session_generation) {
            g_connect_inflight = false;
        }
        tirtc_demo_unlock(flags);
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR,
                              TIRTC_E_LACK_OF_RESOURCE,
                              "建立会话", "会话凭据内存不足。" );
        return TIRTC_E_LACK_OF_RESOURCE;
    }
    peer_id = credentials;
    token = credentials + peer_id_length;

    tirtc_demo_lock(&flags);
    if (generation != g_session_generation ||
        type != g_session_type || !g_connect_inflight ||
        strlen(g_session.peer_id) + 1U != peer_id_length ||
        strlen(g_session.token) + 1U != token_length) {
        if (generation == g_session_generation) {
            g_connect_inflight = false;
        }
        tirtc_demo_unlock(flags);
        memset(credentials, 0, credentials_length);
        free(credentials);
        return TIRTC_DEMO_ERR_SERVICE_PENDING;
    }
    memcpy(peer_id, g_session.peer_id, peer_id_length);
    memcpy(token, g_session.token, token_length);
    tirtc_demo_unlock(flags);

    printf("[tirtc_demo] session connect submit type=%d generation=%lu "
           "peer_id_len=%u token_len=%u stack_free_min=%lu\n",
           (int)type, (unsigned long)generation,
           (unsigned)(peer_id_length - 1U),
           (unsigned)(token_length - 1U),
           (unsigned long)(uxTaskGetStackHighWaterMark(NULL) *
                           sizeof(StackType_t)));

    if (type == TIRTC_DEMO_SESSION_DEVICE) {
        result = TiRtcConnect(peer_id, token, tirtc_demo_connect_result,
                              (void *)(uintptr_t)generation);
    } else {
        result = TiRtcWhipConnect(peer_id, token, tirtc_demo_connect_result,
                                  (void *)(uintptr_t)generation);
    }
    printf("[tirtc_demo] session connect submit returned type=%d "
           "generation=%lu result=%d stack_free_min=%lu\n",
           (int)type, (unsigned long)generation, result,
           (unsigned long)(uxTaskGetStackHighWaterMark(NULL) *
                           sizeof(StackType_t)));
    memset(credentials, 0, credentials_length);
    free(credentials);
    if (result < 0) {
        tirtc_demo_lock(&flags);
        if (generation == g_session_generation) {
            g_connect_result_conn = NULL;
            g_connect_result_error = result;
            g_connect_result_generation = generation;
            g_worker_events |= TIRTC_DEMO_WORK_CONNECT_RESULT;
        }
        tirtc_demo_unlock(flags);
        tirtc_demo_schedule_work(TIRTC_DEMO_WORK_CONNECT_RESULT);
    }
    return result;
}

static void tirtc_demo_wechat_reject_response(const char *body, void *user_data)
{
    (void)body;
    (void)user_data;
}

static int tirtc_demo_reject_wechat_session(
    const tirtc_demo_cloud_session_t *session, int reason)
{
    cJSON *root;
    char *body;
    int result;

    if (session == NULL || session->room_id[0] == '\0') {
        return TIRTC_E_INVALID_PARAMETER;
    }
    root = cJSON_CreateObject();
    if (root == NULL) {
        return TIRTC_E_LACK_OF_RESOURCE;
    }
    cJSON_AddStringToObject(root, "wx_app_id", session->wx_app_id);
    cJSON_AddStringToObject(root, "wx_model_id", session->wx_model_id);
    cJSON_AddStringToObject(root, "wx_session_token",
                           session->wx_session_token);
    cJSON_AddStringToObject(root, "wx_room_id", session->room_id);
    cJSON_AddStringToObject(root, "wx_payload", session->wx_payload);
    cJSON_AddStringToObject(root, "wxa_app_id", session->wx_app_id);
    cJSON_AddStringToObject(root, "wxa_model_id", session->wx_model_id);
    cJSON_AddStringToObject(root, "wxa_session_token",
                           session->wx_session_token);
    cJSON_AddStringToObject(root, "wxa_room_id", session->room_id);
    cJSON_AddStringToObject(root, "wxa_payload", session->wx_payload);
    cJSON_AddNumberToObject(root, "hangup_reason", reason);
    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return TIRTC_E_LACK_OF_RESOURCE;
    }
    result = TiRtcServiceRequest("/v1/wxvoip/reject", body, NULL,
                                 tirtc_demo_wechat_reject_response, NULL);
    memset(body, 0, strlen(body));
    free(body);
    return result;
}

static void tirtc_demo_process_cloud_session(
    tirtc_demo_cloud_session_t *session)
{
    tirtc_demo_session_type_t type;
    tirtc_demo_cloud_runtime_state_t cloud_state;
    bool busy;
    bool cloud_conflict;
    bool accepted = false;
    bool connect_allowed = false;
    unsigned long flags;

    if (session == NULL) {
        return;
    }
    type = tirtc_demo_session_from_cloud(session->type);
    tirtc_demo_cloud_get_runtime_state(&cloud_state);
    cloud_conflict = cloud_state.call_state !=
                         TIRTC_DEMO_CLOUD_CALL_IDLE &&
                     cloud_state.session_type !=
                         TIRTC_DEMO_CLOUD_SESSION_NONE &&
                     cloud_state.session_type != session->type;
    cloud_conflict = cloud_conflict ||
                     (session->incoming &&
                      cloud_state.call_state !=
                          TIRTC_DEMO_CLOUD_CALL_IDLE &&
                      cloud_state.call_state !=
                          TIRTC_DEMO_CLOUD_CALL_INCOMING);
    tirtc_demo_lock(&flags);
    busy = g_session_type != TIRTC_DEMO_SESSION_NONE ||
           g_active_conn != NULL || g_connect_inflight ||
           g_external_media_active || cloud_conflict;
    if (!busy && type != TIRTC_DEMO_SESSION_NONE) {
        ++g_session_generation;
        if (g_session_generation == 0U) {
            g_session_generation = 1U;
        }
        memcpy(&g_session, session, sizeof(g_session));
        g_session_type = type;
        g_session_connect_allowed = !(type == TIRTC_DEMO_SESSION_WECHAT &&
                                      session->incoming);
        connect_allowed = g_session_connect_allowed;
        g_runtime.peer_id = g_cloud_peer_id;
        tirtc_demo_copy_text(g_cloud_peer_id, sizeof(g_cloud_peer_id),
                             g_session.peer_id);
        g_runtime.incoming_call = g_session.incoming;
        g_runtime.wechat_incoming = type == TIRTC_DEMO_SESSION_WECHAT &&
                                    g_session.incoming;
        g_runtime.call_phase = connect_allowed ? "connecting" : "incoming";
        tirtc_demo_copy_text(g_session_type_text, sizeof(g_session_type_text),
                             type == TIRTC_DEMO_SESSION_DEVICE ? "device" :
                             type == TIRTC_DEMO_SESSION_WECHAT ? "wechat" : "ai");
        g_status.state = type == TIRTC_DEMO_SESSION_AI ?
                             TIRTC_DEMO_STATE_AI_TALK :
                             TIRTC_DEMO_STATE_CALLING;
        g_status.last_error = 0;
        g_status.stage = type == TIRTC_DEMO_SESSION_WECHAT ?
                             "微信通话" :
                             type == TIRTC_DEMO_SESSION_AI ? "AI 对讲" : "设备通话";
        g_status.message = connect_allowed ?
                               "会话参数已就绪，正在连接。" :
                               "收到微信来电，请选择接听或拒绝。";
        accepted = true;
    }
    tirtc_demo_unlock(flags);

    if (!accepted) {
        if (busy && type == TIRTC_DEMO_SESSION_WECHAT && session->incoming) {
            (void)tirtc_demo_reject_wechat_session(session, 5);
        }
        if (type != TIRTC_DEMO_SESSION_NONE &&
            !(busy && type == TIRTC_DEMO_SESSION_WECHAT &&
              session->incoming)) {
            tirtc_demo_cloud_notify_session_ended(
                session->type, busy ? "当前设备忙，未建立新会话。" :
                                     "会话类型无效。");
        }
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR,
                              TIRTC_DEMO_CLOUD_ERR_BUSY,
                              "会话", "当前已有会话，新的会话请求未执行。");
        memset(session, 0, sizeof(*session));
        free(session);
        return;
    }

    memset(session, 0, sizeof(*session));
    free(session);
    /* The worker starts the connection after this frame returns. Keeping the
     * SDK's synchronous WHIP path out of session parsing preserves stack headroom. */
}

static int tirtc_demo_send_device_connected(tirtc_conn_t connection,
                                             const char *room_id)
{
    char payload[160];
    int length;

    if (connection == NULL || room_id == NULL || room_id[0] == '\0') {
        return TIRTC_E_INVALID_PARAMETER;
    }
    length = snprintf(payload, sizeof(payload),
                      "{\"room_id\":\"%s\"}", room_id);
    if (length <= 0 || length >= (int)sizeof(payload)) {
        return TIRTC_E_INVALID_PARAMETER;
    }
    return tirtc_demo_sdk_send_command(connection,
                                       TIRTC_DEMO_CMD_CALL_CONNECTED,
                                       payload, (uint32_t)length);
}

static int tirtc_demo_subscribe_monitor_audio(tirtc_conn_t connection)
{
    uint32_t attempt = 0U;
    bool valid;
    bool log_result = false;
    unsigned long flags;
    int result;

    if (connection == NULL) {
        return TIRTC_E_INVALID_HANDLE;
    }
    result = tirtc_demo_sdk_subscribe_audio(
        connection, TIRTC_DEMO_DEVICE_AUDIO_STREAM_ID);

    tirtc_demo_lock(&flags);
    valid = connection == g_active_conn &&
            g_session_type == TIRTC_DEMO_SESSION_MONITOR &&
            g_session_media_started;
    if (valid) {
        if (g_monitor_audio_subscribe_attempts != UINT32_MAX) {
            ++g_monitor_audio_subscribe_attempts;
        }
        attempt = g_monitor_audio_subscribe_attempts;
        if (result >= 0) {
            g_monitor_audio_subscribed = true;
            g_monitor_audio_retry_at_ms = 0U;
            g_runtime.audio_rx_active = true;
            log_result = true;
        } else {
            g_monitor_audio_subscribed = false;
            g_monitor_audio_retry_at_ms = systick_get_time_ms() +
                                              TIRTC_DEMO_REMOTE_AUDIO_RETRY_MS;
            g_runtime.audio_rx_active = false;
            log_result = attempt <= 2U ||
                         (attempt != 0U && (attempt & (attempt - 1U)) == 0U);
        }
    }
    tirtc_demo_unlock(flags);

    if (valid && log_result) {
        printf("[tirtc_demo] monitor remote audio subscribe stream=%u result=%d attempt=%lu\n",
               (unsigned)TIRTC_DEMO_DEVICE_AUDIO_STREAM_ID, result,
               (unsigned long)attempt);
    }
    return valid ? result : TIRTC_E_INVALID_HANDLE;
}

static void tirtc_demo_activate_media(tirtc_demo_session_type_t type)
{
    tirtc_conn_t connection;
    bool enable_video;
    bool media_started;
    uint64_t started_at_ms;
    unsigned long flags;
    int result;

    tirtc_demo_lock(&flags);
    connection = g_active_conn;
    if (connection == NULL || type != g_session_type) {
        tirtc_demo_unlock(flags);
        return;
    }
    media_started = g_session_media_started;
    tirtc_demo_unlock(flags);

    if (!media_started) {
        uint64_t media_begin_ms = systick_get_time_ms();

        result = tirtc_demo_media_start(connection,
                                        tirtc_demo_media_mode(type), false);
        if (result != 0) {
            printf("[tirtc_demo] confirmed media_start failed type=%d result=%d\n",
                   (int)type, result);
            tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, result,
                                  "媒体", "音频媒体初始化失败。");
            tirtc_demo_lock(&flags);
            if (connection == g_active_conn && type == g_session_type) {
                (void)tirtc_demo_queue_disconnect_locked(connection, false);
            }
            tirtc_demo_unlock(flags);
            tirtc_demo_schedule_work(TIRTC_DEMO_WORK_DISCONNECT);
            return;
        }
        tirtc_demo_lock(&flags);
        if (connection != g_active_conn || type != g_session_type) {
            tirtc_demo_unlock(flags);
            tirtc_demo_media_stop(connection);
            return;
        }
        g_session_media_started = true;
        tirtc_demo_unlock(flags);
        printf("[tirtc_demo] confirmed media_start ready type=%d elapsed=%lums\n",
               (int)type,
               (unsigned long)(systick_get_time_ms() - media_begin_ms));
    }
    result = tirtc_demo_media_set_uplink(true);
    if (result != 0) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, result,
                              "媒体", "麦克风上行启动失败。");
        return;
    }

    started_at_ms = systick_get_time_ms();
    tirtc_demo_lock(&flags);
    g_session_waiting_ready = false;
    g_session_ready_deadline_ms = 0U;
    if (type == TIRTC_DEMO_SESSION_AI) {
        g_ai_start_waiting = false;
        g_ai_start_deadline_ms = 0U;
    }
    g_runtime.call_active = type != TIRTC_DEMO_SESSION_MONITOR;
    g_runtime.audio_tx_active = true;
    g_runtime.audio_rx_active = true;
    g_runtime.ai_ready = type == TIRTC_DEMO_SESSION_AI;
    g_runtime.ai_talking = type == TIRTC_DEMO_SESSION_AI;
    if (g_call_started_at_ms == 0U) {
        g_call_started_at_ms = started_at_ms;
    }
    g_runtime.call_phase = "active";
    g_status.state = type == TIRTC_DEMO_SESSION_AI ?
                         TIRTC_DEMO_STATE_AI_TALK :
                         type == TIRTC_DEMO_SESSION_MONITOR ?
                             TIRTC_DEMO_STATE_CONNECTED :
                             TIRTC_DEMO_STATE_IN_CALL;
    g_status.last_error = 0;
    g_status.stage = type == TIRTC_DEMO_SESSION_AI ? "AI 对讲" : "媒体";
    g_status.message = type == TIRTC_DEMO_SESSION_AI ?
                           "AI 会话已建立，可以开始对讲。" :
                           "音频媒体已建立。";
    if (type == TIRTC_DEMO_SESSION_AI) {
        g_ai_heartbeat_at_ms = systick_get_time_ms() + 30000U;
    }
    enable_video = g_video_subscribed &&
                   (type == TIRTC_DEMO_SESSION_MONITOR ||
                    type == TIRTC_DEMO_SESSION_DEVICE);
    tirtc_demo_unlock(flags);
    if (enable_video) {
        result = tirtc_demo_media_set_video(true);
        tirtc_demo_lock(&flags);
        g_runtime.video_tx_active = result == 0;
        tirtc_demo_unlock(flags);
        if (result != 0) {
            tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, result,
                                  "视频", "摄像头视频上行启动失败。");
        }
    }
    tirtc_demo_cloud_notify_session_active(tirtc_demo_cloud_session_type(type));
}

static char *tirtc_demo_build_ai_start_json(void)
{
    tirtc_demo_config_t config;
    char role_id[TIRTC_DEMO_CLOUD_AI_ROLE_ID_MAX];
    const char *query;
    cJSON *root = NULL;
    cJSON *params = NULL;
    cJSON *input = NULL;
    cJSON *output = NULL;
    char *json = NULL;

    tirtc_demo_app_get_config(&config);
    tirtc_demo_copy_text(role_id, sizeof(role_id), g_session.role_id);
    if (role_id[0] == '\0') {
        query = strstr(g_session.peer_id, "role_id=");
        if (query != NULL) {
            size_t length = strcspn(query + 8, "&");
            if (length < sizeof(role_id)) {
                memcpy(role_id, query + 8, length);
                role_id[length] = '\0';
            }
        }
    }
    root = cJSON_CreateObject();
    params = cJSON_CreateObject();
    input = cJSON_CreateObject();
    output = cJSON_CreateObject();
    if (root == NULL || params == NULL || input == NULL || output == NULL) {
        goto done;
    }
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "id", TIRTC_DEMO_AI_START_REQUEST_ID);
    cJSON_AddStringToObject(root, "method", "start_session");
    cJSON_AddStringToObject(params, "device_id", config.device_id);
    cJSON_AddStringToObject(params, "user_id", "user-001");
    cJSON_AddStringToObject(params, "role_id", role_id);
    cJSON_AddStringToObject(input, "codec", "pcm");
    cJSON_AddNumberToObject(input, "sample_rate", 16000);
    cJSON_AddNumberToObject(input, "channels", 1);
    cJSON_AddStringToObject(output, "codec", "opus");
    cJSON_AddNumberToObject(output, "sample_rate", 16000);
    cJSON_AddNumberToObject(output, "channels", 1);
    cJSON_AddItemToObject(params, "input_audio", input);
    input = NULL;
    cJSON_AddItemToObject(params, "output_audio", output);
    output = NULL;
    cJSON_AddItemToObject(root, "params", params);
    params = NULL;
    json = cJSON_PrintUnformatted(root);

done:
    cJSON_Delete(root);
    cJSON_Delete(params);
    cJSON_Delete(input);
    cJSON_Delete(output);
    memset(role_id, 0, sizeof(role_id));
    return json;
}

static int tirtc_demo_send_ai_method(const char *method)
{
    tirtc_conn_t connection;
    char payload[96];
    unsigned long flags;
    int length;

    if (method == NULL || method[0] == '\0') {
        return TIRTC_E_INVALID_PARAMETER;
    }
    tirtc_demo_lock(&flags);
    connection = g_active_conn;
    if (g_session_type != TIRTC_DEMO_SESSION_AI) {
        connection = NULL;
    }
    tirtc_demo_unlock(flags);
    if (connection == NULL) {
        return TIRTC_E_INVALID_HANDLE;
    }
    length = snprintf(payload, sizeof(payload),
                      "{\"jsonrpc\":\"2.0\",\"method\":\"%s\"}",
                      method);
    if (length <= 0 || length >= (int)sizeof(payload)) {
        return TIRTC_E_INVALID_PARAMETER;
    }
    return tirtc_demo_sdk_send_command(connection,
                                       TIRTC_DEMO_CMD_AI_SIGNALING,
                                       payload, (uint32_t)length);
}

static void tirtc_demo_send_ai_start(void)
{
    tirtc_conn_t connection;
    char *json;
    unsigned long flags;
    int result;

    tirtc_demo_lock(&flags);
    connection = g_active_conn;
    if (g_session_type != TIRTC_DEMO_SESSION_AI || !g_ai_start_pending) {
        connection = NULL;
    }
    g_ai_start_pending = false;
    tirtc_demo_unlock(flags);
    if (connection == NULL) {
        return;
    }
    json = tirtc_demo_build_ai_start_json();
    if (json == NULL) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR,
                              TIRTC_E_LACK_OF_RESOURCE,
                              "AI 对讲", "AI 会话请求内存不足。");
        return;
    }
    result = tirtc_demo_sdk_send_command(connection,
                                         TIRTC_DEMO_CMD_AI_SIGNALING,
                                         json, (uint32_t)strlen(json));
    memset(json, 0, strlen(json));
    free(json);
    if (result <= 0) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, result,
                              "AI 对讲", "AI start_session 发送失败。");
        tirtc_demo_lock(&flags);
        (void)tirtc_demo_queue_disconnect_locked(connection, false);
        tirtc_demo_unlock(flags);
        tirtc_demo_schedule_work(TIRTC_DEMO_WORK_DISCONNECT);
    } else {
        tirtc_demo_lock(&flags);
        if (g_active_conn == connection &&
            g_session_type == TIRTC_DEMO_SESSION_AI) {
            g_ai_start_waiting = true;
            g_ai_start_deadline_ms = systick_get_time_ms() +
                                     TIRTC_DEMO_AI_START_TIMEOUT_MS;
        }
        tirtc_demo_unlock(flags);
        tirtc_demo_set_status(TIRTC_DEMO_STATE_AI_TALK, 0,
                              "AI 对讲", "等待 AI 会话确认。");
    }
}

static void tirtc_demo_start_subscribed_video(void)
{
    tirtc_conn_t connection;
    bool enable;
    int media_result;
    unsigned long flags;

    tirtc_demo_lock(&flags);
    connection = g_active_conn;
    enable = g_video_subscribed && !g_runtime.video_tx_active &&
             g_session_media_started &&
             connection != NULL &&
             (g_session_type == TIRTC_DEMO_SESSION_MONITOR ||
              (g_session_type == TIRTC_DEMO_SESSION_DEVICE &&
               !g_session_waiting_ready));
    if (enable) {
        g_worker_events &= ~TIRTC_DEMO_WORK_VIDEO_SUBSCRIBE;
    }
    tirtc_demo_unlock(flags);
    if (!enable) {
        return;
    }

    media_result = tirtc_demo_media_set_video(true);
    tirtc_demo_lock(&flags);
    if (connection == g_active_conn) {
        g_runtime.video_tx_active = media_result == 0 && g_video_subscribed;
    }
    tirtc_demo_unlock(flags);
    if (media_result != 0) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, media_result,
                              "视频", "摄像头视频上行启动失败。");
    }
}

static void tirtc_demo_process_connect_result(void)
{
    tirtc_conn_t connection;
    tirtc_demo_session_type_t type;
    uint32_t generation;
    bool incoming;
    bool prewarm_media;
    char room_id[TIRTC_DEMO_CLOUD_ROOM_ID_MAX];
    int error;
    int media_result = 0;
    int monitor_audio_result = TIRTC_E_INVALID_HANDLE;
    uint64_t media_begin_ms;
    unsigned long flags;

    tirtc_demo_lock(&flags);
    connection = g_connect_result_conn;
    error = g_connect_result_error;
    generation = g_connect_result_generation;
    g_connect_result_conn = NULL;
    if (generation != g_session_generation) {
        if (connection != NULL) {
            (void)tirtc_demo_queue_disconnect_locked(connection, false);
        }
        tirtc_demo_unlock(flags);
        if (connection != NULL) {
            tirtc_demo_schedule_work(TIRTC_DEMO_WORK_DISCONNECT);
        }
        return;
    }
    type = g_session_type;
    g_connect_inflight = false;
    incoming = g_session.incoming;
    tirtc_demo_copy_text(room_id, sizeof(room_id), g_session.room_id);
    if (error != 0 || connection == NULL) {
        if (connection != NULL) {
            (void)tirtc_demo_queue_disconnect_locked(connection, false);
        }
        tirtc_demo_reset_session_locked();
        g_status.state = TIRTC_DEMO_STATE_ERROR;
        g_status.last_error = error != 0 ? error : TIRTC_E_INTERNAL_ERROR;
        g_status.stage = "建立会话";
        g_status.message = "TiRTC 媒体连接失败。";
        tirtc_demo_unlock(flags);
        printf("[tirtc_demo] session connect result type=%d generation=%lu "
               "error=%d connection=%p\n",
               (int)type, (unsigned long)generation, error,
               (void *)connection);
        tirtc_demo_cloud_notify_session_ended(
            tirtc_demo_cloud_session_type(type), "媒体连接失败。");
        if (connection != NULL) {
            tirtc_demo_schedule_work(TIRTC_DEMO_WORK_DISCONNECT);
        }
        return;
    }
    g_active_conn = connection;
    g_runtime.peer_connected = true;
    g_runtime.audio_rx_active = false;
    g_runtime.call_phase = "media-start";
    g_status.state = TIRTC_DEMO_STATE_CONNECTED;
    g_status.last_error = 0;
    g_status.stage = "建立会话";
    g_status.message = "对端已连接，正在启动音视频。";
    g_session_waiting_ready = type == TIRTC_DEMO_SESSION_DEVICE ||
                              type == TIRTC_DEMO_SESSION_WECHAT ||
                              type == TIRTC_DEMO_SESSION_AI;
    tirtc_demo_unlock(flags);
    printf("[tirtc_demo] session connect result type=%d generation=%lu "
           "error=%d connection=%p\n",
           (int)type, (unsigned long)generation, error, (void *)connection);

    prewarm_media = type == TIRTC_DEMO_SESSION_MONITOR ||
                    type == TIRTC_DEMO_SESSION_AI;
    if (prewarm_media) {
        printf("[tirtc_demo] peer connected type=%d media_start=prewarm\n",
               (int)type);

        media_begin_ms = systick_get_time_ms();
        media_result = tirtc_demo_media_start(
            connection, tirtc_demo_media_mode(type), false);
        if (media_result != 0) {
            printf("[tirtc_demo] peer media_start failed type=%d result=%d\n",
                   (int)type, media_result);
            tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, media_result,
                                  "媒体", "音频媒体初始化失败。");
            tirtc_demo_lock(&flags);
            (void)tirtc_demo_queue_disconnect_locked(connection, false);
            tirtc_demo_unlock(flags);
            tirtc_demo_schedule_work(TIRTC_DEMO_WORK_DISCONNECT);
            return;
        }
        printf("[tirtc_demo] peer media_start ready type=%d elapsed=%lums\n",
               (int)type,
               (unsigned long)(systick_get_time_ms() - media_begin_ms));
    } else {
        printf("[tirtc_demo] peer connected type=%d "
               "media_start=deferred-until-0x2000\n",
               (int)type);
    }
    tirtc_demo_lock(&flags);
    g_session_media_started = prewarm_media;
    if (type == TIRTC_DEMO_SESSION_MONITOR) {
        g_monitor_audio_subscribed = false;
        g_monitor_audio_subscribe_attempts = 0U;
        g_monitor_audio_retry_at_ms = systick_get_time_ms();
    }
    if (type == TIRTC_DEMO_SESSION_DEVICE ||
        type == TIRTC_DEMO_SESSION_WECHAT) {
        g_session_ready_deadline_ms = systick_get_time_ms() +
                                      TIRTC_DEMO_SESSION_READY_TIMEOUT_MS;
    }
    tirtc_demo_unlock(flags);

    if (type == TIRTC_DEMO_SESSION_MONITOR) {
        monitor_audio_result = tirtc_demo_subscribe_monitor_audio(connection);
        tirtc_demo_lock(&flags);
        g_runtime.call_phase = "active";
        tirtc_demo_unlock(flags);
        tirtc_demo_set_status(TIRTC_DEMO_STATE_CONNECTED, 0,
                              "设备查看",
                              monitor_audio_result >= 0 ?
                                  "对端已连接，网页音频下行已启用。" :
                                  "对端已连接，网页音频订阅正在重试。");
        tirtc_demo_start_subscribed_video();
    } else if (type == TIRTC_DEMO_SESSION_DEVICE) {
        tirtc_demo_cloud_notify_rtc_connected();
        if (incoming) {
            int result = tirtc_demo_send_device_connected(connection, room_id);
            if (result > 0) {
                tirtc_demo_activate_media(type);
            } else {
                tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, result,
                                      "设备通话", "房间确认发送失败。");
                tirtc_demo_lock(&flags);
                (void)tirtc_demo_queue_disconnect_locked(connection, false);
                tirtc_demo_unlock(flags);
                tirtc_demo_schedule_work(TIRTC_DEMO_WORK_DISCONNECT);
            }
        } else {
            tirtc_demo_set_status(TIRTC_DEMO_STATE_CALLING, 0,
                                  "设备通话", "等待对端房间确认。");
        }
    } else if (type == TIRTC_DEMO_SESSION_WECHAT) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_CALLING, 0,
                              "微信通话", "等待微信通话连接确认。");
    } else if (type == TIRTC_DEMO_SESSION_AI) {
        tirtc_demo_lock(&flags);
        g_ai_start_pending = true;
        g_ai_start_at_ms = systick_get_time_ms() + TIRTC_DEMO_AI_START_DELAY_MS;
        tirtc_demo_unlock(flags);
        tirtc_demo_set_status(TIRTC_DEMO_STATE_AI_TALK, 0,
                              "AI 对讲", "媒体已预热，准备启动 AI 会话。");
    }
}

static bool tirtc_demo_pop_command(tirtc_demo_command_packet_t *packet)
{
    unsigned long flags;

    if (packet == NULL) {
        return false;
    }
    tirtc_demo_lock(&flags);
    if (g_command_count == 0U) {
        tirtc_demo_unlock(flags);
        return false;
    }
    *packet = g_command_queue[g_command_read];
    g_command_read = (uint8_t)((g_command_read + 1U) %
                               TIRTC_DEMO_COMMAND_QUEUE_DEPTH);
    --g_command_count;
    if (g_command_count > 0U) {
        g_worker_events |= TIRTC_DEMO_WORK_COMMAND;
    }
    tirtc_demo_unlock(flags);
    return true;
}

static bool tirtc_demo_ai_audio_spec_supported(const cJSON *spec,
                                                const char *codec_expected)
{
    const char *codec;
    const cJSON *sample_rate;
    const cJSON *channels;

    if (!cJSON_IsObject(spec)) {
        return true;
    }
    codec = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(spec, "codec"));
    sample_rate = cJSON_GetObjectItemCaseSensitive(spec, "sample_rate");
    channels = cJSON_GetObjectItemCaseSensitive(spec, "channels");
    if (codec == NULL || !cJSON_IsNumber(sample_rate) ||
        !cJSON_IsNumber(channels)) {
        return true;
    }
    return codec_expected != NULL && strcmp(codec, codec_expected) == 0 &&
           sample_rate->valueint == 16000 &&
           channels->valueint == 1;
}

static void tirtc_demo_clear_ai_messages_locked(void)
{
    memset(g_ai_messages, 0, sizeof(g_ai_messages));
    g_ai_message_count = 0U;
    g_ai_caption[0] = '\0';
    ++g_runtime.ai_generation;
    if (g_runtime.ai_generation == 0U) {
        g_runtime.ai_generation = 1U;
    }
}

static void tirtc_demo_update_ai_message_locked(uint8_t caption_type,
                                                 int64_t utterance_id,
                                                 const char *text,
                                                 bool final)
{
    tirtc_demo_ai_message_t *message = NULL;

    if (text == NULL || text[0] == '\0') {
        return;
    }
    for (size_t i = g_ai_message_count; i > 0U; --i) {
        tirtc_demo_ai_message_t *candidate = &g_ai_messages[i - 1U];

        if (candidate->caption_type == caption_type &&
            candidate->utterance_id == utterance_id &&
            (!candidate->final || strcmp(candidate->text, text) == 0)) {
            message = candidate;
            break;
        }
    }
    if (message == NULL) {
        if (g_ai_message_count >= TIRTC_DEMO_AI_MESSAGE_MAX) {
            memmove(&g_ai_messages[0], &g_ai_messages[1],
                    sizeof(g_ai_messages[0]) *
                        (TIRTC_DEMO_AI_MESSAGE_MAX - 1U));
            g_ai_message_count = TIRTC_DEMO_AI_MESSAGE_MAX - 1U;
        }
        message = &g_ai_messages[g_ai_message_count++];
        memset(message, 0, sizeof(*message));
        message->caption_type = caption_type;
        message->utterance_id = utterance_id;
    }
    message->final = final;
    tirtc_demo_copy_utf8(message->text, sizeof(message->text), text);
    tirtc_demo_copy_utf8(g_ai_caption, sizeof(g_ai_caption), text);
    ++g_runtime.ai_generation;
    if (g_runtime.ai_generation == 0U) {
        g_runtime.ai_generation = 1U;
    }
}

static void tirtc_demo_process_ai_command(const tirtc_demo_command_packet_t *packet)
{
    cJSON *root;
    cJSON *result;
    cJSON *error;
    cJSON *params;
    const char *method;
    const char *caption;
    const char *request_id;
    const cJSON *caption_type_item;
    const cJSON *utterance_id_item;
    const cJSON *final_item;
    uint8_t caption_type;
    int64_t utterance_id;
    bool final;
    bool start_ok = false;
    bool start_reply;
    bool start_waiting;
    bool end_session = false;
    bool close_session = false;
    unsigned long flags;

    root = cJSON_Parse(packet->data);
    if (root == NULL) {
        return;
    }
    result = cJSON_GetObjectItemCaseSensitive(root, "result");
    error = cJSON_GetObjectItemCaseSensitive(root, "error");
    method = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(root, "method"));
    request_id = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(root, "id"));
    params = cJSON_GetObjectItemCaseSensitive(root, "params");
    tirtc_demo_lock(&flags);
    start_waiting = g_ai_start_waiting;
    tirtc_demo_unlock(flags);
    start_reply = start_waiting && request_id != NULL &&
                  strcmp(request_id, TIRTC_DEMO_AI_START_REQUEST_ID) == 0;
    if (start_reply && cJSON_IsObject(result) && !cJSON_IsObject(error)) {
        start_ok = tirtc_demo_ai_audio_spec_supported(
                       cJSON_GetObjectItemCaseSensitive(result,
                                                        "input_audio"),
                       "pcm") &&
                   tirtc_demo_ai_audio_spec_supported(
                       cJSON_GetObjectItemCaseSensitive(result,
                                                        "output_audio"),
                       "opus");
        if (!start_ok) {
            tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR,
                                  TIRTC_DEMO_ERR_UNSUPPORTED,
                                  "AI 对讲",
                                  "AI 返回的音频格式不是 PCM/16kHz/单声道。");
            close_session = true;
        }
    }
    if (start_reply && cJSON_IsObject(error)) {
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR,
                              cJSON_IsNumber(code) ? code->valueint : -1,
                              "AI 对讲", "AI start_session 被服务端拒绝。");
        close_session = true;
    } else if (method != NULL &&
               (strcmp(method, "caption") == 0 ||
                strcmp(method, "captions") == 0)) {
        caption = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(params, "text"));
        if (caption != NULL) {
            caption_type_item = cJSON_GetObjectItemCaseSensitive(
                params, "caption_type");
            utterance_id_item = cJSON_GetObjectItemCaseSensitive(
                params, "utterance_id");
            final_item = cJSON_GetObjectItemCaseSensitive(params, "is_final");
            caption_type = cJSON_IsNumber(caption_type_item) &&
                                   caption_type_item->valueint == 0 ?
                               0U : 1U;
            utterance_id = cJSON_IsNumber(utterance_id_item) ?
                               (int64_t)utterance_id_item->valuedouble : 0;
            final = cJSON_IsBool(final_item) ? cJSON_IsTrue(final_item) : true;
            tirtc_demo_lock(&flags);
            tirtc_demo_update_ai_message_locked(caption_type, utterance_id,
                                                 caption, final);
            tirtc_demo_unlock(flags);
            printf("[tirtc_demo] AI caption type=%s final=%u bytes=%lu text=%s\n",
                   caption_type == 0U ? "ASR" : "TTS",
                   final ? 1U : 0U, (unsigned long)strlen(caption), caption);
        }
    } else if (method != NULL && strcmp(method, "interrupt") == 0) {
        tirtc_demo_lock(&flags);
        g_runtime.ai_talking = false;
        tirtc_demo_unlock(flags);
    } else if (method != NULL && strcmp(method, "end_session") == 0) {
        end_session = true;
    }
    cJSON_Delete(root);
    if (start_ok) {
        printf("[tirtc_demo] AI media negotiated input=pcm/16000/1 "
               "output=opus/16000/1 stream=%u auto_downlink=1\n",
               (unsigned)TIRTC_DEMO_AI_AUDIO_STREAM_ID);
        tirtc_demo_activate_media(TIRTC_DEMO_SESSION_AI);
    }
    if (end_session || close_session) {
        tirtc_demo_lock(&flags);
        (void)tirtc_demo_queue_disconnect_locked(packet->connection, false);
        tirtc_demo_unlock(flags);
        tirtc_demo_schedule_work(TIRTC_DEMO_WORK_DISCONNECT);
    }
}

static void tirtc_demo_process_web_call_reply(bool accept)
{
    tirtc_conn_t connection;
    uint32_t request;
    uint64_t started_at_ms = accept ? systick_get_time_ms() : 0U;
    unsigned long flags;
    int result;

    tirtc_demo_lock(&flags);
    connection = g_active_conn;
    request = g_pending_web_call_command;
    if (g_session_type != TIRTC_DEMO_SESSION_MONITOR || request == 0U) {
        connection = NULL;
    }
    tirtc_demo_unlock(flags);
    if (connection == NULL) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR,
                              TIRTC_DEMO_ERR_SERVICE_PENDING,
                              "设备查看", "当前没有待处理的 Web 来电。");
        return;
    }

    result = tirtc_demo_send_text_response(
        connection, request,
        accept ? TIRTC_DEMO_WEB_REPLY_ALLOW : TIRTC_DEMO_WEB_REPLY_REJECT);
    if (accept && result > 0) {
        result = tirtc_demo_media_set_uplink(true);
    } else if (result == 0) {
        result = TIRTC_E_INTERNAL_ERROR;
    }

    tirtc_demo_lock(&flags);
    if (connection == g_active_conn && request == g_pending_web_call_command &&
        result >= 0) {
        g_pending_web_call_command = 0U;
        g_runtime.incoming_call = false;
        g_web_call_active = accept;
        g_runtime.call_active = accept;
        g_runtime.audio_tx_active = accept;
        g_runtime.audio_rx_active = accept;
        g_call_started_at_ms = accept ? started_at_ms : 0U;
        g_runtime.call_phase = accept ? "active" : "ending";
    }
    if (!accept && connection == g_active_conn) {
        (void)tirtc_demo_queue_disconnect_locked(connection, false);
    }
    tirtc_demo_unlock(flags);

    if (result < 0) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, result,
                              "设备查看", "Web 来电响应发送失败。");
    } else if (accept) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_IN_CALL, 0,
                              "设备查看", "Web 通话已接听，音频上行已开启。");
    } else {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_READY, 0,
                              "设备查看", "Web 来电已拒绝。");
        tirtc_demo_schedule_work(TIRTC_DEMO_WORK_DISCONNECT);
    }
}

static void tirtc_demo_process_web_command(
    const tirtc_demo_command_packet_t *packet,
    tirtc_demo_session_type_t type)
{
    uint16_t command = tirtc_demo_command_id(packet->command);
    bool response = tirtc_demo_command_is_response(packet->command);
    unsigned long flags;
    int value;
    int result;

    if (response) {
        return;
    }
    switch (command) {
    case TIRTC_DEMO_CMD_CALL: {
        const char *reply = NULL;

        tirtc_demo_lock(&flags);
        if (type != TIRTC_DEMO_SESSION_MONITOR ||
            packet->connection != g_active_conn) {
            reply = TIRTC_DEMO_WEB_REPLY_BUSY;
        } else if (g_web_call_active) {
            reply = TIRTC_DEMO_WEB_REPLY_ALLOW;
        } else if (g_pending_web_call_command != 0U) {
            reply = TIRTC_DEMO_WEB_REPLY_BUSY;
        } else {
            g_pending_web_call_command = packet->command;
            g_runtime.incoming_call = true;
            g_runtime.call_phase = "incoming";
            g_status.state = TIRTC_DEMO_STATE_CALLING;
            g_status.last_error = 0;
            g_status.stage = "设备查看";
            g_status.message = "收到 Web 来电，请选择接听或拒绝。";
        }
        tirtc_demo_unlock(flags);
        if (reply != NULL) {
            (void)tirtc_demo_send_text_response(packet->connection,
                                                packet->command, reply);
        }
        break;
    }
    case TIRTC_DEMO_CMD_VOLUME:
        if (!tirtc_demo_parse_assignment(packet->data, packet->length,
                                          'v', 100, &value)) {
            (void)tirtc_demo_send_text_response(packet->connection,
                                                packet->command,
                                                TIRTC_DEMO_WEB_REPLY_ERROR);
            break;
        }
        result = tirtc_demo_set_volume_now(TIRTC_DEMO_VOLUME_RX, value, false);
        (void)tirtc_demo_send_text_response(
            packet->connection, packet->command,
            result >= 0 ? TIRTC_DEMO_WEB_REPLY_OK :
                          TIRTC_DEMO_WEB_REPLY_ERROR);
        tirtc_demo_set_status(result >= 0 ? TIRTC_DEMO_STATE_CONNECTED :
                                           TIRTC_DEMO_STATE_ERROR,
                              result >= 0 ? 0 : result, "远端音量",
                              result >= 0 ? "Web 已更新设备播放音量。" :
                                            "Web 音量设置失败。");
        break;
    case TIRTC_DEMO_CMD_DOOR:
        if (!tirtc_demo_parse_assignment(packet->data, packet->length,
                                          'd', 1, &value)) {
            (void)tirtc_demo_send_text_response(packet->connection,
                                                packet->command,
                                                TIRTC_DEMO_WEB_REPLY_ERROR);
            break;
        }
        tirtc_demo_lock(&flags);
        g_runtime.door_open = value != 0;
        tirtc_demo_unlock(flags);
        (void)tirtc_demo_send_text_response(packet->connection,
                                            packet->command,
                                            TIRTC_DEMO_WEB_REPLY_OK);
        tirtc_demo_set_status(TIRTC_DEMO_STATE_CONNECTED, 0,
                              "门禁控制",
                              value != 0 ?
                                  "门禁逻辑已打开；当前板型未配置继电器驱动。" :
                                  "门禁逻辑已关闭；当前板型未配置继电器驱动。");
        break;
    case TIRTC_DEMO_CMD_REQUEST_VIDEO:
    case TIRTC_DEMO_CMD_SET_SEND_VIDEO:
        if (!tirtc_demo_parse_assignment(packet->data, packet->length,
                                          'v', 1, &value)) {
            value = 1;
        }
        result = tirtc_demo_media_set_video(value != 0);
        tirtc_demo_lock(&flags);
        g_runtime.video_tx_active = result == 0 && value != 0;
        tirtc_demo_unlock(flags);
        if (command == TIRTC_DEMO_CMD_REQUEST_VIDEO) {
            (void)tirtc_demo_send_text_response(
                packet->connection, packet->command,
                result == 0 ? TIRTC_DEMO_WEB_REPLY_OK :
                              TIRTC_DEMO_WEB_REPLY_NO_CALL);
        }
        break;
    case TIRTC_DEMO_CMD_REQUEST_AUDIO:
    case TIRTC_DEMO_CMD_SET_SEND_AUDIO:
        if (!tirtc_demo_parse_assignment(packet->data, packet->length,
                                          'a', 1, &value)) {
            value = 1;
        }
        result = tirtc_demo_media_set_uplink(value != 0);
        tirtc_demo_lock(&flags);
        g_runtime.audio_tx_active = result == 0 && value != 0;
        tirtc_demo_unlock(flags);
        if (command == TIRTC_DEMO_CMD_REQUEST_AUDIO) {
            (void)tirtc_demo_send_text_response(
                packet->connection, packet->command,
                result == 0 ? TIRTC_DEMO_WEB_REPLY_OK :
                              TIRTC_DEMO_WEB_REPLY_NO_CALL);
        }
        break;
    case TIRTC_DEMO_CMD_STATE: {
        tirtc_demo_web_state_t state = {0};

        tirtc_demo_lock(&flags);
        state.call_active = g_runtime.call_active ? 1U : 0U;
        state.local_video_send_enabled =
            g_runtime.video_tx_active ? 1U : 0U;
        state.local_audio_send_enabled =
            g_runtime.audio_tx_active ? 1U : 0U;
        state.video_stream_active = state.local_video_send_enabled;
        state.audio_stream_active = g_runtime.audio_rx_active ? 1U : 0U;
        tirtc_demo_unlock(flags);
        (void)tirtc_demo_send_command_response(packet->connection,
                                               packet->command,
                                               &state, sizeof(state));
        break;
    }
    case TIRTC_DEMO_CMD_TIME_QUERY: {
        uint32_t now = (uint32_t)time(NULL);
        uint8_t payload[4] = {
            (uint8_t)(now & 0xffU),
            (uint8_t)((now >> 8) & 0xffU),
            (uint8_t)((now >> 16) & 0xffU),
            (uint8_t)((now >> 24) & 0xffU),
        };

        (void)tirtc_demo_send_command_response(packet->connection,
                                               packet->command,
                                               payload, sizeof(payload));
        break;
    }
    case TIRTC_DEMO_CMD_WEB_HANGUP:
        tirtc_demo_lock(&flags);
        (void)tirtc_demo_queue_disconnect_locked(packet->connection, false);
        tirtc_demo_unlock(flags);
        tirtc_demo_schedule_work(TIRTC_DEMO_WORK_DISCONNECT);
        break;
    default:
        break;
    }
}

static void tirtc_demo_process_command(void)
{
    tirtc_demo_command_packet_t packet;
    tirtc_demo_session_type_t type;
    char expected_room[TIRTC_DEMO_CLOUD_ROOM_ID_MAX];
    unsigned long flags;

    if (!tirtc_demo_pop_command(&packet)) {
        return;
    }
    tirtc_demo_normalize_command(&packet);
    tirtc_demo_lock(&flags);
    type = g_session_type;
    tirtc_demo_copy_text(expected_room, sizeof(expected_room), g_session.room_id);
    tirtc_demo_unlock(flags);
    if (tirtc_demo_command_matches(packet.command,
                                   TIRTC_DEMO_CMD_CALL_CONNECTED) ||
        tirtc_demo_command_matches(packet.command,
                                   TIRTC_DEMO_CMD_CALL_HANGUP) ||
        (type == TIRTC_DEMO_SESSION_AI &&
         tirtc_demo_command_matches(packet.command,
                                    TIRTC_DEMO_CMD_AI_SIGNALING))) {
        printf("[tirtc_demo] session command type=%d command=0x%04lx "
               "response=%d bytes=%u\n",
               (int)type,
               (unsigned long)tirtc_demo_command_id(packet.command),
               tirtc_demo_command_is_response(packet.command) ? 1 : 0,
               (unsigned)packet.length);
    }
    if (tirtc_demo_command_matches(packet.command,
                                   TIRTC_DEMO_CMD_CALL_HANGUP) ||
        tirtc_demo_command_matches(packet.command,
                                   TIRTC_DEMO_CMD_WEB_HANGUP)) {
        tirtc_demo_lock(&flags);
        (void)tirtc_demo_queue_disconnect_locked(packet.connection, false);
        tirtc_demo_unlock(flags);
        tirtc_demo_schedule_work(TIRTC_DEMO_WORK_DISCONNECT);
    } else if (tirtc_demo_command_matches(packet.command,
                                          TIRTC_DEMO_CMD_CALL_CONNECTED)) {
        bool confirmed = type == TIRTC_DEMO_SESSION_WECHAT;

        if (type == TIRTC_DEMO_SESSION_DEVICE && packet.length > 0U) {
            cJSON *root = cJSON_Parse(packet.data);
            const char *room = root != NULL ? cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(root, "room_id")) : NULL;
            confirmed = room != NULL && expected_room[0] != '\0' &&
                        strcmp(room, expected_room) == 0;
            if (confirmed) {
                tirtc_demo_cloud_notify_room_confirmed(room);
            }
            cJSON_Delete(root);
        }
        if (confirmed) {
            tirtc_demo_activate_media(type);
        }
    } else if (type == TIRTC_DEMO_SESSION_AI &&
               tirtc_demo_command_matches(packet.command,
                                           TIRTC_DEMO_CMD_AI_SIGNALING)) {
        tirtc_demo_process_ai_command(&packet);
    } else {
        tirtc_demo_process_web_command(&packet, type);
    }
    memset(&packet, 0, sizeof(packet));
}

static void tirtc_demo_disconnect_now(void)
{
    tirtc_demo_disconnect_request_t request = {0};
    tirtc_demo_session_type_t type = TIRTC_DEMO_SESSION_NONE;
    bool active = false;
    bool media_started = false;
    bool release_monitor_audio = false;
    bool restart_pending;
    unsigned long flags;
    int result = 0;

    if (!tirtc_demo_take_disconnect(&request)) {
        return;
    }
    tirtc_demo_lock(&flags);
    active = g_active_conn == request.connection;
    if (active) {
        type = g_session_type;
        media_started = g_session_media_started;
        release_monitor_audio = g_monitor_audio_subscribed &&
                                type == TIRTC_DEMO_SESSION_MONITOR &&
                                !request.already_closed;
        g_monitor_audio_subscribed = false;
        g_monitor_audio_subscribe_attempts = 0U;
        g_monitor_audio_retry_at_ms = 0U;
        g_active_conn = NULL;
        ++g_session_generation;
        if (g_session_generation == 0U) {
            g_session_generation = 1U;
        }
        g_connect_inflight = false;
        g_connect_result_conn = NULL;
        g_command_read = 0U;
        g_command_write = 0U;
        g_command_count = 0U;
        g_runtime.peer_connected = false;
        g_runtime.call_active = false;
        g_runtime.audio_tx_active = false;
        g_runtime.audio_rx_active = false;
        g_runtime.video_tx_active = false;
        g_runtime.video_rx_active = false;
        g_runtime.call_phase = "ending";
    }
    restart_pending = g_restart_pending;
    tirtc_demo_unlock(flags);

    if (!active) {
        if (!request.already_closed) {
            (void)tirtc_demo_sdk_disconnect(request.connection);
        }
        return;
    }
    if (release_monitor_audio) {
        int unsubscribe_result = tirtc_demo_sdk_unsubscribe_audio(
            request.connection, TIRTC_DEMO_DEVICE_AUDIO_STREAM_ID);

        printf("[tirtc_demo] monitor remote audio unsubscribe stream=%u result=%d\n",
               (unsigned)TIRTC_DEMO_DEVICE_AUDIO_STREAM_ID,
               unsubscribe_result);
    }
    if (media_started) {
        tirtc_demo_media_stop(request.connection);
    }
    if (!request.already_closed) {
        result = tirtc_demo_sdk_disconnect(request.connection);
    }
    if (type != TIRTC_DEMO_SESSION_MONITOR &&
        type != TIRTC_DEMO_SESSION_NONE) {
        tirtc_demo_cloud_notify_rtc_disconnected();
    }

    tirtc_demo_lock(&flags);
    tirtc_demo_reset_session_locked();
    if (result >= 0 || result == TIRTC_E_INVALID_HANDLE) {
        g_status.state = TIRTC_DEMO_STATE_READY;
        g_status.last_error = 0;
        g_status.stage = restart_pending ? "重启" : "在线";
        g_status.message = restart_pending ?
                               "当前会话已释放，正在重启 TiRTC 服务。" :
                               "会话已结束，设备继续等待新的连接。";
    }
    tirtc_demo_unlock(flags);
    if (result < 0 && result != TIRTC_E_INVALID_HANDLE) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, result,
                              "断开连接", "TiRTC 对端连接释放失败。");
    }
}

static int tirtc_demo_send_session_hangup(tirtc_conn_t connection,
                                           tirtc_demo_session_type_t type)
{
    static const char wechat_hangup[] = "{\"reason\":1}";

    switch (type) {
    case TIRTC_DEMO_SESSION_MONITOR:
        return tirtc_demo_sdk_send_command(connection,
                                           TIRTC_DEMO_CMD_WEB_HANGUP,
                                           NULL, 0U);
    case TIRTC_DEMO_SESSION_DEVICE:
        return tirtc_demo_sdk_send_command(connection,
                                           TIRTC_DEMO_CMD_CALL_HANGUP,
                                           NULL, 0U);
    case TIRTC_DEMO_SESSION_WECHAT:
        return tirtc_demo_sdk_send_command(
            connection, TIRTC_DEMO_CMD_CALL_HANGUP,
            wechat_hangup, (uint32_t)(sizeof(wechat_hangup) - 1U));
    case TIRTC_DEMO_SESSION_AI:
        return tirtc_demo_send_ai_method("end_session");
    case TIRTC_DEMO_SESSION_NONE:
    default:
        return 0;
    }
}

static void tirtc_demo_process_local_hangup(void)
{
    tirtc_conn_t connection;
    tirtc_demo_session_type_t type;
    tirtc_demo_cloud_session_t session;
    tirtc_demo_cloud_snapshot_t cloud_snapshot;
    bool cloud_call_active;
    bool queued = false;
    unsigned long flags;
    int command_result = 0;
    int wechat_result = 0;
    int cloud_result = 0;

    tirtc_demo_cloud_get_snapshot(&cloud_snapshot);
    cloud_call_active = cloud_snapshot.call_state !=
                        TIRTC_DEMO_CLOUD_CALL_IDLE;
    tirtc_demo_lock(&flags);
    connection = g_active_conn;
    type = g_session_type;
    session = g_session;
    if (connection != NULL) {
        queued = tirtc_demo_queue_disconnect_locked(connection, false);
    } else if (type != TIRTC_DEMO_SESSION_NONE || cloud_call_active) {
        ++g_session_generation;
        if (g_session_generation == 0U) {
            g_session_generation = 1U;
        }
        tirtc_demo_reset_session_locked();
    }
    tirtc_demo_unlock(flags);

    if (connection != NULL) {
        command_result = tirtc_demo_send_session_hangup(connection, type);
    } else if (type == TIRTC_DEMO_SESSION_WECHAT &&
               session.room_id[0] != '\0') {
        wechat_result = tirtc_demo_reject_wechat_session(
            &session, session.incoming ? 7 : 1);
    }
    if (type != TIRTC_DEMO_SESSION_MONITOR &&
        (type != TIRTC_DEMO_SESSION_NONE || cloud_call_active)) {
        cloud_result = tirtc_demo_cloud_hangup();
    }
    printf("[tirtc_demo] lifecycle hangup type=%d connection=%p "
           "inflight_cancelled=%d command=%d wechat=%d cloud=%d queued=%d\n",
           (int)type, (void *)connection,
           connection == NULL && cloud_call_active ? 1 : 0,
           command_result, wechat_result, cloud_result, queued ? 1 : 0);
    if (queued) {
        tirtc_demo_schedule_work(TIRTC_DEMO_WORK_DISCONNECT);
    } else if (connection == NULL) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_READY, 0, "会话",
                              "当前会话已取消，设备继续在线。");
    }
    memset(&session, 0, sizeof(session));
}

static void tirtc_demo_process_wechat_reject(void)
{
    tirtc_demo_cloud_session_t session;
    tirtc_conn_t connection;
    bool valid;
    unsigned long flags;
    int result;

    tirtc_demo_lock(&flags);
    valid = g_session_type == TIRTC_DEMO_SESSION_WECHAT &&
            g_session.incoming;
    session = g_session;
    connection = g_active_conn;
    if (valid) {
        ++g_session_generation;
        if (g_session_generation == 0U) {
            g_session_generation = 1U;
        }
        if (connection != NULL) {
            (void)tirtc_demo_queue_disconnect_locked(connection, false);
        } else {
            tirtc_demo_reset_session_locked();
        }
    }
    tirtc_demo_unlock(flags);
    if (!valid) {
        memset(&session, 0, sizeof(session));
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR,
                              TIRTC_DEMO_CLOUD_ERR_NOT_READY,
                              "微信通话", "当前没有待处理的微信来电。");
        return;
    }
    result = tirtc_demo_reject_wechat_session(&session, 7);
    memset(&session, 0, sizeof(session));
    tirtc_demo_cloud_notify_session_ended(TIRTC_DEMO_CLOUD_SESSION_WECHAT,
                                          "微信来电已拒绝。");
    if (connection != NULL) {
        tirtc_demo_schedule_work(TIRTC_DEMO_WORK_DISCONNECT);
    }
    tirtc_demo_set_status(result == 0 ? TIRTC_DEMO_STATE_READY :
                                        TIRTC_DEMO_STATE_ERROR,
                          result, "微信通话",
                          result == 0 ? "微信来电已拒绝。" :
                                        "微信拒接请求发送失败。");
}

static void tirtc_demo_restart_sdk_now(void)
{
    unsigned long flags;
    bool initialized;
    bool should_stop;
    bool stopping;
    bool wait_for_session;
    int result;

    tirtc_demo_drop_pending_cloud_sessions();

    tirtc_demo_lock(&flags);
    g_restart_pending = true;
    wait_for_session = g_active_conn != NULL || g_disconnect_count > 0U;
    if (g_active_conn != NULL) {
        (void)tirtc_demo_queue_disconnect_locked(g_active_conn, false);
    }
    if (g_connect_inflight) {
        ++g_session_generation;
        if (g_session_generation == 0U) {
            g_session_generation = 1U;
        }
        g_connect_inflight = false;
        tirtc_demo_reset_session_locked();
    }
    initialized = g_sdk_initialized;
    should_stop = g_sdk_started || g_sdk_starting;
    stopping = g_sdk_stopping;
    if (should_stop && !stopping) {
        g_sdk_stopping = true;
        g_runtime.service_starting = true;
        g_status.state = TIRTC_DEMO_STATE_READY;
        g_status.last_error = 0;
        g_status.stage = "重启";
        g_status.message = "正在停止旧的 TiRTC 服务。";
    }
    tirtc_demo_unlock(flags);

    if (wait_for_session) {
        tirtc_demo_schedule_work(TIRTC_DEMO_WORK_DISCONNECT);
        return;
    }
    if (stopping) {
        return;
    }
    if (should_stop) {
        result = TiRtcStop();
        if (result < 0) {
            tirtc_demo_lock(&flags);
            g_sdk_stopping = false;
            g_restart_pending = false;
            tirtc_demo_unlock(flags);
            tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, result,
                                  "重启", "TiRTC 服务停止失败。" );
        }
        return;
    }

    if (initialized) {
        TiRtcUninit();
    }

    tirtc_demo_lock(&flags);
    g_sdk_initialized = false;
    g_sdk_starting = false;
    g_sdk_started = false;
    g_sdk_stopping = false;
    g_restart_pending = false;
    g_runtime.sdk_ready = false;
    g_runtime.service_starting = false;
    tirtc_demo_unlock(flags);

    (void)tirtc_demo_start_sdk_now();
}

static void tirtc_demo_run_diagnostics_now(void)
{
    tirtc_demo_cloud_snapshot_t cloud = {0};
    tirtc_demo_media_stats_t media = {0};
    bool cloud_initialized;
    bool network_ready;
    bool identity_ready;
    bool sdk_ready;
    bool healthy;
    unsigned long flags;
    char summary[TIRTC_DEMO_DIAGNOSTIC_SUMMARY_MAX];

    tirtc_demo_refresh_network_state();
    tirtc_demo_lock(&flags);
    cloud_initialized = g_cloud_initialized;
    network_ready = g_runtime.network_ready;
    identity_ready = g_runtime.identity_ready;
    sdk_ready = g_runtime.sdk_ready;
    tirtc_demo_unlock(flags);
    if (cloud_initialized) {
        tirtc_demo_cloud_get_snapshot(&cloud);
    }
    tirtc_demo_media_get_stats(&media);
    snprintf(summary, sizeof(summary),
             "WiFi:%s 身份:%s 云:%s TiRTC:%s 音频TX/RX:%lu/%lu 视频TX/RX:%lu/%lu",
             network_ready ? "通过" : "未连接",
             identity_ready ? "通过" : "待绑定",
             cloud.mqtt_connected ? "通过" : "未上线",
             sdk_ready ? "通过" : "未启动",
             (unsigned long)media.tx_audio_frames,
             (unsigned long)media.rx_audio_frames,
             (unsigned long)media.tx_video_frames,
             (unsigned long)media.rx_video_frames);
    healthy = network_ready && identity_ready && sdk_ready &&
              cloud.mqtt_connected;

    tirtc_demo_lock(&flags);
    tirtc_demo_copy_text(g_diagnostic_summary,
                         sizeof(g_diagnostic_summary), summary);
    g_runtime.diagnostic_running = false;
    ++g_runtime.diagnostic_generation;
    if (g_runtime.diagnostic_generation == 0U) {
        g_runtime.diagnostic_generation = 1U;
    }
    g_status.state = healthy ? TIRTC_DEMO_STATE_READY :
                               TIRTC_DEMO_STATE_ERROR;
    g_status.last_error = healthy ? 0 : -1;
    g_status.stage = "链路自检";
    g_status.message = g_diagnostic_summary;
    tirtc_demo_unlock(flags);
}

static void tirtc_demo_handle_network_transition(
    bool *was_ready, bool binding_waiting)
{
    unsigned long flags;
    bool network_ready;
    bool config_ready;
    int network_rssi;
    int result;

    if (was_ready == NULL) {
        return;
    }

    tirtc_demo_lock(&flags);
    network_ready = g_runtime.network_ready;
    network_rssi = g_runtime.network_rssi;
    config_ready = tirtc_demo_config_valid(&g_config);
    tirtc_demo_unlock(flags);

    if (network_ready == *was_ready) {
        return;
    }
    *was_ready = network_ready;

    if (!network_ready) {
        printf("[tirtc_demo] network unavailable\n");
        if (!config_ready) {
            tirtc_demo_set_status(TIRTC_DEMO_STATE_UNCONFIGURED,
                                  TIRTC_DEMO_ERR_NETWORK_PENDING,
                                  "等待网络",
                                  "请先通过君正系统设置连接 WiFi，联网后会自动申请绑定。");
        }
        return;
    }

    printf("[tirtc_demo] network available rssi=%d\n", network_rssi);
    if (config_ready || binding_waiting) {
        return;
    }

    result = tirtc_demo_cloud_request_binding();
    printf("[tirtc_demo] network-ready binding queued result=%d\n", result);
    tirtc_demo_set_status(
        result == 0 ? TIRTC_DEMO_STATE_UNCONFIGURED : TIRTC_DEMO_STATE_ERROR,
        result,
        "自动绑定",
        result == 0 ? "WiFi 已连接，正在申请设备绑定验证码。" :
                      "WiFi 已连接，但设备绑定任务启动失败。");
}

static void tirtc_demo_worker(void *data)
{
    uint64_t next_network_poll_ms = 0U;
    uint64_t next_start_retry_ms = 0U;
    bool network_was_ready;
    unsigned long initial_flags;

    (void)data;

    tirtc_demo_lock(&initial_flags);
    network_was_ready = g_runtime.network_ready;
    tirtc_demo_unlock(initial_flags);

    for (;;) {
        uint32_t work = tirtc_demo_take_work();
        tirtc_demo_cloud_session_t *session;
        unsigned long flags;
        bool restart_pending;
        bool connect_ready;
        bool send_ai_start;
        bool send_ai_heartbeat;
        bool abort_waiting_session;
        bool ai_start_expired;
        bool media_ready_expired;
        bool cloud_call_ended;
        bool start_attempted = false;
        bool network_polled = false;
        tirtc_conn_t abort_connection;
        tirtc_conn_t monitor_audio_connection;
        tirtc_demo_session_type_t abort_type;
        tirtc_demo_cloud_runtime_state_t cloud_state;
        uint64_t now = systick_get_time_ms();

        if ((work & TIRTC_DEMO_WORK_DISCONNECT) != 0U) {
            tirtc_demo_disconnect_now();
        }
        if ((work & TIRTC_DEMO_WORK_SESSION_READY) != 0U) {
            session = tirtc_demo_take_cloud_session();
            tirtc_demo_process_cloud_session(session);
        }
        if ((work & TIRTC_DEMO_WORK_CONNECT_RESULT) != 0U) {
            tirtc_demo_process_connect_result();
        }
        if ((work & TIRTC_DEMO_WORK_COMMAND) != 0U) {
            tirtc_demo_process_command();
        }
        if ((work & TIRTC_DEMO_WORK_AUDIO_SUBSCRIBE) != 0U) {
            bool enable;

            tirtc_demo_lock(&flags);
            enable = g_active_conn != NULL &&
                     (g_session_type == TIRTC_DEMO_SESSION_MONITOR ||
                      !g_session_waiting_ready);
            tirtc_demo_unlock(flags);
            if (enable && tirtc_demo_media_set_uplink(true) == 0) {
                tirtc_demo_lock(&flags);
                g_runtime.audio_tx_active = true;
                g_runtime.audio_rx_active = true;
                tirtc_demo_unlock(flags);
            }
        }
        if ((work & TIRTC_DEMO_WORK_AUDIO_UNSUBSCRIBE) != 0U) {
            (void)tirtc_demo_media_set_uplink(false);
            tirtc_demo_lock(&flags);
            g_runtime.audio_tx_active = false;
            tirtc_demo_unlock(flags);
        }
        if ((work & TIRTC_DEMO_WORK_VIDEO_SUBSCRIBE) != 0U) {
            tirtc_demo_start_subscribed_video();
        }
        if ((work & TIRTC_DEMO_WORK_VIDEO_UNSUBSCRIBE) != 0U) {
            (void)tirtc_demo_media_set_video(false);
            tirtc_demo_lock(&flags);
            g_runtime.video_tx_active = false;
            tirtc_demo_unlock(flags);
        }
        if ((work & TIRTC_DEMO_WORK_LOCAL_HANGUP) != 0U) {
            tirtc_demo_process_local_hangup();
        }
        if ((work & TIRTC_DEMO_WORK_WECHAT_REJECT) != 0U) {
            tirtc_demo_process_wechat_reject();
        }
        if ((work & TIRTC_DEMO_WORK_AI_START_TX) != 0U) {
            bool valid;
            int media_result = TIRTC_E_INVALID_HANDLE;

            tirtc_demo_lock(&flags);
            valid = g_active_conn != NULL &&
                    g_session_type == TIRTC_DEMO_SESSION_AI &&
                    g_runtime.ai_ready;
            tirtc_demo_unlock(flags);
            if (valid) {
                media_result = tirtc_demo_media_set_uplink(true);
            }
            tirtc_demo_lock(&flags);
            if (media_result == 0 && valid) {
                g_runtime.ai_talking = true;
                g_runtime.audio_tx_active = true;
            }
            tirtc_demo_unlock(flags);
        }
        if ((work & TIRTC_DEMO_WORK_AI_STOP_TX) != 0U) {
            (void)tirtc_demo_media_set_uplink(false);
            tirtc_demo_lock(&flags);
            g_runtime.ai_talking = false;
            g_runtime.audio_tx_active = false;
            tirtc_demo_unlock(flags);
        }
        if ((work & TIRTC_DEMO_WORK_AI_INTERRUPT) != 0U) {
            int command_result = tirtc_demo_send_ai_method("interrupt");

            tirtc_demo_set_status(command_result > 0 ? TIRTC_DEMO_STATE_AI_TALK :
                                                       TIRTC_DEMO_STATE_ERROR,
                                  command_result > 0 ? 0 : command_result,
                                  "AI 对讲",
                                  command_result > 0 ? "已请求打断当前 AI 回复。" :
                                                       "AI 打断命令发送失败。");
        }
        if ((work & TIRTC_DEMO_WORK_WEB_CALL_ACCEPT) != 0U) {
            tirtc_demo_process_web_call_reply(true);
        }
        if ((work & TIRTC_DEMO_WORK_WEB_CALL_REJECT) != 0U) {
            tirtc_demo_process_web_call_reply(false);
        }
        if ((work & TIRTC_DEMO_WORK_DIAGNOSTICS) != 0U) {
            tirtc_demo_run_diagnostics_now();
        }
        if ((work & TIRTC_DEMO_WORK_RESTART) != 0U) {
            tirtc_demo_restart_sdk_now();
            start_attempted = true;
        } else if ((work & TIRTC_DEMO_WORK_START) != 0U) {
            (void)tirtc_demo_start_sdk_now();
            start_attempted = true;
        }

        if (now >= next_network_poll_ms) {
            tirtc_demo_refresh_network_state();
            next_network_poll_ms = now + TIRTC_DEMO_NETWORK_POLL_MS;
            network_polled = true;
        }
        tirtc_demo_cloud_get_runtime_state(&cloud_state);
        if (network_polled) {
            tirtc_demo_handle_network_transition(&network_was_ready,
                                                  cloud_state.binding_waiting);
        }
        tirtc_demo_lock(&flags);
        restart_pending = g_restart_pending;
        monitor_audio_connection =
            g_active_conn != NULL &&
            g_session_type == TIRTC_DEMO_SESSION_MONITOR &&
            g_session_media_started && !g_monitor_audio_subscribed &&
            g_monitor_audio_retry_at_ms != 0U &&
            now >= g_monitor_audio_retry_at_ms ? g_active_conn : NULL;
        connect_ready = g_sdk_started && g_session_connect_allowed &&
                        !g_connect_inflight && g_active_conn == NULL &&
                        g_session_type != TIRTC_DEMO_SESSION_NONE &&
                        g_session_type != TIRTC_DEMO_SESSION_MONITOR;
        send_ai_start = g_ai_start_pending && now >= g_ai_start_at_ms;
        send_ai_heartbeat = g_session_type == TIRTC_DEMO_SESSION_AI &&
                            g_runtime.ai_ready && g_ai_heartbeat_at_ms != 0U &&
                            now >= g_ai_heartbeat_at_ms;
        if (send_ai_heartbeat) {
            g_ai_heartbeat_at_ms = now + 30000U;
        }
        ai_start_expired = g_ai_start_waiting &&
                           g_ai_start_deadline_ms != 0U &&
                           now >= g_ai_start_deadline_ms;
        media_ready_expired = g_session_waiting_ready &&
                              g_session_ready_deadline_ms != 0U &&
                              now >= g_session_ready_deadline_ms;
        cloud_call_ended =
                           (g_session_type == TIRTC_DEMO_SESSION_DEVICE ||
                            g_session_type == TIRTC_DEMO_SESSION_WECHAT) &&
                           cloud_state.call_state ==
                               TIRTC_DEMO_CLOUD_CALL_IDLE;
        abort_waiting_session = ai_start_expired || media_ready_expired ||
                                cloud_call_ended;
        abort_connection = abort_waiting_session ? g_active_conn : NULL;
        abort_type = abort_waiting_session ? g_session_type :
                                             TIRTC_DEMO_SESSION_NONE;
        if (abort_waiting_session) {
            g_ai_start_waiting = false;
            g_ai_start_deadline_ms = 0U;
            g_session_ready_deadline_ms = 0U;
            if (abort_connection != NULL) {
                (void)tirtc_demo_queue_disconnect_locked(abort_connection,
                                                         false);
            } else if (cloud_call_ended) {
                ++g_session_generation;
                if (g_session_generation == 0U) {
                    g_session_generation = 1U;
                }
                tirtc_demo_reset_session_locked();
            }
            g_status.state = TIRTC_DEMO_STATE_ERROR;
            g_status.last_error = TIRTC_DEMO_ERR_SERVICE_PENDING;
            g_status.stage = ai_start_expired ? "AI 对讲" : "会话确认";
            g_status.message = ai_start_expired ?
                "AI start_session 等待超时，会话已关闭。" :
                media_ready_expired ?
                    "媒体房间确认超时，会话已关闭。" :
                    "业务呼叫已结束，媒体连接同步关闭。";
        }
        tirtc_demo_unlock(flags);
        if (abort_waiting_session) {
            tirtc_demo_cloud_notify_session_ended(
                tirtc_demo_cloud_session_type(abort_type),
                ai_start_expired ? "AI 会话确认超时。" :
                media_ready_expired ? "媒体房间确认超时。" :
                                      "业务呼叫已结束。");
            if (abort_connection != NULL) {
                tirtc_demo_schedule_work(TIRTC_DEMO_WORK_DISCONNECT);
            }
        }
        if (connect_ready) {
            (void)tirtc_demo_start_session_connect();
        }
        if (monitor_audio_connection != NULL) {
            (void)tirtc_demo_subscribe_monitor_audio(
                monitor_audio_connection);
        }
        if (send_ai_start) {
            tirtc_demo_send_ai_start();
        }
        if (send_ai_heartbeat) {
            (void)tirtc_demo_send_ai_method("heartbeat");
        }
        /* A synchronous SYS_STOPPED callback queues RESTART while the current
         * worker pass is already processing the same restart. Consume it on
         * the next pass instead of stopping the newly started listener. */
        if (restart_pending && !start_attempted) {
            tirtc_demo_restart_sdk_now();
            start_attempted = true;
        } else if (!start_attempted && now >= next_start_retry_ms) {
            if (tirtc_demo_app_config_ready()) {
                (void)tirtc_demo_start_sdk_now();
                start_attempted = true;
            } else {
                next_start_retry_ms = now + TIRTC_DEMO_SDK_RETRY_MS;
            }
        }
        if (start_attempted) {
            next_start_retry_ms = now + TIRTC_DEMO_SDK_RETRY_MS;
        }

        if (!tirtc_demo_work_pending()) {
            (void)thread_wait_timeout(TIRTC_DEMO_WORKER_POLL_MS);
        }
    }
}

static int tirtc_demo_ensure_worker(void)
{
    unsigned long flags;
    thread_ptr_t worker;

    tirtc_demo_lock(&flags);
    worker = g_worker_thread;
    tirtc_demo_unlock(flags);
    if (worker != NULL) {
        return 0;
    }

    worker = thread_create("tirtc_service", TIRTC_DEMO_WORKER_STACK_SIZE,
                           tirtc_demo_worker, NULL);
    if (worker == NULL) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, TIRTC_E_LACK_OF_RESOURCE,
                              "服务线程", "TiRTC 业务线程创建失败。" );
        return TIRTC_E_LACK_OF_RESOURCE;
    }

    tirtc_demo_lock(&flags);
    if (g_worker_thread == NULL) {
        g_worker_thread = worker;
        worker = NULL;
    }
    tirtc_demo_unlock(flags);

    if (worker != NULL) {
        thread_delete(worker);
    }
    return 0;
}
#endif

static const char *tirtc_demo_cloud_call_phase(tirtc_demo_cloud_call_state_t state)
{
    switch (state) {
    case TIRTC_DEMO_CLOUD_CALL_OUTGOING:
        return "outgoing";
    case TIRTC_DEMO_CLOUD_CALL_INCOMING:
        return "incoming";
    case TIRTC_DEMO_CLOUD_CALL_CONNECTING:
        return "connecting";
    case TIRTC_DEMO_CLOUD_CALL_ACTIVE:
        return "active";
    case TIRTC_DEMO_CLOUD_CALL_IDLE:
    default:
        return "idle";
    }
}

static bool tirtc_demo_session_transport_ready(void)
{
#ifdef CONFIG_TIRTC
    bool ready;
    unsigned long flags;

    tirtc_demo_lock(&flags);
    ready = g_sdk_started && !g_sdk_starting && !g_sdk_stopping &&
            !g_restart_pending;
    tirtc_demo_unlock(flags);
    return ready;
#else
    return false;
#endif
}

static int tirtc_demo_cloud_save_identity(const char *device_id,
                                           const char *device_secret,
                                           void *context)
{
    tirtc_demo_config_t config;
    unsigned long flags;

    (void)context;
    if (device_id == NULL || device_secret == NULL ||
        device_id[0] == '\0' || device_secret[0] == '\0' ||
        strlen(device_id) >= sizeof(config.device_id) ||
        strlen(device_secret) >= sizeof(config.device_secret)) {
        return -1;
    }

    tirtc_demo_lock(&flags);
    config = g_config;
    tirtc_demo_unlock(flags);
    tirtc_demo_copy_text(config.device_id, sizeof(config.device_id), device_id);
    tirtc_demo_copy_text(config.device_secret, sizeof(config.device_secret), device_secret);
    if (!tirtc_demo_config_valid(&config) ||
        tirtc_demo_save_config_file(&config) != 0) {
        memset(config.device_secret, 0, sizeof(config.device_secret));
        return -1;
    }

    tirtc_demo_apply_config(&config);
    printf("[tirtc_demo] binding credentials saved device_id_len=%u\n",
           (unsigned int)strlen(config.device_id));
    memset(config.device_secret, 0, sizeof(config.device_secret));
    return 0;
}

static void tirtc_demo_cloud_identity_activated(void *context)
{
    (void)context;
#ifdef CONFIG_TIRTC
    tirtc_demo_schedule_work(TIRTC_DEMO_WORK_RESTART);
#endif
}

static int tirtc_demo_ensure_cloud(void)
{
    tirtc_demo_cloud_callbacks_t callbacks = {
        .save_identity = tirtc_demo_cloud_save_identity,
        .identity_activated = tirtc_demo_cloud_identity_activated,
        .session_ready = tirtc_demo_cloud_session_ready,
        .context = NULL,
    };
    tirtc_demo_config_t config;
    unsigned long flags;
    bool initialize;
    int result;

    tirtc_demo_lock(&flags);
    initialize = !g_cloud_initialized;
    if (initialize) {
        g_cloud_initialized = true;
    }
    config = g_config;
    tirtc_demo_unlock(flags);

    if (initialize) {
        result = tirtc_demo_cloud_init(&callbacks);
        if (result != 0) {
            tirtc_demo_lock(&flags);
            g_cloud_initialized = false;
            tirtc_demo_unlock(flags);
            memset(config.device_secret, 0, sizeof(config.device_secret));
            return result;
        }
        if (tirtc_demo_config_valid(&config)) {
            result = tirtc_demo_cloud_apply_identity(config.device_id,
                                                      config.device_secret);
        } else {
            result = tirtc_demo_cloud_apply_identity("", "");
        }
        if (result != 0) {
            memset(config.device_secret, 0, sizeof(config.device_secret));
            return result;
        }
    }
    result = tirtc_demo_cloud_start();
    memset(config.device_secret, 0, sizeof(config.device_secret));
    return result;
}

static void tirtc_demo_sync_cloud_state(void)
{
    tirtc_demo_cloud_snapshot_t snapshot;
    unsigned long flags;
    size_t count;

    if (!g_cloud_initialized) {
        return;
    }
    tirtc_demo_cloud_get_snapshot(&snapshot);

    tirtc_demo_lock(&flags);
    g_runtime.identity_ready = snapshot.identity_ready;
    g_runtime.token_ready = snapshot.token_ready;
    g_runtime.cloud_online = snapshot.mqtt_connected;
    g_runtime.binding_waiting = snapshot.binding_waiting;
    g_runtime.contacts_ready = snapshot.contacts_ready;
    g_runtime.wechat_ready = snapshot.wechat_ready;
    g_runtime.wechat_contacts_ready = snapshot.wechat_contacts_ready;
    g_runtime.wechat_incoming = snapshot.wechat_incoming;
#ifdef CONFIG_TIRTC
    if (g_session_type != TIRTC_DEMO_SESSION_MONITOR ||
        g_pending_web_call_command == 0U) {
        g_runtime.incoming_call = snapshot.incoming_call;
    }
#else
    g_runtime.incoming_call = snapshot.incoming_call;
#endif
#ifdef CONFIG_TIRTC
    if (g_session_type != TIRTC_DEMO_SESSION_MONITOR || !g_web_call_active) {
        g_runtime.call_active = snapshot.call_active;
    }
#else
    g_runtime.call_active = snapshot.call_active;
#endif
    g_runtime.cloud_generation = snapshot.status_generation;
#ifdef CONFIG_TIRTC
    if (g_session_type != TIRTC_DEMO_SESSION_MONITOR) {
        g_runtime.call_phase = tirtc_demo_cloud_call_phase(snapshot.call_state);
    }
    if (g_session_type == TIRTC_DEMO_SESSION_NONE) {
        tirtc_demo_copy_text(
            g_session_type_text, sizeof(g_session_type_text),
            snapshot.session_type == TIRTC_DEMO_CLOUD_SESSION_DEVICE ?
                "device" :
            snapshot.session_type == TIRTC_DEMO_CLOUD_SESSION_WECHAT ?
                "wechat" :
            snapshot.session_type == TIRTC_DEMO_CLOUD_SESSION_AI ?
                "ai" : "none");
    }
#else
    g_runtime.call_phase = tirtc_demo_cloud_call_phase(snapshot.call_state);
#endif
    tirtc_demo_copy_text(g_cloud_binding_code, sizeof(g_cloud_binding_code),
                         snapshot.binding_code);
    g_runtime.binding_code = g_cloud_binding_code;
    if (snapshot.peer_id[0] != '\0') {
        tirtc_demo_copy_text(g_cloud_peer_id, sizeof(g_cloud_peer_id),
                             snapshot.peer_id);
        g_runtime.peer_id = g_cloud_peer_id;
    } else {
        g_runtime.peer_id = g_config.device_id[0] != '\0' ?
                                g_config.device_id : TIRTC_DEMO_UNCONFIGURED_TEXT;
    }

    count = snapshot.contact_count < TIRTC_DEMO_MAX_CONTACTS ?
                snapshot.contact_count : TIRTC_DEMO_MAX_CONTACTS;
    memset(g_contacts, 0, sizeof(g_contacts));
    memset(g_contact_storage, 0, sizeof(g_contact_storage));
    for (size_t i = 0; i < count; ++i) {
        const char *name = snapshot.contacts[i].remark[0] != '\0' ?
                               snapshot.contacts[i].remark :
                               snapshot.contacts[i].device_id;

        tirtc_demo_copy_text(g_contact_storage[i].name,
                             sizeof(g_contact_storage[i].name), name);
        tirtc_demo_copy_text(g_contact_storage[i].device_id,
                             sizeof(g_contact_storage[i].device_id),
                             snapshot.contacts[i].device_id);
        tirtc_demo_copy_text(g_contact_storage[i].last_call,
                             sizeof(g_contact_storage[i].last_call),
                             snapshot.contacts[i].online ? "在线" : "离线");
        g_contacts[i].name = g_contact_storage[i].name;
        g_contacts[i].device_id = g_contact_storage[i].device_id;
        g_contacts[i].last_call = g_contact_storage[i].last_call;
        g_contacts[i].online = snapshot.contacts[i].online;
    }
    g_contact_count = count;

    count = snapshot.wechat_contact_count < TIRTC_DEMO_MAX_CONTACTS ?
                snapshot.wechat_contact_count : TIRTC_DEMO_MAX_CONTACTS;
    memset(g_wechat_contacts, 0, sizeof(g_wechat_contacts));
    memset(g_wechat_contact_storage, 0, sizeof(g_wechat_contact_storage));
    for (size_t i = 0; i < count; ++i) {
        const char *name = snapshot.wechat_contacts[i].name[0] != '\0' ?
                               snapshot.wechat_contacts[i].name :
                               snapshot.wechat_contacts[i].open_id;

        tirtc_demo_copy_text(g_wechat_contact_storage[i].name,
                             sizeof(g_wechat_contact_storage[i].name), name);
        tirtc_demo_copy_text(g_wechat_contact_storage[i].open_id,
                             sizeof(g_wechat_contact_storage[i].open_id),
                             snapshot.wechat_contacts[i].open_id);
        tirtc_demo_copy_text(g_wechat_contact_storage[i].app_id,
                             sizeof(g_wechat_contact_storage[i].app_id),
                             snapshot.wechat_contacts[i].app_id);
        tirtc_demo_copy_text(g_wechat_contact_storage[i].model_id,
                             sizeof(g_wechat_contact_storage[i].model_id),
                             snapshot.wechat_contacts[i].model_id);
        g_wechat_contacts[i].name = g_wechat_contact_storage[i].name;
        g_wechat_contacts[i].open_id = g_wechat_contact_storage[i].open_id;
        g_wechat_contacts[i].app_id = g_wechat_contact_storage[i].app_id;
        g_wechat_contacts[i].model_id = g_wechat_contact_storage[i].model_id;
    }
    g_wechat_contact_count = count;

    if (snapshot.status_generation != 0U &&
        snapshot.status_generation != g_cloud_status_generation) {
        g_cloud_status_generation = snapshot.status_generation;
        tirtc_demo_copy_text(g_cloud_stage, sizeof(g_cloud_stage), snapshot.stage);
        tirtc_demo_copy_text(g_cloud_message, sizeof(g_cloud_message), snapshot.message);
        if (!snapshot.identity_ready || snapshot.binding_waiting) {
            g_status.state = TIRTC_DEMO_STATE_UNCONFIGURED;
        } else if (snapshot.call_active) {
            g_status.state = snapshot.session_type ==
                                 TIRTC_DEMO_CLOUD_SESSION_AI ?
                                 TIRTC_DEMO_STATE_AI_TALK :
                                 TIRTC_DEMO_STATE_IN_CALL;
        } else if (snapshot.call_state != TIRTC_DEMO_CLOUD_CALL_IDLE) {
            g_status.state = snapshot.session_type ==
                                 TIRTC_DEMO_CLOUD_SESSION_AI ?
                                 TIRTC_DEMO_STATE_AI_TALK :
                                 TIRTC_DEMO_STATE_CALLING;
        } else if (snapshot.last_error < 0 &&
                   snapshot.last_error != TIRTC_DEMO_CLOUD_ERR_NOT_READY) {
            g_status.state = TIRTC_DEMO_STATE_ERROR;
        } else if (g_runtime.peer_connected) {
            g_status.state = TIRTC_DEMO_STATE_CONNECTED;
        } else {
            g_status.state = TIRTC_DEMO_STATE_READY;
        }
        g_status.last_error = snapshot.last_error;
        g_status.stage = g_cloud_stage;
        g_status.message = g_cloud_message;
    }
    tirtc_demo_unlock(flags);
}

const char *tirtc_demo_app_state_name(tirtc_demo_state_t state)
{
    switch (state) {
    case TIRTC_DEMO_STATE_IDLE:
        return "未启动";
    case TIRTC_DEMO_STATE_READY:
        return "已就绪";
    case TIRTC_DEMO_STATE_UNCONFIGURED:
        return "待配置";
    case TIRTC_DEMO_STATE_CONNECTED:
        return "已连接";
    case TIRTC_DEMO_STATE_CALLING:
        return "呼叫中";
    case TIRTC_DEMO_STATE_IN_CALL:
        return "通话中";
    case TIRTC_DEMO_STATE_AI_TALK:
        return "AI 对讲";
    case TIRTC_DEMO_STATE_ERROR:
        return "异常";
    default:
        return "未知";
    }
}

static bool tirtc_demo_request_session_close(const char *source)
{
#ifdef CONFIG_TIRTC
    tirtc_demo_cloud_snapshot_t snapshot;
    tirtc_demo_session_type_t type;
    bool active;
    bool connection_active;
    bool connect_inflight;
    unsigned long flags;

    tirtc_demo_cloud_get_snapshot(&snapshot);
    tirtc_demo_lock(&flags);
    type = g_session_type;
    connection_active = g_active_conn != NULL;
    connect_inflight = g_connect_inflight;
    active = connection_active || connect_inflight ||
             type != TIRTC_DEMO_SESSION_NONE ||
             snapshot.call_state != TIRTC_DEMO_CLOUD_CALL_IDLE;
    tirtc_demo_unlock(flags);
    printf("[tirtc_demo] app lifecycle close source=%s type=%d "
           "connection=%d inflight=%d cloud_state=%d action=%s\n",
           source != NULL ? source : "unknown", (int)type,
           connection_active ? 1 : 0, connect_inflight ? 1 : 0,
           (int)snapshot.call_state, active ? "hangup" : "none");
    if (active) {
        tirtc_demo_schedule_work(TIRTC_DEMO_WORK_LOCAL_HANGUP);
    }
    return active;
#else
    (void)source;
    return false;
#endif
}

void tirtc_demo_app_enter(void)
{
    unsigned long flags;

    tirtc_demo_lock(&flags);
    g_external_media_active = false;
    tirtc_demo_unlock(flags);
    tirtc_demo_load_config_once();
    tirtc_demo_refresh_network_state();
    (void)tirtc_demo_app_start();
}

void tirtc_demo_app_service_init(void)
{
    printf("[tirtc_demo] app version=%s build=%s %s\n",
           TIRTC_G32_APP_VERSION, __DATE__, __TIME__);
#ifdef CONFIG_TIRTC
    int result = tirtc_demo_sdk_gate_init();

    if (result == 0) {
        result = tirtc_demo_media_init();
    }

    if (result != 0) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, result,
                              "媒体服务", "君正音视频服务注册失败。");
        return;
    }
#endif
    (void)tirtc_demo_app_start();
}

void tirtc_demo_app_exit(void)
{
    tirtc_demo_refresh_network_state();
    if (tirtc_demo_request_session_close("app-exit")) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_READY, 0, "退出应用",
                              "正在挂断当前会话，设备服务保持在线。");
    }
}

int tirtc_demo_app_return_home(void)
{
    if (tirtc_demo_request_session_close("return-home")) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_READY, 0, "返回主页",
                              "正在挂断当前会话，设备服务保持在线。");
    } else {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_READY, 0, "在线",
                              "当前没有对端连接，设备服务继续监听。");
    }
    return 0;
}

int tirtc_demo_app_start(void)
{
    tirtc_demo_config_t config;
    tirtc_demo_runtime_t runtime;
    tirtc_demo_cloud_snapshot_t cloud_snapshot;
    int result;

    tirtc_demo_load_config_once();
    tirtc_demo_refresh_network_state();
    tirtc_demo_app_get_config(&config);
    tirtc_demo_app_get_runtime(&runtime);

    result = tirtc_demo_ensure_cloud();
    if (result != 0) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, result,
                              "业务服务", "设备业务服务启动失败。" );
        return result;
    }

#ifdef CONFIG_TIRTC
    result = tirtc_demo_ensure_worker();
    if (result < 0) {
        return result;
    }

    if (!tirtc_demo_config_valid(&config)) {
        memset(&cloud_snapshot, 0, sizeof(cloud_snapshot));
        tirtc_demo_cloud_get_snapshot(&cloud_snapshot);
        if (runtime.network_ready && !cloud_snapshot.binding_waiting) {
            (void)tirtc_demo_cloud_request_binding();
        }
        tirtc_demo_set_status(
            TIRTC_DEMO_STATE_UNCONFIGURED,
            TIRTC_DEMO_ERR_CONFIG_REQUIRED,
            runtime.network_ready ? "自动绑定" : "等待网络",
            runtime.network_ready ?
                (cloud_snapshot.binding_waiting ?
                     "绑定验证码已生成，请在管理端完成绑定。" :
                     "正在自动申请设备绑定验证码。") :
                "请先通过君正系统设置连接 WiFi，联网后会自动申请绑定。");
        return TIRTC_DEMO_ERR_CONFIG_REQUIRED;
    }
    if (!runtime.network_ready) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_READY,
                              TIRTC_DEMO_ERR_NETWORK_PENDING,
                              "等待网络", "请先通过君正系统设置连接 WiFi。" );
        return TIRTC_DEMO_ERR_NETWORK_PENDING;
    }
    tirtc_demo_schedule_work(TIRTC_DEMO_WORK_START);
    return 0;
#else
    (void)config;
    (void)runtime;
    result = TIRTC_DEMO_ERR_SERVICE_PENDING;
    tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, result,
                          "SDK", "当前固件未包含 TiRTC SDK。" );
    return result;
#endif
}

int tirtc_demo_app_restart(void)
{
#ifdef CONFIG_TIRTC
    int result = tirtc_demo_ensure_worker();

    if (result < 0) {
        return result;
    }
    tirtc_demo_schedule_work(TIRTC_DEMO_WORK_RESTART);
    return 0;
#else
    return TIRTC_DEMO_ERR_SERVICE_PENDING;
#endif
}

int tirtc_demo_app_refresh_network(void)
{
    tirtc_demo_runtime_t runtime;
    bool config_ready;

    tirtc_demo_refresh_network_state();
    tirtc_demo_app_get_runtime(&runtime);
    config_ready = tirtc_demo_app_config_ready();
    if (runtime.network_ready) {
        (void)tirtc_demo_cloud_start();
        if (!config_ready) {
            (void)tirtc_demo_cloud_request_binding();
            tirtc_demo_set_status(TIRTC_DEMO_STATE_UNCONFIGURED, 0,
                                  "自动绑定",
                                  "WiFi 已连接，正在申请设备绑定验证码。");
            return TIRTC_DEMO_ERR_CONFIG_REQUIRED;
        }
        tirtc_demo_set_status(TIRTC_DEMO_STATE_READY, 0, "网络",
                              "系统 WiFi 已连接，正在启动 TiRTC 服务。" );
#ifdef CONFIG_TIRTC
        tirtc_demo_schedule_work(TIRTC_DEMO_WORK_START);
#endif
        return 0;
    }

    tirtc_demo_set_status(TIRTC_DEMO_STATE_READY,
                          TIRTC_DEMO_ERR_NETWORK_PENDING,
                          "等待网络", "请先通过君正系统设置连接 WiFi。" );
    return TIRTC_DEMO_ERR_NETWORK_PENDING;
}

int tirtc_demo_app_request_binding(void)
{
    tirtc_demo_runtime_t runtime;
    int result = tirtc_demo_ensure_cloud();

    printf("[tirtc_demo] binding request begin cloud_result=%d\n", result);
    tirtc_demo_app_get_runtime(&runtime);
    if (runtime.identity_ready) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_READY, 0,
                              "设备绑定",
                              "设备已经绑定；需要换绑时请先执行重置绑定。");
        printf("[tirtc_demo] binding request rejected identity already ready\n");
        return TIRTC_DEMO_CLOUD_ERR_BUSY;
    }
    if (result == 0) {
        result = tirtc_demo_cloud_request_binding();
    }
    if (result == 0) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_UNCONFIGURED, 0,
                              "设备绑定", "正在申请设备绑定验证码。" );
    } else {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, result,
                              "设备绑定", "设备绑定任务启动失败。" );
    }
    printf("[tirtc_demo] binding request done result=%d\n", result);
    return result;
}

int tirtc_demo_app_reset_binding(void)
{
    tirtc_demo_config_t config;
    int result;

    printf("[tirtc_demo] binding reset begin\n");
    tirtc_demo_app_get_config(&config);
    memset(config.device_id, 0, sizeof(config.device_id));
    memset(config.device_secret, 0, sizeof(config.device_secret));
    result = tirtc_demo_save_config_file(&config);
    if (result != 0) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, result,
                              "重置绑定", "清除设备凭据失败，请检查数据分区。");
        return result;
    }
    tirtc_demo_apply_config(&config);
    result = tirtc_demo_ensure_cloud();
    if (result == 0) {
        result = tirtc_demo_cloud_apply_identity("", "");
    }
    if (result == 0) {
        result = tirtc_demo_cloud_request_binding();
    }
#ifdef CONFIG_TIRTC
    tirtc_demo_schedule_work(TIRTC_DEMO_WORK_RESTART);
#endif
    memset(config.device_secret, 0, sizeof(config.device_secret));
    tirtc_demo_set_status(result == 0 ? TIRTC_DEMO_STATE_UNCONFIGURED :
                                       TIRTC_DEMO_STATE_ERROR,
                          result, "重置绑定",
                          result == 0 ?
                              "旧凭据已清除，正在申请新的绑定验证码。" :
                              "旧凭据已清除，但绑定验证码申请失败。");
    printf("[tirtc_demo] binding reset done result=%d\n", result);
    return result;
}

int tirtc_demo_app_run_diagnostics(void)
{
#ifdef CONFIG_TIRTC
    unsigned long flags;
    int result = tirtc_demo_ensure_worker();

    if (result < 0) {
        return result;
    }
    tirtc_demo_lock(&flags);
    g_runtime.diagnostic_running = true;
    tirtc_demo_unlock(flags);
    tirtc_demo_schedule_work(TIRTC_DEMO_WORK_DIAGNOSTICS);
    return 0;
#else
    return TIRTC_DEMO_ERR_SERVICE_PENDING;
#endif
}

int tirtc_demo_app_refresh_contacts(void)
{
    int result = tirtc_demo_cloud_refresh_contacts();

    if (result == 0) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_READY, 0,
                              "联系人", "正在从业务服务刷新联系人。" );
    } else {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, result,
                              "联系人", "设备尚未上线，无法刷新联系人。" );
    }
    return result;
}

int tirtc_demo_app_request_contact(const char *target_device_id)
{
    int result = tirtc_demo_cloud_request_contact(target_device_id);

    if (result == 0) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_READY, 0,
                              "联系人", "联系人申请已进入发送队列。" );
    } else {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, result,
                              "联系人", "联系人设备 ID 无效或业务服务未上线。" );
    }
    return result;
}

int tirtc_demo_app_disconnect(void)
{
#ifdef CONFIG_TIRTC
    if (tirtc_demo_request_session_close("disconnect-button")) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_READY, 0, "断开连接",
                              "正在结束当前对端连接，设备服务保持在线。" );
    } else {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_READY, 0, "在线",
                              "当前没有对端连接，设备服务继续监听。" );
    }
    return 0;
#else
    return TIRTC_DEMO_ERR_SERVICE_PENDING;
#endif
}

const char *tirtc_demo_app_sdk_version(void)
{
#ifdef CONFIG_TIRTC
    return TiRtcGetVersion();
#else
    return "未链接";
#endif
}

static int tirtc_demo_set_volume_now(tirtc_demo_volume_t volume, int value,
                                     bool report_status)
{
    unsigned long flags;
    int result;

    if (value < 0) {
        value = 0;
    } else if (value > 100) {
        value = 100;
    }
    result = volume == TIRTC_DEMO_VOLUME_TX ?
                 audio_dev_set_cap_volume((unsigned int)value) :
                 audio_dev_set_play_volume((unsigned int)value);
    if (result != 0) {
        if (report_status) {
            tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, result, "音频",
                                  "系统音量设置失败。");
        }
        return result;
    }

    tirtc_demo_lock(&flags);
    if (volume == TIRTC_DEMO_VOLUME_TX) {
        g_tx_volume = value;
        g_tx_muted = value == 0;
        if (value > 0) {
            g_tx_restore_volume = value;
        }
    } else {
        g_rx_volume = value;
        g_rx_muted = value == 0;
        if (value > 0) {
            g_rx_restore_volume = value;
        }
    }
    tirtc_demo_unlock(flags);

    if (report_status) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_READY, 0, "音频",
                              "音量已由君正音频服务更新。");
    }
    return value;
}

int tirtc_demo_app_adjust_volume(tirtc_demo_volume_t volume, int delta)
{
    int current;
    unsigned long flags;

    tirtc_demo_lock(&flags);
    current = volume == TIRTC_DEMO_VOLUME_TX ? g_tx_volume : g_rx_volume;
    tirtc_demo_unlock(flags);
    return tirtc_demo_set_volume_now(volume, current + delta, true);
}

int tirtc_demo_app_toggle_mute(tirtc_demo_volume_t volume)
{
    bool muted;
    int saved_volume;
    unsigned long flags;
    int result;

    tirtc_demo_lock(&flags);
    muted = !(volume == TIRTC_DEMO_VOLUME_TX ? g_tx_muted : g_rx_muted);
    saved_volume = volume == TIRTC_DEMO_VOLUME_TX ?
                       g_tx_restore_volume : g_rx_restore_volume;
    tirtc_demo_unlock(flags);
    result = volume == TIRTC_DEMO_VOLUME_TX ?
                 audio_dev_set_cap_volume(muted ? 0U : (unsigned int)saved_volume) :
                 audio_dev_set_play_volume(muted ? 0U : (unsigned int)saved_volume);
    if (result != 0) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, result, "音频",
                              "系统静音设置失败。" );
        return result;
    }

    tirtc_demo_lock(&flags);
    if (volume == TIRTC_DEMO_VOLUME_TX) {
        g_tx_muted = muted;
        if (!muted) {
            g_tx_volume = saved_volume;
        }
    } else {
        g_rx_muted = muted;
        if (!muted) {
            g_rx_volume = saved_volume;
        }
    }
    tirtc_demo_unlock(flags);

    tirtc_demo_set_status(TIRTC_DEMO_STATE_READY, 0, "音频",
                          muted ? "系统音频已静音。" : "系统音频已恢复。" );
    return muted ? 1 : 0;
}

int tirtc_demo_app_call_contact(size_t index)
{
    char target_device_id[TIRTC_DEMO_DEVICE_ID_MAX];
    unsigned long flags;
    int result;

    tirtc_demo_sync_cloud_state();
    tirtc_demo_lock(&flags);
    if (index >= g_contact_count) {
        tirtc_demo_unlock(flags);
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, -1, "联系人", "联系人不存在。" );
        return -1;
    }
    tirtc_demo_copy_text(target_device_id, sizeof(target_device_id),
                         g_contacts[index].device_id);
    tirtc_demo_unlock(flags);

    if (!tirtc_demo_session_transport_ready()) {
        memset(target_device_id, 0, sizeof(target_device_id));
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR,
                              TIRTC_DEMO_CLOUD_ERR_NOT_READY,
                              "呼叫", "TiRTC 监听服务尚未就绪，请稍后重试。");
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }

    result = tirtc_demo_cloud_call(target_device_id);
    if (result != 0) {
        memset(target_device_id, 0, sizeof(target_device_id));
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, result, "呼叫",
                              "设备业务服务未上线或当前已有通话。" );
        return result;
    }

    tirtc_demo_lock(&flags);
    tirtc_demo_copy_text(g_cloud_peer_id, sizeof(g_cloud_peer_id),
                         target_device_id);
    g_runtime.peer_id = g_cloud_peer_id;
    g_runtime.call_phase = "outgoing";
    g_runtime.incoming_call = false;
    g_runtime.call_active = false;
    tirtc_demo_unlock(flags);
    memset(target_device_id, 0, sizeof(target_device_id));
    tirtc_demo_set_status(TIRTC_DEMO_STATE_CALLING, 0, "呼叫",
                          "正在通过业务服务发起设备呼叫。" );
    return 0;
}

int tirtc_demo_app_delete_contact(size_t index)
{
    bool valid;
    unsigned long flags;

    tirtc_demo_sync_cloud_state();
    tirtc_demo_lock(&flags);
    valid = index < g_contact_count;
    tirtc_demo_unlock(flags);
    if (!valid) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, -1, "联系人", "联系人不存在。" );
        return -1;
    }

    tirtc_demo_set_status(TIRTC_DEMO_STATE_READY, TIRTC_DEMO_ERR_UNSUPPORTED,
                          "联系人",
                          "当前设备端接口不提供删除联系人，请在管理端处理。" );
    return TIRTC_DEMO_ERR_UNSUPPORTED;
}

int tirtc_demo_app_handle_call(tirtc_demo_call_action_t action)
{
    switch (action) {
    case TIRTC_DEMO_CALL_OUTGOING:
    {
        bool has_contact;
        unsigned long flags;

        tirtc_demo_sync_cloud_state();
        tirtc_demo_lock(&flags);
        has_contact = g_contact_count > 0U;
        tirtc_demo_unlock(flags);
        if (!has_contact) {
            tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, -1, "呼叫",
                                  "暂无联系人，请先输入设备 ID 申请或从服务端同步。" );
            return -1;
        }
        return tirtc_demo_app_call_contact(0);
    }
    case TIRTC_DEMO_CALL_ACCEPT:
    {
        bool web_pending;
        unsigned long flags;

#ifdef CONFIG_TIRTC
        tirtc_demo_lock(&flags);
        web_pending = g_session_type == TIRTC_DEMO_SESSION_MONITOR &&
                      g_pending_web_call_command != 0U;
        tirtc_demo_unlock(flags);
        if (web_pending) {
            tirtc_demo_schedule_work(TIRTC_DEMO_WORK_WEB_CALL_ACCEPT);
            return 0;
        }
#else
        (void)flags;
        web_pending = false;
#endif
        if (!tirtc_demo_session_transport_ready()) {
            return tirtc_demo_service_pending(
                "接听", "TiRTC 监听服务尚未就绪，暂不能接听。");
        }
        int result = tirtc_demo_cloud_accept();

        if (result == 0) {
            tirtc_demo_set_status(TIRTC_DEMO_STATE_CALLING, 0, "接听",
                                  "已接听，正在获取媒体连接参数。");
        }
        return result;
    }
    case TIRTC_DEMO_CALL_REJECT:
#ifdef CONFIG_TIRTC
    {
        bool web_pending;
        unsigned long flags;

        tirtc_demo_lock(&flags);
        web_pending = g_session_type == TIRTC_DEMO_SESSION_MONITOR &&
                      g_pending_web_call_command != 0U;
        tirtc_demo_unlock(flags);
        if (web_pending) {
            tirtc_demo_schedule_work(TIRTC_DEMO_WORK_WEB_CALL_REJECT);
            return 0;
        }
        return tirtc_demo_cloud_reject();
    }
#else
        return tirtc_demo_cloud_reject();
#endif
    case TIRTC_DEMO_CALL_HANGUP:
#ifdef CONFIG_TIRTC
        tirtc_demo_schedule_work(TIRTC_DEMO_WORK_LOCAL_HANGUP);
        return 0;
#else
        return TIRTC_DEMO_ERR_SERVICE_PENDING;
#endif
    default:
        return -1;
    }
}

int tirtc_demo_app_handle_ai(tirtc_demo_ai_action_t action)
{
    tirtc_demo_runtime_t runtime;

    tirtc_demo_app_get_runtime(&runtime);

    switch (action) {
    case TIRTC_DEMO_AI_CONNECT:
    {
        bool busy = false;
        unsigned long flags;
        int result;

#ifdef CONFIG_TIRTC
        tirtc_demo_lock(&flags);
        busy = g_active_conn != NULL || g_connect_inflight ||
               g_session_type != TIRTC_DEMO_SESSION_NONE ||
               g_pending_session_count > 0U;
        tirtc_demo_unlock(flags);
#else
        (void)flags;
#endif
        result = !tirtc_demo_session_transport_ready() ?
                     TIRTC_DEMO_CLOUD_ERR_NOT_READY :
                 busy ? TIRTC_DEMO_CLOUD_ERR_BUSY :
                        tirtc_demo_cloud_request_ai_session();

        tirtc_demo_set_status(result == 0 ? TIRTC_DEMO_STATE_AI_TALK :
                                           TIRTC_DEMO_STATE_ERROR,
                              result, "AI 对讲",
                              result == 0 ? "正在获取 AI 会话凭据。" :
                                            "AI 会话请求失败。");
        return result;
    }
    case TIRTC_DEMO_AI_START_TX:
        if (!runtime.ai_ready) {
            return tirtc_demo_service_pending("AI 对讲",
                                              "AI 会话尚未建立，未开启麦克风发送。");
        }
#ifdef CONFIG_TIRTC
        tirtc_demo_schedule_work(TIRTC_DEMO_WORK_AI_START_TX);
        return 0;
#else
        return TIRTC_DEMO_ERR_SERVICE_PENDING;
#endif
    case TIRTC_DEMO_AI_STOP_TX:
#ifdef CONFIG_TIRTC
        tirtc_demo_schedule_work(TIRTC_DEMO_WORK_AI_STOP_TX);
        return 0;
#else
        return TIRTC_DEMO_ERR_SERVICE_PENDING;
#endif
    case TIRTC_DEMO_AI_INTERRUPT:
        if (!runtime.ai_ready) {
            return tirtc_demo_service_pending("AI 对讲",
                                              "AI 会话尚未建立，无法发送打断命令。");
        }
#ifdef CONFIG_TIRTC
        tirtc_demo_schedule_work(TIRTC_DEMO_WORK_AI_INTERRUPT);
        return 0;
#else
        return TIRTC_DEMO_ERR_SERVICE_PENDING;
#endif
    case TIRTC_DEMO_AI_NEW_CHAT:
    {
        bool active_ai = runtime.session_type != NULL &&
                         strcmp(runtime.session_type, "ai") == 0 &&
                         strcmp(runtime.call_phase, "idle") != 0;
        unsigned long flags;

        tirtc_demo_lock(&flags);
        tirtc_demo_clear_ai_messages_locked();
        tirtc_demo_unlock(flags);
#ifdef CONFIG_TIRTC
        if (active_ai) {
            tirtc_demo_schedule_work(TIRTC_DEMO_WORK_LOCAL_HANGUP);
        }
#endif
        tirtc_demo_set_status(TIRTC_DEMO_STATE_READY, 0, "AI 对讲",
                              active_ai ?
                                  "历史已清空，正在结束旧会话；可重新连接开始新对话。" :
                                  "对话历史已清空，可连接 AI 开始新对话。");
        return 0;
    }
    default:
        return -1;
    }
}

int tirtc_demo_app_set_ai_avatar(uint8_t avatar)
{
    tirtc_demo_config_t config;
    int result;

    if (avatar >= TIRTC_DEMO_AI_AVATAR_COUNT) {
        return TIRTC_E_INVALID_PARAMETER;
    }
    tirtc_demo_app_get_config(&config);
    config.ai_avatar = avatar;
    result = tirtc_demo_save_config_file(&config);
    if (result == 0) {
        tirtc_demo_apply_config(&config);
        tirtc_demo_set_status(TIRTC_DEMO_STATE_READY, 0, "AI 形象",
                              avatar == 0U ? "已选择伙伴形象。" :
                                             "已选择新芽形象。");
    } else {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, result, "AI 形象",
                              "AI 形象保存失败。");
    }
    memset(config.device_secret, 0, sizeof(config.device_secret));
    return result;
}

int tirtc_demo_app_refresh_wechat(void)
{
    int profile_result = tirtc_demo_cloud_refresh_wechat_profile();
    int contacts_result = tirtc_demo_cloud_refresh_wechat_contacts();
    int result = profile_result != 0 ? profile_result : contacts_result;

    tirtc_demo_set_status(result == 0 ? TIRTC_DEMO_STATE_READY :
                                       TIRTC_DEMO_STATE_ERROR,
                          result, "微信通话",
                          result == 0 ? "正在刷新微信设备信息和联系人。" :
                                        "微信业务尚未就绪，刷新失败。");
    return result;
}

int tirtc_demo_app_call_wechat(size_t index)
{
    char open_id[TIRTC_DEMO_WECHAT_OPENID_MAX];
    char app_id[TIRTC_DEMO_WECHAT_APP_ID_MAX];
    char model_id[TIRTC_DEMO_WECHAT_MODEL_ID_MAX];
    unsigned long flags;
    int result;

    tirtc_demo_sync_cloud_state();
    tirtc_demo_lock(&flags);
    if (index >= g_wechat_contact_count) {
        tirtc_demo_unlock(flags);
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, -1,
                              "微信联系人", "微信联系人不存在。");
        return -1;
    }
    tirtc_demo_copy_text(open_id, sizeof(open_id),
                         g_wechat_contacts[index].open_id);
    tirtc_demo_copy_text(app_id, sizeof(app_id),
                         g_wechat_contacts[index].app_id);
    tirtc_demo_copy_text(model_id, sizeof(model_id),
                         g_wechat_contacts[index].model_id);
    tirtc_demo_unlock(flags);
    if (!tirtc_demo_session_transport_ready()) {
        memset(open_id, 0, sizeof(open_id));
        memset(app_id, 0, sizeof(app_id));
        memset(model_id, 0, sizeof(model_id));
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR,
                              TIRTC_DEMO_CLOUD_ERR_NOT_READY,
                              "微信通话", "TiRTC 监听服务尚未就绪，请稍后重试。");
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    result = tirtc_demo_cloud_call_wechat(open_id, app_id, model_id);
    memset(open_id, 0, sizeof(open_id));
    memset(app_id, 0, sizeof(app_id));
    memset(model_id, 0, sizeof(model_id));
    tirtc_demo_set_status(result == 0 ? TIRTC_DEMO_STATE_CALLING :
                                       TIRTC_DEMO_STATE_ERROR,
                          result, "微信通话",
                          result == 0 ? "微信呼叫已提交，正在等待入会参数。" :
                                        "微信呼叫失败或当前已有会话。");
    return result;
}

int tirtc_demo_app_add_wechat_contact(const char *open_id)
{
    int result = tirtc_demo_cloud_add_wechat_contact(open_id);

    tirtc_demo_set_status(result == 0 ? TIRTC_DEMO_STATE_READY :
                                       TIRTC_DEMO_STATE_ERROR,
                          result, "微信联系人",
                          result == 0 ? "微信联系人添加请求已提交。" :
                                        "微信 OpenID 无效或服务未就绪。");
    return result;
}

int tirtc_demo_app_delete_wechat_contact(size_t index)
{
    char open_id[TIRTC_DEMO_WECHAT_OPENID_MAX];
    unsigned long flags;
    int result;

    tirtc_demo_sync_cloud_state();
    tirtc_demo_lock(&flags);
    if (index >= g_wechat_contact_count) {
        tirtc_demo_unlock(flags);
        return -1;
    }
    tirtc_demo_copy_text(open_id, sizeof(open_id),
                         g_wechat_contacts[index].open_id);
    tirtc_demo_unlock(flags);
    result = tirtc_demo_cloud_delete_wechat_contact(open_id);
    memset(open_id, 0, sizeof(open_id));
    tirtc_demo_set_status(result == 0 ? TIRTC_DEMO_STATE_READY :
                                       TIRTC_DEMO_STATE_ERROR,
                          result, "微信联系人",
                          result == 0 ? "微信联系人删除请求已提交。" :
                                        "微信联系人删除失败。");
    return result;
}

int tirtc_demo_app_accept_wechat(void)
{
#ifdef CONFIG_TIRTC
    bool valid;
    unsigned long flags;

    tirtc_demo_lock(&flags);
    valid = g_session_type == TIRTC_DEMO_SESSION_WECHAT &&
            g_session.incoming && !g_connect_inflight &&
            g_active_conn == NULL;
    if (valid) {
        g_session_connect_allowed = true;
        g_runtime.incoming_call = false;
        g_runtime.wechat_incoming = false;
        g_runtime.call_phase = "connecting";
    }
    tirtc_demo_unlock(flags);
    if (!valid) {
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    tirtc_demo_cloud_notify_session_connecting(
        TIRTC_DEMO_CLOUD_SESSION_WECHAT);
    tirtc_demo_set_status(TIRTC_DEMO_STATE_CALLING, 0, "微信通话",
                          "已接听，正在建立微信媒体连接。");
    tirtc_demo_schedule_work(TIRTC_DEMO_WORK_START);
    return 0;
#else
    return TIRTC_DEMO_ERR_SERVICE_PENDING;
#endif
}

int tirtc_demo_app_reject_wechat(void)
{
#ifdef CONFIG_TIRTC
    tirtc_demo_schedule_work(TIRTC_DEMO_WORK_WECHAT_REJECT);
    return 0;
#else
    return TIRTC_DEMO_ERR_SERVICE_PENDING;
#endif
}

int tirtc_demo_app_prepare_external_media(void)
{
    tirtc_demo_cloud_snapshot_t snapshot;
    bool busy;
    bool cloud_initialized;
    unsigned long flags;

    memset(&snapshot, 0, sizeof(snapshot));
    tirtc_demo_lock(&flags);
    cloud_initialized = g_cloud_initialized;
    tirtc_demo_unlock(flags);
    if (cloud_initialized) {
        tirtc_demo_cloud_get_snapshot(&snapshot);
    }
    tirtc_demo_lock(&flags);
#ifdef CONFIG_TIRTC
    busy = g_active_conn != NULL || g_connect_inflight ||
           g_session_type != TIRTC_DEMO_SESSION_NONE ||
           g_pending_session_count > 0U;
#else
    busy = false;
#endif
    busy = busy || snapshot.call_state != TIRTC_DEMO_CLOUD_CALL_IDLE;
    if (busy) {
        g_status.state = TIRTC_DEMO_STATE_ERROR;
        g_status.last_error = TIRTC_DEMO_CLOUD_ERR_BUSY;
        g_status.stage = "外设占用";
        g_status.message =
            "请先结束当前通话，再打开音频、摄像头或扫码应用。";
    } else {
        g_external_media_active = true;
    }
    tirtc_demo_unlock(flags);
    return busy ? TIRTC_DEMO_CLOUD_ERR_BUSY : 0;
}

void tirtc_demo_app_get_status(tirtc_demo_status_t *status)
{
    unsigned long flags;

    if (status == NULL) {
        return;
    }

    tirtc_demo_sync_cloud_state();
    tirtc_demo_lock(&flags);
    *status = g_status;
    tirtc_demo_unlock(flags);
}

void tirtc_demo_app_get_runtime(tirtc_demo_runtime_t *runtime)
{
    tirtc_demo_media_stats_t media;
    uint64_t now;
    unsigned long flags;

    if (runtime == NULL) {
        return;
    }

    tirtc_demo_sync_cloud_state();
    tirtc_demo_media_get_stats(&media);
    now = systick_get_time_ms();
    tirtc_demo_lock(&flags);
    g_runtime.rx_video_frames = media.rx_video_frames;
    g_runtime.rx_video_dropped = media.rx_video_dropped;
    g_runtime.video_rx_active = g_runtime.peer_connected &&
                                media.rx_video_frames > 0U;
    *runtime = g_runtime;
    runtime->call_duration_s = g_runtime.call_active &&
                               g_call_started_at_ms != 0U &&
                               now >= g_call_started_at_ms ?
        (uint32_t)((now - g_call_started_at_ms) / 1000U) : 0U;
    tirtc_demo_unlock(flags);
}

void tirtc_demo_app_get_settings(tirtc_demo_settings_t *settings)
{
    unsigned long flags;

    if (settings == NULL) {
        return;
    }

    tirtc_demo_load_config_once();
    memset(settings, 0, sizeof(*settings));
    tirtc_demo_lock(&flags);
    tirtc_demo_copy_text(settings->endpoint, sizeof(settings->endpoint),
                         g_config.endpoint);
    tirtc_demo_copy_text(settings->peer_id, sizeof(settings->peer_id),
                         g_config.device_id[0] != '\0' ?
                             g_config.device_id : TIRTC_DEMO_UNCONFIGURED_TEXT);
    tirtc_demo_copy_text(settings->codec, sizeof(settings->codec), "PCM");
    settings->sample_rate = 16000;
    settings->channels = 1;
    settings->packet_ms = 20;
    tirtc_demo_unlock(flags);
}

void tirtc_demo_app_get_config(tirtc_demo_config_t *config)
{
    unsigned long flags;

    if (config == NULL) {
        return;
    }

    tirtc_demo_load_config_once();
    tirtc_demo_lock(&flags);
    *config = g_config;
    tirtc_demo_unlock(flags);
}

int tirtc_demo_app_save_config(const tirtc_demo_config_t *config)
{
    if (!tirtc_demo_config_valid(config)) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_UNCONFIGURED,
                              TIRTC_DEMO_ERR_CONFIG_REQUIRED,
                              "配置", "服务地址、设备 ID 或设备密钥格式不正确。" );
        return TIRTC_DEMO_ERR_CONFIG_REQUIRED;
    }

    if (tirtc_demo_save_config_file(config) != 0) {
        tirtc_demo_set_status(TIRTC_DEMO_STATE_ERROR, -1, "配置",
                              "配置写入 /data 失败，请检查数据分区。" );
        return -1;
    }

    tirtc_demo_apply_config(config);
    (void)tirtc_demo_ensure_cloud();
    (void)tirtc_demo_cloud_apply_identity(config->device_id,
                                           config->device_secret);
    tirtc_demo_set_status(TIRTC_DEMO_STATE_READY, 0, "配置",
                          "设备凭据已保存，正在重启 TiRTC 服务。" );
    printf("[tirtc_demo] config saved device_id_len=%u\n",
           (unsigned int)strlen(config->device_id));
    return tirtc_demo_app_restart();
}

bool tirtc_demo_app_config_ready(void)
{
    tirtc_demo_config_t config;

    tirtc_demo_app_get_config(&config);
    return tirtc_demo_config_valid(&config);
}

void tirtc_demo_app_copy_device_id(char *device_id, size_t capacity)
{
    unsigned long flags;

    if (device_id == NULL || capacity == 0U) {
        return;
    }

    tirtc_demo_load_config_once();
    tirtc_demo_lock(&flags);
    tirtc_demo_copy_text(device_id, capacity,
                         g_config.device_id[0] != '\0' ?
                             g_config.device_id : TIRTC_DEMO_UNCONFIGURED_TEXT);
    tirtc_demo_unlock(flags);
}

int tirtc_demo_app_get_volume(tirtc_demo_volume_t volume)
{
    unsigned long flags;
    int value;

    tirtc_demo_lock(&flags);
    value = volume == TIRTC_DEMO_VOLUME_TX ? g_tx_volume : g_rx_volume;
    tirtc_demo_unlock(flags);
    return value;
}

bool tirtc_demo_app_is_muted(tirtc_demo_volume_t volume)
{
    unsigned long flags;
    bool muted;

    tirtc_demo_lock(&flags);
    muted = volume == TIRTC_DEMO_VOLUME_TX ? g_tx_muted : g_rx_muted;
    tirtc_demo_unlock(flags);
    return muted;
}

size_t tirtc_demo_app_get_contacts(const tirtc_demo_contact_t **contacts)
{
    size_t count;
    unsigned long flags;

    tirtc_demo_sync_cloud_state();
    tirtc_demo_lock(&flags);
    if (contacts != NULL) {
        *contacts = g_contacts;
    }
    count = g_contact_count;
    tirtc_demo_unlock(flags);

    return count;
}

size_t tirtc_demo_app_get_wechat_contacts(
    const tirtc_demo_wechat_contact_t **contacts)
{
    size_t count;
    unsigned long flags;

    tirtc_demo_sync_cloud_state();
    tirtc_demo_lock(&flags);
    if (contacts != NULL) {
        *contacts = g_wechat_contacts;
    }
    count = g_wechat_contact_count;
    tirtc_demo_unlock(flags);
    return count;
}

size_t tirtc_demo_app_copy_ai_messages(tirtc_demo_ai_message_t *messages,
                                       size_t capacity)
{
    size_t count;
    unsigned long flags;

    tirtc_demo_lock(&flags);
    count = g_ai_message_count;
    if (messages != NULL && capacity > 0U) {
        size_t copy_count = count < capacity ? count : capacity;

        memcpy(messages, g_ai_messages, sizeof(messages[0]) * copy_count);
        count = copy_count;
    }
    tirtc_demo_unlock(flags);
    return count;
}

void tirtc_demo_app_copy_ai_caption(char *caption, size_t capacity)
{
    unsigned long flags;

    if (caption == NULL || capacity == 0U) {
        return;
    }
    tirtc_demo_lock(&flags);
    tirtc_demo_copy_utf8(caption, capacity, g_ai_caption);
    tirtc_demo_unlock(flags);
}
