#include "session_handoff_guard.h"

#include <string.h>

bool session_handoff_request_matches(uint32_t expected_generation,
                                     uint32_t expected_cookie,
                                     uint32_t actual_generation,
                                     uint32_t actual_cookie)
{
    return expected_generation != 0U && expected_cookie != 0U &&
           expected_generation == actual_generation &&
           expected_cookie == actual_cookie;
}

bool session_handoff_action_id_matches(const char *expected_json,
                                       const char *actual_json)
{
    return expected_json != NULL && actual_json != NULL &&
           expected_json[0] != '\0' &&
           strcmp(expected_json, actual_json) == 0;
}

session_handoff_action_disposition_t
session_handoff_classify_concurrent_action(
    bool response_frozen,
    const char *primary_action_id_json,
    const char *pending_busy_action_id_json,
    const char *completed_busy_action_id_json,
    const char *incoming_action_id_json)
{
    if (session_handoff_action_id_matches(primary_action_id_json,
                                          incoming_action_id_json)) {
        return SESSION_HANDOFF_ACTION_PRIMARY_DUPLICATE;
    }
    if (response_frozen) {
        return SESSION_HANDOFF_ACTION_RESPONSE_FROZEN;
    }
    if (pending_busy_action_id_json != NULL &&
        pending_busy_action_id_json[0] != '\0') {
        return session_handoff_action_id_matches(
                   pending_busy_action_id_json,
                   incoming_action_id_json)
                   ? SESSION_HANDOFF_ACTION_BUSY_DUPLICATE
                   : SESSION_HANDOFF_ACTION_RETRY_OVERFLOW;
    }
    if (completed_busy_action_id_json != NULL &&
        completed_busy_action_id_json[0] != '\0') {
        return session_handoff_action_id_matches(
                   completed_busy_action_id_json,
                   incoming_action_id_json)
                   ? SESSION_HANDOFF_ACTION_BUSY_DUPLICATE
                   : SESSION_HANDOFF_ACTION_RETRY_OVERFLOW;
    }
    return SESSION_HANDOFF_ACTION_REJECT_NOW;
}

session_handoff_retry_disposition_t session_handoff_classify_retry(
    int64_t current_ms,
    int64_t retry_at_ms,
    int64_t deadline_ms)
{
    if (deadline_ms != 0 && current_ms >= deadline_ms) {
        return SESSION_HANDOFF_RETRY_EXPIRED;
    }
    return current_ms < retry_at_ms ? SESSION_HANDOFF_RETRY_WAIT
                                    : SESSION_HANDOFF_RETRY_SEND;
}
