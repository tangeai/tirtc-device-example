#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "wechat_voip_thing.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WECHAT_VOIP_CALL_STATE_IDLE = 0,
    WECHAT_VOIP_CALL_STATE_INCOMING,
    WECHAT_VOIP_CALL_STATE_CALLING,
    WECHAT_VOIP_CALL_STATE_CONNECTING,
    WECHAT_VOIP_CALL_STATE_IN_CALL,
    WECHAT_VOIP_CALL_STATE_CLOSING,
} wechat_voip_call_state_t;

esp_err_t wechat_voip_service_start(void);
void wechat_voip_service_stop(void);
esp_err_t wechat_voip_service_answer(void);
esp_err_t wechat_voip_service_reject_or_hangup(void);
esp_err_t wechat_voip_service_request_call(const char *open_id);
esp_err_t wechat_voip_service_add_contact(const char *open_id);
esp_err_t wechat_voip_service_remove_contact(const char *open_id);
bool wechat_voip_service_has_incoming_call(void);
wechat_voip_call_state_t wechat_voip_service_get_call_state(void);
void wechat_voip_service_maintenance(void);
void wechat_voip_service_get_contacts(wechat_voip_contacts_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
