#include <assert.h>
#include <stdio.h>

#include "session_handoff_guard.h"

static void test_request_cookie_is_per_action(void)
{
    assert(session_handoff_request_matches(7U, 11U, 7U, 11U));
    assert(!session_handoff_request_matches(7U, 11U, 7U, 10U));
    assert(!session_handoff_request_matches(7U, 11U, 8U, 11U));
    assert(!session_handoff_request_matches(7U, 0U, 7U, 0U));
}

static void test_duplicate_id_requires_same_json_type_and_value(void)
{
    assert(session_handoff_action_id_matches("\"action-1\"",
                                             "\"action-1\""));
    assert(session_handoff_action_id_matches("17", "17"));
    assert(!session_handoff_action_id_matches("\"17\"", "17"));
    assert(!session_handoff_action_id_matches("\"action-1\"",
                                              "\"action-2\""));
    assert(!session_handoff_action_id_matches("", ""));
    assert(!session_handoff_action_id_matches(NULL, "\"action-1\""));
}

static void test_concurrent_action_policy_is_serialized(void)
{
    assert(session_handoff_classify_concurrent_action(
               false, "\"primary\"", "", "", "\"primary\"") ==
           SESSION_HANDOFF_ACTION_PRIMARY_DUPLICATE);
    assert(session_handoff_classify_concurrent_action(
               true, "\"primary\"", "", "", "\"second\"") ==
           SESSION_HANDOFF_ACTION_RESPONSE_FROZEN);
    assert(session_handoff_classify_concurrent_action(
               false,
               "\"primary\"",
               "\"second\"",
               "",
               "\"second\"") ==
           SESSION_HANDOFF_ACTION_BUSY_DUPLICATE);
    assert(session_handoff_classify_concurrent_action(
               false, "\"primary\"", "", "", "\"second\"") ==
           SESSION_HANDOFF_ACTION_REJECT_NOW);
    assert(session_handoff_classify_concurrent_action(
               false,
               "\"primary\"",
               "\"second\"",
               "",
               "\"third\"") ==
           SESSION_HANDOFF_ACTION_RETRY_OVERFLOW);
    assert(session_handoff_classify_concurrent_action(
               false,
               "\"primary\"",
               "",
               "\"second\"",
               "\"second\"") ==
           SESSION_HANDOFF_ACTION_BUSY_DUPLICATE);
    assert(session_handoff_classify_concurrent_action(
               false,
               "\"primary\"",
               "",
               "\"second\"",
               "\"third\"") ==
           SESSION_HANDOFF_ACTION_RETRY_OVERFLOW);
}

static void test_retry_deadline_precedes_retry_schedule(void)
{
    assert(session_handoff_classify_retry(1000, 1250, 3000) ==
           SESSION_HANDOFF_RETRY_WAIT);
    assert(session_handoff_classify_retry(1250, 1250, 3000) ==
           SESSION_HANDOFF_RETRY_SEND);
    assert(session_handoff_classify_retry(3000, 4000, 3000) ==
           SESSION_HANDOFF_RETRY_EXPIRED);
    assert(session_handoff_classify_retry(3500, 3000, 3000) ==
           SESSION_HANDOFF_RETRY_EXPIRED);
}

int main(void)
{
    test_request_cookie_is_per_action();
    test_duplicate_id_requires_same_json_type_and_value();
    test_concurrent_action_policy_is_serialized();
    test_retry_deadline_precedes_retry_schedule();
    puts("session_handoff_guard_tests: PASS");
    return 0;
}
