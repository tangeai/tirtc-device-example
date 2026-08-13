#ifndef SESSION_HANDOFF_GUARD_H
#define SESSION_HANDOFF_GUARD_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SESSION_HANDOFF_ACTION_PRIMARY_DUPLICATE = 0,
    SESSION_HANDOFF_ACTION_RESPONSE_FROZEN,
    SESSION_HANDOFF_ACTION_BUSY_DUPLICATE,
    SESSION_HANDOFF_ACTION_REJECT_NOW,
    SESSION_HANDOFF_ACTION_RETRY_OVERFLOW,
} session_handoff_action_disposition_t;

typedef enum {
    SESSION_HANDOFF_RETRY_WAIT = 0,
    SESSION_HANDOFF_RETRY_SEND,
    SESSION_HANDOFF_RETRY_EXPIRED,
} session_handoff_retry_disposition_t;

bool session_handoff_request_matches(uint32_t expected_generation,
                                     uint32_t expected_cookie,
                                     uint32_t actual_generation,
                                     uint32_t actual_cookie);
bool session_handoff_action_id_matches(const char *expected_json,
                                       const char *actual_json);
session_handoff_action_disposition_t
session_handoff_classify_concurrent_action(
    bool response_frozen,
    const char *primary_action_id_json,
    const char *pending_busy_action_id_json,
    const char *completed_busy_action_id_json,
    const char *incoming_action_id_json);
session_handoff_retry_disposition_t session_handoff_classify_retry(
    int64_t current_ms,
    int64_t retry_at_ms,
    int64_t deadline_ms);

#endif
