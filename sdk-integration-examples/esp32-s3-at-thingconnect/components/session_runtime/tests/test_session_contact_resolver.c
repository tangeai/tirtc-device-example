#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "session_contact_resolver.h"

static session_device_contact_t contact(const char *id,
                                        const char *remark,
                                        const char *name,
                                        bool online)
{
    session_device_contact_t value = {
        .online = online,
    };
    (void)snprintf(value.device_id, sizeof(value.device_id), "%s", id);
    (void)snprintf(value.remark, sizeof(value.remark), "%s", remark);
    (void)snprintf(value.device_name, sizeof(value.device_name), "%s", name);
    return value;
}

static void test_priority_and_normalization(void)
{
    session_device_contact_t contacts[] = {
        contact("DEV_A", "Kitchen display", "Panel A", true),
        contact("DEV_B", "Office", "Kitchen display", true),
    };

    session_contact_resolution_t result =
        session_contact_resolve(contacts, 2U, " dev_a ");
    assert(result.status == SESSION_CONTACT_RESOLVE_FOUND);
    assert(result.field == SESSION_CONTACT_MATCH_DEVICE_ID);
    assert(result.index == 0U);
    assert(result.exact);

    result = session_contact_resolve(contacts, 2U, "office");
    assert(result.status == SESSION_CONTACT_RESOLVE_FOUND);
    assert(result.field == SESSION_CONTACT_MATCH_REMARK);
    assert(result.index == 1U);
    assert(result.exact);
}

static void test_utf8_and_unique_substring(void)
{
    session_device_contact_t contacts[] = {
        contact("DEV_A", "客厅摄像头", "", true),
        contact("DEV_B", "书房屏幕", "", true),
    };

    session_contact_resolution_t result =
        session_contact_resolve(contacts, 2U, "客厅");
    assert(result.status == SESSION_CONTACT_RESOLVE_FOUND);
    assert(result.field == SESSION_CONTACT_MATCH_REMARK);
    assert(result.index == 0U);
    assert(!result.exact);
}

static void test_ambiguous_offline_and_unknown(void)
{
    session_device_contact_t contacts[] = {
        contact("DEV_A", "Test device one", "", true),
        contact("DEV_B", "Test device two", "", false),
        contact("DEV_C", "Bedroom", "Bedside", false),
    };

    session_contact_resolution_t result =
        session_contact_resolve(contacts, 3U, "Test device");
    assert(result.status == SESSION_CONTACT_RESOLVE_AMBIGUOUS);
    assert(result.match_count == 2U);

    result = session_contact_resolve(contacts, 3U, "Bedroom");
    assert(result.status == SESSION_CONTACT_RESOLVE_OFFLINE);
    assert(result.index == 2U);

    result = session_contact_resolve(contacts, 3U, "Garage");
    assert(result.status == SESSION_CONTACT_RESOLVE_NOT_FOUND);
}

static void test_duplicate_alias_is_ambiguous(void)
{
    session_device_contact_t contacts[] = {
        contact("DEV_A", "Doorbell", "", true),
        contact("DEV_B", "Doorbell", "", true),
    };
    session_contact_resolution_t result =
        session_contact_resolve(contacts, 2U, "Doorbell");
    assert(result.status == SESSION_CONTACT_RESOLVE_AMBIGUOUS);
    assert(result.exact);
}

static void test_invalid_input(void)
{
    session_device_contact_t one = contact("DEV_A", "One", "", true);
    assert(session_contact_resolve(&one, 1U, " \t\r\n").status ==
           SESSION_CONTACT_RESOLVE_INVALID);
    assert(session_contact_resolve(NULL, 0U, "One").status ==
           SESSION_CONTACT_RESOLVE_NOT_FOUND);
}

int main(void)
{
    test_priority_and_normalization();
    test_utf8_and_unique_substring();
    test_ambiguous_offline_and_unknown();
    test_duplicate_alias_is_ambiguous();
    test_invalid_input();
    puts("session_contact_resolver_tests: PASS");
    return 0;
}
