#include "app_ai_device_action.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "app.h"
#include "device_call.h"
#include "device_online.h"
#include "network.h"
#include "wechat_voip_service.h"

static char app_ai_ascii_lower(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static bool app_ai_ascii_equal_ignore_case(const char *lhs, const char *rhs)
{
    if (lhs == NULL || rhs == NULL) {
        return false;
    }
    while (*lhs != '\0' && *rhs != '\0') {
        if (app_ai_ascii_lower(*lhs) != app_ai_ascii_lower(*rhs)) {
            return false;
        }
        ++lhs;
        ++rhs;
    }
    return *lhs == '\0' && *rhs == '\0';
}

static void app_ai_copy_trimmed(char *dst, size_t dst_size, const char *src)
{
    const char *begin = src;
    const char *end = NULL;
    size_t len = 0U;

    if (dst == NULL || dst_size == 0U) {
        return;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return;
    }

    while (*begin == ' ' || *begin == '\t' || *begin == '\r' || *begin == '\n') {
        ++begin;
    }
    end = begin + strlen(begin);
    while (end > begin &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        --end;
    }

    len = (size_t)(end - begin);
    if (len >= dst_size) {
        len = dst_size - 1U;
    }
    memcpy(dst, begin, len);
    dst[len] = '\0';
}

static bool app_ai_action_is_call_device(const char *action)
{
    return app_ai_ascii_equal_ignore_case(action, "call_device") ||
           app_ai_ascii_equal_ignore_case(action, "device_call") ||
           app_ai_ascii_equal_ignore_case(action, "start_device_call") ||
           app_ai_ascii_equal_ignore_case(action, "call");
}

static bool app_ai_call_type_is_audio(const char *call_type)
{
    return call_type == NULL ||
           call_type[0] == '\0' ||
           app_ai_ascii_equal_ignore_case(call_type, "audio") ||
           app_ai_ascii_equal_ignore_case(call_type, "voice");
}

static void app_ai_set_result(ai_chat_device_action_result_t *result,
                              bool ok,
                              const char *status,
                              const char *message)
{
    if (result == NULL) {
        return;
    }
    result->ok = ok;
    strlcpy(result->status, status != NULL ? status : "", sizeof(result->status));
    strlcpy(result->message, message != NULL ? message : "", sizeof(result->message));
}

static bool app_ai_contact_matches(const device_call_contact_t *contact,
                                   const char *target,
                                   bool exact)
{
    if (contact == NULL || target == NULL || target[0] == '\0') {
        return false;
    }
    if (app_ai_ascii_equal_ignore_case(contact->device_id, target)) {
        return true;
    }
    if (contact->remark[0] == '\0') {
        return false;
    }
    if (exact) {
        return strcmp(contact->remark, target) == 0;
    }
    return strstr(contact->remark, target) != NULL || strstr(target, contact->remark) != NULL;
}

static esp_err_t app_ai_resolve_call_target(const char *target,
                                            ai_chat_device_action_result_t *result)
{
    device_call_contacts_snapshot_t contacts = {0};
    const device_call_contact_t *selected = NULL;
    uint8_t match_count = 0U;
    char message[AI_CHAT_DEVICE_ACTION_MESSAGE_MAX] = {0};

    device_call_get_contacts_snapshot(&contacts);
    if (!contacts.ready) {
        if (!contacts.refreshing && device_online_is_online()) {
            (void)device_call_refresh_contacts_async();
        }
        app_ai_set_result(result,
                          false,
                          "contacts_loading",
                          "联系人列表正在同步，请稍后再试");
        return ESP_ERR_INVALID_STATE;
    }
    if (contacts.count == 0U) {
        app_ai_set_result(result, false, "contacts_empty", "当前没有可呼叫的设备联系人");
        return ESP_ERR_NOT_FOUND;
    }

    for (int pass = 0; pass < 2 && match_count == 0U; ++pass) {
        const bool exact = pass == 0;

        for (uint8_t index = 0U; index < contacts.count; ++index) {
            const device_call_contact_t *contact = &contacts.contacts[index];

            if (strlen(contact->device_id) != APP_CALL_CONTACT_DEVICE_ID_LENGTH ||
                !app_ai_contact_matches(contact, target, exact)) {
                continue;
            }
            ++match_count;
            if (selected == NULL) {
                selected = contact;
            }
        }
    }

    if (match_count == 0U) {
        strlcpy(message, "没有找到名为", sizeof(message));
        strlcat(message, target, sizeof(message));
        strlcat(message, "的设备联系人", sizeof(message));
        app_ai_set_result(result, false, "not_found", message);
        return ESP_ERR_NOT_FOUND;
    }
    if (match_count > 1U) {
        app_ai_set_result(result, false, "ambiguous", "匹配到多个联系人，请说得更具体一点");
        return ESP_ERR_INVALID_STATE;
    }
    if (selected == NULL || !selected->online) {
        strlcpy(message,
                selected != NULL && selected->remark[0] != '\0' ? selected->remark : target,
                sizeof(message));
        strlcat(message, "当前不在线", sizeof(message));
        app_ai_set_result(result, false, "offline", message);
        return ESP_ERR_INVALID_STATE;
    }

    result->ok = true;
    result->start_device_call = true;
    /*
     * Device calls are asynchronous. This result means the target and current
     * application state were accepted for lifecycle handoff; it does not claim
     * that the peer has rung or answered yet.
     */
    strlcpy(result->status, "accepted", sizeof(result->status));
    strlcpy(result->target_device_id, selected->device_id, sizeof(result->target_device_id));
    strlcpy(result->matched_name,
            selected->remark[0] != '\0' ? selected->remark : selected->device_id,
            sizeof(result->matched_name));
    strlcpy(result->message, "已受理呼叫", sizeof(result->message));
    strlcat(result->message, result->matched_name, sizeof(result->message));
    return ESP_OK;
}

static bool app_ai_other_call_is_busy(void)
{
    device_call_snapshot_t call = {0};
    const wechat_voip_call_state_t wechat = wechat_voip_service_get_call_state();

    device_call_get_snapshot(&call);
    if (call.pending_incoming ||
        call.state == DEVICE_CALL_STATE_OUTGOING ||
        call.state == DEVICE_CALL_STATE_INCOMING ||
        call.state == DEVICE_CALL_STATE_CONNECTING ||
        call.state == DEVICE_CALL_STATE_IN_CALL) {
        return true;
    }
    return wechat != WECHAT_VOIP_CALL_STATE_IDLE;
}

esp_err_t app_ai_device_action_execute(const ai_chat_device_action_t *action,
                                       ai_chat_device_action_result_t *result)
{
    char target[AI_CHAT_DEVICE_ACTION_TARGET_MAX] = {0};

    if (action == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));

    if (!app_ai_action_is_call_device(action->action)) {
        app_ai_set_result(result, false, "unsupported", "当前只支持呼叫设备联系人");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!app_ai_call_type_is_audio(action->call_type)) {
        app_ai_set_result(result, false, "unsupported_call_type", "当前只支持语音呼叫");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!network_is_connected()) {
        app_ai_set_result(result, false, "network_offline", "设备当前未连接网络");
        return ESP_ERR_INVALID_STATE;
    }
    if (app_ai_other_call_is_busy()) {
        app_ai_set_result(result, false, "busy", "设备当前正在通话");
        return ESP_ERR_INVALID_STATE;
    }

    app_ai_copy_trimmed(target, sizeof(target), action->target);
    if (target[0] == '\0') {
        app_ai_set_result(result, false, "missing_target", "请告诉我要呼叫哪个设备");
        return ESP_ERR_INVALID_ARG;
    }

    return app_ai_resolve_call_target(target, result);
}
