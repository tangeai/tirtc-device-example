#include "wechat_voip_thing.h"

#include <stdbool.h>
#include <stdint.h>
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
#include "thing_service_registry.h"
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
    VOIP_REFRESH_TASK_STACK = 24 * 1024,
    VOIP_CALL_TASK_STACK = 24 * 1024,
    VOIP_TASK_PRIORITY = 5,
    VOIP_CONTACT_REFRESH_INITIAL_CAPACITY = 8,
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
    uint32_t channel_generation;
} voip_msg_item_t;

typedef struct {
    uint32_t channel_generation;
    char openid[WECHAT_VOIP_OPEN_ID_MAX];
    char remark[WECHAT_VOIP_REMARK_MAX];
} contact_remark_job_t;

typedef struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t lifecycle_lock;
    SemaphoreHandle_t dispatch_lock;
    QueueHandle_t msg_queue;
    TaskHandle_t msg_task;
    TaskHandle_t refresh_task;
    TaskHandle_t call_task;
    bool started;
    uint32_t channel_generation;
    thing_mqtt_listener_handle_t mqtt_listener;
    char device_id[TIRTC_SESSION_DEVICE_ID_MAX_LEN];
    char device_key[TIRTC_SESSION_SECRET_KEY_MAX_LEN];
    wechat_voip_incoming_allowed_cb_t incoming_allowed;
    void *incoming_policy_ctx;
    active_call_state_t active_call_state;
    uint32_t active_call_seq;
    int64_t active_call_deadline_us;
    char active_call_openid[WECHAT_VOIP_OPEN_ID_MAX];
    bool remark_update_pending;
    bool remark_update_running;
    contact_remark_job_t remark_update;
} wechat_voip_thing_runtime_t;

/* VoIP bookkeeping and cached strings have no DMA/flash-disabled requirement. */
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
    if (s_voip.dispatch_lock == NULL) {
        s_voip.dispatch_lock = xSemaphoreCreateMutexWithCaps(WECHAT_VOIP_THING_CONTROL_CAPS);
        if (s_voip.dispatch_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_voip.lifecycle_lock == NULL) {
        s_voip.lifecycle_lock = xSemaphoreCreateMutexWithCaps(WECHAT_VOIP_THING_CONTROL_CAPS);
        if (s_voip.lifecycle_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_RETURN_ON_ERROR(wechat_voip_contacts_init(), TAG, "contact repo init failed");
    return ESP_OK;
}

static uint32_t advance_channel_generation_locked(void)
{
    ++s_voip.channel_generation;
    if (s_voip.channel_generation == 0U) {
        ++s_voip.channel_generation;
    }
    return s_voip.channel_generation;
}

esp_err_t wechat_voip_thing_set_incoming_policy(wechat_voip_incoming_allowed_cb_t callback,
                                                void *ctx)
{
    ESP_RETURN_ON_ERROR(ensure_runtime(), TAG, "runtime init failed");
    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    s_voip.incoming_allowed = callback;
    s_voip.incoming_policy_ctx = ctx;
    xSemaphoreGive(s_voip.lock);
    return ESP_OK;
}

static bool incoming_call_allowed(void)
{
    wechat_voip_incoming_allowed_cb_t callback = NULL;
    void *ctx = NULL;

    if (s_voip.lock != NULL) {
        xSemaphoreTake(s_voip.lock, portMAX_DELAY);
        callback = s_voip.incoming_allowed;
        ctx = s_voip.incoming_policy_ctx;
        xSemaphoreGive(s_voip.lock);
    }
    return callback == NULL || callback(ctx);
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
        return;
    }
    token[0] = '\0';
}

typedef struct {
    wechat_voip_auth_user_t *callers;
    size_t count;
    size_t capacity;
    esp_err_t error;
} caller_refresh_ctx_t;

static void caller_refresh_cb(const wechat_voip_auth_user_t *caller, void *ctx)
{
    caller_refresh_ctx_t *refresh = (caller_refresh_ctx_t *)ctx;
    if (refresh == NULL || caller == NULL ||
        caller->openid[0] == '\0' || caller->model_id[0] == '\0' ||
        refresh->error != ESP_OK) {
        return;
    }

    if (refresh->count == refresh->capacity) {
        size_t next_capacity = refresh->capacity == 0U ?
                               VOIP_CONTACT_REFRESH_INITIAL_CAPACITY :
                               refresh->capacity * 2U;
        wechat_voip_auth_user_t *next = heap_caps_realloc(
            refresh->callers,
            next_capacity * sizeof(*next),
            WECHAT_VOIP_THING_PAYLOAD_CAPS);
        if (next == NULL) {
            refresh->error = ESP_ERR_NO_MEM;
            return;
        }
        refresh->callers = next;
        refresh->capacity = next_capacity;
    }
    refresh->callers[refresh->count++] = *caller;
}

static esp_err_t report_profile(void)
{
    char token[DEVICE_AUTH_MQTT_TOKEN_MAX_LEN] = {0};
    get_runtime_mqtt_token(token, sizeof(token));
    if (token[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    return wechat_voip_api_report_profile(thing_service_registry_voip_api_base(), token);
}

static esp_err_t refresh_callers(void)
{
    char token[DEVICE_AUTH_MQTT_TOKEN_MAX_LEN] = {0};
    char device_id[TIRTC_SESSION_DEVICE_ID_MAX_LEN] = {0};
    uint32_t channel_generation = 0;
    caller_refresh_ctx_t refresh = {0};
    esp_err_t ret = ESP_OK;

    /* Always go through device_online so a seven-day token expiry refreshes
     * once at the shared cache owner instead of reusing this service's copy. */
    get_runtime_mqtt_token(token, sizeof(token));
    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    bool started = s_voip.started;
    channel_generation = s_voip.channel_generation;
    copy_str(device_id, sizeof(device_id), s_voip.device_id);
    xSemaphoreGive(s_voip.lock);
    if (!started || device_id[0] == '\0' || token[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    int server_count = 0;
    ret = wechat_voip_api_fetch_callers(thing_service_registry_voip_api_base(),
                                        token,
                                        caller_refresh_cb,
                                        &refresh,
                                        &server_count);
    if (ret != ESP_OK) {
        goto done;
    }
    if (refresh.error != ESP_OK) {
        ret = refresh.error;
        goto done;
    }

    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    bool current_channel = s_voip.started &&
                           s_voip.channel_generation == channel_generation &&
                           strcmp(s_voip.device_id, device_id) == 0;
    xSemaphoreGive(s_voip.lock);
    if (!current_channel) {
        ESP_LOGI(TAG, "drop stale caller refresh after identity/channel change");
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }

    ret = wechat_voip_contacts_replace_for_device(device_id,
                                                   channel_generation,
                                                   refresh.callers,
                                                   refresh.count,
                                                   "callers");
    if (ret != ESP_OK) {
        goto done;
    }
    if ((size_t)server_count > refresh.count) {
        ESP_LOGW(TAG,
                 "authorized caller response contained unusable entries: server=%d valid=%u",
                 server_count,
                 (unsigned)refresh.count);
    }

done:
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        wechat_voip_contacts_note_sync_error_for_device(device_id,
                                                         channel_generation,
                                                         ret);
    }
    free(refresh.callers);
    return ret;
}

static bool take_contact_remark_job(contact_remark_job_t *job)
{
    bool available = false;

    if (job == NULL) {
        return false;
    }
    memset(job, 0, sizeof(*job));

    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    if (s_voip.remark_update_pending) {
        *job = s_voip.remark_update;
        memset(&s_voip.remark_update, 0, sizeof(s_voip.remark_update));
        s_voip.remark_update_pending = false;
        s_voip.remark_update_running = true;
        available = true;
    }
    xSemaphoreGive(s_voip.lock);
    return available;
}

static void finish_contact_remark_job(void)
{
    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    s_voip.remark_update_running = false;
    xSemaphoreGive(s_voip.lock);
}

static esp_err_t update_contact_remark(const contact_remark_job_t *job)
{
    char token[DEVICE_AUTH_MQTT_TOKEN_MAX_LEN] = {0};
    bool current_channel = false;

    if (job == NULL || job->openid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    get_runtime_mqtt_token(token, sizeof(token));
    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    current_channel = s_voip.started &&
                      s_voip.channel_generation == job->channel_generation;
    xSemaphoreGive(s_voip.lock);
    if (!current_channel || token[0] == '\0' || !thing_mqtt_client_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = wechat_voip_api_update_contact_remark(
        thing_service_registry_call_api_base(),
        token,
        job->openid,
        job->remark);
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    current_channel = s_voip.started &&
                      s_voip.channel_generation == job->channel_generation;
    xSemaphoreGive(s_voip.lock);
    if (!current_channel) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t cache_ret = wechat_voip_contacts_update_remark(job->openid,
                                                             job->remark,
                                                             "device-remark");
    if (cache_ret != ESP_OK && cache_ret != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "cache updated contact remark failed: %s", esp_err_to_name(cache_ret));
    }

    ESP_LOGI(TAG,
             "contact remark updated: peer_id_len=%u remark_len=%u",
             (unsigned)strlen(job->openid),
             (unsigned)strlen(job->remark));
    return ESP_OK;
}

static void caller_refresh_task(void *arg)
{
    (void)arg;
    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        contact_remark_job_t remark_job = {0};
        if (take_contact_remark_job(&remark_job)) {
            esp_err_t ret = update_contact_remark(&remark_job);
            finish_contact_remark_job();
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "contact remark update failed: %s", esp_err_to_name(ret));
            }
            esp_err_t refresh_ret = refresh_callers();
            if (refresh_ret != ESP_OK && refresh_ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG,
                         "authoritative caller refresh after remark update failed: %s",
                         esp_err_to_name(refresh_ret));
            }
            continue;
        }

        esp_err_t ret = refresh_callers();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "caller refresh failed: %s", esp_err_to_name(ret));
        }
    }
}

static esp_err_t ensure_caller_refresh_worker(void)
{
    if (s_voip.refresh_task != NULL) {
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreateWithCaps(caller_refresh_task,
                                         "wx_voip_callers",
                                         VOIP_REFRESH_TASK_STACK,
                                         NULL,
                                         VOIP_TASK_PRIORITY,
                                         &s_voip.refresh_task,
                                         WECHAT_VOIP_THING_TASK_CAPS);
    if (ret != pdPASS) {
        s_voip.refresh_task = NULL;
        log_heap_state("create wx_voip_callers failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void request_caller_refresh(void)
{
    TaskHandle_t refresh_task = NULL;

    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    refresh_task = s_voip.refresh_task;
    xSemaphoreGive(s_voip.lock);
    if (refresh_task != NULL) {
        xTaskNotifyGive(refresh_task);
    } else {
        ESP_LOGW(TAG, "caller refresh worker unavailable");
    }
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

    return wechat_voip_api_request_call(thing_service_registry_voip_api_base(),
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

static void handle_call_incoming(cJSON *payload)
{
    if (!cJSON_IsObject(payload)) {
        ESP_LOGW(TAG, "call_incoming missing payload");
        return;
    }

    bool auto_answer = take_active_call_join_pending();
    if (!auto_answer && !incoming_call_allowed()) {
        esp_err_t reject_ret = wechat_voip_session_reject_join_room_busy(payload);
        ESP_LOGW(TAG,
                 "incoming WeChat call rejected busy by application policy: ret=%s",
                 esp_err_to_name(reject_ret));
        return;
    }
    ESP_LOGI(TAG, "%s", auto_answer ? "active call join received, submit WHIP immediately" : "incoming WeChat call");
    esp_err_t ret = wechat_voip_session_handle_join_room(payload, auto_answer);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "call_incoming rejected locally: %s", esp_err_to_name(ret));
        return;
    }
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
    bool channel_is_wechat = channel != NULL && strcmp(channel, "wx") == 0;
    bool channel_missing = channel == NULL || channel[0] == '\0';
    bool is_legacy_join = msg_type_is(type, "wx_join_voip_room", "wxa_join_voip_room", NULL);
    bool is_legacy_cancel = msg_type_is(type, "wx_user_cancel", "wxa_user_cancel", NULL);
    bool is_voip_join = (type != NULL && strcmp(type, "call_incoming") == 0 && channel_is_wechat) ||
                         (is_legacy_join && (channel_is_wechat || channel_missing));
    bool is_voip_cancel = (type != NULL && strcmp(type, "call_cancel") == 0 && channel_is_wechat) ||
                           (is_legacy_cancel && (channel_is_wechat || channel_missing));
    bool is_callers_update = type != NULL && strcmp(type, "callers_update") == 0 && channel_is_wechat;

    if (is_voip_join) {
        handle_call_incoming(payload);
    } else if (is_callers_update) {
        ESP_LOGI(TAG, "callers update received");
        request_caller_refresh();
    } else if (is_voip_cancel) {
        ESP_LOGI(TAG, "wechat cancel message received: type=%s channel=%s",
                 type != NULL ? type : "(null)",
                 channel != NULL && channel[0] != '\0' ? channel : "(none)");
        handle_call_cancel(payload, root);
    } else if (!channel_is_wechat && !channel_missing) {
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
        xSemaphoreTake(s_voip.dispatch_lock, portMAX_DELAY);
        bool current_channel = false;
        xSemaphoreTake(s_voip.lock, portMAX_DELAY);
        current_channel = s_voip.started && item.channel_generation == s_voip.channel_generation;
        xSemaphoreGive(s_voip.lock);
        if (item.json != NULL && current_channel) {
            handle_envelope(item.json);
        } else if (item.json != NULL) {
            ESP_LOGI(TAG, "drop stale VoIP message after identity/channel change");
        }
        xSemaphoreGive(s_voip.dispatch_lock);
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
    uint32_t channel_generation = 0;
    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    bool started = s_voip.started;
    channel_generation = s_voip.channel_generation;
    xSemaphoreGive(s_voip.lock);
    if (!started) {
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
        .channel_generation = channel_generation,
    };
    if (xQueueSend(s_voip.msg_queue, &item, 0) != pdPASS) {
        free(copy);
        ESP_LOGW(TAG, "drop message: queue full");
    }
}

static esp_err_t start_channel_once(uint32_t start_generation)
{
    esp_err_t ret = ESP_OK;
    device_online_credentials_t credentials = {0};

    ret = get_voip_runtime_credentials(&credentials);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "VoIP channel waits for bound device credentials");
        goto done;
    }

    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    bool current_start = !s_voip.started &&
                         s_voip.channel_generation == start_generation &&
                         strcmp(s_voip.device_id, credentials.device_id) == 0;
    xSemaphoreGive(s_voip.lock);
    if (!current_start) {
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }

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
    current_start = !s_voip.started &&
                    s_voip.channel_generation == start_generation &&
                    strcmp(s_voip.device_id, credentials.device_id) == 0;
    if (current_start) {
        s_voip.started = true;
        s_voip.mqtt_listener = listener;
    }
    xSemaphoreGive(s_voip.lock);
    if (!current_start) {
        thing_mqtt_client_remove_listener(listener);
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }

    ret = report_profile();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "profile report failed, incoming listener remains active: %s", esp_err_to_name(ret));
        ret = ESP_OK;
    }
    request_caller_refresh();

done:
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
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
    esp_err_t ret = ensure_runtime();
    ESP_RETURN_ON_ERROR(ret, TAG, "runtime init failed");

    xSemaphoreTake(s_voip.lifecycle_lock, portMAX_DELAY);

    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    if (s_voip.started) {
        xSemaphoreGive(s_voip.lock);
        ret = ESP_OK;
        goto done;
    }
    xSemaphoreGive(s_voip.lock);

    device_online_credentials_t credentials = {0};
    ret = start_prerequisites_ready(&credentials);
    if (ret != ESP_OK) {
        goto done;
    }

    uint32_t start_generation = 0;
    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    start_generation = advance_channel_generation_locked();
    copy_str(s_voip.device_id, sizeof(s_voip.device_id), credentials.device_id);
    copy_str(s_voip.device_key, sizeof(s_voip.device_key), credentials.device_key);
    xSemaphoreGive(s_voip.lock);
    wechat_voip_contacts_reset_for_device(credentials.device_id, start_generation);

    ret = ensure_message_worker();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "message worker failed: %s", esp_err_to_name(ret));
        goto done;
    }
    ret = ensure_caller_refresh_worker();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "caller refresh worker failed: %s", esp_err_to_name(ret));
        goto done;
    }
    ret = start_channel_once(start_generation);

done:
    xSemaphoreGive(s_voip.lifecycle_lock);
    return ret;
}

void wechat_voip_thing_stop(void)
{
    thing_mqtt_listener_handle_t listener = -1;
    uint32_t stopped_generation = 0;

    if (ensure_runtime() != ESP_OK) {
        return;
    }
    xSemaphoreTake(s_voip.lifecycle_lock, portMAX_DELAY);
    if (s_voip.dispatch_lock != NULL) {
        xSemaphoreTake(s_voip.dispatch_lock, portMAX_DELAY);
    }
    if (s_voip.lock != NULL) {
        xSemaphoreTake(s_voip.lock, portMAX_DELAY);
        s_voip.started = false;
        stopped_generation = advance_channel_generation_locked();
        listener = s_voip.mqtt_listener;
        s_voip.mqtt_listener = -1;
        s_voip.device_id[0] = '\0';
        s_voip.device_key[0] = '\0';
        s_voip.remark_update_pending = false;
        memset(&s_voip.remark_update, 0, sizeof(s_voip.remark_update));
        xSemaphoreGive(s_voip.lock);
    }
    if (s_voip.dispatch_lock != NULL) {
        xSemaphoreGive(s_voip.dispatch_lock);
    }
    wechat_voip_thing_cancel_pending_call();
    if (listener >= 0) {
        thing_mqtt_client_remove_listener(listener);
    }
    /* Contacts are server-owned; leaving the app only releases the runtime cache. */
    wechat_voip_contacts_reset_for_device("", stopped_generation);
    xSemaphoreGive(s_voip.lifecycle_lock);
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

esp_err_t wechat_voip_thing_refresh_contacts_async(void)
{
    ESP_RETURN_ON_ERROR(ensure_runtime(), TAG, "runtime init failed");

    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    bool started = s_voip.started;
    TaskHandle_t refresh_task = s_voip.refresh_task;
    xSemaphoreGive(s_voip.lock);
    if (!started || refresh_task == NULL || !thing_mqtt_client_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    xTaskNotifyGive(refresh_task);
    return ESP_OK;
}

esp_err_t wechat_voip_thing_update_contact_remark_async(const char *open_id,
                                                        const char *remark)
{
    if (open_id == NULL || open_id[0] == '\0' || remark == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(open_id) >= WECHAT_VOIP_OPEN_ID_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!wechat_voip_remark_is_valid(remark)) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(ensure_runtime(), TAG, "runtime init failed");

    xSemaphoreTake(s_voip.lock, portMAX_DELAY);
    bool ready = s_voip.started &&
                 s_voip.refresh_task != NULL &&
                 !s_voip.remark_update_pending &&
                 !s_voip.remark_update_running;
    TaskHandle_t refresh_task = s_voip.refresh_task;
    if (ready) {
        s_voip.remark_update.channel_generation = s_voip.channel_generation;
        copy_str(s_voip.remark_update.openid,
                 sizeof(s_voip.remark_update.openid),
                 open_id);
        copy_str(s_voip.remark_update.remark,
                 sizeof(s_voip.remark_update.remark),
                 remark);
        s_voip.remark_update_pending = true;
    }
    xSemaphoreGive(s_voip.lock);

    if (!ready || refresh_task == NULL || !thing_mqtt_client_is_connected()) {
        if (ready) {
            xSemaphoreTake(s_voip.lock, portMAX_DELAY);
            s_voip.remark_update_pending = false;
            memset(&s_voip.remark_update, 0, sizeof(s_voip.remark_update));
            xSemaphoreGive(s_voip.lock);
        }
        return ESP_ERR_INVALID_STATE;
    }

    xTaskNotifyGive(refresh_task);
    return ESP_OK;
}

esp_err_t wechat_voip_thing_add_contact(const char *open_id)
{
    if (open_id == NULL || open_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ensure_runtime(), TAG, "runtime init failed");

    esp_err_t ret = wechat_voip_contacts_check_authorized(open_id);
    if (ret == ESP_ERR_NOT_FOUND || ret == ESP_ERR_INVALID_STATE) {
        request_caller_refresh();
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WeChat contact authorization verified");
    } else if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG,
                 "WeChat contact is not authorized; complete authorization in the mini program first");
    }
    return ret;
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
