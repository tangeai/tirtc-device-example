#pragma once

/* thing-connect VoIP business coordinator. */

#include <stdbool.h>

#include "esp_err.h"
#include "wechat_voip_contacts.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*wechat_voip_incoming_allowed_cb_t)(void *ctx);

esp_err_t wechat_voip_thing_set_incoming_policy(wechat_voip_incoming_allowed_cb_t callback,
                                                void *ctx);
esp_err_t wechat_voip_thing_start(void);
void wechat_voip_thing_stop(void);
bool wechat_voip_thing_is_connected(void);
esp_err_t wechat_voip_thing_request_call(const char *open_id);
/* Compatibility entry: verifies that open_id is already authorized locally. */
esp_err_t wechat_voip_thing_add_contact(const char *open_id);
/* Authorization revocation requires the mini-program user JWT. */
esp_err_t wechat_voip_thing_remove_contact(const char *open_id);
bool wechat_voip_thing_request_call_busy(void);
void wechat_voip_thing_cancel_pending_call(void);
void wechat_voip_thing_maintenance(void);
void wechat_voip_thing_get_contacts(wechat_voip_contacts_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
