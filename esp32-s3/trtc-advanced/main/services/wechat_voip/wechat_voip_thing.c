#include "wechat_voip_thing.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "device_online.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "thing_mqtt_client.h"
#include "tirtc_session.h"
#include "wechat_voip_api.h"
#include "wechat_voip_config.h"
#include "wechat_voip_contacts.h"
#include "wechat_voip_session.h"
#include "wechat_voip_trace.h"

static const char *TAG = "wx_voip_thing";

enum {
    VOIP_MSG_QUEUE_LEN = 8,
    VOIP_MSG_TASK_STACK = 24 * 1024,
    VOIP_CALL_TASK_STACK = 24 * 1024,
    VOIP_TASK_PRIORITY = 5,
    DEVICE_CALLING_TIMEOUT_SEC = 30,
    ACTIVE_CALL_JOIN_WAIT_MS = (DEVICE_CALLING_TIMEOUT_SEC + 5) * 1000,
    ACTIVE_CALL_REQUEST_GUARD_MS = 12000,
};

#define WECHAT_VOIP_THING_TASK_CAPS    (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define WECHAT_VOIP_THING_CONTROL_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define WECHAT_VOIP_THING_PAYLOAD_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

typedef enum {
    ACTIVE_CALL_IDLE = 0,
    ACTIVE_CALL_REQUESTING,
    ACTIVE_CALL_WAIT_JOIN,
} active_call_state_t;

typedef struct {
    char *json;
    size_t len;
} voip_msg_item_t;

typedef struct {
    SemaphoreHandle_t lock;
    QueueHandle_t msg_queue;
    TaskHandle_t msg_task;
    TaskHandle_t call_task;
    bool started;
    thing_mqtt_listener_handle_t mqtt_listener;
    char device_id[TIRTC_SESSION_DEVICE_ID_MAX_LEN];
    char device_key[TIRTC_SESSION_SECRET_KEY_MAX_LEN];
    char mqtt_token[DEVICE_AUTH_MQTT_TOKEN_MAX_LEN];
    active_call_state_t active_call_state;
    uint32_t active_call_seq;
    int64_t active_call_deadline_us;
    char active_call_openid[WECHAT_VOIP_OPEN_ID_MAX];
} wechat_voip_thing_runtime_t;

/*
 * 1936B in the current map. This is VoIP service bookkeeping and cached
 * strings; it has no DMA/flash-disabled requirement, so it belongs in PSRAM.
 */
static EXT_RAM_BSS_ATTR wechat_voip_thing_runtime_t s_voip;

static void log_heap_state(const char *stage)
{
    ESP_LOGW(TAG,
             "%s: internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             stage != NULL ? stage : "heap",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void copy_str(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strlcpy(dst, src, dst_size);
}

static esp_err_t ensure_runtime(void)
{
    if (s_voip.lock == NULL) {
        s_voip.lock = xSemaphoreCreateMutexWithCaps(WECHAT_VOIP_THING_CONTROL_CAPS);
        if (s_voip.lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
        s_voip.mqtt_listener = -1;
    }
    ESP_RETURN_ON_ERROR(wechat_voip_contacts_init(), TAG, "contact repo init failed");
    return ESP_OK;
}

static const char *json_string_any(cJSON *root, const char *name1, const char *name2)
{
    if (root == NULL || name1 == NULL) {
        return NULL;
    }
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, name1));
    if ((value == NULL || value[0] == '\0') && name2 != NULL) {
        value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, name2));
    }
    return value;
}

static const char *json_string_any4(cJSON *root,
                                    const char *name1,
                                    const char *name2,
                                    const char *name3,
                                    const char *name4)
{
    const char *names[] = {name1, name2, name3, name4};
    if (root == NULL) {
        return NULL;
    }
    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, names[index]));
        if (value != NULL && value[0] != '\0') {
            return value;
        }
    }
    return NULL;
}

static bool msg_type_is(const char *type,
                        const char *name1,
                        const char *name2,
                        const char *name3)
{
    if (type == NULL || type[0] == '\0') {
        return false;
    }
    return (name1 != NULL && strcmp(type, name1) == 0) ||
           (name2 != NULL && strcmp(type, name2) == 0) ||
           (name3 != NULL && strcmp(type, name3) == 0);
}

static void extract_query_param(const char *url, const char *key, char *out, size_t out_size)
{
    if (url == NULL || key == NULL || out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';

    char search[64];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(url, search);
    if (p == NULL) {
        return;
    }
    p += strlen(search);

    size_t index = 0;
    while (*p != '\0' && *p != '&' && index < out_size - 1) {
        out[index++] = *p++;
    }
    out[index] = '\0';
}

static void get_runtime_device_id(char *device_id, size_t device_id_size)
{
    if (device_id == NULL || device_id_size == 0 || ensure_runtime() != ESP_OK) {
        return;
    }
    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    copy_str(device_id, device_id_size, s_voip.device_id);
    xSemaphoreGive(s_voip.lock);
    if (device_id[0] == '\0') {
        device_online_credentials_t credentials = {0};
        if (device_online_get_cached_credentials(&credentials) == ESP_OK) {
            copy_str(device_id, device_id_size, credentials.device_id);
            xSemaphoreTake(s_voip.lock, portMAX_DELAY);
            copy_str(s_voip.device_id, sizeof(s_voip.device_id), credentials.device_id);
            copy_str(s_voip.device_key, sizeof(s_voip.device_key), credentials.device_key);
            xSemaphoreGive(s_voip.lock);
        }
    }
}

static esp_err_t get_voip_runtime_credentials(device_online_credentials_t *credentials)
{
    if (credentials == NULL || ensure_runtime() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(credentials, 0, sizeof(*credentials));
    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    copy_str(credentials->device_id, sizeof(credentials->device_id), s_voip.device_id);
    copy_str(credentials->device_key, sizeof(credentials->device_key), s_voip.device_key);
    xSemaphoreGive(s_voip.lock);

    if (credentials->device_id[0] == '\0' || credentials->device_key[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void get_runtime_mqtt_token(char *token, size_t token_size)
{
    if (token == NULL || token_size == 0 || ensure_runtime() != ESP_OK) {
        return;
    }
    device_auth_token_t cached = {0};
    if (device_online_get_cached_mqtt_token(&cached) == ESP_OK) {
        copy_str(token, token_size, cached.mqtt_token);
        xSemaphoreTake(s_voip.lock, portMAX_DELAY);
        copy_str(s_voip.mqtt_token, sizeof(s_voip.mqtt_token), cached.mqtt_token);
        xSemaphoreGive(s_voip.lock);
        return;
    }
    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    token[0] = '\0';
    xSemaphoreGive(s_voip.lock);
}

static void remember_auth_user(const char *openid,
                               const char *model_id,
                               const char *wx_app_id,
                               const char *source)
{
    wechat_voip_auth_user_t user = {0};
    copy_str(user.openid, sizeof(user.openid), openid);
    copy_str(user.model_id, sizeof(user.model_id), model_id);
    copy_str(user.app_id, sizeof(user.app_id), wx_app_id);
    (void)wechat_voip_contacts_remember(&user, source);
}

static void caller_refresh_cb(const wechat_voip_auth_user_t *caller, void *ctx)
{
    (void)ctx;
    (void)wechat_voip_contacts_remember(caller, "callers");
}

static esp_err_t report_profile(void)
{
    char token[DEVICE_AUTH_MQTT_TOKEN_MAX_LEN] = {0};
    get_runtime_mqtt_token(token, sizeof(token));
    if (token[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    return wechat_voip_api_report_profile(WECHAT_VOIP_API_BASE, token);
}

static esp_err_t refresh_callers(void)
{
    char token[DEVICE_AUTH_MQTT_TOKEN_MAX_LEN] = {0};
    get_runtime_mqtt_token(token, sizeof(token));
    if (token[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    int count = 0;
    return wechat_voip_api_fetch_callers(WECHAT_VOIP_API_BASE,
                                         token,
                                         caller_refresh_cb,
                                         NULL,
                                         &count);
}

static const char *active_call_state_name(active_call_state_t state)
{
    switch (state) {
    case ACTIVE_CALL_REQUESTING:
        return "requesting";
    case ACTIVE_CALL_WAIT_JOIN:
        return "waiting-join";
    case ACTIVE_CALL_IDLE:
    default:
        return "idle";
    }
}

static void active_call_set_idle_locked(void)
{
    s_voip.active_call_state = ACTIVE_CALL_IDLE;
    s_voip.active_call_deadline_us = 0;
    s_voip.active_call_openid[0] = '\0';
}

static void active_call_reset_if_expired(const char *reason)
{
    int64_t now_us = esp_timer_get_time();
    bool expired = false;
    active_call_state_t old_state = ACTIVE_CALL_IDLE;

    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    if (s_voip.active_call_state != ACTIVE_CALL_IDLE &&
        s_voip.active_call_deadline_us > 0 &&
        now_us >= s_voip.active_call_deadline_us) {
        old_state = s_voip.active_call_state;
        ++s_voip.active_call_seq;
        active_call_set_idle_locked();
        expired = true;
    }
    xSemaphoreGive(s_voip.lock);

    if (expired) {
        ESP_LOGW(TAG, "active call expired: state=%s reason=%s",
                 active_call_state_name(old_state),
                 reason != NULL ? reason : "timeout");
    }
}

static esp_err_t active_call_begin(const char *open_id, uint32_t *seq)
{
    if (open_id == NULL || open_id[0] == '\0' || seq == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    active_call_reset_if_expired("before-start");

    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    if (s_voip.active_call_state != ACTIVE_CALL_IDLE) {
        active_call_state_t state = s_voip.active_call_state;
        xSemaphoreGive(s_voip.lock);
        ESP_LOGW(TAG, "active call busy: state=%s", active_call_state_name(state));
        return ESP_ERR_INVALID_STATE;
    }

    s_voip.active_call_state = ACTIVE_CALL_REQUESTING;
    s_voip.active_call_deadline_us = esp_timer_get_time() + (int64_t)ACTIVE_CALL_REQUEST_GUARD_MS * 1000;
    copy_str(s_voip.active_call_openid, sizeof(s_voip.active_call_openid), open_id);
    *seq = ++s_voip.active_call_seq;
    xSemaphoreGive(s_voip.lock);
    return ESP_OK;
}

static void active_call_abort(uint32_t seq)
{
    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    if (seq == s_voip.active_call_seq) {
        active_call_set_idle_locked();
    }
    xSemaphoreGive(s_voip.lock);
}

static bool active_call_is_current(uint32_t seq, active_call_state_t expected)
{
    bool current = false;
    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    current = seq == s_voip.active_call_seq && s_voip.active_call_state == expected;
    xSemaphoreGive(s_voip.lock);
    return current;
}

static void active_call_finish(uint32_t seq, esp_err_t result)
{
    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    if (seq == s_voip.active_call_seq && s_voip.active_call_state == ACTIVE_CALL_REQUESTING) {
        if (result == ESP_OK) {
            s_voip.active_call_state = ACTIVE_CALL_WAIT_JOIN;
            s_voip.active_call_deadline_us = esp_timer_get_time() + (int64_t)ACTIVE_CALL_JOIN_WAIT_MS * 1000;
            ESP_LOGI(TAG, "active call submitted, waiting for call_incoming");
        } else {
            active_call_set_idle_locked();
        }
    }
    xSemaphoreGive(s_voip.lock);
}

static bool take_active_call_join_pending(void)
{
    active_call_reset_if_expired("join");
    bool pending = false;

    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    if (s_voip.active_call_state == ACTIVE_CALL_WAIT_JOIN) {
        pending = true;
        active_call_set_idle_locked();
    }
    xSemaphoreGive(s_voip.lock);
    return pending;
}

static esp_err_t do_active_call(uint32_t seq)
{
    char openid[WECHAT_VOIP_OPEN_ID_MAX] = {0};
    wechat_voip_auth_user_t target = {0};

    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    copy_str(openid, sizeof(openid), s_voip.active_call_openid);
    xSemaphoreGive(s_voip.lock);

    wechat_voip_contacts_find(openid, &target);
    if (target.openid[0] == '\0' || target.model_id[0] == '\0') {
        (void)refresh_callers();
        wechat_voip_contacts_find(openid, &target);
    }
    if (target.openid[0] == '\0' || target.model_id[0] == '\0') {
        ESP_LOGW(TAG, "active call target missing auth: openid_len=%u", (unsigned)strlen(openid));
        return ESP_ERR_INVALID_STATE;
    }
    if (!wechat_voip_session_ready_for_next_call(true)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!active_call_is_current(seq, ACTIVE_CALL_REQUESTING)) {
        return ESP_ERR_INVALID_STATE;
    }

    char device_id[TIRTC_SESSION_DEVICE_ID_MAX_LEN] = {0};
    char token[DEVICE_AUTH_MQTT_TOKEN_MAX_LEN] = {0};
    get_runtime_device_id(device_id, sizeof(device_id));
    get_runtime_mqtt_token(token, sizeof(token));
    if (device_id[0] == '\0' || token[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    return wechat_voip_api_request_call(WECHAT_VOIP_API_BASE,
                                        token,
                                        device_id,
                                        &target,
                                        WECHAT_VOIP_ACTIVE_CALL_VERSION_TYPE);
}

static void active_call_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t seq = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &seq, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        esp_err_t ret = do_active_call(seq);
        active_call_finish(seq, ret);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "active call failed: %s", esp_err_to_name(ret));
        }
    }
}

static esp_err_t ensure_active_call_worker(void)
{
    if (s_voip.call_task != NULL) {
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreateWithCaps(active_call_task,
                                         "wx_voip_call",
                                         VOIP_CALL_TASK_STACK,
                                         NULL,
                                         VOIP_TASK_PRIORITY,
                                         &s_voip.call_task,
                                         WECHAT_VOIP_THING_TASK_CAPS);
    if (ret != pdPASS) {
        s_voip.call_task = NULL;
        log_heap_state("create wx_voip_call failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t post_user_auth(const char *open_id)
{
    char device_id[TIRTC_SESSION_DEVICE_ID_MAX_LEN] = {0};
    get_runtime_device_id(device_id, sizeof(device_id));
    if (device_id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    return wechat_voip_api_report_auth(WECHAT_VOIP_API_BASE, device_id, open_id);
}

static esp_err_t post_delete_auth(const wechat_voip_auth_user_t *removed)
{
    char device_id[TIRTC_SESSION_DEVICE_ID_MAX_LEN] = {0};
    get_runtime_device_id(device_id, sizeof(device_id));
    if (device_id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    return wechat_voip_api_delete_auth(WECHAT_VOIP_API_BASE, device_id, removed);
}

static void handle_call_incoming(cJSON *payload)
{
    if (!cJSON_IsObject(payload)) {
        ESP_LOGW(TAG, "call_incoming missing payload");
        return;
    }

    bool auto_answer = take_active_call_join_pending();
    const char *peer_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "peer_id"));
    const char *openid = json_string_any4(payload,
                                          "wx_user_openid",
                                          "wxa_user_openid",
                                          "wx_open_id",
                                          "wxa_open_id");
    const char *model_id = json_string_any(payload, "wx_model_id", "wxa_model_id");
    const char *app_id = json_string_any(payload, "wx_app_id", "wxa_app_id");
    char model_from_peer[WECHAT_VOIP_MODEL_ID_MAX] = {0};
    char app_from_peer[WECHAT_VOIP_APP_ID_MAX] = {0};
    if ((model_id == NULL || model_id[0] == '\0') && peer_id != NULL) {
        extract_query_param(peer_id, "x_wx_model_id", model_from_peer, sizeof(model_from_peer));
        model_id = model_from_peer;
    }
    if ((app_id == NULL || app_id[0] == '\0') && peer_id != NULL) {
        extract_query_param(peer_id, "x_wx_app_id", app_from_peer, sizeof(app_from_peer));
        app_id = app_from_peer;
    }

    ESP_LOGI(TAG, "%s", auto_answer ? "active call join received, submit WHIP immediately" : "incoming WeChat call");
    esp_err_t ret = wechat_voip_session_handle_join_room(payload, auto_answer);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "call_incoming rejected locally: %s", esp_err_to_name(ret));
        return;
    }
    remember_auth_user(openid, model_id, app_id, "call_incoming");
}

static void handle_call_cancel(cJSON *payload, cJSON *root)
{
    const char *room_id = json_string_any(payload, "wx_room_id", "wxa_room_id");
    if (room_id == NULL || room_id[0] == '\0') {
        room_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "room_id"));
    }
    if (room_id == NULL || room_id[0] == '\0') {
        room_id = json_string_any(root, "wx_room_id", "wxa_room_id");
    }
    if (room_id == NULL || room_id[0] == '\0') {
        room_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "room_id"));
    }

    bool matched = wechat_voip_session_cancel_by_room(room_id);
    if (matched) {
        ESP_LOGI(TAG, "wechat cancel matched active room: room=%s",
                 room_id != NULL && room_id[0] != '\0' ? room_id : "(empty)");
    } else {
        wechat_voip_thing_cancel_pending_call();
        ESP_LOGI(TAG, "wechat cancel did not match active room: room=%s",
                 room_id != NULL && room_id[0] != '\0' ? room_id : "(empty)");
    }
}

static void handle_envelope(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGW(TAG, "business message is not JSON");
        return;
    }

    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "type"));
    const char *channel = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "channel"));
    cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    bool is_voip_join = msg_type_is(type, "call_incoming", "wx_join_voip_room", "wxa_join_voip_room");
    bool is_voip_cancel = msg_type_is(type, "call_cancel", "wx_user_cancel", "wxa_user_cancel");
    bool is_callers_update = (type != NULL && strcmp(type, "callers_update") == 0);

    if (is_voip_join) {
        handle_call_incoming(payload);
    } else if (is_callers_update) {
        ESP_LOGI(TAG, "callers update received");
        (void)refresh_callers();
    } else if (is_voip_cancel) {
        ESP_LOGI(TAG, "wechat cancel message received: type=%s channel=%s",
                 type != NULL ? type : "(null)",
                 channel != NULL && channel[0] != '\0' ? channel : "(none)");
        handle_call_cancel(payload, root);
    } else if (channel != NULL && channel[0] != '\0' && strcmp(channel, "wx") != 0) {
        ESP_LOGI(TAG,
                 "ignore non-wechat business message: type=%s channel=%s",
                 type != NULL ? type : "(null)",
                 channel);
    } else {
        ESP_LOGW(TAG,
                 "unhandled business message type=%s channel=%s",
                 type != NULL ? type : "(null)",
                 channel != NULL && channel[0] != '\0' ? channel : "(none)");
    }
    cJSON_Delete(root);
}

static void message_task(void *arg)
{
    (void)arg;
    while (true) {
        voip_msg_item_t item = {0};
        if (xQueueReceive(s_voip.msg_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (item.json != NULL) {
            handle_envelope(item.json);
        }
        free(item.json);
    }
}

static esp_err_t ensure_message_worker(void)
{
    if (s_voip.msg_queue == NULL) {
        s_voip.msg_queue = xQueueCreateWithCaps(VOIP_MSG_QUEUE_LEN,
                                                sizeof(voip_msg_item_t),
                                                WECHAT_VOIP_THING_CONTROL_CAPS);
        if (s_voip.msg_queue == NULL) {
            log_heap_state("create wx_voip_msg queue failed");
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_voip.msg_task != NULL) {
        return ESP_OK;
    }
    BaseType_t ret = xTaskCreateWithCaps(message_task,
                                         "wx_voip_msg",
                                         VOIP_MSG_TASK_STACK,
                                         NULL,
                                         VOIP_TASK_PRIORITY,
                                         &s_voip.msg_task,
                                         WECHAT_VOIP_THING_TASK_CAPS);
    if (ret != pdPASS) {
        s_voip.msg_task = NULL;
        log_heap_state("create wx_voip_msg failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void mqtt_message_cb(const char *topic, const char *payload, size_t payload_len, void *ctx)
{
    (void)topic;
    (void)ctx;
    if (payload == NULL || payload_len == 0) {
        return;
    }
    if (ensure_message_worker() != ESP_OK) {
        ESP_LOGW(TAG, "message worker unavailable");
        return;
    }

    char *copy = heap_caps_malloc(payload_len + 1, WECHAT_VOIP_THING_PAYLOAD_CAPS);
    if (copy == NULL) {
        copy = malloc(payload_len + 1);
    }
    if (copy == NULL) {
        ESP_LOGW(TAG, "drop message: no memory len=%u", (unsigned)payload_len);
        return;
    }
    memcpy(copy, payload, payload_len);
    copy[payload_len] = '\0';

    voip_msg_item_t item = {
        .json = copy,
        .len = payload_len,
    };
    if (xQueueSend(s_voip.msg_queue, &item, 0) != pdPASS) {
        free(copy);
        ESP_LOGW(TAG, "drop message: queue full");
    }
}

static esp_err_t start_channel_once(void)
{
    esp_err_t ret = ESP_OK;
    device_online_credentials_t credentials = {0};

    ret = get_voip_runtime_credentials(&credentials);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "VoIP channel waits for bound device credentials");
        goto done;
    }

    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    copy_str(s_voip.device_id, sizeof(s_voip.device_id), credentials.device_id);
    copy_str(s_voip.device_key, sizeof(s_voip.device_key), credentials.device_key);
    s_voip.mqtt_token[0] = '\0';
    xSemaphoreGive(s_voip.lock);

    if (!thing_mqtt_client_is_started()) {
        ESP_LOGW(TAG, "VoIP channel waits for device online MQTT");
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }

    thing_mqtt_listener_handle_t listener = -1;
    ret = thing_mqtt_client_add_listener(mqtt_message_cb, NULL, &listener);
    if (ret != ESP_OK) {
        goto done;
    }
    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    s_voip.started = true;
    s_voip.mqtt_listener = listener;
    xSemaphoreGive(s_voip.lock);

    ret = report_profile();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "profile report failed, incoming listener remains active: %s", esp_err_to_name(ret));
        ret = ESP_OK;
    }
    esp_err_t callers_ret = refresh_callers();
    if (callers_ret != ESP_OK) {
        ESP_LOGW(TAG, "caller refresh failed, incoming listener remains active: %s", esp_err_to_name(callers_ret));
    }

done:
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "VoIP thing channel start failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t start_prerequisites_ready(device_online_credentials_t *out_credentials)
{
    device_online_credentials_t credentials = {0};

    esp_err_t ret = device_online_get_cached_credentials(&credentials);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!thing_mqtt_client_is_started()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_credentials != NULL) {
        *out_credentials = credentials;
    }
    return ESP_OK;
}

esp_err_t wechat_voip_thing_start(void)
{
    ESP_RETURN_ON_ERROR(ensure_runtime(), TAG, "runtime init failed");

    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    if (s_voip.started) {
        xSemaphoreGive(s_voip.lock);
        return ESP_OK;
    }
    xSemaphoreGive(s_voip.lock);

    device_online_credentials_t credentials = {0};
    esp_err_t ready_ret = start_prerequisites_ready(&credentials);
    if (ready_ret != ESP_OK) {
        return ready_ret;
    }

    /*
     * Contact NVS loading is routed through platform_nvs_async, so this path
     * no longer requires its caller to own an internal-RAM stack.
     */
    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    copy_str(s_voip.device_id, sizeof(s_voip.device_id), credentials.device_id);
    copy_str(s_voip.device_key, sizeof(s_voip.device_key), credentials.device_key);
    s_voip.mqtt_token[0] = '\0';
    xSemaphoreGive(s_voip.lock);
    wechat_voip_contacts_reset_for_device(credentials.device_id);
    wechat_voip_contacts_load(credentials.device_id);

    ESP_RETURN_ON_ERROR(ensure_message_worker(), TAG, "message worker failed");
    ESP_RETURN_ON_ERROR(ensure_active_call_worker(), TAG, "call worker failed");
    return start_channel_once();
}

void wechat_voip_thing_stop(void)
{
    thing_mqtt_listener_handle_t listener = -1;

    if (s_voip.lock != NULL) {
        xSemaphoreTake(s_voip.lock, portMAX_DELAY);
        s_voip.started = false;
        listener = s_voip.mqtt_listener;
        s_voip.mqtt_listener = -1;
        xSemaphoreGive(s_voip.lock);
    }
    wechat_voip_thing_cancel_pending_call();
    if (listener >= 0) {
        thing_mqtt_client_remove_listener(listener);
    }
}

bool wechat_voip_thing_is_connected(void)
{
    bool started = false;

    if (s_voip.lock != NULL) {
        xSemaphoreTake(s_voip.lock, portMAX_DELAY);
        started = s_voip.started;
        xSemaphoreGive(s_voip.lock);
    }
    return started && thing_mqtt_client_is_connected();
}

esp_err_t wechat_voip_thing_request_call(const char *open_id)
{
    if (open_id == NULL || open_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ensure_runtime(), TAG, "runtime init failed");
    if (!thing_mqtt_client_is_connected()) {
        ESP_LOGW(TAG, "cannot call: thing MQTT not connected");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t seq = 0;
    esp_err_t ret = active_call_begin(open_id, &seq);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ensure_active_call_worker();
    if (ret != ESP_OK) {
        active_call_abort(seq);
        return ret;
    }
    if (xTaskNotify(s_voip.call_task, seq, eSetValueWithOverwrite) != pdPASS) {
        active_call_abort(seq);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t wechat_voip_thing_add_contact(const char *open_id)
{
    if (open_id == NULL || open_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ensure_runtime(), TAG, "runtime init failed");
    esp_err_t ret = post_user_auth(open_id);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "report auth failed: ret=%s", esp_err_to_name(ret));
        return ret;
    }
    ret = refresh_callers();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "contact added");
    }
    return ret;
}

esp_err_t wechat_voip_thing_remove_contact(const char *open_id)
{
    if (open_id == NULL || open_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ensure_runtime(), TAG, "runtime init failed");

    wechat_voip_auth_user_t removed = {0};
    if (!wechat_voip_contacts_remove(open_id, &removed)) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t ret = post_delete_auth(&removed);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "delete auth sync failed, local contact removed: %s", esp_err_to_name(ret));
        return ESP_OK;
    }
    ESP_LOGI(TAG, "contact removed");
    return ESP_OK;
}

bool wechat_voip_thing_request_call_busy(void)
{
    if (ensure_runtime() != ESP_OK) {
        return false;
    }
    active_call_reset_if_expired("status");
    bool busy = false;
    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    busy = s_voip.active_call_state != ACTIVE_CALL_IDLE;
    xSemaphoreGive(s_voip.lock);
    return busy;
}

void wechat_voip_thing_cancel_pending_call(void)
{
    if (ensure_runtime() != ESP_OK) {
        return;
    }
    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    if (s_voip.active_call_state != ACTIVE_CALL_IDLE) {
        ++s_voip.active_call_seq;
        active_call_set_idle_locked();
        ESP_LOGI(TAG, "pending active call cancelled");
    }
    xSemaphoreGive(s_voip.lock);
}

void wechat_voip_thing_maintenance(void)
{
    if (ensure_runtime() != ESP_OK) {
        return;
    }
    active_call_reset_if_expired("maintenance");
}

void wechat_voip_thing_get_contacts(wechat_voip_contacts_snapshot_t *snapshot)
{
    wechat_voip_contacts_get_snapshot(snapshot);
}
