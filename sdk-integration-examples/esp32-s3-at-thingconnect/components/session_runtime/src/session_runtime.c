#include "session_runtime_internal.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "media_runtime.h"
#include "platform_client.h"

#define SESSION_QUEUE_DEPTH 16U
#define SESSION_EVENT_POOL_SIZE 20U
#define SESSION_HTTP_CONTEXT_COUNT 12U
#define SESSION_TASK_STACK_BYTES 24576U
#define SESSION_TASK_PERIOD_MS 20U
#define SESSION_ROOM_POLL_INTERVAL_MS 30000

#define SESSION_RECONCILE_ADAPTER (1U << 0)
#define SESSION_RECONCILE_COMMAND (1U << 1)
#define SESSION_RECONCILE_SIGNAL (1U << 2)
#define SESSION_RECONCILE_HTTP (1U << 3)
#define SESSION_RECONCILE_MEDIA (1U << 4)

typedef struct {
    atomic_bool in_use;
    session_http_request_t request;
    uint32_t generation;
    uint32_t request_cookie;
    uint32_t origin_request_id;
    char correlation_id[SESSION_RUNTIME_ID_MAX];
} session_http_context_t;

static const char *TAG = "session_runtime";
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static session_internal_event_t s_event_pool[SESSION_EVENT_POOL_SIZE];
static atomic_bool s_event_used[SESSION_EVENT_POOL_SIZE];
static session_http_context_t s_http_contexts[SESSION_HTTP_CONTEXT_COUNT];
static atomic_uint_fast32_t s_reconcile_flags;
static atomic_bool s_recovery_inflight;
static session_context_t s_context;

static portMUX_TYPE s_public_lock = portMUX_INITIALIZER_UNLOCKED;
static session_runtime_snapshot_t s_snapshot;
static session_runtime_observer_t s_observer;
static void *s_observer_user_data;

static uint32_t next_generation(uint32_t generation)
{
    generation++;
    return generation == 0U ? 1U : generation;
}

int64_t session_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void session_secure_zero(void *memory, size_t size)
{
    volatile unsigned char *cursor = memory;
    while (cursor != NULL && size-- > 0U) {
        *cursor++ = 0;
    }
}

bool session_copy_string(char *destination,
                         size_t destination_size,
                         const char *source)
{
    if (destination == NULL || destination_size == 0U || source == NULL ||
        strlen(source) >= destination_size) {
        return false;
    }
    (void)snprintf(destination, destination_size, "%s", source);
    return true;
}

bool session_copy_json(char *destination,
                       size_t destination_size,
                       const char *source)
{
    if (session_copy_string(destination, destination_size, source)) {
        return true;
    }
    (void)session_copy_string(destination,
                              destination_size,
                              "{\"truncated\":true}");
    return false;
}

bool session_json_string(const cJSON *object,
                         const char *name,
                         char *destination,
                         size_t destination_size)
{
    const cJSON *item = object == NULL
                            ? NULL
                            : cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring != NULL &&
           item->valuestring[0] != '\0' &&
           session_copy_string(destination, destination_size, item->valuestring);
}

int session_json_response_code(const cJSON *root)
{
    const cJSON *code = root == NULL
                            ? NULL
                            : cJSON_GetObjectItemCaseSensitive(root, "code");
    return cJSON_IsNumber(code) ? code->valueint : -1;
}

const cJSON *session_json_response_data(const cJSON *root)
{
    return root == NULL ? NULL
                        : cJSON_GetObjectItemCaseSensitive(root, "data");
}

void session_make_rpc_id(session_context_t *context,
                         const char *prefix,
                         char *destination,
                         size_t destination_size)
{
    context->rpc_sequence++;
    (void)snprintf(destination,
                   destination_size,
                   "%s-%08lx-%08lx",
                   prefix,
                   (unsigned long)context->generation,
                   (unsigned long)(context->rpc_sequence ^ esp_random()));
}

static void publish_snapshot(session_context_t *context)
{
    context->revision = next_generation(context->revision);
    session_runtime_snapshot_t snapshot = {
        .owner = context->owner,
        .state = context->state,
        .generation = context->generation,
        .revision = context->revision,
        .origin_request_id = context->last_origin_request_id,
        .deadline_ms = context->deadline_ms,
        .pending_incoming_call =
            context->owner == DEVICE_SERVICE_CALL &&
            context->call.phase == SESSION_CALL_PHASE_RINGING,
        .caller = context->owner == DEVICE_SERVICE_CALL &&
                  context->call.caller,
        .ai_update_pending =
            context->owner == DEVICE_SERVICE_AI &&
            context->ai.update_request_id[0] != '\0',
        .ai_action_pending =
            context->owner == DEVICE_SERVICE_AI &&
            context->ai.action_id[0] != '\0',
        .ai_call_handoff_pending =
            context->ai_call_handoff.phase !=
                SESSION_RUNTIME_HANDOFF_NONE,
        .call_type = context->call.call_type,
        .ai_call_handoff_phase =
            context->ai_call_handoff.phase,
        .ai_update_deadline_ms = context->ai.update_deadline_ms,
        .ai_action_deadline_ms = context->ai.action_deadline_ms,
        .ai_call_handoff_deadline_ms =
            context->ai_call_handoff.deadline_ms,
    };
    (void)session_copy_string(snapshot.room_id,
                              sizeof(snapshot.room_id),
                              context->call.room_id);
    (void)session_copy_string(snapshot.peer_id,
                              sizeof(snapshot.peer_id),
                              context->owner == DEVICE_SERVICE_CALL
                                  ? context->call.peer_id
                                  : "");
    (void)session_copy_string(snapshot.ai_session_id,
                              sizeof(snapshot.ai_session_id),
                              context->ai.session_id);
    (void)session_copy_string(snapshot.ai_action_id,
                              sizeof(snapshot.ai_action_id),
                              context->ai.action_id);

    portENTER_CRITICAL(&s_public_lock);
    s_snapshot = snapshot;
    portEXIT_CRITICAL(&s_public_lock);
}

void session_refresh_snapshot(session_context_t *context)
{
    if (context != NULL) {
        publish_snapshot(context);
    }
}

void session_emit(session_context_t *context,
                  const session_runtime_event_t *event)
{
    if (context == NULL || event == NULL) {
        return;
    }
    session_runtime_event_t copy = *event;
    copy.owner = context->owner;
    copy.state = context->state;
    copy.generation = context->generation;
    copy.origin_request_id = context->event_origin_request_id;

    session_runtime_observer_t observer;
    void *user_data;
    portENTER_CRITICAL(&s_public_lock);
    observer = s_observer;
    user_data = s_observer_user_data;
    portEXIT_CRITICAL(&s_public_lock);
    if (observer != NULL) {
        observer(&copy, user_data);
    }
}

void session_emit_operation(session_context_t *context,
                            const char *name,
                            int status,
                            const char *text,
                            const char *json)
{
    session_runtime_event_t event = {
        .type = SESSION_RUNTIME_EVENT_OPERATION,
        .status = status,
    };
    (void)session_copy_string(event.name, sizeof(event.name), name == NULL ? "" : name);
    (void)session_copy_string(event.text, sizeof(event.text), text == NULL ? "" : text);
    (void)session_copy_json(event.json,
                           sizeof(event.json),
                           json == NULL ? "" : json);
    session_emit(context, &event);
}

void session_emit_diagnostic(session_context_t *context,
                             int status,
                             const char *text)
{
    session_runtime_event_t event = {
        .type = SESSION_RUNTIME_EVENT_DIAGNOSTIC,
        .status = status,
    };
    (void)session_copy_string(event.text, sizeof(event.text), text == NULL ? "" : text);
    session_emit(context, &event);
}

void session_set_state(session_context_t *context,
                       device_session_state_t state,
                       const char *reason,
                       int status)
{
    device_session_state_t previous = context->state;
    context->state = state;
    publish_snapshot(context);
    ESP_LOGI(TAG,
             "state %s -> %s owner=%s generation=%lu reason=%s status=%d",
             device_session_state_name(previous),
             device_session_state_name(state),
             device_service_name(context->owner),
             (unsigned long)context->generation,
             reason == NULL ? "" : reason,
             status);
    session_runtime_event_t event = {
        .type = SESSION_RUNTIME_EVENT_STATE,
        .status = status,
    };
    (void)session_copy_string(event.name,
                              sizeof(event.name),
                              device_session_state_name(state));
    (void)session_copy_string(event.text,
                              sizeof(event.text),
                              reason == NULL ? "" : reason);
    (void)session_copy_string(event.room_id,
                              sizeof(event.room_id),
                              context->call.room_id);
    (void)session_copy_string(event.peer_id,
                              sizeof(event.peer_id),
                              context->owner == DEVICE_SERVICE_CALL
                                  ? context->call.peer_id
                                  : "");
    session_emit(context, &event);
}

bool session_claim(session_context_t *context,
                   device_service_t owner,
                   device_session_state_t initial_state,
                   tirtc_adapter_media_profile_t media_profile,
                   bool uplink_video_enabled,
                   const char *reason)
{
    if (context->owner != DEVICE_SERVICE_NONE || owner == DEVICE_SERVICE_NONE ||
        !media_runtime_ready()) {
        return false;
    }

    uint32_t generation = next_generation(context->generation);
    int rc = tirtc_adapter_set_media_profile(media_profile,
                                             generation,
                                             owner == DEVICE_SERVICE_CALL &&
                                                 uplink_video_enabled);
    if (rc != 0) {
        ESP_LOGW(TAG, "cannot claim TiRTC media profile rc=%d", rc);
        return false;
    }
    esp_err_t err = media_runtime_set_session(media_profile,
                                              generation,
                                              uplink_video_enabled);
    if (err != ESP_OK) {
        (void)tirtc_adapter_set_media_profile(TIRTC_ADAPTER_MEDIA_NONE, 0, false);
        ESP_LOGW(TAG, "cannot claim recorded-media profile: %s", esp_err_to_name(err));
        return false;
    }

    session_secure_zero(&context->ai, sizeof(context->ai));
    session_secure_zero(&context->call, sizeof(context->call));
    context->generation = generation;
    context->owner = owner;
    context->session_origin_request_id = context->event_origin_request_id;
    context->last_origin_request_id = context->event_origin_request_id;
    context->deadline_ms = 0;
    session_set_state(context, initial_state, reason, 0);
    return true;
}

void session_finish(session_context_t *context,
                    const char *reason,
                    int status)
{
    bool preserve_handoff =
        context->ai_call_handoff.phase ==
        SESSION_RUNTIME_HANDOFF_ADAPTER_DRAIN;
    uint32_t owned_generation = context->generation;
    if (context->owner != DEVICE_SERVICE_NONE) {
        session_set_state(context, DEVICE_SESSION_ENDING, reason, status);
    }

    media_runtime_set_uplink_active(false);
    media_runtime_clear_session(owned_generation);
    (void)tirtc_adapter_cancel_connect();
    (void)tirtc_adapter_disconnect();
    int rc = tirtc_adapter_set_media_profile(TIRTC_ADAPTER_MEDIA_NONE, 0, false);
    if (rc != 0) {
        ESP_LOGW(TAG, "media profile clear deferred rc=%d", rc);
    }

    session_secure_zero(&context->ai, sizeof(context->ai));
    session_secure_zero(&context->call, sizeof(context->call));
    if (!preserve_handoff) {
        session_secure_zero(&context->ai_call_handoff,
                            sizeof(context->ai_call_handoff));
    }
    context->owner = DEVICE_SERVICE_NONE;
    context->deadline_ms = 0;
    context->generation = next_generation(context->generation);
    session_set_state(context, DEVICE_SESSION_IDLE, reason, status);
    context->session_origin_request_id = 0U;
    context->event_origin_request_id = 0U;
    context->last_origin_request_id = 0U;
    session_refresh_snapshot(context);
}

static session_internal_event_t *event_allocate(void)
{
    for (size_t index = 0; index < SESSION_EVENT_POOL_SIZE; ++index) {
        bool expected = false;
        if (atomic_compare_exchange_strong_explicit(&s_event_used[index],
                                                    &expected,
                                                    true,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire)) {
            memset(&s_event_pool[index], 0, sizeof(s_event_pool[index]));
            return &s_event_pool[index];
        }
    }
    return NULL;
}

static void event_release(session_internal_event_t *event)
{
    if (event == NULL || event < &s_event_pool[0] ||
        event >= &s_event_pool[SESSION_EVENT_POOL_SIZE]) {
        return;
    }
    size_t index = (size_t)(event - &s_event_pool[0]);
    atomic_store_explicit(&s_event_used[index], false, memory_order_release);
}

static bool event_queue(session_internal_event_t *event,
                        uint32_t reconcile_flag)
{
    if (event != NULL && s_queue != NULL &&
        xQueueSend(s_queue, &event, 0) == pdTRUE) {
        return true;
    }
    event_release(event);
    if (reconcile_flag != 0U) {
        (void)atomic_fetch_or_explicit(&s_reconcile_flags,
                                      reconcile_flag,
                                      memory_order_acq_rel);
    }
    return false;
}

static session_http_context_t *http_context_allocate(session_http_request_t request,
                                                     uint32_t generation,
                                                     uint32_t request_cookie,
                                                     uint32_t origin_request_id,
                                                     const char *correlation_id)
{
    for (size_t index = 0; index < SESSION_HTTP_CONTEXT_COUNT; ++index) {
        bool expected = false;
        if (atomic_compare_exchange_strong_explicit(
                &s_http_contexts[index].in_use,
                &expected,
                true,
                memory_order_acq_rel,
                memory_order_acquire)) {
            s_http_contexts[index].request = request;
            s_http_contexts[index].generation = generation;
            s_http_contexts[index].request_cookie = request_cookie;
            s_http_contexts[index].origin_request_id = origin_request_id;
            s_http_contexts[index].correlation_id[0] = '\0';
            (void)session_copy_string(s_http_contexts[index].correlation_id,
                                      sizeof(s_http_contexts[index].correlation_id),
                                      correlation_id == NULL ? "" : correlation_id);
            return &s_http_contexts[index];
        }
    }
    return NULL;
}

static void http_context_release(session_http_context_t *context)
{
    if (context != NULL) {
        atomic_store_explicit(&context->in_use, false, memory_order_release);
    }
}

static void http_response_callback(const char *body, void *user_data)
{
    session_http_context_t *http = (session_http_context_t *)user_data;
    if (http == NULL) {
        (void)atomic_fetch_or_explicit(&s_reconcile_flags,
                                      SESSION_RECONCILE_HTTP,
                                      memory_order_acq_rel);
        return;
    }

    session_internal_event_t *event = event_allocate();
    if (event != NULL) {
        event->type = SESSION_INT_HTTP_RESPONSE;
        event->http_request = http->request;
        event->generation = http->generation;
        event->request_cookie = http->request_cookie;
        event->origin_request_id = http->origin_request_id;
        (void)session_copy_string(event->first,
                                  sizeof(event->first),
                                  http->correlation_id);
        if (body != NULL) {
            size_t length = strlen(body);
            event->truncated = length >= sizeof(event->payload);
            event->length = (uint32_t)(event->truncated
                                           ? sizeof(event->payload) - 1U
                                           : length);
            memcpy(event->payload, body, event->length);
            event->payload[event->length] = '\0';
        }
    }
    if (http->request == SESSION_HTTP_CALL_RECOVER) {
        atomic_store_explicit(&s_recovery_inflight, false, memory_order_release);
    }
    http_context_release(http);
    (void)event_queue(event, SESSION_RECONCILE_HTTP);
}

esp_err_t session_submit_http_correlated(session_context_t *context,
                                         session_http_request_t request,
                                         uint32_t generation,
                                         uint32_t request_cookie,
                                         const char *method,
                                         const char *path,
                                         const char *json_body)
{
    (void)context;
    if (method == NULL || path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *correlation_id =
        generation != 0U && context != NULL ? context->call.room_id : NULL;
    session_http_context_t *http = http_context_allocate(request,
                                                         generation,
                                                         request_cookie,
                                                         context == NULL
                                                             ? 0U
                                                             : context->event_origin_request_id,
                                                         correlation_id);
    if (http == NULL) {
        return ESP_ERR_NO_MEM;
    }

    platform_service_t service = request == SESSION_HTTP_AI_TOKEN
                                     ? PLATFORM_SERVICE_AI
                                     : PLATFORM_SERVICE_CALL;
    esp_err_t err = platform_client_request_ex(service,
                                               method,
                                               path,
                                               json_body,
                                               http_response_callback,
                                               http);

    if (err != ESP_OK) {
        http_context_release(http);
    }
    return err;
}

esp_err_t session_submit_http(session_context_t *context,
                              session_http_request_t request,
                              uint32_t generation,
                              const char *method,
                              const char *path,
                              const char *json_body)
{
    return session_submit_http_correlated(context,
                                          request,
                                          generation,
                                          0U,
                                          method,
                                          path,
                                          json_body);
}

static void adapter_connection_callback(
    const tirtc_adapter_connection_event_t *adapter_event,
    void *user_data)
{
    (void)user_data;
    session_internal_event_t *event = event_allocate();
    if (event != NULL && adapter_event != NULL) {
        event->type = SESSION_INT_ADAPTER_CONNECTION;
        event->generation = adapter_event->session_generation;
        event->connection = *adapter_event;
    }
    (void)event_queue(event, SESSION_RECONCILE_ADAPTER);
}

static void adapter_command_callback(
    const tirtc_adapter_command_event_t *adapter_event,
    void *user_data)
{
    (void)user_data;
    session_internal_event_t *event = event_allocate();
    if (event != NULL && adapter_event != NULL) {
        event->type = SESSION_INT_ADAPTER_COMMAND;
        event->generation = adapter_event->session_generation;
        event->connection_generation =
            adapter_event->connection_generation;
        event->command = adapter_event->command;
        event->truncated = adapter_event->length >= sizeof(event->payload);
        event->length = event->truncated
                            ? sizeof(event->payload) - 1U
                            : adapter_event->length;
        if (adapter_event->data != NULL && event->length > 0U) {
            memcpy(event->payload, adapter_event->data, event->length);
            event->payload[event->length] = '\0';
        }
    }
    (void)event_queue(event, SESSION_RECONCILE_COMMAND);
}

static esp_err_t platform_signal_callback(const char *json,
                                          size_t length,
                                          void *user_data)
{
    (void)user_data;
    if (json == NULL || length == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (length >= sizeof(s_event_pool[0].payload)) {
        (void)atomic_fetch_or_explicit(&s_reconcile_flags,
                                      SESSION_RECONCILE_SIGNAL,
                                      memory_order_acq_rel);
        return ESP_ERR_INVALID_SIZE;
    }
    session_internal_event_t *event = event_allocate();
    if (event == NULL) {
        (void)event_queue(NULL, SESSION_RECONCILE_SIGNAL);
        return ESP_ERR_NO_MEM;
    }
    event->type = SESSION_INT_PLATFORM_SIGNAL;
    event->length = (uint32_t)length;
    memcpy(event->payload, json, event->length);
    event->payload[event->length] = '\0';
    return event_queue(event, SESSION_RECONCILE_SIGNAL)
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static void media_error_callback(uint32_t generation,
                                 esp_err_t error,
                                 const char *stage,
                                 void *user_data)
{
    (void)user_data;
    session_internal_event_t *event = event_allocate();
    if (event != NULL) {
        event->type = SESSION_INT_MEDIA_ERROR;
        event->generation = generation;
        event->status = error;
        (void)session_copy_string(event->first,
                                  sizeof(event->first),
                                  stage == NULL ? "media" : stage);
    }
    (void)event_queue(event, SESSION_RECONCILE_MEDIA);
}

static bool event_is_ai(session_internal_event_type_t type)
{
    return type >= SESSION_INT_AI_START && type <= SESSION_INT_AI_STOP;
}

static bool event_is_call(session_internal_event_type_t type)
{
    return type >= SESSION_INT_CALL_START &&
           type <= SESSION_INT_CONTACTS_DELETE;
}

static bool event_is_intent(session_internal_event_type_t type)
{
    return type >= SESSION_INT_ABORT;
}

static bool event_is_session_intent(session_internal_event_type_t type)
{
    return type == SESSION_INT_ABORT ||
           event_is_ai(type) ||
           (type >= SESSION_INT_CALL_START &&
            type <= SESSION_INT_CALL_RECOVER);
}

static bool event_starts_or_ends_session(session_internal_event_type_t type)
{
    return type == SESSION_INT_AI_START ||
           type == SESSION_INT_AI_STOP ||
           type == SESSION_INT_CALL_START ||
           type == SESSION_INT_CALL_ACCEPT ||
           type == SESSION_INT_CALL_REJECT ||
           type == SESSION_INT_CALL_CANCEL ||
           type == SESSION_INT_CALL_HANGUP;
}

static void handle_internal_event(session_internal_event_t *event)
{
    if (event == NULL) {
        return;
    }

    if (event->type == SESSION_INT_ADAPTER_CONNECTION) {
        uint32_t generation = event->connection.connection_generation;
        uint32_t current_generation =
            tirtc_adapter_connection_generation();
        bool newer =
            s_context.last_connection_generation == 0U ||
            (int32_t)(generation -
                      s_context.last_connection_generation) > 0;
        if (generation == 0U || generation != current_generation || !newer) {
            ESP_LOGI(TAG,
                     "discarding superseded connection event=%lu current=%lu last=%lu",
                     (unsigned long)generation,
                     (unsigned long)current_generation,
                     (unsigned long)s_context.last_connection_generation);
            return;
        }
        s_context.last_connection_generation = generation;
    } else if (event->type == SESSION_INT_ADAPTER_COMMAND) {
        uint32_t current_generation =
            tirtc_adapter_connection_generation();
        if (event->connection_generation == 0U ||
            event->connection_generation != current_generation) {
            ESP_LOGI(TAG,
                     "discarding command from superseded connection=%lu current=%lu",
                     (unsigned long)event->connection_generation,
                     (unsigned long)current_generation);
            return;
        }
    }

    if ((event->type == SESSION_INT_ADAPTER_CONNECTION ||
         event->type == SESSION_INT_ADAPTER_COMMAND ||
         event->type == SESSION_INT_MEDIA_ERROR) &&
        event->generation != 0U &&
        event->generation != s_context.generation) {
        ESP_LOGI(TAG,
                 "discarding stale event type=%d generation=%lu current=%lu",
                 (int)event->type,
                 (unsigned long)event->generation,
                 (unsigned long)s_context.generation);
        return;
    }

    s_context.event_origin_request_id = 0U;
    if (event_is_intent(event->type)) {
        s_context.event_origin_request_id = event->origin_request_id;
        if (event_is_session_intent(event->type) &&
            event->origin_request_id != 0U) {
            s_context.last_origin_request_id = event->origin_request_id;
        }
        if (event_starts_or_ends_session(event->type)) {
            s_context.session_origin_request_id = event->origin_request_id;
        }
    } else if (event->type == SESSION_INT_HTTP_RESPONSE) {
        s_context.event_origin_request_id = event->origin_request_id;
    } else if (event->type == SESSION_INT_ADAPTER_CONNECTION ||
               event->type == SESSION_INT_MEDIA_ERROR) {
        s_context.event_origin_request_id =
            s_context.session_origin_request_id;
    } else if (event->type == SESSION_INT_ADAPTER_COMMAND &&
               event->command == SESSION_COMMAND_CALL_CONFIRM) {
        s_context.event_origin_request_id =
            s_context.session_origin_request_id;
    }

    if (event->type == SESSION_INT_ABORT) {
        session_finish(&s_context,
                       event->first[0] == '\0' ? "abort" : event->first,
                       ESP_ERR_INVALID_STATE);
        goto done;
    }
    if (event->type == SESSION_INT_MEDIA_ERROR) {
        session_finish(&s_context, event->first, event->status);
        goto done;
    }

    if (event_is_ai(event->type) ||
        session_ai_handles(event, &s_context)) {
        session_ai_handle(&s_context, event);
        goto done;
    }
    if (event_is_call(event->type) ||
        session_call_handles(event, &s_context)) {
        session_call_handle(&s_context, event);
        goto done;
    }
    session_emit_diagnostic(&s_context, ESP_ERR_NOT_SUPPORTED, "unhandled-session-event");

done:
    if (event_is_session_intent(event->type) &&
        event->origin_request_id != 0U) {
        publish_snapshot(&s_context);
    }
    s_context.event_origin_request_id = 0U;
}

static void reconcile_lost_events(void)
{
    uint32_t flags = (uint32_t)atomic_exchange_explicit(
        &s_reconcile_flags, 0, memory_order_acq_rel);
    if (flags == 0U) {
        return;
    }
    char reason[64];
    (void)snprintf(reason,
                   sizeof(reason),
                   "event-overflow-flags-0x%lx",
                   (unsigned long)flags);
    session_emit_diagnostic(&s_context, ESP_ERR_TIMEOUT, reason);
    if (s_context.owner != DEVICE_SERVICE_NONE) {
        session_finish(&s_context, "event-overflow-reconcile", ESP_ERR_TIMEOUT);
    }
}

static void session_task(void *argument)
{
    (void)argument;
    memset(&s_context, 0, sizeof(s_context));
    s_context.owner = DEVICE_SERVICE_NONE;
    s_context.state = DEVICE_SESSION_IDLE;
    s_context.generation = 1U;
    publish_snapshot(&s_context);
    session_set_state(&s_context, DEVICE_SESSION_IDLE, "started", 0);

    int64_t next_room_poll_ms = session_now_ms() + 2000;
    for (;;) {
        session_internal_event_t *event = NULL;
        if (xQueueReceive(s_queue,
                          &event,
                          pdMS_TO_TICKS(SESSION_TASK_PERIOD_MS)) == pdTRUE) {
            handle_internal_event(event);
            event_release(event);
        }

        reconcile_lost_events();
        int64_t current_ms = session_now_ms();
        s_context.event_origin_request_id =
            s_context.session_origin_request_id;
        session_ai_tick(&s_context, current_ms);
        session_call_tick(&s_context, current_ms);
        session_ai_call_tick(&s_context, current_ms);
        s_context.event_origin_request_id = 0U;

        tirtc_adapter_metrics_t adapter_metrics = {0};
        tirtc_adapter_get_metrics(&adapter_metrics);
        bool adapter_quiescent =
            adapter_metrics.adapter_state == TIRTC_ADAPTER_RUNNING &&
            !adapter_metrics.connected &&
            adapter_metrics.active_profile == TIRTC_ADAPTER_MEDIA_NONE &&
            !adapter_metrics.connect_request_pending &&
            !adapter_metrics.connect_callback_pending &&
            adapter_metrics.accept_callbacks_pending == 0U &&
            adapter_metrics.disconnects_pending == 0U &&
            adapter_metrics.connection_users == 0U &&
            !adapter_metrics.incoming_armed;
        if (s_context.owner == DEVICE_SERVICE_NONE &&
            !session_ai_call_reserved(&s_context) &&
            adapter_quiescent &&
            current_ms >= next_room_poll_ms && platform_client_ready()) {
            next_room_poll_ms = current_ms + SESSION_ROOM_POLL_INTERVAL_MS;
            bool expected = false;
            if (atomic_compare_exchange_strong_explicit(
                    &s_recovery_inflight,
                    &expected,
                    true,
                    memory_order_acq_rel,
                    memory_order_acquire)) {
                esp_err_t err = session_submit_http(&s_context,
                                                    SESSION_HTTP_CALL_RECOVER,
                                                    0,
                                                    "GET",
                                                    "/v1/call/room",
                                                    NULL);
                if (err != ESP_OK) {
                    atomic_store_explicit(&s_recovery_inflight,
                                          false,
                                          memory_order_release);
                }
            }
        }
    }
}

static esp_err_t enqueue_intent(session_internal_event_type_t type,
                                const char *first,
                                const char *second,
                                bool flag,
                                session_runtime_call_type_t call_type,
                                uint32_t origin_request_id)
{
    session_internal_event_t *event = event_allocate();
    if (event == NULL) {
        return ESP_ERR_NO_MEM;
    }
    event->type = type;
    event->origin_request_id = origin_request_id;
    event->flag = flag;
    event->call_type = call_type;
    if ((first != NULL &&
         !session_copy_string(event->first, sizeof(event->first), first)) ||
        (second != NULL &&
         !session_copy_string(event->second, sizeof(event->second), second))) {
        event_release(event);
        return ESP_ERR_INVALID_SIZE;
    }
    return event_queue(event, 0) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static device_service_t public_owner(void)
{
    device_service_t owner;
    portENTER_CRITICAL(&s_public_lock);
    owner = s_snapshot.owner;
    portEXIT_CRITICAL(&s_public_lock);
    return owner;
}

static bool public_handoff_pending(void)
{
    bool pending;
    portENTER_CRITICAL(&s_public_lock);
    pending = s_snapshot.ai_call_handoff_pending;
    portEXIT_CRITICAL(&s_public_lock);
    return pending;
}

const char *session_runtime_handoff_phase_name(
    session_runtime_handoff_phase_t phase)
{
    switch (phase) {
    case SESSION_RUNTIME_HANDOFF_NONE:
        return "none";
    case SESSION_RUNTIME_HANDOFF_RESOLVING:
        return "resolving";
    case SESSION_RUNTIME_HANDOFF_ACTION_RESPONSE:
        return "action-response";
    case SESSION_RUNTIME_HANDOFF_RESPONSE_DRAIN:
        return "response-drain";
    case SESSION_RUNTIME_HANDOFF_ADAPTER_DRAIN:
        return "adapter-drain";
    default:
        return "unknown";
    }
}

esp_err_t session_runtime_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    s_queue = xQueueCreate(SESSION_QUEUE_DEPTH,
                           sizeof(session_internal_event_t *));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const tirtc_adapter_event_handlers_t handlers = {
        .on_connection_changed = adapter_connection_callback,
        .on_command = adapter_command_callback,
        .user_data = NULL,
    };
    tirtc_adapter_set_event_handlers(&handlers);
    platform_client_set_signal_handler(platform_signal_callback, NULL);
    media_runtime_set_error_callback(media_error_callback, NULL);

    BaseType_t created = xTaskCreateWithCaps(
        session_task,
        "session",
        SESSION_TASK_STACK_BYTES,
        NULL,
        6,
        &s_task,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        media_runtime_set_error_callback(NULL, NULL);
        platform_client_set_signal_handler(NULL, NULL);
        tirtc_adapter_set_event_handlers(NULL);
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void session_runtime_set_observer(session_runtime_observer_t observer,
                                  void *user_data)
{
    portENTER_CRITICAL(&s_public_lock);
    s_observer = observer;
    s_observer_user_data = user_data;
    portEXIT_CRITICAL(&s_public_lock);
}

esp_err_t session_runtime_get_snapshot(session_runtime_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_public_lock);
    *snapshot = s_snapshot;
    portEXIT_CRITICAL(&s_public_lock);
    return ESP_OK;
}

device_session_state_t session_runtime_state(void)
{
    device_session_state_t state;
    portENTER_CRITICAL(&s_public_lock);
    state = s_snapshot.state;
    portEXIT_CRITICAL(&s_public_lock);
    return state;
}

device_service_t session_runtime_service(void)
{
    return public_owner();
}

esp_err_t session_runtime_abort(const char *reason, uint32_t origin_request_id)
{
    return enqueue_intent(SESSION_INT_ABORT,
                          reason == NULL ? "network-or-identity-abort" : reason,
                          NULL,
                          false,
                          SESSION_RUNTIME_CALL_AUDIO,
                          origin_request_id);
}

esp_err_t session_runtime_ai_start(uint32_t origin_request_id)
{
    if (public_owner() != DEVICE_SERVICE_NONE ||
        public_handoff_pending()) {
        return ESP_ERR_INVALID_STATE;
    }
    return enqueue_intent(SESSION_INT_AI_START, NULL, NULL, false,
                          SESSION_RUNTIME_CALL_AUDIO, origin_request_id);
}

esp_err_t session_runtime_ai_interrupt(uint32_t origin_request_id)
{
    if (public_owner() != DEVICE_SERVICE_AI ||
        public_handoff_pending()) {
        return ESP_ERR_INVALID_STATE;
    }
    return enqueue_intent(SESSION_INT_AI_INTERRUPT, NULL, NULL, false,
                          SESSION_RUNTIME_CALL_AUDIO, origin_request_id);
}

esp_err_t session_runtime_ai_submit_speech(uint32_t origin_request_id)
{
    if (public_owner() != DEVICE_SERVICE_AI ||
        public_handoff_pending()) {
        return ESP_ERR_INVALID_STATE;
    }
    return enqueue_intent(SESSION_INT_AI_SUBMIT, NULL, NULL, false,
                          SESSION_RUNTIME_CALL_AUDIO, origin_request_id);
}

esp_err_t session_runtime_ai_update_config(const char *extra_params_json,
                                           uint32_t origin_request_id)
{
    if (public_owner() != DEVICE_SERVICE_AI ||
        public_handoff_pending() || extra_params_json == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return enqueue_intent(SESSION_INT_AI_UPDATE,
                          extra_params_json,
                          NULL,
                          false,
                          SESSION_RUNTIME_CALL_AUDIO,
                          origin_request_id);
}

esp_err_t session_runtime_ai_action_result(const char *action_id,
                                           bool ok,
                                           const char *result_json_or_message,
                                           uint32_t origin_request_id)
{
    if (public_owner() != DEVICE_SERVICE_AI ||
        public_handoff_pending() || action_id == NULL ||
        action_id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    return enqueue_intent(SESSION_INT_AI_ACTION_RESULT,
                          action_id,
                          result_json_or_message == NULL ? "" : result_json_or_message,
                          ok,
                          SESSION_RUNTIME_CALL_AUDIO,
                          origin_request_id);
}

esp_err_t session_runtime_ai_stop(uint32_t origin_request_id)
{
    if (public_owner() != DEVICE_SERVICE_AI ||
        public_handoff_pending()) {
        return ESP_ERR_INVALID_STATE;
    }
    return enqueue_intent(SESSION_INT_AI_STOP, NULL, NULL, false,
                          SESSION_RUNTIME_CALL_AUDIO, origin_request_id);
}

esp_err_t session_runtime_call_start(const char *target_device_id,
                                     session_runtime_call_type_t call_type,
                                     uint32_t origin_request_id)
{
    if (public_owner() != DEVICE_SERVICE_NONE ||
        public_handoff_pending() || target_device_id == NULL ||
        target_device_id[0] == '\0' ||
        (call_type != SESSION_RUNTIME_CALL_AUDIO &&
         call_type != SESSION_RUNTIME_CALL_VIDEO)) {
        return ESP_ERR_INVALID_STATE;
    }
    return enqueue_intent(SESSION_INT_CALL_START,
                          target_device_id,
                          NULL,
                          false,
                          call_type,
                          origin_request_id);
}

esp_err_t session_runtime_call_accept(uint32_t origin_request_id)
{
    return enqueue_intent(SESSION_INT_CALL_ACCEPT, NULL, NULL, false,
                          SESSION_RUNTIME_CALL_AUDIO, origin_request_id);
}

esp_err_t session_runtime_call_reject(uint32_t origin_request_id)
{
    return enqueue_intent(SESSION_INT_CALL_REJECT, NULL, NULL, false,
                          SESSION_RUNTIME_CALL_AUDIO, origin_request_id);
}

esp_err_t session_runtime_call_cancel(uint32_t origin_request_id)
{
    return enqueue_intent(SESSION_INT_CALL_CANCEL, NULL, NULL, false,
                          SESSION_RUNTIME_CALL_AUDIO, origin_request_id);
}

esp_err_t session_runtime_call_hangup(uint32_t origin_request_id)
{
    return enqueue_intent(SESSION_INT_CALL_HANGUP, NULL, NULL, false,
                          SESSION_RUNTIME_CALL_AUDIO, origin_request_id);
}

esp_err_t session_runtime_call_recover(uint32_t origin_request_id)
{
    if (public_owner() != DEVICE_SERVICE_NONE ||
        public_handoff_pending()) {
        return ESP_ERR_INVALID_STATE;
    }
    return enqueue_intent(SESSION_INT_CALL_RECOVER, NULL, NULL, false,
                          SESSION_RUNTIME_CALL_AUDIO, origin_request_id);
}

esp_err_t session_runtime_contacts_list(uint32_t origin_request_id)
{
    return enqueue_intent(SESSION_INT_CONTACTS_LIST, NULL, NULL, false,
                          SESSION_RUNTIME_CALL_AUDIO, origin_request_id);
}

esp_err_t session_runtime_contacts_pending(uint32_t origin_request_id)
{
    return enqueue_intent(SESSION_INT_CONTACTS_PENDING, NULL, NULL, false,
                          SESSION_RUNTIME_CALL_AUDIO, origin_request_id);
}

esp_err_t session_runtime_contacts_request(const char *target_device_id,
                                           uint32_t origin_request_id)
{
    if (target_device_id == NULL || target_device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return enqueue_intent(SESSION_INT_CONTACTS_REQUEST,
                          target_device_id,
                          NULL,
                          false,
                          SESSION_RUNTIME_CALL_AUDIO,
                          origin_request_id);
}

esp_err_t session_runtime_contacts_respond(const char *peer_device_id,
                                           bool accept,
                                           uint32_t origin_request_id)
{
    if (peer_device_id == NULL || peer_device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return enqueue_intent(SESSION_INT_CONTACTS_RESPOND,
                          peer_device_id,
                          NULL,
                          accept,
                          SESSION_RUNTIME_CALL_AUDIO,
                          origin_request_id);
}

esp_err_t session_runtime_contacts_remark(const char *peer_device_id,
                                          const char *remark,
                                          uint32_t origin_request_id)
{
    if (peer_device_id == NULL || peer_device_id[0] == '\0' || remark == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return enqueue_intent(SESSION_INT_CONTACTS_REMARK,
                          peer_device_id,
                          remark,
                          false,
                          SESSION_RUNTIME_CALL_AUDIO,
                          origin_request_id);
}

esp_err_t session_runtime_contacts_delete(const char *peer_device_id,
                                          uint32_t origin_request_id)
{
    if (peer_device_id == NULL || peer_device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return enqueue_intent(SESSION_INT_CONTACTS_DELETE,
                          peer_device_id,
                          NULL,
                          false,
                          SESSION_RUNTIME_CALL_AUDIO,
                          origin_request_id);
}
