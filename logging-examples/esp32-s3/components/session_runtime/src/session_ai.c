#include "session_runtime_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device/device_utf8.h"
#include "esp_err.h"
#include "media_runtime.h"
#include "platform_client.h"

#define AI_HTTP_TIMEOUT_MS 10000
#define AI_WHIP_TIMEOUT_MS 10000
#define AI_START_DELAY_MS 300
#define AI_START_RESPONSE_TIMEOUT_MS 10000
#define AI_ACTION_TIMEOUT_MS 15000
#define AI_UPDATE_TIMEOUT_MS 10000
#define AI_PROMPT_TIMEOUT_MS 30000
#define AI_PROMPT_DRAIN_TIMEOUT_MS 5000

static void finish_local_ai(session_context_t *context,
                            const char *reason,
                            int terminal_status);

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

static const char *ai_codec_name(device_audio_codec_t codec)
{
    switch (codec) {
    case DEVICE_AUDIO_CODEC_G711A:
        return "g711a";
    case DEVICE_AUDIO_CODEC_AMR_NB:
    case DEVICE_AUDIO_CODEC_AMR_WB:
        return "amr";
    case DEVICE_AUDIO_CODEC_OPUS:
        return "opus";
    default:
        return NULL;
    }
}

static bool json_id_matches(const cJSON *id, const char *expected)
{
    if (cJSON_IsString(id) && id->valuestring != NULL &&
        id->valuestring[0] != '\0') {
        return strcmp(id->valuestring, expected) == 0;
    }
    if (cJSON_IsNumber(id)) {
        char number[32];
        (void)snprintf(number, sizeof(number), "%.0f", id->valuedouble);
        return strcmp(number, expected) == 0;
    }
    return false;
}

static bool audio_format_matches(const cJSON *format,
                                 const device_audio_config_t *audio)
{
    /* The platform contract guarantees result.session_id; older deployments
     * may omit the optional negotiated format objects. In that case the
     * explicitly requested format remains in force. If a format is returned,
     * reject any mismatch instead of silently sending the wrong media. */
    if (format == NULL) {
        return true;
    }
    if (!cJSON_IsObject(format)) {
        return false;
    }
    const cJSON *codec = format == NULL
                             ? NULL
                             : cJSON_GetObjectItemCaseSensitive(format, "codec");
    const cJSON *sample_rate = format == NULL
                                   ? NULL
                                   : cJSON_GetObjectItemCaseSensitive(format,
                                                                      "sample_rate");
    const cJSON *channels = format == NULL
                                ? NULL
                                : cJSON_GetObjectItemCaseSensitive(format,
                                                                   "channels");
    const char *expected_codec = ai_codec_name(audio->codec);
    bool codec_matches = cJSON_IsString(codec) &&
                         codec->valuestring != NULL &&
                         expected_codec != NULL &&
                         strcmp(codec->valuestring, expected_codec) == 0;
    if (!codec_matches && audio->codec == DEVICE_AUDIO_CODEC_G711A &&
        cJSON_IsString(codec) && codec->valuestring != NULL) {
        /* The public AI contract names this codec g711a while the live
         * negotiation response uses the equivalent SDK name alaw. */
        codec_matches = strcmp(codec->valuestring, "alaw") == 0;
    }
    return codec_matches &&
           cJSON_IsNumber(sample_rate) &&
           sample_rate->valuedouble == (double)audio->sample_rate_hz &&
           cJSON_IsNumber(channels) &&
           channels->valuedouble == (double)audio->channels;
}

static int send_json(session_context_t *context, cJSON *root)
{
    char *json = root == NULL ? NULL : cJSON_PrintUnformatted(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int rc = tirtc_adapter_send_command(context->generation,
                                        SESSION_COMMAND_AI,
                                        json,
                                        (uint32_t)strlen(json));
    free(json);
    return rc;
}

static int send_notification(session_context_t *context,
                             const char *method,
                             cJSON *params)
{
    cJSON *root = cJSON_CreateObject();
    bool ok = root != NULL &&
              cJSON_AddStringToObject(root, "jsonrpc", "2.0") != NULL &&
              cJSON_AddStringToObject(root, "method", method) != NULL;
    if (ok && params != NULL) {
        cJSON_AddItemToObject(root, "params", params);
        params = NULL;
    }
    int rc = ok ? send_json(context, root) : ESP_ERR_NO_MEM;
    cJSON_Delete(params);
    cJSON_Delete(root);
    return rc;
}

static bool add_audio_format(cJSON *parent,
                             const char *name,
                             const device_audio_config_t *audio)
{
    const char *codec = ai_codec_name(audio->codec);
    cJSON *format = cJSON_CreateObject();
    bool ok = codec != NULL && format != NULL &&
              cJSON_AddStringToObject(format, "codec", codec) != NULL &&
              cJSON_AddNumberToObject(format,
                                     "sample_rate",
                                     audio->sample_rate_hz) != NULL &&
              cJSON_AddNumberToObject(format, "channels", audio->channels) != NULL;
    if (ok) {
        cJSON_AddItemToObject(parent, name, format);
    } else {
        cJSON_Delete(format);
    }
    return ok;
}

static void send_start_session(session_context_t *context)
{
    const device_media_config_t *media = media_runtime_config();
    const char *device_id = platform_client_device_id();
    if (media == NULL || device_id == NULL || device_id[0] == '\0') {
        session_finish(context, "ai-device-config-unavailable", ESP_ERR_INVALID_STATE);
        return;
    }

    session_make_rpc_id(context,
                        "ai-start",
                        context->ai.start_request_id,
                        sizeof(context->ai.start_request_id));
    cJSON *root = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();
    bool ok = root != NULL && params != NULL &&
              cJSON_AddStringToObject(root, "jsonrpc", "2.0") != NULL &&
              cJSON_AddStringToObject(root,
                                     "id",
                                     context->ai.start_request_id) != NULL &&
              cJSON_AddStringToObject(root, "method", "start_session") != NULL &&
              cJSON_AddStringToObject(params,
                                     "device_id",
                                     device_id) != NULL &&
              cJSON_AddStringToObject(params,
                                     "role_id",
                                     context->ai.role_id) != NULL &&
              add_audio_format(params, "input_audio", &media->audio) &&
              add_audio_format(params, "output_audio", &media->audio);
    if (ok) {
        cJSON_AddItemToObject(root, "params", params);
        params = NULL;
    }
    int rc = ok ? send_json(context, root) : ESP_ERR_NO_MEM;
    cJSON_Delete(params);
    cJSON_Delete(root);
    if (rc != 0) {
        session_finish(context, "ai-start-command-send-failed", rc);
        return;
    }
    context->ai.phase = SESSION_AI_PHASE_START_RESPONSE;
    context->deadline_ms = session_now_ms() + AI_START_RESPONSE_TIMEOUT_MS;
    session_set_state(context,
                      DEVICE_SESSION_AI_STARTING,
                      "ai-start-session-sent",
                      0);
}

static void handle_ai_start_intent(session_context_t *context)
{
    if (context->owner != DEVICE_SERVICE_NONE) {
        session_emit_operation(context,
                               "ai-start",
                               ESP_ERR_INVALID_STATE,
                               "busy",
                               NULL);
        return;
    }
    if (!platform_client_ready() ||
        tirtc_adapter_state() != TIRTC_ADAPTER_RUNNING) {
        session_emit_operation(context,
                               "ai-start",
                               ESP_ERR_INVALID_STATE,
                               "platform-or-tirtc-not-ready",
                               NULL);
        return;
    }
    if (!session_claim(context,
                       DEVICE_SERVICE_AI,
                       DEVICE_SESSION_AI_CONNECTING,
                       TIRTC_ADAPTER_MEDIA_AI,
                       false,
                       "ai-token-request")) {
        session_emit_operation(context,
                               "ai-start",
                               ESP_ERR_INVALID_STATE,
                               "media-profile-busy",
                               NULL);
        return;
    }
    context->ai.phase = SESSION_AI_PHASE_TOKEN;
    context->deadline_ms = session_now_ms() + AI_HTTP_TIMEOUT_MS;
    esp_err_t err = session_submit_http(context,
                                        SESSION_HTTP_AI_TOKEN,
                                        context->generation,
                                        "GET",
                                        "/v1/ai/token",
                                        NULL);
    if (err != ESP_OK) {
        session_finish(context, "ai-token-submit-failed", err);
    } else {
        session_emit_operation(context, "ai-start", 0, "accepted", NULL);
    }
}

static void handle_ai_token(session_context_t *context,
                            const session_internal_event_t *event)
{
    if (event->generation != context->generation ||
        context->ai.phase != SESSION_AI_PHASE_TOKEN) {
        return;
    }
    if (event->truncated) {
        session_finish(context, "ai-token-response-too-large", ESP_ERR_INVALID_SIZE);
        return;
    }
    cJSON *root = parse_json_exact(event->payload, event->length);
    const cJSON *data = session_json_response_data(root);
    int code = session_json_response_code(root);
    char token[SESSION_RUNTIME_TOKEN_MAX] = {0};
    bool valid = (code == 0 || code == 200) && cJSON_IsObject(data) &&
                 session_json_string(data,
                                     "peer_id",
                                     context->ai.peer_id,
                                     sizeof(context->ai.peer_id)) &&
                 session_json_string(data,
                                     "token",
                                     token,
                                     sizeof(token)) &&
                 session_json_string(data,
                                     "role_id",
                                     context->ai.role_id,
                                     sizeof(context->ai.role_id));
    cJSON_Delete(root);
    if (!valid) {
        session_finish(context, "ai-token-invalid-response", code);
        return;
    }

    context->ai.phase = SESSION_AI_PHASE_WHIP;
    context->deadline_ms = session_now_ms() + AI_WHIP_TIMEOUT_MS;
    int rc = tirtc_adapter_whip_connect(context->ai.peer_id,
                                        token,
                                        context->generation);
    session_secure_zero(token, sizeof(token));
    if (rc != 0) {
        session_finish(context, "ai-whip-submit-failed", rc);
    } else {
        session_set_state(context,
                          DEVICE_SESSION_AI_CONNECTING,
                          "ai-whip-connecting",
                          0);
    }
}

static void handle_connection(session_context_t *context,
                              const session_internal_event_t *event)
{
    if (event->generation != context->generation ||
        context->owner != DEVICE_SERVICE_AI) {
        return;
    }
    if (!event->connection.connected) {
        session_finish(context,
                       context->ai.phase == SESSION_AI_PHASE_ACTIVE
                           ? "ai-remote-disconnected"
                           : "ai-whip-connect-failed",
                       event->connection.error);
        return;
    }
    if (context->ai.phase != SESSION_AI_PHASE_WHIP) {
        session_finish(context,
                       "ai-unexpected-connection",
                       ESP_ERR_INVALID_STATE);
        return;
    }
    context->ai.phase = SESSION_AI_PHASE_DELAY;
    context->deadline_ms = session_now_ms() + AI_START_DELAY_MS;
    session_set_state(context,
                      DEVICE_SESSION_AI_STARTING,
                      "ai-whip-connected-delay",
                      0);
}

static void emit_ai_method(session_context_t *context,
                           const char *method,
                           const cJSON *params)
{
    session_runtime_event_t event = {
        .type = SESSION_RUNTIME_EVENT_AI_EVENT,
    };
    (void)session_copy_string(event.name, sizeof(event.name), method);
    char *json = params == NULL ? NULL : cJSON_PrintUnformatted(params);
    if (json != NULL) {
        (void)session_copy_json(event.json, sizeof(event.json), json);
        free(json);
    }
    session_emit(context, &event);
}

static bool caption_id_string(const cJSON *params,
                              char *destination,
                              size_t destination_size)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(params, "utterance_id");
    if (cJSON_IsString(id) && id->valuestring != NULL &&
        id->valuestring[0] != '\0') {
        return session_copy_string(destination,
                                   destination_size,
                                   id->valuestring);
    }
    if (cJSON_IsNumber(id) && id->valuedouble >= 0) {
        int length = snprintf(destination,
                              destination_size,
                              "%.0f",
                              id->valuedouble);
        return length > 0 && (size_t)length < destination_size;
    }
    return false;
}

static bool utf8_boundary(const char *text, size_t offset)
{
    if (text == NULL || offset == 0U || text[offset] == '\0') {
        return text != NULL;
    }
    return (((unsigned char)text[offset]) & 0xc0U) != 0x80U;
}

static size_t caption_overlap(const char *existing, const char *incoming)
{
    size_t existing_length = strlen(existing);
    size_t incoming_length = strlen(incoming);
    size_t maximum = existing_length < incoming_length
                         ? existing_length
                         : incoming_length;
    for (size_t overlap = maximum; overlap > 0U; --overlap) {
        size_t existing_offset = existing_length - overlap;
        if (utf8_boundary(existing, existing_offset) &&
            utf8_boundary(incoming, overlap) &&
            memcmp(existing + existing_offset, incoming, overlap) == 0) {
            return overlap;
        }
    }
    return 0U;
}

static bool apply_caption_text(session_caption_group_t *group,
                               int caption_type,
                               int mode,
                               const char *incoming)
{
    if (group == NULL || incoming == NULL ||
        strlen(incoming) >= sizeof(group->text)) {
        return false;
    }
    if (mode != 1 || caption_type == 0 || group->text[0] == '\0') {
        return session_copy_string(group->text, sizeof(group->text), incoming);
    }
    size_t current_length = strlen(group->text);
    size_t incoming_length = strlen(incoming);
    if (strcmp(group->text, incoming) == 0 ||
        (incoming_length < current_length &&
         memcmp(group->text, incoming, incoming_length) == 0)) {
        return true;
    }
    if (incoming_length > current_length &&
        memcmp(incoming, group->text, current_length) == 0) {
        return session_copy_string(group->text, sizeof(group->text), incoming);
    }
    size_t overlap = caption_overlap(group->text, incoming);
    const char *suffix = incoming + overlap;
    size_t suffix_length = strlen(suffix);
    if (current_length + suffix_length >= sizeof(group->text)) {
        return false;
    }
    memcpy(group->text + current_length, suffix, suffix_length + 1U);
    return true;
}

static void emit_caption(session_context_t *context, const cJSON *params)
{
    session_runtime_event_t event = {
        .type = SESSION_RUNTIME_EVENT_AI_CAPTION,
        .caption_type = -1,
        .caption_mode = -1,
    };
    const cJSON *caption_type = cJSON_GetObjectItemCaseSensitive(params,
                                                                 "caption_type");
    const cJSON *seq_num = cJSON_GetObjectItemCaseSensitive(params, "seq_num");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(params, "mode");
    const cJSON *final = cJSON_GetObjectItemCaseSensitive(params, "is_final");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(params, "text");
    if (!cJSON_IsString(text)) {
        text = cJSON_GetObjectItemCaseSensitive(params, "caption");
    }
    bool seq_present = seq_num != NULL && !cJSON_IsNull(seq_num);
    bool seq_valid =
        !seq_present ||
        (cJSON_IsNumber(seq_num) &&
         seq_num->valuedouble >= 0.0 &&
         seq_num->valuedouble <= (double)INT_MAX &&
         seq_num->valuedouble ==
             (double)(uint32_t)seq_num->valuedouble);
    bool has_seq = seq_present && seq_valid;
    if (cJSON_IsNumber(caption_type)) {
        event.caption_type = caption_type->valueint;
    }
    if (has_seq) {
        event.seq_num = (uint32_t)seq_num->valuedouble;
    }
    if (cJSON_IsNumber(mode)) {
        event.caption_mode = mode->valueint;
    }
    event.final = cJSON_IsTrue(final);
    bool valid_id = caption_id_string(params,
                                      event.request_id,
                                      sizeof(event.request_id));
    bool valid = seq_valid &&
                 (event.caption_type == 0 || event.caption_type == 1) &&
                 cJSON_IsString(text) && text->valuestring != NULL &&
                 valid_id &&
                 device_utf8_validate(event.request_id) &&
                 device_utf8_validate(text->valuestring);
    if (!valid) {
        session_emit_diagnostic(context,
                                ESP_ERR_INVALID_RESPONSE,
                                "ai-caption-invalid");
        return;
    }

    session_caption_group_t *group =
        &context->ai.captions[event.caption_type];
    bool new_group = !group->active || group->final ||
                     strcmp(group->utterance_id, event.request_id) != 0;
    if (new_group) {
        memset(group, 0, sizeof(*group));
        group->active = session_copy_string(group->utterance_id,
                                            sizeof(group->utterance_id),
                                            event.request_id);
    }
    if (!group->active ||
        (has_seq && group->seq_valid && event.seq_num < group->last_seq)) {
        return;
    }
    if (!apply_caption_text(group,
                            event.caption_type,
                            event.caption_mode,
                            text->valuestring)) {
        session_emit_diagnostic(context,
                                ESP_ERR_INVALID_SIZE,
                                "ai-caption-text-too-large");
        return;
    }
    group->seq_valid = has_seq;
    group->last_seq = event.seq_num;
    group->final = event.final;
    (void)session_copy_string(event.text, sizeof(event.text), group->text);
    char *json = cJSON_PrintUnformatted(params);
    if (json != NULL) {
        (void)session_copy_json(event.json, sizeof(event.json), json);
        free(json);
    }
    session_emit(context, &event);
}

static bool capture_json_id(const cJSON *id,
                            char *display,
                            size_t display_size,
                            char *raw,
                            size_t raw_size)
{
    char *printed = cJSON_PrintUnformatted(id);
    bool raw_ok = printed != NULL &&
                  session_copy_string(raw, raw_size, printed);
    free(printed);
    if (!raw_ok) {
        return false;
    }
    if (cJSON_IsString(id) && id->valuestring != NULL &&
        id->valuestring[0] != '\0') {
        if (!session_copy_string(display, display_size, id->valuestring)) {
            return false;
        }
    } else if (cJSON_IsNumber(id)) {
        (void)snprintf(display, display_size, "%.0f", id->valuedouble);
    } else {
        return false;
    }
    return true;
}

int session_ai_send_action_response(session_context_t *context,
                                    const char *id_json,
                                    bool ok,
                                    const char *payload)
{
    cJSON *id = parse_json_exact(id_json, strlen(id_json));
    cJSON *root = cJSON_CreateObject();
    bool built = id != NULL && root != NULL &&
                 cJSON_AddStringToObject(root, "jsonrpc", "2.0") != NULL;
    if (built) {
        cJSON_AddItemToObject(root, "id", id);
        id = NULL;
    }
    if (built && ok) {
        cJSON *result = cJSON_CreateObject();
        built = result != NULL &&
                cJSON_AddBoolToObject(result, "ok", true) != NULL;
        if (built && payload != NULL && payload[0] != '\0') {
            cJSON *data = parse_json_exact(payload, strlen(payload));
            if (data != NULL) {
                cJSON_AddItemToObject(result, "data", data);
            } else {
                built = cJSON_AddStringToObject(result, "message", payload) != NULL;
            }
        }
        if (built) {
            cJSON_AddItemToObject(root, "result", result);
        } else {
            cJSON_Delete(result);
        }
    } else if (built) {
        cJSON *error = cJSON_CreateObject();
        built = error != NULL &&
                cJSON_AddNumberToObject(error, "code", -32000) != NULL &&
                cJSON_AddStringToObject(error,
                                       "message",
                                       payload == NULL || payload[0] == '\0'
                                           ? "device action failed"
                                           : payload) != NULL;
        if (built) {
            cJSON_AddItemToObject(root, "error", error);
        } else {
            cJSON_Delete(error);
        }
    }
    int rc = built ? send_json(context, root) : ESP_ERR_NO_MEM;
    cJSON_Delete(id);
    cJSON_Delete(root);
    return rc;
}

int session_ai_complete_action(session_context_t *context,
                               bool ok,
                               const char *payload)
{
    if (context == NULL || context->ai.action_id[0] == '\0' ||
        context->ai.action_id_json[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    int rc = session_ai_send_action_response(context,
                                             context->ai.action_id_json,
                                             ok,
                                             payload);
    if (rc == 0) {
        context->ai.action_id[0] = '\0';
        context->ai.action_id_json[0] = '\0';
        context->ai.action_deadline_ms = 0;
        session_refresh_snapshot(context);
    }
    return rc;
}

static int send_rpc_error(session_context_t *context,
                          const cJSON *id_value,
                          int code,
                          const char *message)
{
    cJSON *id = cJSON_Duplicate(id_value, true);
    cJSON *root = cJSON_CreateObject();
    cJSON *error = cJSON_CreateObject();
    bool built = id != NULL && root != NULL && error != NULL &&
                 cJSON_AddStringToObject(root, "jsonrpc", "2.0") != NULL &&
                 cJSON_AddNumberToObject(error, "code", code) != NULL &&
                 cJSON_AddStringToObject(error, "message", message) != NULL;
    if (built) {
        cJSON_AddItemToObject(root, "id", id);
        id = NULL;
        cJSON_AddItemToObject(root, "error", error);
        error = NULL;
    }
    int rc = built ? send_json(context, root) : ESP_ERR_NO_MEM;
    cJSON_Delete(id);
    cJSON_Delete(error);
    cJSON_Delete(root);
    return rc;
}

static void handle_device_action(session_context_t *context,
                                 const cJSON *root,
                                 const cJSON *params)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    char display[sizeof(context->ai.action_id)] = {0};
    char raw[sizeof(context->ai.action_id_json)] = {0};
    if (!capture_json_id(id,
                         display,
                         sizeof(display),
                         raw,
                         sizeof(raw))) {
        if (session_ai_call_reserved(context)) {
            session_emit_diagnostic(
                context,
                ESP_ERR_INVALID_RESPONSE,
                "ai-device-action-invalid-during-handoff-ignored");
            return;
        }
        int rc = raw[0] == '\0'
                     ? ESP_ERR_INVALID_RESPONSE
                     : session_ai_send_action_response(
                           context,
                           raw,
                           false,
                           "device action id is invalid or too large");
        session_emit_diagnostic(context,
                                rc == 0 ? ESP_ERR_INVALID_SIZE : rc,
                                 "ai-device-action-id-invalid");
        if (raw[0] != '\0' && rc != 0) {
            session_finish(context,
                           "ai-device-action-id-response-failed",
                           rc);
        }
        return;
    }
    if (session_ai_call_reserved(context)) {
        session_ai_call_reject_concurrent_action(context, raw);
        return;
    }
    const cJSON *action = cJSON_IsObject(params)
                              ? cJSON_GetObjectItemCaseSensitive(params,
                                                                 "action")
                              : NULL;
    if (!cJSON_IsString(action) || action->valuestring == NULL ||
        action->valuestring[0] == '\0' ||
        strlen(action->valuestring) >=
            sizeof(((session_runtime_event_t *)0)->name)) {
        int rc = session_ai_send_action_response(context,
                                                 raw,
                                                 false,
                                                 "invalid device action");
        session_emit_diagnostic(context,
                                rc == 0 ? ESP_ERR_INVALID_RESPONSE : rc,
                                "ai-device-action-invalid");
        if (rc != 0) {
            session_finish(context,
                           "ai-device-action-invalid-response-failed",
                           rc);
        }
        return;
    }
    if (context->ai.action_id[0] != '\0') {
        if (strcmp(context->ai.action_id_json, raw) == 0) {
            session_emit_diagnostic(
                context,
                0,
                "ai-device-action-duplicate-ignored");
            return;
        }
        int rc = session_ai_send_action_response(
            context,
            raw,
            false,
            "another device action is pending");
        if (rc != 0) {
            session_emit_diagnostic(context,
                                    rc,
                                    "ai-device-action-busy-response-send-failed");
            session_finish(context,
                           "ai-device-action-busy-response-failed",
                           rc);
        }
        return;
    }
    (void)session_copy_string(context->ai.action_id,
                              sizeof(context->ai.action_id),
                              display);
    (void)session_copy_string(context->ai.action_id_json,
                              sizeof(context->ai.action_id_json),
                              raw);
    context->ai.action_deadline_ms = session_now_ms() + AI_ACTION_TIMEOUT_MS;
    session_refresh_snapshot(context);

    session_runtime_event_t event = {
        .type = SESSION_RUNTIME_EVENT_AI_ACTION,
    };
    (void)session_copy_string(event.name,
                              sizeof(event.name),
                              action->valuestring);
    (void)session_copy_string(event.request_id,
                              sizeof(event.request_id),
                              display);
    char *json = params == NULL ? NULL : cJSON_PrintUnformatted(params);
    if (json != NULL) {
        (void)session_copy_json(event.json, sizeof(event.json), json);
        free(json);
    }
    session_emit(context, &event);
    if (strcmp(action->valuestring, "call_device") == 0) {
        (void)session_ai_call_begin(context, params);
    }
}

static void handle_start_response(session_context_t *context,
                                  const cJSON *root)
{
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (error != NULL) {
        session_finish(context, "ai-start-session-rejected", ESP_FAIL);
        return;
    }
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    const cJSON *input = result == NULL
                             ? NULL
                             : cJSON_GetObjectItemCaseSensitive(result,
                                                                "input_audio");
    const cJSON *output = result == NULL
                              ? NULL
                              : cJSON_GetObjectItemCaseSensitive(result,
                                                                 "output_audio");
    const device_media_config_t *media = media_runtime_config();
    bool valid = cJSON_IsObject(result) && media != NULL &&
                 session_json_string(result,
                                     "session_id",
                                     context->ai.session_id,
                                     sizeof(context->ai.session_id)) &&
                 audio_format_matches(input, &media->audio) &&
                 audio_format_matches(output, &media->audio);
    if (!valid) {
        session_finish(context,
                       "ai-negotiated-format-invalid",
                       ESP_ERR_INVALID_RESPONSE);
        return;
    }
    int subscribe_rc = tirtc_adapter_subscribe_downlink(context->generation,
                                                        true,
                                                        false);
    if (subscribe_rc != 0) {
        session_finish(context,
                       "ai-downlink-subscribe-failed",
                       subscribe_rc);
        return;
    }
    context->ai.phase = SESSION_AI_PHASE_ACTIVE;
    context->deadline_ms = 0;
    media_runtime_set_uplink_active(false);
    session_set_state(context, DEVICE_SESSION_AI_ACTIVE, "ai-active", 0);
    session_emit_operation(context, "ai-start", 0, "completed", NULL);
}

static void handle_ai_command(session_context_t *context,
                              const session_internal_event_t *event)
{
    if (event->generation != context->generation ||
        event->command != SESSION_COMMAND_AI || event->truncated) {
        if (event->truncated) {
            session_finish(context,
                           "ai-command-too-large",
                           ESP_ERR_INVALID_SIZE);
        }
        return;
    }
    cJSON *root = parse_json_exact(event->payload, event->length);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        session_emit_diagnostic(context,
                                ESP_ERR_INVALID_RESPONSE,
                                "ai-command-invalid-json");
        return;
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *jsonrpc = cJSON_GetObjectItemCaseSensitive(root, "jsonrpc");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    bool protocol_valid =
        cJSON_IsString(jsonrpc) && jsonrpc->valuestring != NULL &&
        strcmp(jsonrpc->valuestring, "2.0") == 0;
    bool response_shape =
        method == NULL && ((result != NULL) != (error != NULL));
    if (!protocol_valid) {
        session_emit_diagnostic(context,
                                ESP_ERR_INVALID_RESPONSE,
                                "ai-jsonrpc-version-invalid");
        cJSON_Delete(root);
        return;
    }

    if (id != NULL && context->ai.phase == SESSION_AI_PHASE_START_RESPONSE &&
        json_id_matches(id, context->ai.start_request_id)) {
        if (!response_shape) {
            cJSON_Delete(root);
            session_finish(context,
                           "ai-start-response-envelope-invalid",
                           ESP_ERR_INVALID_RESPONSE);
            return;
        }
        context->event_origin_request_id =
            context->session_origin_request_id;
        session_emit_operation(context,
                               "ai-start-response",
                               0,
                               "received",
                               event->payload);
        handle_start_response(context, root);
        cJSON_Delete(root);
        return;
    }
    if (id != NULL && context->ai.update_request_id[0] != '\0' &&
        json_id_matches(id, context->ai.update_request_id)) {
        context->event_origin_request_id =
            context->ai.update_origin_request_id;
        bool ok = response_shape && error == NULL;
        int status = !response_shape
                         ? ESP_ERR_INVALID_RESPONSE
                         : (ok ? 0 : ESP_FAIL);
        const char *phase = !response_shape
                                ? "invalid-response"
                                : (ok ? "completed" : "rejected");
        session_emit_operation(context,
                               "ai-update-config",
                               status,
                               phase,
                               event->payload);
        context->ai.update_request_id[0] = '\0';
        context->ai.update_origin_request_id = 0U;
        context->ai.update_deadline_ms = 0;
        session_refresh_snapshot(context);
        cJSON_Delete(root);
        return;
    }
    if (id != NULL && method != NULL &&
        session_ai_call_response_frozen(context)) {
        session_emit_diagnostic(
            context,
            ESP_ERR_INVALID_STATE,
            "ai-request-ignored-during-call-handoff-drain");
        cJSON_Delete(root);
        return;
    }
    if (!cJSON_IsString(method) || method->valuestring == NULL ||
        method->valuestring[0] == '\0' || result != NULL || error != NULL ||
        (params != NULL && !cJSON_IsObject(params))) {
        session_emit_diagnostic(context,
                                ESP_ERR_INVALID_RESPONSE,
                                "ai-unmatched-response");
        cJSON_Delete(root);
        return;
    }

    if (strcmp(method->valuestring, "caption") == 0) {
        if (id != NULL || !cJSON_IsObject(params)) {
            if (id != NULL) {
                (void)send_rpc_error(context,
                                     id,
                                     -32600,
                                     "caption must be a notification");
            }
            session_emit_diagnostic(context,
                                    ESP_ERR_INVALID_RESPONSE,
                                    "ai-caption-envelope-invalid");
        } else {
            emit_caption(context, params);
        }
    } else if (strcmp(method->valuestring, "device_action") == 0) {
        if (id == NULL || !cJSON_IsObject(params)) {
            session_emit_diagnostic(context,
                                    ESP_ERR_INVALID_RESPONSE,
                                    "ai-device-action-envelope-invalid");
        } else {
            handle_device_action(context, root, params);
        }
    } else if (strcmp(method->valuestring, "end_session") == 0) {
        if (id != NULL) {
            (void)send_rpc_error(context,
                                 id,
                                 -32600,
                                 "end_session must be a notification");
            session_emit_diagnostic(context,
                                    ESP_ERR_INVALID_RESPONSE,
                                    "ai-end-session-envelope-invalid");
            cJSON_Delete(root);
            return;
        }
        emit_ai_method(context, method->valuestring, params);
        cJSON_Delete(root);
        session_finish(context, "ai-remote-end-session", 0);
        return;
    } else if (id != NULL) {
        int rc = send_rpc_error(context, id, -32601, "method not found");
        session_emit_diagnostic(context,
                                rc == 0 ? ESP_ERR_NOT_SUPPORTED : rc,
                                "ai-request-method-unsupported");
    } else {
        /* round_start/end, interrupt, submit_speech, update_config and
         * adapter-specific events such as forward remain visible to AT. */
        emit_ai_method(context, method->valuestring, params);
    }
    cJSON_Delete(root);
}

static void send_simple_control(session_context_t *context,
                                const char *method,
                                const char *operation)
{
    if (context->ai.phase != SESSION_AI_PHASE_ACTIVE) {
        session_emit_operation(context,
                               operation,
                               ESP_ERR_INVALID_STATE,
                               "ai-not-active",
                               NULL);
        return;
    }
    int rc = send_notification(context, method, NULL);
    session_emit_operation(context,
                           operation,
                           rc,
                           rc == 0 ? "submitted" : "send-failed",
                           NULL);
}

static void handle_ai_prompt(session_context_t *context, const char *name)
{
    media_runtime_ai_prompt_t prompt;
    if (context->ai.phase != SESSION_AI_PHASE_ACTIVE ||
        context->ai.update_request_id[0] != '\0' ||
        context->ai.action_id[0] != '\0' ||
        context->ai.prompt_pending || session_ai_call_reserved(context)) {
        session_emit_operation(context,
                               "ai-prompt",
                               ESP_ERR_INVALID_STATE,
                               "ai-not-ready-or-prompt-pending",
                               NULL);
        return;
    }
    if (!media_runtime_ai_prompt_parse(name, &prompt)) {
        session_emit_operation(context,
                               "ai-prompt",
                               ESP_ERR_INVALID_ARG,
                               "unknown-preset",
                               NULL);
        return;
    }

    esp_err_t err = media_runtime_play_ai_prompt(prompt, context->generation);
    if (err != ESP_OK) {
        session_emit_operation(context,
                               "ai-prompt",
                               err,
                               "media-busy-or-unavailable",
                               NULL);
        return;
    }
    context->ai.prompt_pending = true;
    context->ai.prompt_draining = false;
    context->ai.prompt_origin_request_id = context->event_origin_request_id;
    context->ai.prompt_deadline_ms =
        session_now_ms() + AI_PROMPT_TIMEOUT_MS;
    (void)session_copy_string(context->ai.prompt_name,
                              sizeof(context->ai.prompt_name),
                              media_runtime_ai_prompt_name(prompt));
    char detail[64];
    (void)snprintf(detail,
                   sizeof(detail),
                   "{\"preset\":\"%s\"}",
                   context->ai.prompt_name);
    session_emit_operation(context, "ai-prompt", 0, "submitted", detail);
}

static void clear_ai_prompt(session_context_t *context)
{
    context->ai.prompt_pending = false;
    context->ai.prompt_draining = false;
    context->ai.prompt_origin_request_id = 0U;
    context->ai.prompt_deadline_ms = 0;
    context->ai.prompt_name[0] = '\0';
}

static void fail_ai_prompt_and_close(session_context_t *context,
                                     int status,
                                     const char *phase,
                                     const char *detail)
{
    media_runtime_cancel_ai_prompt(context->generation);
    clear_ai_prompt(context);
    session_emit_operation(context, "ai-prompt", status, phase, detail);
    finish_local_ai(context, "ai-prompt-failed", status);
}

static void handle_ai_prompt_done(session_context_t *context,
                                  const session_internal_event_t *event)
{
    if (event->generation != context->generation ||
        !context->ai.prompt_pending) {
        return;
    }
    if (strcmp(event->first, context->ai.prompt_name) != 0) {
        session_emit_diagnostic(context,
                                ESP_ERR_INVALID_STATE,
                                "stale-ai-prompt-completion");
        return;
    }

    context->event_origin_request_id =
        context->ai.prompt_origin_request_id;
    char detail[64];
    (void)snprintf(detail,
                   sizeof(detail),
                   "{\"preset\":\"%s\"}",
                   context->ai.prompt_name);
    if (event->status != 0) {
        fail_ai_prompt_and_close(context,
                                 event->status,
                                 "media-failed",
                                 detail);
        return;
    }
    if (session_ai_call_reserved(context)) {
        clear_ai_prompt(context);
        session_emit_operation(context,
                               "ai-prompt",
                               ESP_ERR_INVALID_STATE,
                               "cancelled-for-call-handoff",
                               detail);
        return;
    }
    context->ai.prompt_draining = true;
    context->ai.prompt_deadline_ms =
        session_now_ms() + AI_PROMPT_DRAIN_TIMEOUT_MS;
}

static void handle_update_config(session_context_t *context,
                                 const char *extra_params_json)
{
    if (context->ai.phase != SESSION_AI_PHASE_ACTIVE ||
        context->ai.update_request_id[0] != '\0') {
        session_emit_operation(context,
                               "ai-update-config",
                               ESP_ERR_INVALID_STATE,
                               "ai-not-active-or-update-pending",
                               NULL);
        return;
    }
    cJSON *extra =
        parse_json_exact(extra_params_json, strlen(extra_params_json));
    if (!cJSON_IsObject(extra)) {
        cJSON_Delete(extra);
        session_emit_operation(context,
                               "ai-update-config",
                               ESP_ERR_INVALID_ARG,
                               "extra-params-must-be-json-object",
                               NULL);
        return;
    }
    session_make_rpc_id(context,
                        "ai-update",
                        context->ai.update_request_id,
                        sizeof(context->ai.update_request_id));
    context->ai.update_origin_request_id =
        context->event_origin_request_id;
    cJSON *root = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();
    bool ok = root != NULL && params != NULL &&
              cJSON_AddStringToObject(root, "jsonrpc", "2.0") != NULL &&
              cJSON_AddStringToObject(root,
                                     "id",
                                     context->ai.update_request_id) != NULL &&
              cJSON_AddStringToObject(root, "method", "update_config") != NULL;
    if (ok) {
        cJSON_AddItemToObject(params, "extra_params", extra);
        extra = NULL;
        cJSON_AddItemToObject(root, "params", params);
        params = NULL;
    }
    int rc = ok ? send_json(context, root) : ESP_ERR_NO_MEM;
    cJSON_Delete(extra);
    cJSON_Delete(params);
    cJSON_Delete(root);
    if (rc != 0) {
        context->ai.update_request_id[0] = '\0';
        context->ai.update_origin_request_id = 0U;
    } else {
        context->ai.update_deadline_ms = session_now_ms() + AI_UPDATE_TIMEOUT_MS;
    }
    session_refresh_snapshot(context);
    session_emit_operation(context,
                           "ai-update-config",
                           rc,
                           rc == 0 ? "submitted" : "send-failed",
                           NULL);
}

static void handle_action_result(session_context_t *context,
                                 const session_internal_event_t *event)
{
    if (session_ai_call_reserved(context)) {
        session_emit_operation(context,
                               "ai-action-result",
                               ESP_ERR_INVALID_STATE,
                               "action-owned-by-firmware",
                               NULL);
        return;
    }
    if (context->ai.phase != SESSION_AI_PHASE_ACTIVE ||
        context->ai.action_id[0] == '\0' ||
        strcmp(context->ai.action_id, event->first) != 0) {
        session_emit_operation(context,
                               "ai-action-result",
                               ESP_ERR_INVALID_STATE,
                               "action-id-not-pending",
                               NULL);
        return;
    }
    int rc = session_ai_complete_action(context,
                                        event->flag,
                                        event->second);
    session_emit_operation(context,
                           "ai-action-result",
                           rc,
                           rc == 0 ? "submitted" : "send-failed",
                           NULL);
}

static void finish_local_ai(session_context_t *context,
                            const char *reason,
                            int terminal_status)
{
    int end_rc = 0;
    bool had_connection = tirtc_adapter_has_connection();
    tirtc_adapter_metrics_t stop_metrics = {0};
    char stop_evidence[128] = {0};
    tirtc_adapter_get_metrics(&stop_metrics);
    (void)snprintf(
        stop_evidence,
        sizeof(stop_evidence),
        "{\"connect_request_pending\":%s,"
        "\"connect_callback_pending\":%s}",
        stop_metrics.connect_request_pending ? "true" : "false",
        stop_metrics.connect_callback_pending ? "true" : "false");
    media_runtime_set_uplink_active(false);
    if (had_connection) {
        end_rc = send_notification(context, "end_session", NULL);
    }
    session_emit_operation(context,
                           "ai-stop",
                           end_rc,
                           !had_connection
                               ? "local-cleanup-no-connection"
                               : (end_rc == 0 ? "end-submitted"
                                              : "end-send-failed"),
                           stop_evidence);
    context->ai.phase = SESSION_AI_PHASE_ENDING;
    session_finish(context,
                   reason,
                   terminal_status != 0 ? terminal_status : end_rc);
}

void session_ai_end_for_call_handoff(session_context_t *context)
{
    if (context != NULL && context->owner == DEVICE_SERVICE_AI) {
        finish_local_ai(context, "ai-call-device-transfer", 0);
    }
}

bool session_ai_handles(const session_internal_event_t *event,
                        const session_context_t *context)
{
    if (event->type == SESSION_INT_HTTP_RESPONSE &&
        event->http_request == SESSION_HTTP_AI_TOKEN) {
        return true;
    }
    if (context->owner != DEVICE_SERVICE_AI) {
        return false;
    }
    return event->type == SESSION_INT_ADAPTER_CONNECTION ||
           event->type == SESSION_INT_MEDIA_PROMPT_DONE ||
           (event->type == SESSION_INT_ADAPTER_COMMAND &&
            event->command == SESSION_COMMAND_AI);
}

void session_ai_handle(session_context_t *context,
                       const session_internal_event_t *event)
{
    bool handoff_reserved = session_ai_call_reserved(context);
    switch (event->type) {
    case SESSION_INT_MEDIA_PROMPT_DONE:
        handle_ai_prompt_done(context, event);
        break;
    case SESSION_INT_AI_START:
        handle_ai_start_intent(context);
        break;
    case SESSION_INT_AI_INTERRUPT:
        if (handoff_reserved) {
            session_emit_operation(context,
                                   "ai-interrupt",
                                   ESP_ERR_INVALID_STATE,
                                   "call-handoff-committed",
                                   NULL);
        } else {
            send_simple_control(context, "interrupt", "ai-interrupt");
        }
        break;
    case SESSION_INT_AI_SUBMIT:
        if (handoff_reserved) {
            session_emit_operation(context,
                                   "ai-submit-speech",
                                   ESP_ERR_INVALID_STATE,
                                   "call-handoff-committed",
                                   NULL);
        } else {
            send_simple_control(context,
                                "submit_speech",
                                "ai-submit-speech");
        }
        break;
    case SESSION_INT_AI_UPDATE:
        if (handoff_reserved) {
            session_emit_operation(context,
                                   "ai-update-config",
                                   ESP_ERR_INVALID_STATE,
                                   "call-handoff-committed",
                                   NULL);
        } else {
            handle_update_config(context, event->first);
        }
        break;
    case SESSION_INT_AI_PROMPT:
        if (handoff_reserved) {
            session_emit_operation(context,
                                   "ai-prompt",
                                   ESP_ERR_INVALID_STATE,
                                   "call-handoff-committed",
                                   NULL);
        } else {
            handle_ai_prompt(context, event->first);
        }
        break;
    case SESSION_INT_AI_ACTION_RESULT:
        handle_action_result(context, event);
        break;
    case SESSION_INT_AI_STOP:
        if (context->owner == DEVICE_SERVICE_AI) {
            if (handoff_reserved) {
                session_emit_operation(context,
                                       "ai-stop",
                                       ESP_ERR_INVALID_STATE,
                                       "call-handoff-committed",
                                       NULL);
            } else {
                finish_local_ai(context, "ai-local-stop", 0);
            }
        }
        break;
    case SESSION_INT_HTTP_RESPONSE:
        handle_ai_token(context, event);
        break;
    case SESSION_INT_ADAPTER_CONNECTION:
        handle_connection(context, event);
        break;
    case SESSION_INT_ADAPTER_COMMAND:
        handle_ai_command(context, event);
        break;
    default:
        break;
    }
}

void session_ai_tick(session_context_t *context, int64_t current_ms)
{
    if (context->owner != DEVICE_SERVICE_AI) {
        return;
    }
    if (context->ai.phase == SESSION_AI_PHASE_DELAY &&
        context->deadline_ms != 0 && current_ms >= context->deadline_ms) {
        send_start_session(context);
        return;
    }
    if ((context->ai.phase == SESSION_AI_PHASE_TOKEN ||
         context->ai.phase == SESSION_AI_PHASE_WHIP ||
         context->ai.phase == SESSION_AI_PHASE_START_RESPONSE) &&
        context->deadline_ms != 0 && current_ms >= context->deadline_ms) {
        session_finish(context, "ai-phase-timeout", ESP_ERR_TIMEOUT);
        return;
    }
    if (!session_ai_call_owns_action(context) &&
        context->ai.action_deadline_ms != 0 &&
        current_ms >= context->ai.action_deadline_ms) {
        uint32_t previous_origin = context->event_origin_request_id;
        context->event_origin_request_id = 0U;
        int rc = session_ai_complete_action(context,
                                            false,
                                            "device action timed out");
        session_emit_operation(context,
                               "ai-action-result",
                               rc == 0 ? ESP_ERR_TIMEOUT : rc,
                               rc == 0
                                   ? "action-timeout"
                                   : "timeout-response-send-failed",
                               NULL);
        context->event_origin_request_id = previous_origin;
    }
    if (context->ai.update_deadline_ms != 0 &&
        current_ms >= context->ai.update_deadline_ms) {
        uint32_t previous_origin = context->event_origin_request_id;
        context->event_origin_request_id =
            context->ai.update_origin_request_id;
        session_emit_operation(context,
                               "ai-update-config",
                               ESP_ERR_TIMEOUT,
                               "response-timeout",
                               NULL);
        context->ai.update_request_id[0] = '\0';
        context->ai.update_origin_request_id = 0U;
        context->ai.update_deadline_ms = 0;
        session_refresh_snapshot(context);
        context->event_origin_request_id = previous_origin;
    }
    if (context->ai.prompt_pending && context->ai.prompt_draining) {
        uint32_t previous_origin = context->event_origin_request_id;
        context->event_origin_request_id =
            context->ai.prompt_origin_request_id;
        char detail[64];
        (void)snprintf(detail,
                       sizeof(detail),
                       "{\"preset\":\"%s\"}",
                       context->ai.prompt_name);
        size_t used_bytes = 0U;
        int rc = tirtc_adapter_get_send_buffer_used(context->generation,
                                                    &used_bytes);
        if (rc == 0 && used_bytes == 0U) {
            rc = send_notification(context, "submit_speech", NULL);
            if (rc == 0) {
                clear_ai_prompt(context);
                session_emit_operation(context,
                                       "ai-prompt",
                                       0,
                                       "completed",
                                       detail);
            } else {
                fail_ai_prompt_and_close(context,
                                         rc,
                                         "submit-failed",
                                         detail);
            }
        } else if (context->ai.prompt_deadline_ms != 0 &&
                   current_ms >= context->ai.prompt_deadline_ms) {
            int status = rc == 0 ? ESP_ERR_TIMEOUT : rc;
            fail_ai_prompt_and_close(context,
                                     status,
                                     "media-drain-timeout",
                                     detail);
        }
        context->event_origin_request_id = previous_origin;
    } else if (context->ai.prompt_pending &&
        context->ai.prompt_deadline_ms != 0 &&
        current_ms >= context->ai.prompt_deadline_ms) {
        uint32_t previous_origin = context->event_origin_request_id;
        context->event_origin_request_id =
            context->ai.prompt_origin_request_id;
        char detail[64];
        (void)snprintf(detail,
                       sizeof(detail),
                       "{\"preset\":\"%s\"}",
                       context->ai.prompt_name);
        fail_ai_prompt_and_close(context,
                                 ESP_ERR_TIMEOUT,
                                 "media-timeout",
                                 detail);
        context->event_origin_request_id = previous_origin;
    }
}
