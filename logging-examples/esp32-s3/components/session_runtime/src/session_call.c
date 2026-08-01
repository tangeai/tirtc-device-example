#include "session_runtime_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device/device_utf8.h"
#include "esp_err.h"
#include "media_runtime.h"
#include "platform_client.h"

#define CALL_HTTP_TIMEOUT_MS 10000
#define CALL_OUTGOING_TIMEOUT_MS 30000
#define CALL_INCOMING_TIMEOUT_MS 45000
#define CALL_CONNECT_TIMEOUT_MS 10000
#define CALL_CONFIRM_TIMEOUT_MS 10000

static cJSON *parse_json_exact(const char *payload, size_t length)
{
    if (payload == NULL) {
        return NULL;
    }
    const char *parse_end = NULL;
    cJSON *root =
        cJSON_ParseWithLengthOpts(payload, length + 1U, &parse_end, true);
    if (parse_end != payload + length) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static const char *call_type_name(session_runtime_call_type_t type)
{
    return type == SESSION_RUNTIME_CALL_AUDIO ? "audio" : "video";
}

static bool safe_device_id(const char *value)
{
    if (value == NULL || value[0] == '\0' ||
        strlen(value) >= SESSION_RUNTIME_ID_MAX) {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor != '\0';
         ++cursor) {
        if (!isalnum(*cursor) && *cursor != '-' && *cursor != '_' &&
            *cursor != '.' && *cursor != ':') {
            return false;
        }
    }
    return true;
}

static void clear_device_contact_cache(session_context_t *context,
                                       bool pending)
{
    if (pending) {
        memset(context->pending_device_contacts,
               0,
               sizeof(context->pending_device_contacts));
        context->pending_device_contact_count = 0U;
    } else {
        memset(context->device_contacts, 0, sizeof(context->device_contacts));
        context->device_contact_count = 0U;
        context->device_contact_cache_complete = false;
    }
}

static void cache_pending_device_contact(session_context_t *context,
                                         const char *device_id)
{
    if (!safe_device_id(device_id)) {
        return;
    }
    for (uint8_t index = 0U;
         index < context->pending_device_contact_count;
         ++index) {
        if (strcmp(context->pending_device_contacts[index], device_id) == 0) {
            return;
        }
    }
    if (context->pending_device_contact_count >=
        SESSION_DEVICE_CONTACT_CACHE_MAX) {
        return;
    }
    uint8_t index = context->pending_device_contact_count;
    (void)session_copy_string(context->pending_device_contacts[index],
                              sizeof(context->pending_device_contacts[index]),
                              device_id);
    context->pending_device_contact_count++;
}

static void copy_optional_contact_string(const cJSON *item,
                                         const char *name,
                                         char *destination,
                                         size_t destination_size)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(item, name);
    if (cJSON_IsString(value) && value->valuestring != NULL) {
        (void)session_copy_string(destination,
                                  destination_size,
                                  value->valuestring);
    }
}

static void cache_device_contact(session_context_t *context,
                                 const cJSON *item)
{
    if (context->device_contact_count >=
        SESSION_DEVICE_CONTACT_CACHE_MAX) {
        return;
    }
    session_device_contact_t *entry =
        &context->device_contacts[context->device_contact_count];
    if (!session_json_string(item,
                             "device_id",
                             entry->device_id,
                             sizeof(entry->device_id))) {
        return;
    }
    copy_optional_contact_string(item,
                                 "remark",
                                 entry->remark,
                                 sizeof(entry->remark));
    copy_optional_contact_string(item,
                                 "device_name",
                                 entry->device_name,
                                 sizeof(entry->device_name));
    if (entry->device_name[0] == '\0') {
        copy_optional_contact_string(item,
                                     "name",
                                     entry->device_name,
                                     sizeof(entry->device_name));
    }
    entry->online =
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "online"));
    context->device_contact_count++;
}

static bool cached_device_contact(const session_context_t *context,
                                  bool pending,
                                  const char *device_id)
{
    if (!safe_device_id(device_id)) {
        return false;
    }
    uint8_t count = pending ? context->pending_device_contact_count
                            : context->device_contact_count;
    for (uint8_t index = 0U; index < count; ++index) {
        const char *cached_id =
            pending ? context->pending_device_contacts[index]
                    : context->device_contacts[index].device_id;
        if (strcmp(cached_id, device_id) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t submit_json(session_context_t *context,
                             session_http_request_t request,
                             uint32_t generation,
                             const char *method,
                             const char *path,
                             cJSON *root)
{
    char *body = root == NULL ? NULL : cJSON_PrintUnformatted(root);
    if (root != NULL && body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = session_submit_http(context,
                                        request,
                                        generation,
                                        method,
                                        path,
                                        body);
    free(body);
    return err;
}

static esp_err_t submit_room_action_for(session_context_t *context,
                                        session_http_request_t request,
                                        uint32_t generation,
                                        const char *path,
                                        const char *room_id,
                                        const char *reason)
{
    if (room_id == NULL || room_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    bool ok = root != NULL &&
              cJSON_AddStringToObject(root, "room_id", room_id) != NULL &&
              (reason == NULL ||
               cJSON_AddStringToObject(root, "reason", reason) != NULL);
    esp_err_t err = ok ? submit_json(context,
                                     request,
                                     generation,
                                     "POST",
                                     path,
                                     root)
                       : ESP_ERR_NO_MEM;
    cJSON_Delete(root);
    return err;
}

static esp_err_t submit_room_action(session_context_t *context,
                                    session_http_request_t request,
                                    const char *path,
                                    const char *reason)
{
    return submit_room_action_for(context,
                                  request,
                                  context->generation,
                                  path,
                                  context->call.room_id,
                                  reason);
}

static void emit_call_event(session_context_t *context,
                            const char *name,
                            int status,
                            const char *text,
                            const char *json)
{
    session_runtime_event_t event = {
        .type = SESSION_RUNTIME_EVENT_CALL_EVENT,
        .status = status,
    };
    (void)session_copy_string(event.name, sizeof(event.name), name);
    (void)session_copy_string(event.room_id,
                              sizeof(event.room_id),
                              context->call.room_id);
    (void)session_copy_string(event.peer_id,
                              sizeof(event.peer_id),
                              context->call.peer_id);
    (void)session_copy_string(event.text,
                              sizeof(event.text),
                              text == NULL ? "" : text);
    (void)session_copy_json(event.json,
                            sizeof(event.json),
                            json == NULL ? "" : json);
    session_emit(context, &event);
}

static void emit_incoming(session_context_t *context, const char *reason)
{
    session_runtime_event_t event = {
        .type = SESSION_RUNTIME_EVENT_CALL_INCOMING,
        .status = 0,
    };
    (void)session_copy_string(event.name,
                              sizeof(event.name),
                              call_type_name(context->call.call_type));
    (void)session_copy_string(event.room_id,
                              sizeof(event.room_id),
                              context->call.room_id);
    (void)session_copy_string(event.peer_id,
                              sizeof(event.peer_id),
                              context->call.peer_id);
    (void)session_copy_string(event.text,
                              sizeof(event.text),
                              context->call.peer_name[0] == '\0'
                                  ? reason
                                  : context->call.peer_name);
    session_emit(context, &event);
}

static int send_call_command(session_context_t *context,
                             uint32_t command,
                             const char *json)
{
    return tirtc_adapter_send_command(context->generation,
                                      command,
                                      json,
                                      json == NULL ? 0U : (uint32_t)strlen(json));
}

static int send_room_confirmation(session_context_t *context)
{
    cJSON *root = cJSON_CreateObject();
    bool ok = root != NULL &&
              cJSON_AddStringToObject(root,
                                     "room_id",
                                     context->call.room_id) != NULL;
    char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int rc = send_call_command(context, SESSION_COMMAND_CALL_CONFIRM, json);
    free(json);
    return rc;
}

static void finish_call_with_platform(session_context_t *context,
                                      const char *reason,
                                      int status)
{
    if (context->call.room_id[0] != '\0') {
        (void)submit_room_action(context,
                                 SESSION_HTTP_CALL_HANGUP,
                                 "/v1/call/hangup",
                                 reason);
    }
    session_finish(context, reason, status);
}

static bool activate_call_media(session_context_t *context,
                                const char *reason)
{
    bool video = context->call.call_type == SESSION_RUNTIME_CALL_VIDEO;
    int rc = tirtc_adapter_subscribe_downlink(context->generation,
                                              true,
                                              video);
    if (rc != 0) {
        finish_call_with_platform(context,
                                  "call-downlink-subscribe-failed",
                                  rc);
        return false;
    }
    context->call.phase = SESSION_CALL_PHASE_ACTIVE;
    context->deadline_ms = 0;
    session_secure_zero(context->call.connect_token,
                        sizeof(context->call.connect_token));
    media_runtime_set_uplink_active(true);
    session_set_state(context,
                      DEVICE_SESSION_IN_CALL,
                      reason == NULL ? "call-active" : reason,
                      0);
    return true;
}

static void connect_once(session_context_t *context)
{
    if (context->owner != DEVICE_SERVICE_CALL ||
        context->call.connect_token[0] == '\0' ||
        context->call.peer_id[0] == '\0') {
        finish_call_with_platform(context,
                                  "call-connect-missing-token-or-peer",
                                  ESP_ERR_INVALID_STATE);
        return;
    }
    context->call.phase = SESSION_CALL_PHASE_CONNECTING;
    context->deadline_ms = session_now_ms() + CALL_CONNECT_TIMEOUT_MS;
    session_set_state(context,
                      DEVICE_SESSION_CALL_CONNECTING,
                      "call-p2p-connecting",
                      0);
    int rc = tirtc_adapter_connect(context->call.peer_id,
                                   context->call.connect_token,
                                   context->generation);
    if (rc != 0) {
        finish_call_with_platform(context, "call-connect-submit-failed", rc);
    } else {
        session_set_state(context,
                          DEVICE_SESSION_CALL_CONNECTING,
                          "call-p2p-connect-submitted",
                          0);
    }
}

static bool handle_call_start(session_context_t *context,
                              const session_internal_event_t *event,
                              bool from_ai_handoff)
{
    if (context->owner != DEVICE_SERVICE_NONE ||
        (session_ai_call_reserved(context) && !from_ai_handoff)) {
        session_emit_operation(context,
                               "call-start",
                               ESP_ERR_INVALID_STATE,
                               "busy",
                               NULL);
        return false;
    }
    if (!safe_device_id(event->first) || !platform_client_ready() ||
        tirtc_adapter_state() != TIRTC_ADAPTER_RUNNING) {
        session_emit_operation(context,
                               "call-start",
                               ESP_ERR_INVALID_STATE,
                               "invalid-target-or-platform-not-ready",
                               NULL);
        return false;
    }
    bool video = event->call_type == SESSION_RUNTIME_CALL_VIDEO;
    if (!session_claim(context,
                       DEVICE_SERVICE_CALL,
                       DEVICE_SESSION_CALLING,
                       TIRTC_ADAPTER_MEDIA_CALL,
                       video,
                       "call-request")) {
        session_emit_operation(context,
                               "call-start",
                               ESP_ERR_INVALID_STATE,
                               "media-profile-busy-or-unsupported",
                               NULL);
        return false;
    }
    context->call.phase = SESSION_CALL_PHASE_REQUEST;
    context->call.caller = true;
    context->call.call_type = event->call_type;
    (void)session_copy_string(context->call.target_id,
                              sizeof(context->call.target_id),
                              event->first);
    (void)session_copy_string(context->call.peer_id,
                              sizeof(context->call.peer_id),
                              event->first);
    int incoming_rc =
        tirtc_adapter_expect_incoming(context->generation);
    if (incoming_rc != 0) {
        session_emit_operation(context,
                               "call-start",
                               incoming_rc,
                               "incoming-connection-arm-failed",
                               NULL);
        session_finish(context,
                       "call-incoming-arm-failed",
                       incoming_rc);
        return false;
    }
    context->deadline_ms = session_now_ms() + CALL_HTTP_TIMEOUT_MS;

    cJSON *root = cJSON_CreateObject();
    cJSON *targets = cJSON_CreateArray();
    bool ok = root != NULL && targets != NULL &&
              cJSON_AddItemToArray(targets,
                                  cJSON_CreateString(event->first)) &&
              cJSON_AddStringToObject(root,
                                     "call_type",
                                     call_type_name(event->call_type)) != NULL;
    if (ok) {
        cJSON_AddItemToObject(root, "targets", targets);
        targets = NULL;
    }
    esp_err_t err = ok ? submit_json(context,
                                     SESSION_HTTP_CALL_REQUEST,
                                     context->generation,
                                     "POST",
                                     "/v1/call/request",
                                     root)
                       : ESP_ERR_NO_MEM;
    cJSON_Delete(targets);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        session_finish(context, "call-request-submit-failed", err);
        return false;
    } else {
        session_emit_operation(context, "call-start", 0, "accepted", NULL);
    }
    return true;
}

bool session_call_start_handoff(session_context_t *context,
                                const char *target_device_id)
{
    if (context == NULL || !session_ai_call_reserved(context) ||
        target_device_id == NULL) {
        return false;
    }
    session_internal_event_t event = {
        .type = SESSION_INT_CALL_START,
        .call_type = SESSION_RUNTIME_CALL_AUDIO,
    };
    if (!session_copy_string(event.first,
                             sizeof(event.first),
                             target_device_id)) {
        return false;
    }
    return handle_call_start(context, &event, true);
}

static void cancel_stale_request_room(session_context_t *context,
                                      const cJSON *data)
{
    char room_id[SESSION_RUNTIME_ID_MAX] = {0};
    if (cJSON_IsObject(data) &&
        session_json_string(data, "room_id", room_id, sizeof(room_id))) {
        (void)submit_room_action_for(context,
                                     SESSION_HTTP_CALL_CANCEL,
                                     0,
                                     "/v1/call/cancel",
                                     room_id,
                                     NULL);
        session_emit_diagnostic(context,
                                ESP_ERR_INVALID_STATE,
                                "cancelled-stale-call-request-room");
    }
}

static void handle_call_request_response(session_context_t *context,
                                         const session_internal_event_t *event)
{
    cJSON *root = event->truncated
                      ? NULL
                      : parse_json_exact(event->payload, event->length);
    const cJSON *data = session_json_response_data(root);
    int code = session_json_response_code(root);
    if (event->generation != context->generation ||
        context->owner != DEVICE_SERVICE_CALL ||
        context->call.phase != SESSION_CALL_PHASE_REQUEST) {
        if (code == 0 || code == 200) {
            cancel_stale_request_room(context, data);
        }
        cJSON_Delete(root);
        return;
    }
    if (code == 40202) {
        cJSON_Delete(root);
        session_emit_operation(context,
                               "call-start",
                               code,
                               "existing-room-requires-recovery",
                               event->payload);
        session_finish(context, "call-request-existing-room", code);
        return;
    }
    char room_id[SESSION_RUNTIME_ID_MAX] = {0};
    bool valid = (code == 0 || code == 200) && cJSON_IsObject(data) &&
                 session_json_string(data,
                                     "room_id",
                                     room_id,
                                     sizeof(room_id));
    cJSON_Delete(root);
    if (!valid) {
        session_finish(context, "call-request-rejected", code);
        return;
    }
    if (!session_copy_string(context->call.room_id,
                             sizeof(context->call.room_id),
                             room_id)) {
        session_finish(context, "call-room-id-too-large", ESP_ERR_INVALID_SIZE);
        return;
    }
    if (context->call.room_confirmed &&
        strcmp(context->call.confirm_room_id, room_id) != 0) {
        finish_call_with_platform(context,
                                  "call-early-confirm-room-mismatch",
                                  ESP_ERR_INVALID_RESPONSE);
        return;
    }
    if (context->call.p2p_connected && context->call.room_confirmed) {
        if (!activate_call_media(context, "call-active")) {
            return;
        }
    } else {
        context->call.phase = context->call.p2p_connected
                                  ? SESSION_CALL_PHASE_WAIT_CONFIRM
                                  : SESSION_CALL_PHASE_OUTGOING_WAIT;
        context->deadline_ms =
            session_now_ms() + (context->call.p2p_connected
                                    ? CALL_CONFIRM_TIMEOUT_MS
                                    : CALL_OUTGOING_TIMEOUT_MS);
        session_set_state(context,
                          DEVICE_SESSION_CALLING,
                          context->call.p2p_connected
                              ? "call-p2p-connected-wait-room-confirm"
                              : "call-waiting-for-callee",
                          code);
    }
    session_emit_operation(context,
                           "call-start",
                           0,
                           "room-created",
                           NULL);
}

static void handle_accept_intent(session_context_t *context)
{
    if (context->owner != DEVICE_SERVICE_CALL ||
        context->call.phase != SESSION_CALL_PHASE_RINGING ||
        context->call.room_id[0] == '\0' ||
        context->call.peer_id[0] == '\0') {
        session_emit_operation(context,
                               "call-accept",
                               ESP_ERR_INVALID_STATE,
                               "no-pending-call",
                               NULL);
        return;
    }
    context->call.phase = SESSION_CALL_PHASE_ACCEPT_TOKEN;
    context->deadline_ms = session_now_ms() + CALL_HTTP_TIMEOUT_MS;
    session_set_state(context,
                      DEVICE_SESSION_CALL_CONNECTING,
                      "call-accept-token-request",
                      0);
    cJSON *root = cJSON_CreateObject();
    bool ok = root != NULL &&
              cJSON_AddStringToObject(root,
                                     "device_id",
                                     context->call.peer_id) != NULL &&
              cJSON_AddStringToObject(root,
                                     "room_id",
                                     context->call.room_id) != NULL &&
              cJSON_AddStringToObject(root, "purpose", "call") != NULL;
    esp_err_t err = ok ? submit_json(context,
                                     SESSION_HTTP_CALL_ACCEPT,
                                     context->generation,
                                     "POST",
                                     "/v1/call/device/info",
                                     root)
                       : ESP_ERR_NO_MEM;
    cJSON_Delete(root);
    if (err != ESP_OK) {
        finish_call_with_platform(context, "call-accept-submit-failed", err);
    } else {
        session_emit_operation(context, "call-accept", 0, "accepted", NULL);
    }
}

static void handle_stale_accept_response(session_context_t *context,
                                         const session_internal_event_t *event)
{
    if (event->first[0] != '\0') {
        (void)submit_room_action_for(context,
                                     SESSION_HTTP_CALL_HANGUP,
                                     0,
                                     "/v1/call/hangup",
                                     event->first,
                                     "stale_accept");
    }
}

static void handle_accept_response(session_context_t *context,
                                   const session_internal_event_t *event)
{
    if (event->generation != context->generation ||
        context->owner != DEVICE_SERVICE_CALL ||
        context->call.phase != SESSION_CALL_PHASE_ACCEPT_TOKEN) {
        handle_stale_accept_response(context, event);
        return;
    }
    cJSON *root = event->truncated
                      ? NULL
                      : parse_json_exact(event->payload, event->length);
    const cJSON *data = session_json_response_data(root);
    int code = session_json_response_code(root);
    char token[sizeof(context->call.connect_token)] = {0};
    char peer_id[sizeof(context->call.peer_id)] = {0};
    bool valid = (code == 0 || code == 200) && cJSON_IsObject(data) &&
                 session_json_string(data, "token", token, sizeof(token)) &&
                 session_json_string(data,
                                     "device_id",
                                     peer_id,
                                     sizeof(peer_id));
    cJSON_Delete(root);
    if (!valid || !safe_device_id(peer_id)) {
        session_secure_zero(token, sizeof(token));
        session_finish(context, "call-accept-rejected", code);
        return;
    }
    (void)session_copy_string(context->call.connect_token,
                              sizeof(context->call.connect_token),
                              token);
    session_secure_zero(token, sizeof(token));
    (void)session_copy_string(context->call.peer_id,
                              sizeof(context->call.peer_id),
                              peer_id);
    connect_once(context);
}

static void handle_connection(session_context_t *context,
                              const session_internal_event_t *event)
{
    if (event->generation != context->generation ||
        context->owner != DEVICE_SERVICE_CALL) {
        return;
    }
    if (!event->connection.connected) {
        finish_call_with_platform(context,
                                  context->call.phase == SESSION_CALL_PHASE_ACTIVE
                                      ? "call-peer-disconnected"
                                      : "call-connect-failed",
                                  event->connection.error);
        return;
    }

    context->call.p2p_connected = true;
    if (context->call.caller &&
        context->call.phase == SESSION_CALL_PHASE_REQUEST) {
        session_set_state(context,
                          DEVICE_SESSION_CALLING,
                          "call-p2p-connected-wait-room-response",
                          0);
        return;
    }
    if (context->call.caller &&
        (context->call.phase == SESSION_CALL_PHASE_OUTGOING_WAIT ||
         context->call.phase == SESSION_CALL_PHASE_WAIT_CONFIRM)) {
        if (context->call.room_confirmed) {
            (void)activate_call_media(context, "call-active");
            return;
        }
        context->call.phase = SESSION_CALL_PHASE_WAIT_CONFIRM;
        context->deadline_ms = session_now_ms() + CALL_CONFIRM_TIMEOUT_MS;
        session_set_state(context,
                          DEVICE_SESSION_CALLING,
                          "call-p2p-connected-wait-room-confirm",
                          0);
        return;
    }
    if (!context->call.caller &&
        context->call.phase == SESSION_CALL_PHASE_CONNECTING) {
        int rc = send_room_confirmation(context);
        if (rc != 0) {
            finish_call_with_platform(context,
                                      "call-room-confirm-send-failed",
                                      rc);
            return;
        }
        context->call.room_confirmed = true;
        (void)activate_call_media(context, "call-active");
        return;
    }
    finish_call_with_platform(context,
                              "call-unexpected-connection",
                              ESP_ERR_INVALID_STATE);
}

static void handle_call_command(session_context_t *context,
                                const session_internal_event_t *event)
{
    if (event->generation != context->generation ||
        context->owner != DEVICE_SERVICE_CALL) {
        return;
    }
    if (event->command == SESSION_COMMAND_CALL_HANGUP) {
        session_finish(context, "call-remote-hangup", 0);
        return;
    }
    if (event->command != SESSION_COMMAND_CALL_CONFIRM || event->truncated) {
        return;
    }
    cJSON *root = parse_json_exact(event->payload, event->length);
    char room_id[SESSION_RUNTIME_ID_MAX] = {0};
    bool valid = cJSON_IsObject(root) &&
                 session_json_string(root,
                                     "room_id",
                                     room_id,
                                     sizeof(room_id));
    cJSON_Delete(root);
    if (!valid) {
        finish_call_with_platform(context,
                                  "call-room-confirm-invalid",
                                  ESP_ERR_INVALID_RESPONSE);
        return;
    }
    if (context->call.caller &&
        context->call.phase == SESSION_CALL_PHASE_REQUEST) {
        if (!session_copy_string(context->call.confirm_room_id,
                                 sizeof(context->call.confirm_room_id),
                                 room_id)) {
            finish_call_with_platform(context,
                                      "call-confirm-room-id-too-large",
                                      ESP_ERR_INVALID_SIZE);
            return;
        }
        context->call.room_confirmed = true;
        session_set_state(context,
                          DEVICE_SESSION_CALLING,
                          "call-room-confirmed-wait-room-response",
                          0);
        return;
    }
    if (strcmp(room_id, context->call.room_id) != 0) {
        finish_call_with_platform(context,
                                  "call-room-confirm-mismatch",
                                  ESP_ERR_INVALID_RESPONSE);
        return;
    }
    context->call.room_confirmed = true;
    if (context->call.caller &&
        context->call.phase == SESSION_CALL_PHASE_WAIT_CONFIRM) {
        (void)activate_call_media(context, "call-active");
    }
}

static void handle_reject_intent(session_context_t *context)
{
    if (context->owner != DEVICE_SERVICE_CALL ||
        context->call.phase != SESSION_CALL_PHASE_RINGING) {
        session_emit_operation(context,
                               "call-reject",
                               ESP_ERR_INVALID_STATE,
                               "no-pending-call",
                               NULL);
        return;
    }
    esp_err_t err = submit_room_action(context,
                                       SESSION_HTTP_CALL_REJECT,
                                       "/v1/call/reject",
                                       "decline");
    session_emit_operation(context,
                           "call-reject",
                           err,
                           err == ESP_OK ? "submitted" : "submit-failed",
                           NULL);
    session_finish(context, "call-local-reject", err);
}

static void handle_cancel_intent(session_context_t *context)
{
    if (context->owner != DEVICE_SERVICE_CALL || !context->call.caller ||
        (context->call.phase != SESSION_CALL_PHASE_REQUEST &&
         context->call.phase != SESSION_CALL_PHASE_OUTGOING_WAIT &&
         context->call.phase != SESSION_CALL_PHASE_WAIT_CONFIRM)) {
        session_emit_operation(context,
                               "call-cancel",
                               ESP_ERR_INVALID_STATE,
                               "no-outgoing-call",
                               NULL);
        return;
    }
    esp_err_t err = ESP_OK;
    if (context->call.room_id[0] != '\0') {
        err = submit_room_action(context,
                                 SESSION_HTTP_CALL_CANCEL,
                                 "/v1/call/cancel",
                                 NULL);
    }
    session_emit_operation(context,
                           "call-cancel",
                           err,
                           err == ESP_OK ? "submitted" : "submit-failed",
                           NULL);
    session_finish(context, "call-local-cancel", err);
}

static void handle_hangup_intent(session_context_t *context)
{
    if (context->owner != DEVICE_SERVICE_CALL ||
        context->call.phase == SESSION_CALL_PHASE_RINGING ||
        context->call.phase == SESSION_CALL_PHASE_REQUEST) {
        session_emit_operation(context,
                               "call-hangup",
                               ESP_ERR_INVALID_STATE,
                               "call-not-connected",
                               NULL);
        return;
    }
    if (tirtc_adapter_has_connection()) {
        (void)send_call_command(context,
                                SESSION_COMMAND_CALL_HANGUP,
                                "{\"reason\":0}");
    }
    esp_err_t err = submit_room_action(context,
                                       SESSION_HTTP_CALL_HANGUP,
                                       "/v1/call/hangup",
                                       "hangup");
    session_emit_operation(context,
                           "call-hangup",
                           err,
                           err == ESP_OK ? "submitted" : "submit-failed",
                           NULL);
    session_finish(context, "call-local-hangup", err);
}

static void submit_busy_reject(session_context_t *context,
                               const char *room_id)
{
    esp_err_t err = submit_room_action_for(context,
                                           SESSION_HTTP_CALL_BUSY_REJECT,
                                           context->generation,
                                           "/v1/call/reject",
                                           room_id,
                                           "busy");
    emit_call_event(context,
                    "incoming-busy",
                    err,
                    room_id,
                    NULL);
}

static bool parse_call_type(const cJSON *payload,
                            session_runtime_call_type_t *call_type)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(payload, "call_type");
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        return false;
    }
    if (strcmp(type->valuestring, "audio") == 0) {
        *call_type = SESSION_RUNTIME_CALL_AUDIO;
        return true;
    }
    if (strcmp(type->valuestring, "video") == 0) {
        *call_type = SESSION_RUNTIME_CALL_VIDEO;
        return true;
    }
    return false;
}

static void handle_incoming_signal(session_context_t *context,
                                   const cJSON *payload)
{
    char room_id[SESSION_RUNTIME_ID_MAX] = {0};
    char caller_id[SESSION_RUNTIME_ID_MAX] = {0};
    char caller_name[SESSION_RUNTIME_ID_MAX] = {0};
    bool valid = session_json_string(payload,
                                     "room_id",
                                     room_id,
                                     sizeof(room_id)) &&
                 session_json_string(payload,
                                     "caller_id",
                                     caller_id,
                                     sizeof(caller_id));
    (void)session_json_string(payload,
                              "caller_name",
                              caller_name,
                              sizeof(caller_name));
    if (!valid || !safe_device_id(caller_id)) {
        session_emit_diagnostic(context,
                                ESP_ERR_INVALID_RESPONSE,
                                "invalid-call-incoming-signal");
        return;
    }
    if (context->owner == DEVICE_SERVICE_CALL &&
        strcmp(context->call.room_id, room_id) == 0) {
        emit_call_event(context, "incoming-duplicate", 0, NULL, NULL);
        return;
    }
    if (context->owner != DEVICE_SERVICE_NONE ||
        session_ai_call_reserved(context)) {
        submit_busy_reject(context, room_id);
        return;
    }

    session_runtime_call_type_t type;
    if (!parse_call_type(payload, &type)) {
        session_emit_diagnostic(context,
                                ESP_ERR_INVALID_RESPONSE,
                                "invalid-call-type");
        return;
    }
    bool video = type == SESSION_RUNTIME_CALL_VIDEO;
    if (!session_claim(context,
                       DEVICE_SERVICE_CALL,
                       DEVICE_SESSION_RINGING,
                       TIRTC_ADAPTER_MEDIA_CALL,
                       video,
                       "call-incoming")) {
        submit_busy_reject(context, room_id);
        return;
    }
    context->call.phase = SESSION_CALL_PHASE_RINGING;
    context->call.caller = false;
    context->call.call_type = type;
    (void)session_copy_string(context->call.room_id,
                              sizeof(context->call.room_id),
                              room_id);
    (void)session_copy_string(context->call.peer_id,
                              sizeof(context->call.peer_id),
                              caller_id);
    (void)session_copy_string(context->call.peer_name,
                              sizeof(context->call.peer_name),
                              caller_name);
    context->deadline_ms = session_now_ms() + CALL_INCOMING_TIMEOUT_MS;
    session_set_state(context, DEVICE_SESSION_RINGING, "call-incoming", 0);
    emit_incoming(context, "incoming");
}

static void handle_platform_signal(session_context_t *context,
                                   const session_internal_event_t *event)
{
    if (event->truncated) {
        if (context->owner != DEVICE_SERVICE_NONE) {
            session_finish(context,
                           "platform-signal-too-large",
                           ESP_ERR_INVALID_SIZE);
        }
        return;
    }
    cJSON *root = parse_json_exact(event->payload, event->length);
    const cJSON *type = root == NULL
                            ? NULL
                            : cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *channel = root == NULL
                               ? NULL
                               : cJSON_GetObjectItemCaseSensitive(root, "channel");
    const cJSON *payload = root == NULL
                               ? NULL
                               : cJSON_GetObjectItemCaseSensitive(root, "payload");
    const char *type_name = cJSON_IsString(type) ? type->valuestring : "";
    const char *channel_name = cJSON_IsString(channel) ? channel->valuestring : "";
    if (strcmp(type_name, "unbind") == 0) {
        cJSON_Delete(root);
        session_finish(context, "platform-unbind", ESP_ERR_INVALID_STATE);
        return;
    }
    if (strcmp(channel_name, "device") != 0 || !cJSON_IsObject(payload)) {
        cJSON_Delete(root);
        return;
    }
    if (strcmp(type_name, "call_incoming") == 0) {
        handle_incoming_signal(context, payload);
    } else {
        char room_id[SESSION_RUNTIME_ID_MAX] = {0};
        (void)session_json_string(payload,
                                  "room_id",
                                  room_id,
                                  sizeof(room_id));
        bool matches = context->owner == DEVICE_SERVICE_CALL &&
                       context->call.room_id[0] != '\0' &&
                       strcmp(context->call.room_id, room_id) == 0;
        if (strcmp(type_name, "room_cancel") == 0 && matches) {
            char *json = cJSON_PrintUnformatted(payload);
            emit_call_event(context,
                            "room-cancel",
                            0,
                            NULL,
                            json);
            free(json);
            cJSON_Delete(root);
            session_finish(context, "call-room-cancel", 0);
            return;
        }
        if (strcmp(type_name, "call_reject") == 0 && matches) {
            char *json = cJSON_PrintUnformatted(payload);
            emit_call_event(context, "call-reject", 0, NULL, json);
            free(json);
        } else if (strcmp(type_name, "callee_answered") == 0 && matches) {
            char *json = cJSON_PrintUnformatted(payload);
            emit_call_event(context, "callee-answered", 0, NULL, json);
            free(json);
        } else if (strcmp(type_name, "callers_update") == 0) {
            char *json = cJSON_PrintUnformatted(payload);
            emit_call_event(context, "callers-update", 0, NULL, json);
            free(json);
            esp_err_t contacts_err = session_submit_http(
                context,
                SESSION_HTTP_CONTACTS_LIST,
                0,
                "GET",
                "/v1/call/device/contacts",
                NULL);
            esp_err_t pending_err = session_submit_http(
                context,
                SESSION_HTTP_CONTACTS_PENDING,
                0,
                "GET",
                "/v1/call/device/contacts/pending",
                NULL);
            if (contacts_err != ESP_OK || pending_err != ESP_OK) {
                session_emit_operation(
                    context,
                    "callers-refresh",
                    contacts_err != ESP_OK ? contacts_err : pending_err,
                    "automatic-refresh-submit-failed",
                    NULL);
            }
        }
    }
    cJSON_Delete(root);
}

static void emit_contact_item(session_context_t *context,
                              const char *kind,
                              uint32_t index,
                              const cJSON *item)
{
    session_runtime_event_t event = {
        .type = SESSION_RUNTIME_EVENT_CONTACT,
        .index = index,
    };
    (void)session_copy_string(event.name, sizeof(event.name), kind);
    const cJSON *online = cJSON_GetObjectItemCaseSensitive(item, "online");
    event.online = cJSON_IsTrue(online);
    if (!session_json_string(item,
                             strcmp(kind, "pending") == 0
                                 ? "peer_device_id"
                                 : "device_id",
                             event.peer_id,
                             sizeof(event.peer_id))) {
        return;
    }
    if (strcmp(kind, "pending") == 0) {
        (void)session_json_string(item,
                                  "created_at",
                                  event.text,
                                  sizeof(event.text));
    } else {
        (void)session_json_string(item,
                                  "remark",
                                  event.text,
                                  sizeof(event.text));
    }
    char *json = cJSON_PrintUnformatted(item);
    if (json != NULL) {
        (void)session_copy_json(event.json, sizeof(event.json), json);
        free(json);
    }
    session_emit(context, &event);
}

static bool optional_contact_string_valid(const cJSON *item,
                                          const char *name,
                                          size_t capacity)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(item, name);
    if (value == NULL || cJSON_IsNull(value)) {
        return true;
    }
    if (!cJSON_IsString(value) || value->valuestring == NULL ||
        strlen(value->valuestring) >= capacity ||
        !device_utf8_validate(value->valuestring)) {
        return false;
    }
    char sanitized[SESSION_CONTACT_LABEL_MAX];
    (void)device_utf8_sanitize_line(value->valuestring,
                                    sanitized,
                                    sizeof(sanitized),
                                    capacity - 1U);
    return strcmp(value->valuestring, sanitized) == 0;
}

static bool device_contacts_valid(const cJSON *array,
                                  uint32_t *device_count)
{
    uint32_t count = 0U;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsObject(item)) {
            return false;
        }
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        if (!cJSON_IsString(type) || type->valuestring == NULL) {
            return false;
        }
        if (strcmp(type->valuestring, "device") != 0) {
            continue;
        }
        const cJSON *id =
            cJSON_GetObjectItemCaseSensitive(item, "device_id");
        const cJSON *online =
            cJSON_GetObjectItemCaseSensitive(item, "online");
        if (!cJSON_IsString(id) || id->valuestring == NULL ||
            !safe_device_id(id->valuestring) || !cJSON_IsBool(online) ||
            !optional_contact_string_valid(
                item, "remark", SESSION_CONTACT_LABEL_MAX) ||
            !optional_contact_string_valid(
                item, "device_name", SESSION_CONTACT_LABEL_MAX) ||
            !optional_contact_string_valid(
                item, "name", SESSION_CONTACT_LABEL_MAX)) {
            return false;
        }
        for (const cJSON *previous = array->child;
             previous != item;
             previous = previous->next) {
            const cJSON *previous_type =
                cJSON_GetObjectItemCaseSensitive(previous, "type");
            const cJSON *previous_id =
                cJSON_GetObjectItemCaseSensitive(previous, "device_id");
            if (cJSON_IsString(previous_type) &&
                previous_type->valuestring != NULL &&
                strcmp(previous_type->valuestring, "device") == 0 &&
                cJSON_IsString(previous_id) &&
                previous_id->valuestring != NULL &&
                strcmp(previous_id->valuestring, id->valuestring) == 0) {
                return false;
            }
        }
        count++;
        if (count > SESSION_DEVICE_CONTACT_CACHE_MAX) {
            return false;
        }
    }
    if (device_count != NULL) {
        *device_count = count;
    }
    return true;
}

static void handle_contacts_response(session_context_t *context,
                                     const session_internal_event_t *event,
                                     bool pending,
                                     bool ai_call_refresh)
{
    const char *operation =
        ai_call_refresh ? "ai-call-device"
                        : (pending ? "contacts-pending" : "contacts-list");
    if (event->truncated) {
        session_emit_operation(context,
                               operation,
                               ESP_ERR_INVALID_SIZE,
                               "response-too-large",
                               NULL);
        if (ai_call_refresh) {
            session_ai_call_contacts_response(context, event, false);
        }
        return;
    }
    cJSON *root = parse_json_exact(event->payload, event->length);
    const cJSON *data = session_json_response_data(root);
    const cJSON *array = cJSON_IsObject(data)
                             ? cJSON_GetObjectItemCaseSensitive(
                                   data,
                                   pending ? "pending" : "contacts")
                             : NULL;
    int code = session_json_response_code(root);
    if ((code != 0 && code != 200) || !cJSON_IsArray(array)) {
        cJSON_Delete(root);
        session_emit_operation(context,
                               operation,
                               code,
                               "request-failed-or-invalid-response",
                               ai_call_refresh ? NULL : event->payload);
        if (ai_call_refresh) {
            session_ai_call_contacts_response(context, event, false);
        }
        return;
    }
    uint32_t device_count = 0U;
    bool valid = pending || device_contacts_valid(array, &device_count);
    if (!valid) {
        cJSON_Delete(root);
        session_emit_operation(context,
                               operation,
                               ESP_ERR_INVALID_RESPONSE,
                               "contact-snapshot-invalid-or-too-large",
                               NULL);
        if (ai_call_refresh) {
            session_ai_call_contacts_response(context, event, false);
        }
        return;
    }

    clear_device_contact_cache(context, pending);
    uint32_t emitted = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsObject(item)) {
            continue;
        }
        if (!pending) {
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
            if (!cJSON_IsString(type) || type->valuestring == NULL ||
                strcmp(type->valuestring, "device") != 0) {
                continue;
            }
        }
        if (pending) {
            char device_id[SESSION_RUNTIME_ID_MAX] = {0};
            if (session_json_string(item,
                                    "peer_device_id",
                                    device_id,
                                    sizeof(device_id))) {
                cache_pending_device_contact(context, device_id);
            }
        } else {
            cache_device_contact(context, item);
        }
        if (!ai_call_refresh) {
            emit_contact_item(context,
                              pending ? "pending" : "contact",
                              emitted,
                              item);
        }
        emitted++;
    }
    if (!pending) {
        context->device_contact_cache_complete =
            context->device_contact_count == device_count;
    }
    cJSON_Delete(root);
    if (ai_call_refresh) {
        session_ai_call_contacts_response(
            context,
            event,
            context->device_contact_cache_complete);
        return;
    }
    session_runtime_event_t done = {
        .type = SESSION_RUNTIME_EVENT_CONTACTS_DONE,
        .status = 0,
        .count = emitted,
    };
    (void)session_copy_string(done.name, sizeof(done.name), operation);
    session_emit(context, &done);
}

static void handle_mutation_response(session_context_t *context,
                                     const session_internal_event_t *event,
                                     const char *operation)
{
    cJSON *root = event->truncated
                      ? NULL
                      : parse_json_exact(event->payload, event->length);
    int code = session_json_response_code(root);
    bool ok = code == 0 || code == 200;
    cJSON_Delete(root);
    session_emit_operation(context,
                           operation,
                           ok ? 0 : code,
                           ok ? "completed" : "rejected",
                           event->payload);
}

static void handle_recovery_response(session_context_t *context,
                                     const session_internal_event_t *event)
{
    if (context->owner != DEVICE_SERVICE_NONE ||
        session_ai_call_reserved(context)) {
        return;
    }
    cJSON *root = event->truncated
                      ? NULL
                      : parse_json_exact(event->payload, event->length);
    const cJSON *data = session_json_response_data(root);
    int code = session_json_response_code(root);
    if (code != 0 && code != 200) {
        cJSON_Delete(root);
        if (event->generation != 0U) {
            session_emit_operation(context,
                                   "call-recover",
                                   code,
                                   "request-failed",
                                   event->payload);
        }
        return;
    }
    if (data == NULL || cJSON_IsNull(data)) {
        cJSON_Delete(root);
        if (event->generation != 0U) {
            session_emit_operation(context, "call-recover", 0, "no-room", NULL);
        }
        return;
    }

    char room_id[SESSION_RUNTIME_ID_MAX] = {0};
    char caller_id[SESSION_RUNTIME_ID_MAX] = {0};
    char role[17] = {0};
    char status[17] = {0};
    bool valid = cJSON_IsObject(data) &&
                 session_json_string(data,
                                     "room_id",
                                     room_id,
                                     sizeof(room_id)) &&
                 session_json_string(data, "role", role, sizeof(role)) &&
                 session_json_string(data, "status", status, sizeof(status)) &&
                 session_json_string(data,
                                     "caller",
                                     caller_id,
                                     sizeof(caller_id));
    session_runtime_call_type_t call_type;
    valid = valid && parse_call_type(data, &call_type);
    cJSON_Delete(root);
    if (!valid || (strcmp(role, "caller") != 0 &&
                   strcmp(role, "callee") != 0) ||
        (strcmp(status, "active") != 0 &&
         strcmp(status, "answered") != 0) ||
        !safe_device_id(caller_id)) {
        session_emit_operation(context,
                               "call-recover",
                               ESP_ERR_INVALID_RESPONSE,
                               "invalid-room-data",
                               event->payload);
        return;
    }
    if (strcmp(status, "answered") == 0) {
        esp_err_t err = submit_room_action_for(context,
                                               SESSION_HTTP_CALL_HANGUP,
                                               0,
                                               "/v1/call/hangup",
                                               room_id,
                                               "p2p_error");
        session_emit_operation(context,
                               "call-recover",
                               err,
                               err == ESP_OK
                                   ? "answered-room-release-submitted"
                                   : "answered-room-release-failed",
                               event->payload);
        return;
    }
    if (!session_claim(context,
                       DEVICE_SERVICE_CALL,
                       DEVICE_SESSION_RECOVERING,
                       TIRTC_ADAPTER_MEDIA_CALL,
                       call_type == SESSION_RUNTIME_CALL_VIDEO,
                       "call-room-recovery")) {
        return;
    }
    if (!session_copy_string(context->call.room_id,
                             sizeof(context->call.room_id),
                             room_id)) {
        session_finish(context,
                       "recovered-room-id-too-large",
                       ESP_ERR_INVALID_SIZE);
        return;
    }
    context->call.call_type = call_type;
    context->call.caller = strcmp(role, "caller") == 0;
    if (context->call.caller) {
        int incoming_rc =
            tirtc_adapter_expect_incoming(context->generation);
        if (incoming_rc != 0) {
            session_emit_operation(context,
                                   "call-recover",
                                   incoming_rc,
                                   "incoming-connection-arm-failed",
                                   NULL);
            session_finish(context,
                           "call-recovery-incoming-arm-failed",
                           incoming_rc);
            return;
        }
        context->call.phase = SESSION_CALL_PHASE_OUTGOING_WAIT;
        context->deadline_ms = session_now_ms() + CALL_OUTGOING_TIMEOUT_MS;
        session_set_state(context,
                          DEVICE_SESSION_RECOVERING,
                          "recovered-caller-waiting-p2p",
                          0);
    } else {
        if (!session_copy_string(context->call.peer_id,
                                 sizeof(context->call.peer_id),
                                 caller_id)) {
            session_finish(context,
                           "recovered-caller-id-too-large",
                           ESP_ERR_INVALID_SIZE);
            return;
        }
        context->call.phase = SESSION_CALL_PHASE_RINGING;
        context->deadline_ms = session_now_ms() + CALL_INCOMING_TIMEOUT_MS;
        session_set_state(context,
                          DEVICE_SESSION_RINGING,
                          "recovered-callee-awaiting-accept",
                          0);
        emit_incoming(context, "recovered");
    }
    session_emit_operation(context, "call-recover", 0, "room-reconciled", NULL);
}

static void submit_contact_intent(session_context_t *context,
                                  const session_internal_event_t *event)
{
    esp_err_t err = ESP_OK;
    const char *operation = "contacts";
    switch (event->type) {
    case SESSION_INT_CONTACTS_LIST:
        operation = "contacts-list";
        err = session_submit_http(context,
                                  SESSION_HTTP_CONTACTS_LIST,
                                  0,
                                  "GET",
                                  "/v1/call/device/contacts",
                                  NULL);
        break;
    case SESSION_INT_CONTACTS_PENDING:
        operation = "contacts-pending";
        err = session_submit_http(context,
                                  SESSION_HTTP_CONTACTS_PENDING,
                                  0,
                                  "GET",
                                  "/v1/call/device/contacts/pending",
                                  NULL);
        break;
    case SESSION_INT_CONTACTS_REQUEST: {
        operation = "contacts-request";
        if (!safe_device_id(event->first)) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        cJSON *root = cJSON_CreateObject();
        bool ok = root != NULL &&
                  cJSON_AddStringToObject(root,
                                         "target_device_id",
                                         event->first) != NULL;
        err = ok ? submit_json(context,
                               SESSION_HTTP_CONTACTS_REQUEST,
                               0,
                               "POST",
                               "/v1/call/device/contacts/request",
                               root)
                 : ESP_ERR_NO_MEM;
        cJSON_Delete(root);
        break;
    }
    case SESSION_INT_CONTACTS_RESPOND: {
        operation = "contacts-respond";
        if (!cached_device_contact(context, true, event->first)) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        cJSON *root = cJSON_CreateObject();
        bool ok = root != NULL &&
                  cJSON_AddStringToObject(root,
                                         "peer_device_id",
                                         event->first) != NULL &&
                  cJSON_AddStringToObject(root,
                                         "action",
                                         event->flag ? "accept" : "reject") != NULL;
        err = ok ? submit_json(context,
                               SESSION_HTTP_CONTACTS_RESPOND,
                               0,
                               "POST",
                               "/v1/call/device/contacts/respond",
                               root)
                 : ESP_ERR_NO_MEM;
        cJSON_Delete(root);
        break;
    }
    case SESSION_INT_CONTACTS_REMARK: {
        operation = "contacts-remark";
        if (!cached_device_contact(context, false, event->first)) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        cJSON *root = cJSON_CreateObject();
        bool ok = root != NULL &&
                  cJSON_AddStringToObject(root, "peer_id", event->first) != NULL &&
                  cJSON_AddStringToObject(root, "remark", event->second) != NULL;
        err = ok ? submit_json(context,
                               SESSION_HTTP_CONTACTS_REMARK,
                               0,
                               "PUT",
                               "/v1/call/device/contacts/remark",
                               root)
                 : ESP_ERR_NO_MEM;
        cJSON_Delete(root);
        break;
    }
    case SESSION_INT_CONTACTS_DELETE: {
        operation = "contacts-delete";
        if (!cached_device_contact(context, false, event->first)) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        char path[256];
        int length = snprintf(path,
                              sizeof(path),
                              "/v1/call/device/contacts?peer_id=%s",
                              event->first);
        err = length > 0 && (size_t)length < sizeof(path)
                  ? session_submit_http(context,
                                        SESSION_HTTP_CONTACTS_DELETE,
                                        0,
                                        "DELETE",
                                        path,
                                        NULL)
                  : ESP_ERR_INVALID_SIZE;
        break;
    }
    default:
        return;
    }
    session_emit_operation(context,
                           operation,
                           err,
                           err == ESP_OK ? "submitted" : "submit-failed",
                           NULL);
}

static void handle_http_response(session_context_t *context,
                                 const session_internal_event_t *event)
{
    switch (event->http_request) {
    case SESSION_HTTP_CALL_REQUEST:
        handle_call_request_response(context, event);
        break;
    case SESSION_HTTP_CALL_ACCEPT:
        handle_accept_response(context, event);
        break;
    case SESSION_HTTP_CALL_RECOVER:
        handle_recovery_response(context, event);
        break;
    case SESSION_HTTP_CONTACTS_LIST:
        handle_contacts_response(context, event, false, false);
        break;
    case SESSION_HTTP_AI_CALL_CONTACTS:
        if (session_ai_call_reserved(context) &&
            context->ai_call_handoff.phase ==
                SESSION_RUNTIME_HANDOFF_RESOLVING &&
            context->owner == DEVICE_SERVICE_AI &&
            event->generation == context->generation &&
            session_handoff_request_matches(
                context->ai_call_handoff.ai_generation,
                context->ai_call_handoff.contacts_request_cookie,
                event->generation,
                event->request_cookie)) {
            int64_t current_ms = session_now_ms();
            if (context->ai_call_handoff.deadline_ms != 0 &&
                current_ms >=
                    context->ai_call_handoff.deadline_ms) {
                session_ai_call_contacts_response(
                    context,
                    event,
                    false);
            } else {
                handle_contacts_response(context,
                                         event,
                                         false,
                                         true);
            }
        }
        break;
    case SESSION_HTTP_CONTACTS_PENDING:
        handle_contacts_response(context, event, true, false);
        break;
    case SESSION_HTTP_CONTACTS_REQUEST:
        handle_mutation_response(context, event, "contacts-request");
        break;
    case SESSION_HTTP_CONTACTS_RESPOND:
        handle_mutation_response(context, event, "contacts-respond");
        break;
    case SESSION_HTTP_CONTACTS_REMARK:
        handle_mutation_response(context, event, "contacts-remark");
        break;
    case SESSION_HTTP_CONTACTS_DELETE:
        handle_mutation_response(context, event, "contacts-delete");
        break;
    case SESSION_HTTP_CALL_BUSY_REJECT:
        handle_mutation_response(context, event, "call-busy-reject");
        break;
    case SESSION_HTTP_CALL_REJECT:
    case SESSION_HTTP_CALL_CANCEL:
    case SESSION_HTTP_CALL_HANGUP:
        /* Submission is already reported before local idempotent cleanup. */
        break;
    default:
        break;
    }
}

static void handle_recover_intent(session_context_t *context)
{
    if (context->owner != DEVICE_SERVICE_NONE ||
        session_ai_call_reserved(context)) {
        session_emit_operation(context,
                               "call-recover",
                               ESP_ERR_INVALID_STATE,
                               "busy",
                               NULL);
        return;
    }
    esp_err_t err = session_submit_http(context,
                                        SESSION_HTTP_CALL_RECOVER,
                                        context->generation,
                                        "GET",
                                        "/v1/call/room",
                                        NULL);
    session_emit_operation(context,
                           "call-recover",
                           err,
                           err == ESP_OK ? "submitted" : "submit-failed",
                           NULL);
}

bool session_call_handles(const session_internal_event_t *event,
                          const session_context_t *context)
{
    if (event->type == SESSION_INT_PLATFORM_SIGNAL) {
        return true;
    }
    if (event->type == SESSION_INT_HTTP_RESPONSE &&
        event->http_request != SESSION_HTTP_AI_TOKEN) {
        return true;
    }
    if (context->owner != DEVICE_SERVICE_CALL) {
        return false;
    }
    return event->type == SESSION_INT_ADAPTER_CONNECTION ||
           (event->type == SESSION_INT_ADAPTER_COMMAND &&
            (event->command == SESSION_COMMAND_CALL_CONFIRM ||
             event->command == SESSION_COMMAND_CALL_HANGUP));
}

void session_call_handle(session_context_t *context,
                         const session_internal_event_t *event)
{
    switch (event->type) {
    case SESSION_INT_CALL_START:
        (void)handle_call_start(context, event, false);
        break;
    case SESSION_INT_CALL_ACCEPT:
        handle_accept_intent(context);
        break;
    case SESSION_INT_CALL_REJECT:
        handle_reject_intent(context);
        break;
    case SESSION_INT_CALL_CANCEL:
        handle_cancel_intent(context);
        break;
    case SESSION_INT_CALL_HANGUP:
        handle_hangup_intent(context);
        break;
    case SESSION_INT_CALL_RECOVER:
        handle_recover_intent(context);
        break;
    case SESSION_INT_CONTACTS_LIST:
    case SESSION_INT_CONTACTS_PENDING:
    case SESSION_INT_CONTACTS_REQUEST:
    case SESSION_INT_CONTACTS_RESPOND:
    case SESSION_INT_CONTACTS_REMARK:
    case SESSION_INT_CONTACTS_DELETE:
        submit_contact_intent(context, event);
        break;
    case SESSION_INT_PLATFORM_SIGNAL:
        handle_platform_signal(context, event);
        break;
    case SESSION_INT_HTTP_RESPONSE:
        handle_http_response(context, event);
        break;
    case SESSION_INT_ADAPTER_CONNECTION:
        handle_connection(context, event);
        break;
    case SESSION_INT_ADAPTER_COMMAND:
        handle_call_command(context, event);
        break;
    default:
        break;
    }
}

void session_call_tick(session_context_t *context, int64_t current_ms)
{
    if (context->owner != DEVICE_SERVICE_CALL) {
        return;
    }
    if (context->deadline_ms == 0 || current_ms < context->deadline_ms) {
        return;
    }
    switch (context->call.phase) {
    case SESSION_CALL_PHASE_RINGING:
        (void)submit_room_action(context,
                                 SESSION_HTTP_CALL_REJECT,
                                 "/v1/call/reject",
                                 "decline");
        session_finish(context, "call-incoming-timeout", ESP_ERR_TIMEOUT);
        break;
    case SESSION_CALL_PHASE_REQUEST:
        session_finish(context, "call-request-timeout", ESP_ERR_TIMEOUT);
        break;
    case SESSION_CALL_PHASE_OUTGOING_WAIT:
    case SESSION_CALL_PHASE_WAIT_CONFIRM:
        (void)submit_room_action(context,
                                 SESSION_HTTP_CALL_CANCEL,
                                 "/v1/call/cancel",
                                 NULL);
        session_finish(context, "call-outgoing-timeout", ESP_ERR_TIMEOUT);
        break;
    case SESSION_CALL_PHASE_ACCEPT_TOKEN:
        finish_call_with_platform(context,
                                  "call-accept-token-timeout",
                                  ESP_ERR_TIMEOUT);
        break;
    case SESSION_CALL_PHASE_CONNECTING:
        (void)tirtc_adapter_cancel_connect();
        finish_call_with_platform(context,
                                  "call-connect-timeout",
                                  ESP_ERR_TIMEOUT);
        break;
    default:
        break;
    }
}
