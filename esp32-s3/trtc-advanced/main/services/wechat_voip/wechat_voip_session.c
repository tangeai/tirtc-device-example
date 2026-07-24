/*
 * 微信 VoIP 会话管理.
 *
 * 业务服务器下发入会通知后,本文件保存本次 peer_id/token,
 * 再由界面接听或主动呼叫自动入会触发 WHIP 建连.
 * WHIP 建连后等待 CALL_CONNECTED, 确认通话进入媒体阶段后再发音频.
 */
#include "wechat_voip_session.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "tiRTC.h"
#include "tirtc_session.h"
#include "tirtc_voip_cmdw.h"
#include "wechat_voip_config.h"
#include "wechat_voip_media.h"
#include "wechat_voip_trace.h"

static const char *TAG = "wx_voip";

enum
{
    VOIP_ANSWER_TASK_STACK = 96 * 1024,
    VOIP_ANSWER_TASK_PRIORITY = 5,
    VOIP_UI_ANSWER_DELAY_MS = 0,
    VOIP_ACTIVE_ANSWER_DELAY_MS = 0,
    VOIP_ANSWER_RETRY_DELAY_MS = 100,
    VOIP_RING_TIMEOUT_MS = 35000,
    VOIP_CONNECT_TIMEOUT_MS = 20000,
    VOIP_CONNECTED_WAIT_TIMEOUT_MS = 15000,
    VOIP_OUTBOUND_CONNECTED_WAIT_TIMEOUT_MS = VOIP_RING_TIMEOUT_MS,
    VOIP_CLOSE_WAIT_TIMEOUT_MS = 1500,
    VOIP_RECALL_GUARD_MS = 1200,
    VOIP_ERROR_RECALL_GUARD_MS = 15000,
    VOIP_STATUS_WARN_INTERVAL_MS = 2000,
    VOIP_MEDIA_STOP_WAIT_MS = 300,
    VOIP_PENDING_JOIN_POLL_MS = 50,
    VOIP_PENDING_JOIN_TIMEOUT_MS = 50000,
    VOIP_PENDING_JOIN_TASK_STACK = 16 * 1024,
    VOIP_PENDING_JOIN_TASK_PRIORITY = 5,
    VOIP_WORK_QUEUE_LEN = 8,
    VOIP_WORK_TASK_STACK = 24 * 1024,
    VOIP_WORK_TASK_PRIORITY = 5,
    VOIP_DISCONNECT_DELAY_MS = 120,
    VOIP_WHIP_MIN_INTERNAL_FREE = 24 * 1024,
    VOIP_WHIP_WARN_INTERNAL_FREE = 32 * 1024,
    VOIP_WHIP_MIN_INTERNAL_LARGEST = 8 * 1024,
    VOIP_WHIP_WARN_INTERNAL_LARGEST = 16 * 1024,
    VOIP_WHIP_PEER_ID_MAX = 2048,
    VOIP_WHIP_TOKEN_MAX = 1536,
    VOIP_WX_SESSION_TOKEN_MAX = 1024,
};

typedef enum
{
    VOIP_STATE_IDLE,
    VOIP_STATE_RINGING,
    VOIP_STATE_CONNECTING,
    VOIP_STATE_AWAITING_CONNECTED,
    VOIP_STATE_IN_CALL,
    VOIP_STATE_CLOSING,
} voip_state_t;

typedef struct
{
    voip_state_t state;
    tirtc_conn_t hconn;
    char peer_id[VOIP_WHIP_PEER_ID_MAX];
    char token[VOIP_WHIP_TOKEN_MAX];
    char wx_app_id[64];
    char wx_model_id[64];
    char wx_session_token[VOIP_WX_SESSION_TOKEN_MAX];
    char wx_room_id[128];
    char wx_payload[256];
    int64_t deadline_us;
    bool outbound_call;
} voip_session_t;

typedef struct
{
    char wx_app_id[64];
    char wx_model_id[64];
    char wx_session_token[VOIP_WX_SESSION_TOKEN_MAX];
    char wx_room_id[128];
    char wx_payload[256];
} voip_reject_info_t;

typedef enum
{
    VOIP_WORK_DISCONNECT,
    VOIP_WORK_HANGUP,
    VOIP_WORK_REJECT,
} voip_work_type_t;

typedef struct
{
    voip_work_type_t type;
    tirtc_conn_t hconn;
    tirtc_voip_hangup_reason_t reason;
    bool only_if_current;
    voip_reject_info_t reject;
} voip_work_item_t;

typedef struct
{
    voip_state_t state;
    tirtc_conn_t hconn;
    bool answer_pending;
    TaskHandle_t answer_task;
    bool work_busy;
    voip_work_type_t work_type;
    uint32_t work_queue_len;
    bool media_running;
    wechat_voip_media_stats_t media;
    bool rtc_active_connection;
    bool rtc_call_active;
    bool pending_join;
    uint32_t pending_join_seq;
    tirtc_session_state_t rtc_state;
    int64_t deadline_left_ms;
    int64_t last_close_ago_ms;
    int64_t recall_wait_ms;
} voip_status_t;

typedef struct
{
    voip_session_t session;
    bool valid;
    uint32_t seq;
    int64_t expire_us;
} voip_pending_join_t;

static SemaphoreHandle_t s_mutex;
/*
 * 5144B in the current map. This is WeChat room/token text plus call state;
 * it is not DMA data and is not used by flash/NVS/OTA code while cache is
 * disabled, so keep it in PSRAM and leave internal RAM for SDK/audio/flash.
 */
static EXT_RAM_BSS_ATTR voip_session_t s_session;
static EXT_RAM_BSS_ATTR voip_pending_join_t s_pending_join;
static TaskHandle_t s_answer_worker_task;
static TaskHandle_t s_pending_join_worker_task;
static QueueHandle_t s_work_queue;
static TaskHandle_t s_work_task;
static int64_t s_last_close_us;
static int64_t s_recall_block_until_us;
static bool s_answer_pending;
static char s_answer_source[32];
static uint32_t s_answer_delay_ms;
static uint32_t s_answer_request_seq;
static portMUX_TYPE s_work_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_work_busy;
static voip_work_type_t s_work_type;
static int64_t s_last_status_warn_us;
static bool s_idle_not_ready_reported;

static void voip_work_task(void *arg);
static esp_err_t ensure_work_worker(void);
static esp_err_t start_answer_worker(void);
static esp_err_t start_pending_join_worker(void);
static esp_err_t enqueue_work(const voip_work_item_t *item);
static esp_err_t send_reject_info(const voip_reject_info_t *info, tirtc_voip_hangup_reason_t reason);
static esp_err_t reject_info_later(const voip_reject_info_t *info, tirtc_voip_hangup_reason_t reason);
static void collect_status(voip_status_t *status);
static bool status_available_for_ringing(const voip_status_t *status);
static bool status_ready_for_next_call(const voip_status_t *status);
static const char *status_not_ready_reason(const voip_status_t *status);

static void ensure_init(void)
{
    if (s_mutex != NULL)
    {
        return;
    }

    s_mutex = xSemaphoreCreateMutexWithCaps(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    configASSERT(s_mutex != NULL);
    s_session.state = VOIP_STATE_IDLE;
    (void)ensure_work_worker();
    (void)start_answer_worker();
    (void)start_pending_join_worker();
}

static void lock_session(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock_session(void)
{
    xSemaphoreGive(s_mutex);
}

static bool lock_session_wait(const char *where, uint32_t timeout_ms)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
    {
        return true;
    }

    ESP_LOGW(TAG, "等待会话锁超时: %s", where ? where : "未知位置");
    return false;
}

static void copy_str(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0)
    {
        return;
    }

    if (src == NULL)
    {
        dst[0] = '\0';
        return;
    }

    strlcpy(dst, src, dst_size);
}

static const char *state_name(voip_state_t state)
{
    switch (state)
    {
    case VOIP_STATE_IDLE:
        return "空闲";
    case VOIP_STATE_RINGING:
        return "振铃";
    case VOIP_STATE_CONNECTING:
        return "连接中";
    case VOIP_STATE_AWAITING_CONNECTED:
        return "等待接通";
    case VOIP_STATE_IN_CALL:
        return "通话中";
    case VOIP_STATE_CLOSING:
        return "关闭中";
    default:
        return "未知";
    }
}

static const char *work_type_name(voip_work_type_t type)
{
    switch (type)
    {
    case VOIP_WORK_DISCONNECT:
        return "断开";
    case VOIP_WORK_HANGUP:
        return "挂断";
    case VOIP_WORK_REJECT:
        return "拒接";
    default:
        return "未知";
    }
}

static void clear_session_locked(void)
{
    WX_VOIP_TRACEI(TAG, "清空会话: old_state=%s hconn=%p", state_name(s_session.state), s_session.hconn);
    memset(&s_session, 0, sizeof(s_session));
    s_session.state = VOIP_STATE_IDLE;
    s_answer_pending = false;
    s_answer_source[0] = '\0';
    s_answer_delay_ms = 0;
}

static void finish_session_locked(void)
{
    WX_VOIP_TRACEI(TAG, "结束会话: state=%s hconn=%p", state_name(s_session.state), s_session.hconn);
    clear_session_locked();
    s_last_close_us = esp_timer_get_time();
}
static void block_recall_locked(uint32_t guard_ms)
{
    int64_t now_us = esp_timer_get_time();
    int64_t until_us = now_us + (int64_t)guard_ms * 1000;

    if (until_us > s_recall_block_until_us)
    {
        s_recall_block_until_us = until_us;
    }
}

static void set_deadline_locked(uint32_t timeout_ms)
{
    s_session.deadline_us = timeout_ms == 0 ? 0 : esp_timer_get_time() + (int64_t)timeout_ms * 1000;
}

static void fill_join_session(voip_session_t *session,
                              const char *peer_id,
                              const char *token,
                              const char *app_id,
                              const char *model_id,
                              const char *session_token,
                              const char *room_id,
                              const char *wx_payload,
                              bool auto_answer)
{
    if (session == NULL)
    {
        return;
    }

    memset(session, 0, sizeof(*session));
    copy_str(session->peer_id, sizeof(session->peer_id), peer_id);
    copy_str(session->token, sizeof(session->token), token);
    copy_str(session->wx_app_id, sizeof(session->wx_app_id), app_id);
    copy_str(session->wx_model_id, sizeof(session->wx_model_id), model_id);
    copy_str(session->wx_session_token, sizeof(session->wx_session_token), session_token);
    copy_str(session->wx_room_id, sizeof(session->wx_room_id), room_id);
    copy_str(session->wx_payload, sizeof(session->wx_payload), wx_payload);
    session->outbound_call = auto_answer;
    session->state = VOIP_STATE_RINGING;
}

static void begin_close_locked(void)
{
    WX_VOIP_TRACEI(TAG, "进入关闭状态: old_state=%s hconn=%p", state_name(s_session.state), s_session.hconn);
    s_session.state = VOIP_STATE_CLOSING;
    s_answer_pending = false;
    s_answer_source[0] = '\0';
    s_answer_delay_ms = 0;
    set_deadline_locked(VOIP_CLOSE_WAIT_TIMEOUT_MS);
}

static bool connection_is_current(tirtc_conn_t hconn)
{
    if (hconn == NULL)
    {
        return false;
    }

    lock_session();
    bool current = (s_session.hconn == hconn && s_session.state != VOIP_STATE_IDLE);
    unlock_session();
    return current;
}

static void disconnect_later(tirtc_conn_t hconn, bool only_if_current)
{
    if (hconn == NULL)
    {
        return;
    }

    WX_VOIP_TRACEI(TAG,
                   "投递断开任务: hconn=%p only_if_current=%d",
                   hconn,
                   only_if_current ? 1 : 0);

    voip_work_item_t item = {
        .type = VOIP_WORK_DISCONNECT,
        .hconn = hconn,
        .only_if_current = only_if_current,
    };
    if (enqueue_work(&item) != ESP_OK)
    {
        ESP_LOGW(TAG, "断开任务投递失败,等待下一次维护");
    }
}

static bool voip_cmd_is(uint32_t cmdw, uint32_t expected)
{
    return cmdw == expected ||
           (cmdw & 0xffffU) == expected ||
           (cmdw & 0x7fffU) == expected;
}

static void extract_query_param(const char *url, const char *key, char *out, size_t out_size)
{
    if (url == NULL || key == NULL || out == NULL || out_size == 0)
    {
        return;
    }
    out[0] = '\0';

    char search[64];
    snprintf(search, sizeof(search), "%s=", key);

    const char *p = strstr(url, search);
    if (p == NULL)
    {
        return;
    }
    p += strlen(search);

    size_t i = 0;
    while (*p != '\0' && *p != '&' && i < out_size - 1)
    {
        out[i++] = *p++;
    }
    out[i] = '\0';
}

static const char *json_string_any(cJSON *root, const char *name1, const char *name2)
{
    if (root == NULL || name1 == NULL)
    {
        return NULL;
    }

    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, name1));
    if ((value == NULL || value[0] == '\0') && name2 != NULL)
    {
        value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, name2));
    }
    return value;
}

static void on_reject_response(const char *body, void *user_data)
{
    (void)user_data;
    if (body != NULL && body[0] != '\0')
    {
        WX_VOIP_TRACEI(TAG, "拒接响应: %.120s", body);
    }
}

static esp_err_t send_reject(const voip_reject_info_t *info, tirtc_voip_hangup_reason_t reason)
{
    if (info == NULL || info->wx_room_id[0] == '\0')
    {
        ESP_LOGW(TAG, "拒接请求缺少房间信息");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        ESP_LOGE(TAG, "拒接请求内容创建失败");
        return ESP_ERR_NO_MEM;
    }
    /* 平台拒接接口使用 wx_* 字段; wxa_* 作为旧示例服务端兼容字段保留. */
    cJSON_AddStringToObject(root, "wx_app_id", info->wx_app_id);
    cJSON_AddStringToObject(root, "wx_model_id", info->wx_model_id);
    cJSON_AddStringToObject(root, "wx_session_token", info->wx_session_token);
    cJSON_AddStringToObject(root, "wx_room_id", info->wx_room_id);
    cJSON_AddStringToObject(root, "wx_payload", info->wx_payload);
    cJSON_AddStringToObject(root, "wxa_app_id", info->wx_app_id);
    cJSON_AddStringToObject(root, "wxa_model_id", info->wx_model_id);
    cJSON_AddStringToObject(root, "wxa_session_token", info->wx_session_token);
    cJSON_AddStringToObject(root, "wxa_room_id", info->wx_room_id);
    cJSON_AddStringToObject(root, "wxa_payload", info->wx_payload);
    cJSON_AddNumberToObject(root, "hangup_reason", (int)reason);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL)
    {
        ESP_LOGE(TAG, "拒接请求内容序列化失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "发送微信拒接: reason=%d", (int)reason);
    WX_VOIP_TRACEI(TAG,
                   "调用 TiRTC service request 拒接: room=%s app_id=%s model_id=%s session_token_len=%u payload_len=%u",
                   info->wx_room_id,
                   info->wx_app_id[0] ? info->wx_app_id : "(空)",
                   info->wx_model_id[0] ? info->wx_model_id : "(空)",
                   (unsigned)strlen(info->wx_session_token),
                   (unsigned)strlen(info->wx_payload));
    int rc = tirtc_session_service_request("/v1/wxvoip/reject", body, NULL, on_reject_response, NULL);
    free(body);
    if (rc != 0)
    {
        ESP_LOGW(TAG, "通知 TiRTC 拒接失败: %d %s", rc, tirtc_session_error_string(rc));
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void fill_reject_info(voip_reject_info_t *info, const voip_session_t *sess)
{
    if (info == NULL)
    {
        return;
    }

    memset(info, 0, sizeof(*info));
    if (sess == NULL)
    {
        return;
    }

    copy_str(info->wx_app_id, sizeof(info->wx_app_id), sess->wx_app_id);
    copy_str(info->wx_model_id, sizeof(info->wx_model_id), sess->wx_model_id);
    copy_str(info->wx_session_token, sizeof(info->wx_session_token), sess->wx_session_token);
    copy_str(info->wx_room_id, sizeof(info->wx_room_id), sess->wx_room_id);
    copy_str(info->wx_payload, sizeof(info->wx_payload), sess->wx_payload);
}

static esp_err_t send_reject_info(const voip_reject_info_t *info, tirtc_voip_hangup_reason_t reason)
{
    if (info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return send_reject(info, reason);
}

static void fill_reject_info_from_join(voip_reject_info_t *info, cJSON *payload)
{
    const char *peer_id = NULL;
    const char *app_id = NULL;
    const char *model_id = NULL;
    const char *session_token = NULL;
    const char *room_id = NULL;
    const char *wx_payload = NULL;
    char app_id_from_peer[64] = {0};
    char model_id_from_peer[64] = {0};

    if (info == NULL) {
        return;
    }
    memset(info, 0, sizeof(*info));
    if (!cJSON_IsObject(payload)) {
        return;
    }

    peer_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "peer_id"));
    app_id = json_string_any(payload, "wx_app_id", "wxa_app_id");
    model_id = json_string_any(payload, "wx_model_id", "wxa_model_id");
    session_token = json_string_any(payload, "wx_session_token", "wxa_session_token");
    room_id = json_string_any(payload, "wx_room_id", "wxa_room_id");
    wx_payload = json_string_any(payload, "wx_payload", "wxa_payload");
    if (session_token == NULL || session_token[0] == '\0') {
        session_token = json_string_any(payload, "wx_server_token", "wxa_server_token");
    }
    if ((app_id == NULL || app_id[0] == '\0') && peer_id != NULL) {
        extract_query_param(peer_id, "x_wx_app_id", app_id_from_peer, sizeof(app_id_from_peer));
        if (app_id_from_peer[0] == '\0') {
            extract_query_param(peer_id, "x_wxa_app_id", app_id_from_peer, sizeof(app_id_from_peer));
        }
        app_id = app_id_from_peer;
    }
    if ((model_id == NULL || model_id[0] == '\0') && peer_id != NULL) {
        extract_query_param(peer_id, "x_wx_model_id", model_id_from_peer, sizeof(model_id_from_peer));
        if (model_id_from_peer[0] == '\0') {
            extract_query_param(peer_id, "x_wxa_model_id", model_id_from_peer, sizeof(model_id_from_peer));
        }
        model_id = model_id_from_peer;
    }

    copy_str(info->wx_app_id, sizeof(info->wx_app_id), app_id);
    copy_str(info->wx_model_id, sizeof(info->wx_model_id), model_id);
    copy_str(info->wx_session_token, sizeof(info->wx_session_token), session_token);
    copy_str(info->wx_room_id, sizeof(info->wx_room_id), room_id);
    copy_str(info->wx_payload, sizeof(info->wx_payload), wx_payload);
}

esp_err_t wechat_voip_session_reject_join_room_busy(cJSON *payload)
{
    voip_reject_info_t reject_info = {0};

    ensure_init();
    fill_reject_info_from_join(&reject_info, payload);
    if (reject_info.wx_room_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return reject_info_later(&reject_info, TIRTC_VOIP_HANGUP_REASON_BUSY);
}

static void set_work_busy(bool busy, voip_work_type_t type)
{
    portENTER_CRITICAL(&s_work_state_lock);
    s_work_busy = busy;
    s_work_type = type;
    portEXIT_CRITICAL(&s_work_state_lock);
    WX_VOIP_TRACEI(TAG, "工作队列状态: busy=%d type=%s", busy ? 1 : 0, work_type_name(type));
}

static void voip_work_task(void *arg)
{
    (void)arg;

    /*
     * TiRTC 的挂断、拒接、断开都可能触发网络和 SDK 内部收尾.
     * 统一放到固定 worker 中串行执行,避免按键任务、维护任务和 SDK 回调被拖住.
     */
    while (true)
    {
        voip_work_item_t item;
        if (xQueueReceive(s_work_queue, &item, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        set_work_busy(true, item.type);
        WX_VOIP_TRACEI(TAG,
                       "开始处理工作项: type=%s hconn=%p reason=%d only_if_current=%d",
                       work_type_name(item.type),
                       item.hconn,
                       (int)item.reason,
                       item.only_if_current ? 1 : 0);
        switch (item.type)
        {
        case VOIP_WORK_HANGUP:
            if (item.hconn != NULL)
            {
                char body[32];
                int n = snprintf(body, sizeof(body), "{\"reason\":%d}", (int)item.reason);
                if (connection_is_current(item.hconn))
                {
                    (void)wechat_voip_media_stop_wait(item.hconn, VOIP_MEDIA_STOP_WAIT_MS);
                    (void)tirtc_session_set_external_audio_call_active(item.hconn, false);
                    if (n > 0 && n < (int)sizeof(body))
                    {
                        WX_VOIP_TRACEI(TAG,
                                       "发送 TiRTC 挂断命令: hconn=%p reason=%d",
                                       item.hconn,
                                       (int)item.reason);
                        (void)tirtc_session_send_command_raw(item.hconn, TIRTC_VOIP_HANGUP, body, strlen(body));
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(VOIP_DISCONNECT_DELAY_MS));
                if (connection_is_current(item.hconn))
                {
                    WX_VOIP_TRACEI(TAG, "调用 TiRTC disconnect wrapper: hconn=%p", item.hconn);
                    (void)tirtc_session_disconnect_connection(item.hconn);
                }
                ESP_LOGI(TAG, "微信通话挂断流程已提交");
            }
            break;

        case VOIP_WORK_DISCONNECT:
            if (item.hconn != NULL)
            {
                if (!item.only_if_current || connection_is_current(item.hconn))
                {
                    (void)wechat_voip_media_stop_wait(item.hconn, VOIP_MEDIA_STOP_WAIT_MS);
                    (void)tirtc_session_set_external_audio_call_active(item.hconn, false);
                    vTaskDelay(pdMS_TO_TICKS(VOIP_DISCONNECT_DELAY_MS));
                    if (!item.only_if_current || connection_is_current(item.hconn))
                    {
                        ESP_LOGI(TAG, "释放微信通话连接");
                        WX_VOIP_TRACEI(TAG, "调用 TiRTC disconnect wrapper: hconn=%p", item.hconn);
                        (void)tirtc_session_disconnect_connection(item.hconn);
                    }
                }
            }
            break;

        case VOIP_WORK_REJECT:
            if (send_reject_info(&item.reject, item.reason) == ESP_OK)
            {
                ESP_LOGI(TAG, "%s", item.reason == TIRTC_VOIP_HANGUP_REASON_REJECT ? "已拒接微信来电" : "已取消微信通话");
            }
            break;
        }
        WX_VOIP_TRACEI(TAG, "工作项处理完成: type=%s", work_type_name(item.type));
        set_work_busy(false, item.type);
    }
}

static esp_err_t ensure_work_worker(void)
{
    if (s_work_queue == NULL)
    {
        s_work_queue = xQueueCreateWithCaps(VOIP_WORK_QUEUE_LEN,
                                            sizeof(voip_work_item_t),
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (s_work_queue == NULL)
        {
            ESP_LOGE(TAG, "创建微信 VoIP 工作队列失败");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_work_task != NULL)
    {
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreateWithCaps(voip_work_task,
                                         "wx_voip_work",
                                         VOIP_WORK_TASK_STACK,
                                         NULL,
                                         VOIP_WORK_TASK_PRIORITY,
                                         &s_work_task,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS)
    {
        s_work_task = NULL;
        ESP_LOGE(TAG, "创建微信 VoIP 工作任务失败");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t enqueue_work(const voip_work_item_t *item)
{
    if (item == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ensure_work_worker();
    if (ret != ESP_OK)
    {
        return ret;
    }
    WX_VOIP_TRACEI(TAG,
                   "投递工作项: type=%s hconn=%p reason=%d queue_before=%u",
                   work_type_name(item->type),
                   item->hconn,
                   (int)item->reason,
                   s_work_queue ? (unsigned)uxQueueMessagesWaiting(s_work_queue) : 0);
    return xQueueSend(s_work_queue, item, 0) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t reject_info_later(const voip_reject_info_t *info, tirtc_voip_hangup_reason_t reason)
{
    voip_work_item_t item = {
        .type = VOIP_WORK_REJECT,
        .reason = reason,
    };
    if (info != NULL)
    {
        item.reject = *info;
    }

    esp_err_t ret = enqueue_work(&item);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "拒接任务投递失败: %s", esp_err_to_name(ret));
    }
    return ret;
}

static void on_whip_connect(int error, tirtc_conn_t hconn, void *user_data)
{
    (void)user_data;
    ensure_init();

    WX_VOIP_TRACEI(TAG,
                   "WHIP 回调: error=%d hconn=%p",
                   error,
                   hconn);

    if (error != 0)
    {
        ESP_LOGE(TAG, "通话连接失败: %d %s", error, tirtc_session_error_string(error));
        voip_reject_info_t failed = {0};
        lock_session();
        fill_reject_info(&failed, &s_session);
        finish_session_locked();
        unlock_session();
        (void)reject_info_later(&failed, TIRTC_VOIP_HANGUP_REASON_EXCEPTION);
        return;
    }

    if (hconn == NULL)
    {
        ESP_LOGE(TAG, "通话连接失败: WHIP 返回空连接");
        voip_reject_info_t failed = {0};
        lock_session();
        fill_reject_info(&failed, &s_session);
        finish_session_locked();
        unlock_session();
        (void)reject_info_later(&failed, TIRTC_VOIP_HANGUP_REASON_EXCEPTION);
        return;
    }

    bool should_accept = false;
    lock_session();
    if (s_session.state == VOIP_STATE_CONNECTING)
    {
        should_accept = true;
    }
    unlock_session();

    if (!should_accept)
    {
        ESP_LOGW(TAG, "连接回调到达时会话已结束,释放连接");
        disconnect_later(hconn, false);
        return;
    }

    esp_err_t track_ret = tirtc_session_track_external_connection(hconn, false);
    if (track_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "登记 WHIP 连接失败: %s", esp_err_to_name(track_ret));
        (void)tirtc_session_disconnect_connection(hconn);
        voip_reject_info_t failed = {0};
        lock_session();
        fill_reject_info(&failed, &s_session);
        finish_session_locked();
        unlock_session();
        (void)reject_info_later(&failed, TIRTC_VOIP_HANGUP_REASON_EXCEPTION);
        return;
    }

    lock_session();
    if (s_session.state == VOIP_STATE_CONNECTING)
    {
        bool outbound_call = s_session.outbound_call;
        WX_VOIP_TRACEI(TAG,
                       "WHIP 连接成功: old_state=%s hconn=%p outbound=%d",
                       state_name(s_session.state),
                       hconn,
                       outbound_call ? 1 : 0);
        s_session.hconn = hconn;
        s_session.state = VOIP_STATE_AWAITING_CONNECTED;
        set_deadline_locked(outbound_call
                                ? VOIP_OUTBOUND_CONNECTED_WAIT_TIMEOUT_MS
                                : VOIP_CONNECTED_WAIT_TIMEOUT_MS);
        unlock_session();

        ESP_LOGI(TAG,
                 "%s",
                 outbound_call ? "主动呼叫 WHIP 已连接,等待微信 0x2000/0x2001" : "通话连接已建立,等待接通确认");
        return;
    }
    if ((s_session.state == VOIP_STATE_AWAITING_CONNECTED || s_session.state == VOIP_STATE_IN_CALL) &&
        s_session.hconn == hconn)
    {
        voip_state_t state = s_session.state;
        unlock_session();
        ESP_LOGD(TAG, "连接回调重复到达,当前状态=%s", state_name(state));
        return;
    }
    unlock_session();

    ESP_LOGW(TAG, "连接回调到达时会话已结束,释放连接");
    disconnect_later(hconn, false);
}

static esp_err_t answer_current_call(const char *source)
{
    tirtc_session_stats_t rtc_stats = {0};
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (internal_free < VOIP_WHIP_MIN_INTERNAL_FREE ||
        internal_largest < VOIP_WHIP_MIN_INTERNAL_LARGEST)
    {
        ESP_LOGW(TAG,
                 "微信接听等待内存恢复: internal_free=%u internal_largest=%u need=%u/%u",
                 (unsigned)internal_free,
                 (unsigned)internal_largest,
                 (unsigned)VOIP_WHIP_MIN_INTERNAL_FREE,
                 (unsigned)VOIP_WHIP_MIN_INTERNAL_LARGEST);
        return ESP_ERR_NO_MEM;
    }
    if (internal_free < VOIP_WHIP_WARN_INTERNAL_FREE)
    {
        ESP_LOGW(TAG,
                 "微信接听内部余量偏低,继续尝试: internal_free=%u internal_largest=%u warn=%u",
                 (unsigned)internal_free,
                 (unsigned)internal_largest,
                 (unsigned)VOIP_WHIP_WARN_INTERNAL_FREE);
    }
    if (internal_largest < VOIP_WHIP_WARN_INTERNAL_LARGEST)
    {
        ESP_LOGW(TAG,
                 "微信接听内部最大块偏低,继续尝试: internal_free=%u internal_largest=%u warn=%u",
                 (unsigned)internal_free,
                 (unsigned)internal_largest,
                 (unsigned)VOIP_WHIP_WARN_INTERNAL_LARGEST);
    }

    tirtc_session_get_stats(&rtc_stats);
    if (!rtc_stats.sdk_started || rtc_stats.state == TIRTC_SESSION_STATE_STARTING ||
        rtc_stats.state == TIRTC_SESSION_STATE_DISCONNECTING)
    {
        ESP_LOGW(TAG,
                 "微信接听等待 RTC 就绪: sdk_init=%d sdk_start=%d state=%d",
                 rtc_stats.sdk_initialized ? 1 : 0,
                 rtc_stats.sdk_started ? 1 : 0,
                 (int)rtc_stats.state);
        return ESP_ERR_INVALID_STATE;
    }
    if (rtc_stats.active_connection || rtc_stats.call_active ||
        rtc_stats.state == TIRTC_SESSION_STATE_CONNECTED ||
        rtc_stats.state == TIRTC_SESSION_STATE_MEDIA_BOOTSTRAPPING)
    {
        /*
         * A second WHIP must not be submitted while the previous TiRTC
         * connection is still active or being media-bootstrapped. The SDK will
         * create ICE sockets from its rtc_thread; overlapping that with an old
         * connection can corrupt the LWIP/heap path before the app sees a clean
         * error. Treat it as a transient wait condition and let answer_task
         * retry after the disconnect callback has drained.
         */
        ESP_LOGW(TAG,
                 "微信接听等待上一通 RTC 连接释放: active=%d call=%d state=%d last=%s",
                 rtc_stats.active_connection ? 1 : 0,
                 rtc_stats.call_active ? 1 : 0,
                 (int)rtc_stats.state,
                 rtc_stats.last_event);
        return ESP_ERR_INVALID_STATE;
    }

    lock_session();
    if (s_session.state != VOIP_STATE_RINGING)
    {
        unlock_session();
        return ESP_ERR_INVALID_STATE;
    }

    s_session.state = VOIP_STATE_CONNECTING;
    set_deadline_locked(VOIP_CONNECT_TIMEOUT_MS);
    voip_session_t snapshot = s_session;
    unlock_session();

    ESP_LOGI(TAG,
             "%s,正在建立通话: room=%s peer_id_len=%u token_len=%u",
             source ? source : "(空)",
             snapshot.wx_room_id[0] ? snapshot.wx_room_id : "(空)",
             (unsigned)strlen(snapshot.peer_id),
             (unsigned)strlen(snapshot.token));
    WX_VOIP_TRACEI(TAG,
                   "准备调用 TiRTC WHIP external: source=%s room=%s peer_id_len=%u token_len=%u",
                   source ? source : "(空)",
                   snapshot.wx_room_id[0] ? snapshot.wx_room_id : "(空)",
                   (unsigned)strlen(snapshot.peer_id),
                   (unsigned)strlen(snapshot.token));
    int64_t start_us = esp_timer_get_time();
    ESP_LOGI(TAG,
             "WHIP submit begin external: service_desc_len=%u token_len=%u",
             (unsigned)strlen(snapshot.peer_id),
             (unsigned)strlen(snapshot.token));
    int rc = tirtc_session_whip_connect_external(snapshot.peer_id, snapshot.token, on_whip_connect, NULL);
    int64_t cost_ms = (esp_timer_get_time() - start_us) / 1000;
    WX_VOIP_TRACEI(TAG, "TiRTC WHIP external 返回: rc=%d cost=%lldms", rc, (long long)cost_ms);
    if (rc == 0)
    {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "提交通话连接失败: %d %s", rc, tirtc_session_error_string(rc));
    voip_reject_info_t failed = {0};
    lock_session();
    fill_reject_info(&failed, &s_session);
    finish_session_locked();
    unlock_session();
    (void)reject_info_later(&failed, TIRTC_VOIP_HANGUP_REASON_EXCEPTION);
    return ESP_FAIL;
}

/* 常驻接听 worker: 大栈放在 PSRAM 且只分配一次,避免反复创建/删除导致堆碎片。 */
static void answer_task(void *arg)
{
    (void)arg;

    while (true)
    {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        char source[sizeof(s_answer_source)];
        uint32_t delay_ms = 0;
        uint32_t request_seq = 0;

        lock_session();
        bool has_request = s_answer_pending && s_session.state == VOIP_STATE_RINGING;
        if (has_request)
        {
            copy_str(source, sizeof(source), s_answer_source[0] ? s_answer_source : "界面接听");
            delay_ms = s_answer_delay_ms;
            request_seq = s_answer_request_seq;
        }
        unlock_session();

        if (!has_request)
        {
            continue;
        }

        /* 设备主呼收到 call_incoming 后要立即 WHIP 建连,后续接听/挂断由 0x2000/0x2001 控制。 */
        if (delay_ms > 0)
        {
            WX_VOIP_TRACEI(TAG, "接听任务等待房间稳定: source=%s delay=%ums", source, (unsigned)delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }

        lock_session();
        bool still_pending = s_answer_pending &&
                             s_session.state == VOIP_STATE_RINGING &&
                             s_answer_request_seq == request_seq;
        unlock_session();
        if (!still_pending)
        {
            WX_VOIP_TRACEI(TAG, "接听任务取消: source=%s", source);
            continue;
        }

        WX_VOIP_TRACEI(TAG, "接听任务开始: source=%s", source);
        esp_err_t ret = answer_current_call(source);

        bool retry_pending = false;
        bool transient_wait = (ret == ESP_ERR_INVALID_STATE || ret == ESP_ERR_NO_MEM);
        lock_session();
        if (s_answer_request_seq == request_seq)
        {
            if (transient_wait && s_answer_pending && s_session.state == VOIP_STATE_RINGING)
            {
                retry_pending = true;
            }
            else
            {
                s_answer_pending = false;
                s_answer_source[0] = '\0';
                s_answer_delay_ms = 0;
            }
        }
        unlock_session();

        if (ret == ESP_ERR_INVALID_STATE)
        {
            ESP_LOGW(TAG, "当前暂不能接听,等待来电或 RTC 就绪");
        }
        else if (ret == ESP_ERR_NO_MEM)
        {
            ESP_LOGW(TAG, "当前内存不足,保留来电状态,请稍后再次接听");
        }
        if (retry_pending)
        {
            vTaskDelay(pdMS_TO_TICKS(VOIP_ANSWER_RETRY_DELAY_MS));
            xTaskNotifyGive(s_answer_worker_task);
        }

        WX_VOIP_TRACEI(TAG,
                       "接听任务结束: ret=%s stack_hwm=%u",
                       esp_err_to_name(ret),
                       (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
}

static esp_err_t start_answer_worker(void)
{
    if (s_answer_worker_task != NULL)
    {
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreateWithCaps(answer_task,
                                         "wx_voip_answer",
                                         VOIP_ANSWER_TASK_STACK,
                                         NULL,
                                         VOIP_ANSWER_TASK_PRIORITY,
                                         &s_answer_worker_task,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS)
    {
        s_answer_worker_task = NULL;
        ESP_LOGE(TAG,
                 "创建接听任务失败: stack=%u internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
                 (unsigned)VOIP_ANSWER_TASK_STACK,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t start_answer_task(const char *source, uint32_t delay_ms)
{
    if (!lock_session_wait("启动接听任务", 1000))
    {
        return ESP_ERR_TIMEOUT;
    }
    bool ringing = (s_session.state == VOIP_STATE_RINGING);
    bool pending = s_answer_pending;
    voip_state_t state = s_session.state;
    if (ringing && !pending)
    {
        s_answer_pending = true;
        copy_str(s_answer_source, sizeof(s_answer_source), source ? source : "界面接听");
        s_answer_delay_ms = delay_ms;
        s_answer_request_seq++;
        if (s_answer_request_seq == 0)
        {
            s_answer_request_seq = 1;
        }
    }
    unlock_session();
    if (!ringing)
    {
        return ESP_ERR_INVALID_STATE;
    }

    WX_VOIP_TRACEI(TAG,
                   "请求启动接听任务: source=%s state=%s pending=%d delay=%ums",
                   source ? source : "界面接听",
                   state_name(state),
                   pending ? 1 : 0,
                   (unsigned)delay_ms);

    ESP_LOGD(TAG, "准备启动接听任务: %s", source ? source : "界面接听");

    if (pending)
    {
        ESP_LOGW(TAG, "正在接听,请勿重复触发");
        return ESP_OK;
    }

    ESP_LOGD(TAG, "%s请求已收到", source ? source : "界面接听");

    esp_err_t worker_ret = start_answer_worker();
    if (worker_ret != ESP_OK)
    {
        if (lock_session_wait("回滚接听任务状态", 1000))
        {
            s_answer_pending = false;
            s_answer_source[0] = '\0';
            s_answer_delay_ms = 0;
            unlock_session();
        }
        return worker_ret;
    }

    TaskHandle_t worker = s_answer_worker_task;
    if (worker == NULL)
    {
        if (lock_session_wait("回滚接听任务状态", 1000))
        {
            s_answer_pending = false;
            s_answer_source[0] = '\0';
            s_answer_delay_ms = 0;
            unlock_session();
        }
        return ESP_ERR_NO_MEM;
    }

    xTaskNotifyGive(worker);
    ESP_LOGD(TAG, "接听任务已唤醒");
    return ESP_OK;
}

esp_err_t wechat_voip_session_handle_join_room(cJSON *root, bool auto_answer)
{
    ensure_init();

    if (!cJSON_IsObject(root))
    {
        ESP_LOGE(TAG, "来电消息为空");
        return ESP_ERR_INVALID_ARG;
    }

    WX_VOIP_TRACEI(TAG, "开始处理入会消息: auto=%d", auto_answer ? 1 : 0);
    WX_VOIP_TRACEI(TAG, "入会消息已解析");

    const char *peer_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "peer_id"));
    const char *token = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "token"));
    const char *app_id = json_string_any(root, "wx_app_id", "wxa_app_id");
    const char *model_id = json_string_any(root, "wx_model_id", "wxa_model_id");
    const char *session_token = json_string_any(root, "wx_session_token", "wxa_session_token");
    const char *room_id = json_string_any(root, "wx_room_id", "wxa_room_id");
    const char *wx_payload = json_string_any(root, "wx_payload", "wxa_payload");

    if (session_token == NULL || session_token[0] == '\0')
    {
        session_token = json_string_any(root, "wx_server_token", "wxa_server_token");
    }

    char app_id_from_peer[64] = {0};
    char model_id_from_peer[64] = {0};
    if ((app_id == NULL || app_id[0] == '\0') && peer_id != NULL)
    {
        extract_query_param(peer_id, "x_wx_app_id", app_id_from_peer, sizeof(app_id_from_peer));
        if (app_id_from_peer[0] == '\0')
        {
            extract_query_param(peer_id, "x_wxa_app_id", app_id_from_peer, sizeof(app_id_from_peer));
        }
        app_id = app_id_from_peer;
    }
    if ((model_id == NULL || model_id[0] == '\0') && peer_id != NULL)
    {
        extract_query_param(peer_id, "x_wx_model_id", model_id_from_peer, sizeof(model_id_from_peer));
        if (model_id_from_peer[0] == '\0')
        {
            extract_query_param(peer_id, "x_wxa_model_id", model_id_from_peer, sizeof(model_id_from_peer));
        }
        model_id = model_id_from_peer;
    }

    if (peer_id == NULL || peer_id[0] == '\0' || token == NULL || token[0] == '\0')
    {
        ESP_LOGE(TAG, "来电消息缺少连接信息");
        return ESP_ERR_INVALID_ARG;
    }
    size_t peer_id_len = strlen(peer_id);
    size_t token_len = strlen(token);
    size_t session_token_len = session_token ? strlen(session_token) : 0U;
    bool peer_id_truncated = peer_id_len >= sizeof(s_session.peer_id);
    bool token_truncated = token_len >= sizeof(s_session.token);
    bool session_token_truncated = session_token_len >= sizeof(s_session.wx_session_token);

    WX_VOIP_TRACEI(TAG,
                   "入会连接信息: peer_id_len=%u token_len=%u room_id=%s app_id=%s model_id=%s session_token_len=%u",
                   (unsigned)peer_id_len,
                   (unsigned)token_len,
                   room_id && room_id[0] ? room_id : "(空)",
                   app_id && app_id[0] ? app_id : "(空)",
                   model_id && model_id[0] ? model_id : "(空)",
                   (unsigned)session_token_len);
    ESP_LOGI(TAG,
             "微信入会参数: room=%s peer_id_len=%u token_len=%u app_id=%s model_id=%s truncated=%d/%d/%d",
             room_id && room_id[0] ? room_id : "(空)",
             (unsigned)peer_id_len,
             (unsigned)token_len,
             app_id && app_id[0] ? app_id : "(空)",
             model_id && model_id[0] ? model_id : "(空)",
             peer_id_truncated ? 1 : 0,
             token_truncated ? 1 : 0,
             session_token_truncated ? 1 : 0);
    if (peer_id_truncated || token_truncated || session_token_truncated)
    {
        ESP_LOGE(TAG,
                 "微信入会连接信息过长,已拒绝保存: peer_id_len=%u/%u token_len=%u/%u session_token_len=%u/%u",
                 (unsigned)peer_id_len,
                 (unsigned)sizeof(s_session.peer_id),
                 (unsigned)token_len,
                 (unsigned)sizeof(s_session.token),
                 (unsigned)session_token_len,
                 (unsigned)sizeof(s_session.wx_session_token));
        return ESP_ERR_INVALID_SIZE;
    }
    WX_VOIP_TRACEI(TAG, "准备保存入会信息");

    voip_status_t status;
    collect_status(&status);
    if (!status_available_for_ringing(&status))
    {
        ESP_LOGW(TAG,
                 "微信入会忙线拒接: reason=%s",
                 status_not_ready_reason(&status));
        esp_err_t reject_ret = wechat_voip_session_reject_join_room_busy(root);
        return reject_ret == ESP_OK ? ESP_ERR_INVALID_STATE : reject_ret;
    }

    if (!lock_session_wait("保存入会信息", 1000))
    {
        return ESP_ERR_TIMEOUT;
    }
    if (s_session.state != VOIP_STATE_IDLE)
    {
        voip_state_t state = s_session.state;
        unlock_session();
        ESP_LOGW(TAG,
                 "当前状态=%s,微信入会忙线拒接",
                 state_name(state));
        esp_err_t reject_ret = wechat_voip_session_reject_join_room_busy(root);
        return reject_ret == ESP_OK ? ESP_ERR_INVALID_STATE : reject_ret;
    }

    clear_session_locked();
    fill_join_session(&s_session,
                      peer_id,
                      token,
                      app_id,
                      model_id,
                      session_token,
                      room_id,
                      wx_payload,
                      auto_answer);
    set_deadline_locked(VOIP_RING_TIMEOUT_MS);
    unlock_session();

    WX_VOIP_TRACEI(TAG, "入会信息已保存: auto=%d", auto_answer ? 1 : 0);

    if (auto_answer)
    {
        ESP_LOGI(TAG, "主动呼叫入会参数已收到,立即 WHIP 建连,等待微信 0x2000/0x2001");
        return start_answer_task("主动呼叫入会", VOIP_ACTIVE_ANSWER_DELAY_MS);
    }

    ESP_LOGI(TAG, "收到微信来电,等待界面接听");
    return ESP_OK;
}

esp_err_t wechat_voip_session_answer(void)
{
    ensure_init();
    return start_answer_task("界面接听", VOIP_UI_ANSWER_DELAY_MS);
}

bool wechat_voip_session_has_incoming_call(void)
{
    bool pending = false;

    if (s_mutex == NULL) {
        return false;
    }
    lock_session();
    pending = (s_session.state == VOIP_STATE_RINGING && !s_answer_pending);
    unlock_session();
    return pending;
}

wechat_voip_session_state_t wechat_voip_session_get_state(void)
{
    wechat_voip_session_state_t state = WECHAT_VOIP_SESSION_STATE_IDLE;

    if (s_mutex == NULL) {
        return state;
    }
    lock_session();
    switch (s_session.state)
    {
    case VOIP_STATE_RINGING:
        state = s_answer_pending ? WECHAT_VOIP_SESSION_STATE_CONNECTING :
                                   WECHAT_VOIP_SESSION_STATE_RINGING;
        break;
    case VOIP_STATE_CONNECTING:
        state = WECHAT_VOIP_SESSION_STATE_CONNECTING;
        break;
    case VOIP_STATE_AWAITING_CONNECTED:
        state = WECHAT_VOIP_SESSION_STATE_AWAITING_CONNECTED;
        break;
    case VOIP_STATE_IN_CALL:
        state = WECHAT_VOIP_SESSION_STATE_IN_CALL;
        break;
    case VOIP_STATE_CLOSING:
        state = WECHAT_VOIP_SESSION_STATE_CLOSING;
        break;
    case VOIP_STATE_IDLE:
    default:
        state = WECHAT_VOIP_SESSION_STATE_IDLE;
        break;
    }
    unlock_session();

    return state;
}

esp_err_t wechat_voip_session_reject_incoming(void)
{
    ensure_init();

    voip_state_t state = VOIP_STATE_IDLE;
    char room_id[sizeof(s_session.wx_room_id)] = {0};
    voip_reject_info_t reject_info = {0};

    lock_session();
    if (s_session.state != VOIP_STATE_RINGING)
    {
        unlock_session();
        return ESP_ERR_INVALID_STATE;
    }

    state = s_session.state;
    copy_str(room_id, sizeof(room_id), s_session.wx_room_id);
    fill_reject_info(&reject_info, &s_session);
    WX_VOIP_TRACEI(TAG,
                   "界面拒接: room=%s state=%s",
                   room_id[0] ? room_id : "(空)",
                   state_name(state));
    finish_session_locked();
    unlock_session();

    esp_err_t ret = reject_info_later(&reject_info, TIRTC_VOIP_HANGUP_REASON_REJECT);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "已拒接微信来电");
    }
    return ret;
}

bool wechat_voip_session_is_idle(void)
{
    if (s_mutex == NULL) {
        return true;
    }

    lock_session();
    bool idle = (s_session.state == VOIP_STATE_IDLE);
    unlock_session();
    return idle;
}

bool wechat_voip_session_is_closing(void)
{
    if (s_mutex == NULL) {
        return false;
    }

    lock_session();
    bool closing = (s_session.state == VOIP_STATE_CLOSING);
    unlock_session();
    return closing;
}

static void collect_status(voip_status_t *status)
{
    if (status == NULL)
    {
        return;
    }

    memset(status, 0, sizeof(*status));
    int64_t now_us = esp_timer_get_time();
    tirtc_session_stats_t rtc_stats = {0};
    tirtc_session_get_stats(&rtc_stats);

    lock_session();
    status->state = s_session.state;
    status->hconn = s_session.hconn;
    status->answer_pending = s_answer_pending;
    status->answer_task = s_answer_worker_task;
    status->deadline_left_ms = s_session.deadline_us > 0 ? (s_session.deadline_us - now_us) / 1000 : 0;
    status->last_close_ago_ms = s_last_close_us > 0 ? (now_us - s_last_close_us) / 1000 : -1;
    status->recall_wait_ms = s_recall_block_until_us > now_us ? (s_recall_block_until_us - now_us) / 1000 : 0;
    status->pending_join = s_pending_join.valid;
    status->pending_join_seq = s_pending_join.seq;
    unlock_session();

    portENTER_CRITICAL(&s_work_state_lock);
    status->work_busy = s_work_busy;
    status->work_type = s_work_type;
    portEXIT_CRITICAL(&s_work_state_lock);

    status->work_queue_len = s_work_queue ? (uint32_t)uxQueueMessagesWaiting(s_work_queue) : 0;
    wechat_voip_media_get_stats(&status->media);
    status->media_running = status->media.running;
    status->rtc_active_connection = rtc_stats.active_connection;
    status->rtc_call_active = rtc_stats.call_active;
    status->rtc_state = rtc_stats.state;
}

static bool status_ready_for_next_call_internal(const voip_status_t *status, bool ignore_pending_join)
{
    return status != NULL &&
           status->state == VOIP_STATE_IDLE &&
           status->hconn == NULL &&
           !status->answer_pending &&
           !status->work_busy &&
           status->work_queue_len == 0 &&
           !status->media_running &&
           (ignore_pending_join || !status->pending_join) &&
           !status->rtc_active_connection &&
           !status->rtc_call_active &&
           status->rtc_state != TIRTC_SESSION_STATE_STARTING &&
           status->rtc_state != TIRTC_SESSION_STATE_CONNECTED &&
           status->rtc_state != TIRTC_SESSION_STATE_MEDIA_BOOTSTRAPPING &&
           status->rtc_state != TIRTC_SESSION_STATE_DISCONNECTING &&
           status->recall_wait_ms <= 0 &&
           (status->last_close_ago_ms < 0 || status->last_close_ago_ms >= VOIP_RECALL_GUARD_MS);
}

static bool status_available_for_ringing(const voip_status_t *status)
{
    /* Ringing owns no media yet. An H5 stream may remain active until the user
     * accepts, while the application policy rejects AI/VoIP/device-call conflicts. */
    return status != NULL &&
           status->state == VOIP_STATE_IDLE &&
           status->hconn == NULL &&
           !status->answer_pending &&
           !status->work_busy &&
           status->work_queue_len == 0 &&
           !status->media_running &&
           !status->pending_join &&
           status->recall_wait_ms <= 0 &&
           (status->last_close_ago_ms < 0 || status->last_close_ago_ms >= VOIP_RECALL_GUARD_MS);
}

static bool status_ready_for_next_call(const voip_status_t *status)
{
    return status_ready_for_next_call_internal(status, false);
}

static bool status_ready_for_pending_join(const voip_status_t *status)
{
    return status_ready_for_next_call_internal(status, true);
}

static const char *status_not_ready_reason(const voip_status_t *status)
{
    if (status == NULL)
    {
        return "状态不可用";
    }
    if (status->state != VOIP_STATE_IDLE)
    {
        return "会话仍未空闲";
    }
    if (status->hconn != NULL)
    {
        return "连接句柄未清空";
    }
    if (status->answer_pending)
    {
        return "接听请求处理中";
    }
    if (status->work_busy)
    {
        return "后台动作处理中";
    }
    if (status->work_queue_len > 0)
    {
        return "后台动作队列未清空";
    }
    if (status->media_running)
    {
        return "媒体发送未停止";
    }
    if (status->pending_join)
    {
        return "挂起来电等待中";
    }
    if (status->rtc_active_connection || status->rtc_call_active)
    {
        return "RTC连接仍未释放";
    }
    if (status->rtc_state == TIRTC_SESSION_STATE_STARTING ||
        status->rtc_state == TIRTC_SESSION_STATE_CONNECTED ||
        status->rtc_state == TIRTC_SESSION_STATE_MEDIA_BOOTSTRAPPING ||
        status->rtc_state == TIRTC_SESSION_STATE_DISCONNECTING)
    {
        return "RTC状态暂不可重入";
    }
    if (status->recall_wait_ms > 0)
    {
        return "异常断开冷却中";
    }
    if (status->last_close_ago_ms >= 0 && status->last_close_ago_ms < VOIP_RECALL_GUARD_MS)
    {
        return "挂断冷却中";
    }
    return "状态检查未通过";
}

static void log_status(const char *reason, const voip_status_t *status, bool warning)
{
    if (status == NULL)
    {
        return;
    }

    if (warning)
    {
        ESP_LOGW(TAG,
                 "通话资源%s: %s state=%s hconn=%p answer_task=%p answer_pending=%d "
                 "work_busy=%d work=%s workq=%u media=%d pending=%d/%lu rtc=%d/%d/%d media_tx=%llu drop=%lu fail=%lu "
                 "deadline=%lldms last_close=%lldms recall_wait=%lldms",
                 status_ready_for_next_call(status) ? "已就绪" : "未就绪",
                 reason ? reason : "状态检查",
                 state_name(status->state),
                 status->hconn,
                 status->answer_task,
                 status->answer_pending ? 1 : 0,
                 status->work_busy ? 1 : 0,
                 work_type_name(status->work_type),
                 (unsigned)status->work_queue_len,
                 status->media_running ? 1 : 0,
                 status->pending_join ? 1 : 0,
                 (unsigned long)status->pending_join_seq,
                 status->rtc_active_connection ? 1 : 0,
                 status->rtc_call_active ? 1 : 0,
                 (int)status->rtc_state,
                 (unsigned long long)status->media.tx_frames,
                 (unsigned long)status->media.dropped_frames,
                 (unsigned long)status->media.tx_failures,
                 (long long)status->deadline_left_ms,
                 (long long)status->last_close_ago_ms,
                 (long long)status->recall_wait_ms);
        return;
    }

    ESP_LOGI(TAG,
             "通话资源%s: %s state=%s hconn=%p answer_task=%p answer_pending=%d "
             "work_busy=%d work=%s workq=%u media=%d pending=%d/%lu rtc=%d/%d/%d media_tx=%llu drop=%lu fail=%lu "
             "deadline=%lldms last_close=%lldms recall_wait=%lldms",
             status_ready_for_next_call(status) ? "已就绪" : "未就绪",
             reason ? reason : "状态检查",
             state_name(status->state),
             status->hconn,
             status->answer_task,
             status->answer_pending ? 1 : 0,
             status->work_busy ? 1 : 0,
             work_type_name(status->work_type),
             (unsigned)status->work_queue_len,
             status->media_running ? 1 : 0,
             status->pending_join ? 1 : 0,
             (unsigned long)status->pending_join_seq,
             status->rtc_active_connection ? 1 : 0,
             status->rtc_call_active ? 1 : 0,
             (int)status->rtc_state,
             (unsigned long long)status->media.tx_frames,
             (unsigned long)status->media.dropped_frames,
             (unsigned long)status->media.tx_failures,
             (long long)status->deadline_left_ms,
             (long long)status->last_close_ago_ms,
             (long long)status->recall_wait_ms);
}

static void pending_join_task(void *arg)
{
    (void)arg;

    while (true)
    {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        int64_t last_log_us = 0;
        while (true)
        {
            uint32_t seq = 0;
            int64_t expire_us = 0;
            bool auto_answer = false;

            lock_session();
            bool valid = s_pending_join.valid;
            if (valid)
            {
                seq = s_pending_join.seq;
                expire_us = s_pending_join.expire_us;
                auto_answer = s_pending_join.session.outbound_call;
            }
            unlock_session();

            if (!valid)
            {
                break;
            }

            int64_t now_us = esp_timer_get_time();
            if (expire_us > 0 && now_us >= expire_us)
            {
                lock_session();
                if (s_pending_join.valid && s_pending_join.seq == seq)
                {
                    ESP_LOGW(TAG,
                             "挂起微信入会等待 RTC 释放超时,取消本地等待: seq=%lu room=%s",
                             (unsigned long)seq,
                             s_pending_join.session.wx_room_id[0] ? s_pending_join.session.wx_room_id : "(空)");
                    memset(&s_pending_join, 0, sizeof(s_pending_join));
                }
                unlock_session();
                break;
            }

            voip_status_t status;
            collect_status(&status);
            if (status_ready_for_pending_join(&status))
            {
                bool committed = false;
                lock_session();
                if (s_pending_join.valid &&
                    s_pending_join.seq == seq &&
                    s_session.state == VOIP_STATE_IDLE)
                {
                    s_session = s_pending_join.session;
                    set_deadline_locked(VOIP_RING_TIMEOUT_MS);
                    memset(&s_pending_join, 0, sizeof(s_pending_join));
                    committed = true;
                }
                unlock_session();

                if (committed)
                {
                    ESP_LOGI(TAG,
                             "RTC 已释放,恢复挂起微信入会: seq=%lu auto=%d",
                             (unsigned long)seq,
                             auto_answer ? 1 : 0);
                    if (auto_answer)
                    {
                        (void)start_answer_task("挂起入会恢复", VOIP_ACTIVE_ANSWER_DELAY_MS);
                    }
                }
                break;
            }

            if (now_us - last_log_us >= 1000 * 1000)
            {
                last_log_us = now_us;
                log_status("挂起来电等待 RTC 释放", &status, true);
            }
            vTaskDelay(pdMS_TO_TICKS(VOIP_PENDING_JOIN_POLL_MS));
        }
    }
}

static esp_err_t start_pending_join_worker(void)
{
    if (s_pending_join_worker_task != NULL)
    {
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreateWithCaps(pending_join_task,
                                         "wx_voip_wait",
                                         VOIP_PENDING_JOIN_TASK_STACK,
                                         NULL,
                                         VOIP_PENDING_JOIN_TASK_PRIORITY,
                                         &s_pending_join_worker_task,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS)
    {
        s_pending_join_worker_task = NULL;
        ESP_LOGE(TAG,
                 "创建挂起来电等待任务失败: stack=%u internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
                 (unsigned)VOIP_PENDING_JOIN_TASK_STACK,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool wechat_voip_session_ready_for_next_call(bool log_detail)
{
    ensure_init();

    voip_status_t status;
    collect_status(&status);
    bool ready = status_ready_for_next_call(&status);
    if (log_detail || !ready)
    {
        log_status("发起呼叫前检查", &status, !ready);
    }
    return ready;
}

void wechat_voip_session_dump_status(const char *reason)
{
    ensure_init();

    voip_status_t status;
    collect_status(&status);
    log_status(reason, &status, !status_ready_for_next_call(&status));
}

void wechat_voip_session_maintenance(void)
{
    if (s_mutex == NULL) {
        return;
    }

    int64_t now_us = esp_timer_get_time();
    voip_state_t state = VOIP_STATE_IDLE;
    tirtc_conn_t hconn = NULL;
    int64_t deadline_us = 0;
    voip_work_item_t item = {0};

    lock_session();
    bool expired = (s_session.state != VOIP_STATE_IDLE &&
                    s_session.deadline_us > 0 &&
                    now_us >= s_session.deadline_us);
    state = s_session.state;
    hconn = s_session.hconn;
    deadline_us = s_session.deadline_us;
    if (expired && hconn == NULL)
    {
        fill_reject_info(&item.reject, &s_session);
    }
    if (expired)
    {
        if (s_session.state == VOIP_STATE_CLOSING)
        {
            set_deadline_locked(VOIP_CLOSE_WAIT_TIMEOUT_MS);
        }
        else if (s_session.hconn == NULL)
        {
            finish_session_locked();
        }
        else
        {
            begin_close_locked();
        }
    }
    unlock_session();

    if (!expired)
    {
        voip_status_t status;
        collect_status(&status);
        bool ready = status_ready_for_next_call(&status);
        if (!ready &&
            status.state == VOIP_STATE_IDLE &&
            now_us - s_last_status_warn_us >= (int64_t)VOIP_STATUS_WARN_INTERVAL_MS * 1000)
        {
            s_last_status_warn_us = now_us;
            s_idle_not_ready_reported = true;
            log_status(status_not_ready_reason(&status), &status, true);
        }
        else if (ready && s_idle_not_ready_reported)
        {
            s_idle_not_ready_reported = false;
            log_status("空闲资源已就绪", &status, false);
        }
        return;
    }

    WX_VOIP_TRACEW(TAG,
                   "会话超时维护: state=%s hconn=%p deadline=%lld now=%lld",
                   state_name(state),
                   hconn,
                   (long long)deadline_us,
                   (long long)now_us);

    if (state == VOIP_STATE_CLOSING)
    {
        ESP_LOGW(TAG, "微信通话关闭等待超时,重试断开");
        wechat_voip_media_stop(hconn);
        disconnect_later(hconn, true);
        return;
    }

    ESP_LOGW(TAG, "微信通话%s超时,准备结束", state_name(state));
    if (hconn != NULL)
    {
        item.type = VOIP_WORK_HANGUP;
        item.hconn = hconn;
        item.reason = TIRTC_VOIP_HANGUP_REASON_TIMEOUT;
        (void)enqueue_work(&item);
        return;
    }

    item.type = VOIP_WORK_REJECT;
    item.reason = TIRTC_VOIP_HANGUP_REASON_TIMEOUT;
    (void)enqueue_work(&item);
}

static void abort_connected_media_start(tirtc_conn_t hconn, const char *reason)
{
    if (hconn == NULL)
    {
        return;
    }

    ESP_LOGW(TAG, "微信通话媒体启动失败,释放连接: %s", reason != NULL ? reason : "unknown");
    wechat_voip_media_stop(hconn);
    (void)tirtc_session_set_external_audio_call_active(hconn, false);

    lock_session();
    if (s_session.hconn == hconn && s_session.state != VOIP_STATE_IDLE)
    {
        begin_close_locked();
    }
    unlock_session();

    disconnect_later(hconn, true);
}

bool wechat_voip_session_on_command(tirtc_conn_t hconn, uint32_t cmdw, const void *data, uint32_t len)
{
    ensure_init();

    if (hconn == NULL)
    {
        return false;
    }

    lock_session();
    tirtc_conn_t current = s_session.hconn;
    voip_state_t state = s_session.state;
    bool outbound_call = s_session.outbound_call;
    if (current == NULL &&
        (state == VOIP_STATE_CONNECTING || state == VOIP_STATE_AWAITING_CONNECTED) &&
        (voip_cmd_is(cmdw, TIRTC_VOIP_CALL_CONNECTED) ||
         voip_cmd_is(cmdw, TIRTC_VOIP_HANGUP)))
    {
        s_session.hconn = hconn;
        current = hconn;
        ESP_LOGD(TAG, "通话命令先于连接回调到达 hconn=%p cmdw=0x%08x",
                 hconn, (unsigned)cmdw);
    }
    unlock_session();

    ESP_LOGI(TAG,
             "微信信令命令: hconn=%p current=%p cmdw=0x%08x len=%u state=%s outbound=%d",
             hconn,
             current,
             (unsigned)cmdw,
             (unsigned)len,
             state_name(state),
             outbound_call ? 1 : 0);

    if (hconn != current)
    {
        ESP_LOGD(TAG,
                 "忽略非当前通话命令 hconn=%p current=%p state=%s cmdw=0x%08x",
                 hconn,
                 current,
                 state_name(state),
                 (unsigned)cmdw);
        return false;
    }

    if (voip_cmd_is(cmdw, TIRTC_VOIP_CALL_CONNECTED))
    {
        bool already_in_call = false;
        bool should_start_media = false;
        voip_state_t command_state = VOIP_STATE_IDLE;

        lock_session();
        command_state = s_session.state;
        already_in_call = (s_session.state == VOIP_STATE_IN_CALL);
        if (s_session.state == VOIP_STATE_AWAITING_CONNECTED || s_session.state == VOIP_STATE_CONNECTING)
        {
            s_session.state = VOIP_STATE_IN_CALL;
            set_deadline_locked(0);
            should_start_media = true;
        }
        unlock_session();

        if (already_in_call)
        {
            WX_VOIP_TRACEI(TAG, "忽略重复 CALL_CONNECTED: hconn=%p", hconn);
            return true;
        }

        if (!should_start_media)
        {
            ESP_LOGW(TAG,
                     "忽略非等待状态的 CALL_CONNECTED: state=%s hconn=%p",
                     state_name(command_state),
                     hconn);
            return true;
        }

        ESP_LOGI(TAG, "微信 0x2000 接听命令已收到,启动媒体");
        WX_VOIP_TRACEI(TAG, "收到 CALL_CONNECTED,启动媒体: hconn=%p", hconn);
        esp_err_t media_ret = wechat_voip_media_start(hconn);
        if (media_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "微信真实音频启动失败: %s", esp_err_to_name(media_ret));
            abort_connected_media_start(hconn, "real audio start failed");
            return true;
        }
        esp_err_t rtc_ret = tirtc_session_set_external_audio_call_active(hconn, true);
        if (rtc_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "微信通话态同步失败: %s", esp_err_to_name(rtc_ret));
            abort_connected_media_start(hconn, "rtc state sync failed");
        }
        return true;
    }

    if (voip_cmd_is(cmdw, TIRTC_VOIP_HANGUP))
    {
        int reason = -1;
        if (data != NULL && len > 0)
        {
            char buf[96];
            if (len < sizeof(buf))
            {
                memcpy(buf, data, len);
                buf[len] = '\0';
                cJSON *root = cJSON_Parse(buf);
                if (root != NULL)
                {
                    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "reason");
                    if (cJSON_IsNumber(item))
                    {
                        reason = item->valueint;
                    }
                    cJSON_Delete(root);
                }
            }
            else
            {
                ESP_LOGD(TAG, "挂断命令内容过长,跳过原因解析 len=%u", (unsigned)len);
            }
        }

        ESP_LOGI(TAG,
                 "微信 0x2001 挂断命令已收到,原因=%d,state=%s,outbound=%d",
                 reason,
                 state_name(state),
                 outbound_call ? 1 : 0);
        WX_VOIP_TRACEI(TAG,
                       "收到 HANGUP: hconn=%p reason=%d payload_len=%u",
                       hconn,
                       reason,
                       (unsigned)len);

        lock_session();
        if (s_session.hconn == hconn)
        {
            begin_close_locked();
        }
        else
        {
            finish_session_locked();
        }
        unlock_session();

        (void)tirtc_session_set_external_audio_call_active(hconn, false);
        disconnect_later(hconn, true);
        return true;
    }

    ESP_LOGD(TAG, "通话中收到未处理命令 cmdw=0x%08x", (unsigned)cmdw);
    return true;
}

bool wechat_voip_session_on_conn_error(tirtc_conn_t hconn, int error)
{
    ensure_init();

    WX_VOIP_TRACEI(TAG,
                   "会话连接错误回调: hconn=%p error=%d",
                   hconn,
                   error);

    lock_session();
    bool mine = (hconn != NULL && hconn == s_session.hconn);
    voip_state_t state = s_session.state;
    bool outbound_call = s_session.outbound_call;
    bool waiting_remote_answer = outbound_call &&
                                 state == VOIP_STATE_AWAITING_CONNECTED &&
                                 (error == TIRTC_E_CONN_TIMEOUTCLOSE ||
                                  error == TIRTC_E_CONN_REMOTECLOSE);
    if (mine)
    {
        if (!waiting_remote_answer)
        {
            block_recall_locked(VOIP_ERROR_RECALL_GUARD_MS);
        }
        finish_session_locked();
    }
    unlock_session();

    if (!mine)
    {
        return false;
    }

    if (waiting_remote_answer)
    {
        ESP_LOGI(TAG,
                 "主动呼叫未接通即关闭: %d %s state=%s",
                 error,
                 tirtc_session_error_string(error),
                 state_name(state));
    }
    else
    {
        ESP_LOGW(TAG, "微信通话连接错误: %d %s", error, tirtc_session_error_string(error));
    }
    wechat_voip_media_stop(hconn);
    (void)tirtc_session_set_external_audio_call_active(hconn, false);
    disconnect_later(hconn, false);
    return true;
}

bool wechat_voip_session_on_disconnected(tirtc_conn_t hconn)
{
    ensure_init();

    WX_VOIP_TRACEI(TAG, "会话断开回调: hconn=%p", hconn);

    lock_session();
    bool mine = (hconn != NULL && hconn == s_session.hconn);
    if (mine)
    {
        finish_session_locked();
    }
    unlock_session();

    if (mine)
    {
        wechat_voip_media_stop(hconn);
        (void)tirtc_session_set_external_audio_call_active(hconn, false);
        ESP_LOGI(TAG, "微信通话已断开");
        wechat_voip_session_dump_status("断开回调后");
    }

    return mine;
}

bool wechat_voip_session_cancel_by_room(const char *room_id)
{
    ensure_init();

    WX_VOIP_TRACEI(TAG, "微信取消事件: room=%s", room_id && room_id[0] ? room_id : "(空)");

    tirtc_conn_t hconn = NULL;
    char current_room_id[sizeof(s_session.wx_room_id)] = {0};
    char pending_room_id[sizeof(s_pending_join.session.wx_room_id)] = {0};

    lock_session();
    bool active = (s_session.state != VOIP_STATE_IDLE);
    bool pending = s_pending_join.valid;
    hconn = s_session.hconn;
    copy_str(current_room_id, sizeof(current_room_id), s_session.wx_room_id);
    copy_str(pending_room_id, sizeof(pending_room_id), s_pending_join.session.wx_room_id);
    bool room_match = true;
    if (active && current_room_id[0] != '\0')
    {
        room_match = (room_id != NULL && room_id[0] != '\0' &&
                      strcmp(current_room_id, room_id) == 0);
    }
    if (active && room_match)
    {
        if (s_session.hconn != NULL)
        {
            begin_close_locked();
        }
        else
        {
            finish_session_locked();
        }
    }
    bool pending_room_match = false;
    if (!active && pending)
    {
        pending_room_match = (pending_room_id[0] == '\0') ||
                             (room_id != NULL && room_id[0] != '\0' &&
                              strcmp(pending_room_id, room_id) == 0);
        if (pending_room_match)
        {
            memset(&s_pending_join, 0, sizeof(s_pending_join));
        }
    }
    unlock_session();

    if (!active && pending_room_match)
    {
        ESP_LOGI(TAG, "微信侧已取消挂起中的来电");
        return true;
    }
    if (!active)
    {
        return false;
    }
    if (!room_match)
    {
        ESP_LOGD(TAG, "忽略非当前房间取消: room_id=%s current=%s",
                 room_id ? room_id : "(空)",
                 current_room_id);
        return false;
    }

    if (hconn != NULL)
    {
        wechat_voip_media_stop(hconn);
        (void)tirtc_session_set_external_audio_call_active(hconn, false);
        disconnect_later(hconn, true);
        ESP_LOGI(TAG, "微信侧已结束当前通话");
    }
    else
    {
        ESP_LOGI(TAG, "微信侧已取消当前来电");
    }

    return true;
}

void wechat_voip_session_hangup(void)
{
    ensure_init();

    voip_state_t state = VOIP_STATE_IDLE;
    tirtc_conn_t hconn = NULL;
    voip_work_item_t item = {0};

    lock_session();
    bool active = (s_session.state != VOIP_STATE_IDLE);
    state = s_session.state;
    hconn = s_session.hconn;
    if (active && hconn == NULL)
    {
        fill_reject_info(&item.reject, &s_session);
    }
    WX_VOIP_TRACEI(TAG,
                   "本地请求挂断: active=%d state=%s hconn=%p",
                   active ? 1 : 0,
                   state_name(state),
                   hconn);
    if (active && s_session.state == VOIP_STATE_CLOSING)
    {
        unlock_session();
        ESP_LOGI(TAG, "微信通话正在关闭");
        wechat_voip_session_dump_status("重复挂断");
        return;
    }
    if (active && s_session.hconn != NULL)
    {
        begin_close_locked();
    }
    else
    {
        finish_session_locked();
    }
    unlock_session();

    if (!active)
    {
        return;
    }

    if (hconn != NULL)
    {
        item.type = VOIP_WORK_HANGUP;
        item.hconn = hconn;
        item.reason = TIRTC_VOIP_HANGUP_REASON_MANUAL;
        if (enqueue_work(&item) != ESP_OK)
        {
            ESP_LOGW(TAG, "挂断任务投递失败,等待维护重试");
            wechat_voip_media_stop(hconn);
            (void)tirtc_session_set_external_audio_call_active(hconn, false);
        }
        return;
    }

    item.type = VOIP_WORK_REJECT;
    item.reason = state == VOIP_STATE_RINGING
                                             ? TIRTC_VOIP_HANGUP_REASON_REJECT
                                             : TIRTC_VOIP_HANGUP_REASON_MANUAL;
    if (enqueue_work(&item) == ESP_OK)
    {
        ESP_LOGI(TAG, "%s", state == VOIP_STATE_RINGING ? "已拒接微信来电" : "已取消微信通话");
    }
}
