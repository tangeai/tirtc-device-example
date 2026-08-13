#include "session_runtime_internal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "media_runtime.h"
#include "platform_client.h"

#define AI_CALL_RESPONSE_DRAIN_TIMEOUT_MS 5000
#define AI_CALL_ADAPTER_DRAIN_TIMEOUT_MS 10000
#define AI_CALL_RETRY_DELAY_MS 2000
#define AI_CALL_MAX_RETRIES 6U
#define AI_CALL_ACTION_RESPONSE_RETRY_MS 250
#define AI_CALL_BUSY_RESPONSE_TIMEOUT_MS 2000

static void clear_handoff(session_context_t *context)
{
    if (context == NULL) {
        return;
    }
    session_secure_zero(&context->ai_call_handoff,
                        sizeof(context->ai_call_handoff));
    session_refresh_snapshot(context);
}

static void clear_busy_response(session_context_t *context)
{
    context->ai_call_handoff.busy_action_id_json[0] = '\0';
    context->ai_call_handoff.busy_response_retry_count = 0U;
    context->ai_call_handoff.busy_response_retry_at_ms = 0;
    context->ai_call_handoff.busy_response_deadline_ms = 0;
    session_refresh_snapshot(context);
}

void session_ai_call_reject_concurrent_action(
    session_context_t *context,
    const char *action_id_json)
{
    if (context == NULL || action_id_json == NULL ||
        action_id_json[0] == '\0' ||
        !session_ai_call_reserved(context)) {
        return;
    }

    session_handoff_action_disposition_t disposition =
        session_handoff_classify_concurrent_action(
            session_ai_call_response_frozen(context),
            context->ai_call_handoff.action_id_json,
            context->ai_call_handoff.busy_action_id_json,
            context->ai_call_handoff.completed_busy_action_id_json,
            action_id_json);
    if (disposition == SESSION_HANDOFF_ACTION_PRIMARY_DUPLICATE) {
        session_emit_operation(context,
                               "ai-call-device",
                               0,
                               "duplicate-request-ignored",
                               NULL);
        return;
    }
    if (disposition == SESSION_HANDOFF_ACTION_RESPONSE_FROZEN) {
        session_emit_operation(context,
                               "ai-call-device",
                               ESP_ERR_INVALID_STATE,
                               "handoff-closing-request-ignored",
                               NULL);
        return;
    }
    if (disposition == SESSION_HANDOFF_ACTION_BUSY_DUPLICATE) {
        session_emit_operation(context,
                               "ai-call-device",
                               0,
                               "handoff-busy-duplicate-ignored",
                               NULL);
        return;
    }
    if (disposition == SESSION_HANDOFF_ACTION_RETRY_OVERFLOW) {
        session_emit_operation(context,
                               "ai-call-device",
                               ESP_ERR_NO_MEM,
                               "handoff-busy-response-overflow",
                               NULL);
        session_finish(context,
                       "ai-call-device-busy-response-overflow",
                       ESP_ERR_NO_MEM);
        return;
    }

    int send_status = session_ai_send_action_response(
        context,
        action_id_json,
        false,
        "a device call handoff is already in progress");
    if (send_status == 0) {
        if (!session_copy_string(
                context->ai_call_handoff.completed_busy_action_id_json,
                sizeof(context->ai_call_handoff
                           .completed_busy_action_id_json),
                action_id_json)) {
            session_emit_diagnostic(
                context,
                ESP_ERR_INVALID_SIZE,
                "ai-device-action-handoff-busy-id-too-large");
            session_finish(context,
                           "ai-call-device-busy-id-invalid",
                           ESP_ERR_INVALID_SIZE);
            return;
        }
        session_emit_operation(context,
                               "ai-call-device",
                               ESP_ERR_INVALID_STATE,
                               "handoff-busy",
                               NULL);
        return;
    }
    if (!session_copy_string(
            context->ai_call_handoff.busy_action_id_json,
            sizeof(context->ai_call_handoff.busy_action_id_json),
            action_id_json)) {
        session_emit_diagnostic(
            context,
            ESP_ERR_INVALID_SIZE,
            "ai-device-action-handoff-busy-id-too-large");
        session_finish(context,
                       "ai-call-device-busy-id-invalid",
                       ESP_ERR_INVALID_SIZE);
        return;
    }
    int64_t current_ms = session_now_ms();
    context->ai_call_handoff.busy_response_retry_count = 1U;
    context->ai_call_handoff.busy_response_retry_at_ms =
        current_ms + AI_CALL_ACTION_RESPONSE_RETRY_MS;
    context->ai_call_handoff.busy_response_deadline_ms =
        current_ms + AI_CALL_BUSY_RESPONSE_TIMEOUT_MS;
    session_refresh_snapshot(context);
    session_emit_operation(context,
                           "ai-call-device",
                           send_status,
                           "handoff-busy-response-retry",
                           NULL);
    session_emit_diagnostic(
        context,
        send_status,
        "ai-device-action-handoff-busy-response-send-failed");
}

static void retry_busy_response(session_context_t *context,
                                int64_t current_ms)
{
    if (context->ai_call_handoff.busy_action_id_json[0] == '\0') {
        return;
    }
    if (context->owner != DEVICE_SERVICE_AI ||
        context->generation !=
            context->ai_call_handoff.ai_generation) {
        clear_busy_response(context);
        return;
    }
    if (context->ai_call_handoff.phase !=
            SESSION_RUNTIME_HANDOFF_RESOLVING &&
        context->ai_call_handoff.phase !=
            SESSION_RUNTIME_HANDOFF_ACTION_RESPONSE) {
        session_emit_diagnostic(
            context,
            ESP_ERR_INVALID_STATE,
            "ai-device-action-handoff-busy-response-frozen");
        clear_busy_response(context);
        return;
    }
    session_handoff_retry_disposition_t retry_disposition =
        session_handoff_classify_retry(
            current_ms,
            context->ai_call_handoff.busy_response_retry_at_ms,
            context->ai_call_handoff.busy_response_deadline_ms);
    if (retry_disposition == SESSION_HANDOFF_RETRY_EXPIRED) {
        session_emit_diagnostic(
            context,
            ESP_ERR_TIMEOUT,
            "ai-device-action-handoff-busy-response-retry-exhausted");
        session_finish(context,
                       "ai-call-device-busy-response-failed",
                       ESP_ERR_TIMEOUT);
        return;
    }
    if (retry_disposition == SESSION_HANDOFF_RETRY_WAIT) {
        return;
    }

    int rc = session_ai_send_action_response(
        context,
        context->ai_call_handoff.busy_action_id_json,
        false,
        "a device call handoff is already in progress");
    if (rc == 0) {
        if (!session_copy_string(
                context->ai_call_handoff.completed_busy_action_id_json,
                sizeof(context->ai_call_handoff
                           .completed_busy_action_id_json),
                context->ai_call_handoff.busy_action_id_json)) {
            session_emit_diagnostic(
                context,
                ESP_ERR_INVALID_SIZE,
                "ai-device-action-handoff-busy-id-too-large");
            session_finish(context,
                           "ai-call-device-busy-id-invalid",
                           ESP_ERR_INVALID_SIZE);
            return;
        }
        clear_busy_response(context);
        session_emit_operation(context,
                               "ai-call-device",
                               0,
                               "handoff-busy-response-submitted",
                               NULL);
        return;
    }

    context->ai_call_handoff.busy_response_retry_count++;
    int64_t failure_ms = session_now_ms();
    if (failure_ms >=
            context->ai_call_handoff.busy_response_deadline_ms ||
        context->ai_call_handoff.busy_response_retry_count >=
            AI_CALL_MAX_RETRIES) {
        clear_busy_response(context);
        session_emit_diagnostic(
            context,
            rc,
            "ai-device-action-handoff-busy-response-retry-exhausted");
        session_finish(context,
                       "ai-call-device-busy-response-failed",
                       rc);
        return;
    }
    context->ai_call_handoff.busy_response_retry_at_ms =
        failure_ms + AI_CALL_ACTION_RESPONSE_RETRY_MS *
                         context->ai_call_handoff.busy_response_retry_count;
    session_refresh_snapshot(context);
    session_emit_operation(context,
                           "ai-call-device",
                           rc,
                           "handoff-busy-response-retry",
                           NULL);
}

static void arm_action_response(session_context_t *context,
                                bool ok,
                                int status,
                                const char *message,
                                const char *phase,
                                int send_status,
                                uint8_t retry_count,
                                const char *operation_phase)
{
    int64_t current_ms = session_now_ms();
    int64_t action_deadline = context->ai.action_deadline_ms;
    context->ai_call_handoff.phase =
        SESSION_RUNTIME_HANDOFF_ACTION_RESPONSE;
    context->ai_call_handoff.ai_generation = context->generation;
    context->ai_call_handoff.retry_count = retry_count;
    context->ai_call_handoff.response_ok = ok;
    context->ai_call_handoff.failure_status = status;
    context->ai_call_handoff.deadline_ms =
        current_ms +
        (retry_count == 0U ? 0 : AI_CALL_ACTION_RESPONSE_RETRY_MS);
    context->ai_call_handoff.terminal_deadline_ms =
        action_deadline > current_ms + AI_CALL_RETRY_DELAY_MS
            ? action_deadline
            : current_ms + AI_CALL_RETRY_DELAY_MS;
    (void)session_copy_string(
        context->ai_call_handoff.action_id_json,
        sizeof(context->ai_call_handoff.action_id_json),
        context->ai.action_id_json);
    (void)session_copy_string(
        context->ai_call_handoff.failure_phase,
        sizeof(context->ai_call_handoff.failure_phase),
        phase == NULL ? "action-failed" : phase);
    (void)session_copy_string(
        context->ai_call_handoff.failure_message,
        sizeof(context->ai_call_handoff.failure_message),
        message == NULL ? "device action failed" : message);
    session_refresh_snapshot(context);
    session_emit_operation(context,
                           "ai-call-device",
                           send_status,
                           operation_phase,
                           NULL);
}

static void fail_pending_action(session_context_t *context,
                                int status,
                                const char *message,
                                const char *phase)
{
    if (context->ai_call_handoff.busy_action_id_json[0] != '\0') {
        arm_action_response(context,
                            false,
                            status,
                            message,
                            phase,
                            ESP_ERR_INVALID_STATE,
                            0U,
                            "response-waiting-for-busy");
        return;
    }
    int rc = session_ai_complete_action(context, false, message);
    if (rc == 0) {
        session_emit_operation(context,
                               "ai-call-device",
                               status,
                               phase,
                               NULL);
        clear_handoff(context);
        return;
    }
    arm_action_response(context,
                        false,
                        status,
                        message,
                        phase,
                        rc,
                        1U,
                        "response-send-retry");
}

static bool adapter_quiescent(void)
{
    tirtc_adapter_metrics_t metrics = {0};
    tirtc_adapter_get_metrics(&metrics);
    return metrics.adapter_state == TIRTC_ADAPTER_RUNNING &&
           !metrics.connected &&
           metrics.active_profile == TIRTC_ADAPTER_MEDIA_NONE &&
           !metrics.connect_request_pending &&
           !metrics.connect_callback_pending &&
           metrics.accept_callbacks_pending == 0U &&
           metrics.disconnects_pending == 0U &&
           metrics.connection_users == 0U &&
           !metrics.incoming_armed;
}

bool session_ai_call_reserved(const session_context_t *context)
{
    return context != NULL &&
           context->ai_call_handoff.phase != SESSION_RUNTIME_HANDOFF_NONE;
}

bool session_ai_call_response_frozen(const session_context_t *context)
{
    return context != NULL &&
           (context->ai_call_handoff.phase ==
                SESSION_RUNTIME_HANDOFF_RESPONSE_DRAIN ||
            context->ai_call_handoff.phase ==
                SESSION_RUNTIME_HANDOFF_ADAPTER_DRAIN);
}

bool session_ai_call_owns_action(const session_context_t *context)
{
    return context != NULL &&
           (context->ai_call_handoff.phase ==
                SESSION_RUNTIME_HANDOFF_RESOLVING ||
            context->ai_call_handoff.phase ==
                SESSION_RUNTIME_HANDOFF_ACTION_RESPONSE);
}

void session_ai_call_cancel(session_context_t *context)
{
    clear_handoff(context);
}

bool session_ai_call_begin(session_context_t *context,
                           const cJSON *params)
{
    if (context == NULL) {
        return false;
    }
    const cJSON *data = cJSON_IsObject(params)
                            ? cJSON_GetObjectItemCaseSensitive(params, "data")
                            : NULL;
    const cJSON *target = cJSON_IsObject(data)
                              ? cJSON_GetObjectItemCaseSensitive(data, "target")
                              : NULL;
    if (context->owner != DEVICE_SERVICE_AI ||
        context->ai.phase != SESSION_AI_PHASE_ACTIVE) {
        fail_pending_action(context,
                            ESP_ERR_INVALID_STATE,
                            "The AI session is not ready for a device call.",
                            "ai-not-active");
        return true;
    }
    if (session_ai_call_reserved(context)) {
        int rc = session_ai_complete_action(
            context,
            false,
            "Another device call handoff is already pending.");
        session_emit_operation(context,
                               "ai-call-device",
                               rc == 0 ? ESP_ERR_INVALID_STATE : rc,
                               rc == 0 ? "handoff-busy"
                                       : "response-send-failed",
                               NULL);
        return true;
    }
    if (!cJSON_IsString(target) || target->valuestring == NULL ||
        target->valuestring[0] == '\0' ||
        strlen(target->valuestring) >=
            sizeof(context->ai_call_handoff.target)) {
        fail_pending_action(
            context,
            ESP_ERR_INVALID_ARG,
            "Please specify one contact remark or device ID.",
            "target-invalid");
        return true;
    }

    context->ai_call_handoff.phase =
        SESSION_RUNTIME_HANDOFF_RESOLVING;
    context->ai_call_handoff.ai_generation = context->generation;
    (void)session_copy_string(
        context->ai_call_handoff.action_id_json,
        sizeof(context->ai_call_handoff.action_id_json),
        context->ai.action_id_json);
    context->http_sequence++;
    if (context->http_sequence == 0U) {
        context->http_sequence = 1U;
    }
    context->ai_call_handoff.contacts_request_cookie =
        context->http_sequence;
    context->ai_call_handoff.deadline_ms =
        context->ai.action_deadline_ms;
    if (!session_copy_string(context->ai_call_handoff.target,
                             sizeof(context->ai_call_handoff.target),
                             target->valuestring)) {
        fail_pending_action(
            context,
            ESP_ERR_INVALID_SIZE,
            "The requested contact target is too long.",
            "target-too-long");
        return true;
    }
    session_refresh_snapshot(context);

    esp_err_t err = session_submit_http_correlated(
        context,
        SESSION_HTTP_AI_CALL_CONTACTS,
        context->generation,
        context->ai_call_handoff.contacts_request_cookie,
        "GET",
        "/v1/call/device/contacts",
        NULL);
    if (err != ESP_OK) {
        fail_pending_action(
            context,
            err,
            "The contact list is unavailable. Please try again.",
            "contacts-submit-failed");
        return true;
    }
    session_emit_operation(context,
                           "ai-call-device",
                           0,
                           "contacts-refresh-submitted",
                           NULL);
    return true;
}

void session_ai_call_contacts_response(
    session_context_t *context,
    const session_internal_event_t *event,
    bool cache_valid)
{
    if (context == NULL || event == NULL ||
        context->ai_call_handoff.phase !=
            SESSION_RUNTIME_HANDOFF_RESOLVING ||
        context->owner != DEVICE_SERVICE_AI ||
        event->generation != context->generation ||
        !session_handoff_request_matches(
            context->ai_call_handoff.ai_generation,
            context->ai_call_handoff.contacts_request_cookie,
            event->generation,
            event->request_cookie)) {
        return;
    }
    int64_t current_ms = session_now_ms();
    if (context->ai_call_handoff.deadline_ms != 0 &&
        current_ms >= context->ai_call_handoff.deadline_ms) {
        fail_pending_action(
            context,
            ESP_ERR_TIMEOUT,
            "The contact lookup timed out. Please try again.",
            "contacts-timeout");
        return;
    }
    if (!cache_valid || !context->device_contact_cache_complete) {
        fail_pending_action(
            context,
            ESP_ERR_INVALID_RESPONSE,
            "The contact list is unavailable. Please try again.",
            "contacts-invalid");
        return;
    }

    session_contact_resolution_t resolution =
        session_contact_resolve(context->device_contacts,
                                context->device_contact_count,
                                context->ai_call_handoff.target);
    switch (resolution.status) {
    case SESSION_CONTACT_RESOLVE_FOUND:
        break;
    case SESSION_CONTACT_RESOLVE_OFFLINE:
        fail_pending_action(
            context,
            ESP_ERR_INVALID_STATE,
            "The target device is offline. Please try again later.",
            "target-offline");
        return;
    case SESSION_CONTACT_RESOLVE_AMBIGUOUS:
        fail_pending_action(
            context,
            ESP_ERR_INVALID_ARG,
            "More than one contact matched. Please use a unique remark or device ID.",
            "target-ambiguous");
        return;
    case SESSION_CONTACT_RESOLVE_INVALID:
        fail_pending_action(
            context,
            ESP_ERR_INVALID_ARG,
            "Please specify one contact remark or device ID.",
            "target-invalid");
        return;
    case SESSION_CONTACT_RESOLVE_NOT_FOUND:
    default:
        fail_pending_action(
            context,
            ESP_ERR_NOT_FOUND,
            "No contact matched. Please say the remark or device ID clearly.",
            "target-not-found");
        return;
    }

    if (resolution.index >= context->device_contact_count ||
        !session_copy_string(
            context->ai_call_handoff.target_device_id,
            sizeof(context->ai_call_handoff.target_device_id),
            context->device_contacts[resolution.index].device_id)) {
        fail_pending_action(
            context,
            ESP_ERR_INVALID_RESPONSE,
            "The matched contact is invalid. Please try again.",
            "target-invalid");
        return;
    }

    media_runtime_set_uplink_active(false);
    if (context->ai_call_handoff.busy_action_id_json[0] != '\0') {
        arm_action_response(
            context,
            true,
            0,
            "Call request accepted. Switching to the target device.",
            "response-submitted",
            ESP_ERR_INVALID_STATE,
            0U,
            "response-waiting-for-busy");
        return;
    }
    int rc = session_ai_complete_action(
        context,
        true,
        "Call request accepted. Switching to the target device.");
    if (rc != 0) {
        arm_action_response(
            context,
            true,
            0,
            "Call request accepted. Switching to the target device.",
            "response-submitted",
            rc,
            1U,
            "response-send-retry");
        return;
    }

    context->ai_call_handoff.phase =
        SESSION_RUNTIME_HANDOFF_RESPONSE_DRAIN;
    context->ai_call_handoff.deadline_ms =
        session_now_ms() + AI_CALL_RESPONSE_DRAIN_TIMEOUT_MS;
    session_refresh_snapshot(context);
    session_emit_operation(context,
                           "ai-call-device",
                           0,
                           "response-submitted",
                           NULL);
}

bool session_ai_call_prepare_ai_finish(session_context_t *context)
{
    if (context == NULL ||
        context->ai_call_handoff.phase !=
            SESSION_RUNTIME_HANDOFF_RESPONSE_DRAIN) {
        return false;
    }
    context->ai_call_handoff.phase =
        SESSION_RUNTIME_HANDOFF_ADAPTER_DRAIN;
    context->ai_call_handoff.retry_count = 0U;
    context->ai_call_handoff.terminal_deadline_ms = 0;
    context->ai_call_handoff.deadline_ms =
        session_now_ms() + AI_CALL_ADAPTER_DRAIN_TIMEOUT_MS;
    session_refresh_snapshot(context);
    return true;
}

static bool start_reserved_call(session_context_t *context,
                                int64_t current_ms)
{
    char target[SESSION_RUNTIME_ID_MAX] = {0};
    if (!session_copy_string(target,
                             sizeof(target),
                             context->ai_call_handoff.target_device_id)) {
        session_emit_operation(context,
                               "ai-call-device",
                               ESP_ERR_INVALID_RESPONSE,
                               "reserved-target-invalid",
                               NULL);
        clear_handoff(context);
        return false;
    }

    bool started = session_call_start_handoff(context, target);
    session_secure_zero(target, sizeof(target));
    if (started) {
        clear_handoff(context);
        return true;
    }

    context->ai_call_handoff.call_start_retry_count++;
    if (context->ai_call_handoff.call_start_retry_count >=
        AI_CALL_MAX_RETRIES) {
        session_emit_operation(context,
                               "ai-call-device",
                               ESP_ERR_TIMEOUT,
                               "call-start-terminal-failed",
                               NULL);
        clear_handoff(context);
        return false;
    }
    context->ai_call_handoff.deadline_ms =
        current_ms + AI_CALL_RETRY_DELAY_MS;
    session_refresh_snapshot(context);
    session_emit_operation(context,
                           "ai-call-device",
                           ESP_ERR_INVALID_STATE,
                           "call-start-retry",
                           NULL);
    return false;
}

void session_ai_call_tick(session_context_t *context, int64_t current_ms)
{
    if (context == NULL || !session_ai_call_reserved(context)) {
        return;
    }
    retry_busy_response(context, current_ms);
    if (!session_ai_call_reserved(context)) {
        return;
    }

    switch (context->ai_call_handoff.phase) {
    case SESSION_RUNTIME_HANDOFF_RESOLVING:
        if (context->owner != DEVICE_SERVICE_AI ||
            context->generation !=
                context->ai_call_handoff.ai_generation) {
            clear_handoff(context);
            return;
        }
        if (context->ai_call_handoff.deadline_ms != 0 &&
            current_ms >= context->ai_call_handoff.deadline_ms) {
            fail_pending_action(
                context,
                ESP_ERR_TIMEOUT,
                "The contact lookup timed out. Please try again.",
                "contacts-timeout");
        }
        return;

    case SESSION_RUNTIME_HANDOFF_ACTION_RESPONSE: {
        if (context->owner != DEVICE_SERVICE_AI ||
            context->generation !=
                context->ai_call_handoff.ai_generation) {
            clear_handoff(context);
            return;
        }
        if (context->ai_call_handoff.terminal_deadline_ms != 0 &&
            current_ms >=
                context->ai_call_handoff.terminal_deadline_ms) {
            session_emit_operation(context,
                                   "ai-call-device",
                                   ESP_ERR_TIMEOUT,
                                   "response-retry-exhausted",
                                   NULL);
            session_finish(context,
                           "ai-call-device-response-failed",
                           ESP_ERR_TIMEOUT);
            return;
        }
        if (context->ai_call_handoff.busy_action_id_json[0] != '\0') {
            return;
        }
        if (current_ms < context->ai_call_handoff.deadline_ms) {
            return;
        }
        int rc = session_ai_complete_action(
            context,
            context->ai_call_handoff.response_ok,
            context->ai_call_handoff.failure_message);
        if (rc == 0) {
            if (context->ai_call_handoff.response_ok) {
                context->ai_call_handoff.phase =
                    SESSION_RUNTIME_HANDOFF_RESPONSE_DRAIN;
                context->ai_call_handoff.retry_count = 0U;
                context->ai_call_handoff.terminal_deadline_ms = 0;
                context->ai_call_handoff.deadline_ms =
                    current_ms + AI_CALL_RESPONSE_DRAIN_TIMEOUT_MS;
                session_refresh_snapshot(context);
                session_emit_operation(context,
                                       "ai-call-device",
                                       0,
                                       "response-submitted",
                                       NULL);
                return;
            }
            int failure_status =
                context->ai_call_handoff.failure_status;
            char failure_phase[65] = {0};
            (void)session_copy_string(
                failure_phase,
                sizeof(failure_phase),
                context->ai_call_handoff.failure_phase);
            clear_handoff(context);
            session_emit_operation(context,
                                   "ai-call-device",
                                   failure_status,
                                   failure_phase,
                                   NULL);
            return;
        }
        context->ai_call_handoff.retry_count++;
        if (current_ms >=
                context->ai_call_handoff.terminal_deadline_ms ||
            context->ai_call_handoff.retry_count >=
                AI_CALL_MAX_RETRIES) {
            session_emit_operation(context,
                                   "ai-call-device",
                                   rc,
                                   "response-retry-exhausted",
                                   NULL);
            session_finish(context,
                           "ai-call-device-response-failed",
                           rc);
            return;
        }
        context->ai_call_handoff.deadline_ms =
            current_ms + AI_CALL_ACTION_RESPONSE_RETRY_MS *
                             context->ai_call_handoff.retry_count;
        session_refresh_snapshot(context);
        session_emit_operation(context,
                               "ai-call-device",
                               rc,
                               "response-send-retry",
                               NULL);
        return;
    }

    case SESSION_RUNTIME_HANDOFF_RESPONSE_DRAIN: {
        if (context->owner != DEVICE_SERVICE_AI ||
            context->generation !=
                context->ai_call_handoff.ai_generation) {
            clear_handoff(context);
            return;
        }
        size_t used_bytes = 0U;
        int rc = tirtc_adapter_get_send_buffer_used(
            context->ai_call_handoff.ai_generation,
            &used_bytes);
        if (rc == 0 && used_bytes == 0U) {
            session_emit_operation(context,
                                   "ai-call-device",
                                   0,
                                   "response-drained",
                                   "{\"send_buffer_bytes\":0}");
            if (session_ai_call_prepare_ai_finish(context)) {
                session_ai_end_for_call_handoff(context);
            }
            return;
        }
        if (context->ai_call_handoff.deadline_ms != 0 &&
            current_ms >= context->ai_call_handoff.deadline_ms) {
            int failure_status =
                rc == 0 ? ESP_ERR_TIMEOUT : rc;
            session_emit_operation(context,
                                   "ai-call-device",
                                   failure_status,
                                   "response-drain-terminal-failed",
                                   NULL);
            session_ai_call_cancel(context);
            session_finish(context,
                           "ai-call-device-response-undrained",
                           failure_status);
        }
        return;
    }

    case SESSION_RUNTIME_HANDOFF_ADAPTER_DRAIN:
        if (context->owner != DEVICE_SERVICE_NONE) {
            session_emit_operation(context,
                                   "ai-call-device",
                                   ESP_ERR_INVALID_STATE,
                                   "adapter-drain-owner-conflict",
                                   NULL);
            clear_handoff(context);
            return;
        }
        if ((context->ai_call_handoff.adapter_retry_count > 0U ||
             context->ai_call_handoff.call_start_retry_count > 0U) &&
            current_ms < context->ai_call_handoff.deadline_ms) {
            return;
        }
        if (adapter_quiescent()) {
            session_emit_operation(context,
                                   "ai-call-device",
                                   0,
                                   "adapter-drained",
                                   NULL);
            (void)start_reserved_call(context, current_ms);
            return;
        }
        if (context->ai_call_handoff.deadline_ms != 0 &&
            current_ms >= context->ai_call_handoff.deadline_ms) {
            context->ai_call_handoff.adapter_retry_count++;
            session_emit_operation(context,
                                   "ai-call-device",
                                   ESP_ERR_TIMEOUT,
                                   "adapter-drain-retry",
                                   NULL);
            (void)tirtc_adapter_cancel_connect();
            (void)tirtc_adapter_disconnect();
            (void)tirtc_adapter_set_media_profile(
                TIRTC_ADAPTER_MEDIA_NONE,
                0,
                false);
            if (context->ai_call_handoff.adapter_retry_count >=
                AI_CALL_MAX_RETRIES) {
                session_emit_operation(context,
                                       "ai-call-device",
                                       ESP_ERR_TIMEOUT,
                                       "adapter-drain-terminal-failed",
                                       NULL);
                clear_handoff(context);
                return;
            }
            context->ai_call_handoff.deadline_ms =
                current_ms + AI_CALL_RETRY_DELAY_MS;
            session_refresh_snapshot(context);
        }
        return;

    case SESSION_RUNTIME_HANDOFF_NONE:
    default:
        return;
    }
}
