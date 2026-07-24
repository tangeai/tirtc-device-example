#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WECHAT_VOIP_CONTACT_MAX 3
#define WECHAT_VOIP_OPEN_ID_MAX 96
#define WECHAT_VOIP_MODEL_ID_MAX 64
#define WECHAT_VOIP_APP_ID_MAX 64

typedef struct {
    char open_id[WECHAT_VOIP_OPEN_ID_MAX];
} wechat_voip_contact_t;

typedef struct {
    uint8_t count;
    wechat_voip_contact_t contacts[WECHAT_VOIP_CONTACT_MAX];
} wechat_voip_contacts_snapshot_t;

typedef struct {
    char openid[WECHAT_VOIP_OPEN_ID_MAX];
    char model_id[WECHAT_VOIP_MODEL_ID_MAX];
    char app_id[WECHAT_VOIP_APP_ID_MAX];
} wechat_voip_auth_user_t;

esp_err_t wechat_voip_contacts_init(void);
void wechat_voip_contacts_reset_for_device(const char *device_id, uint32_t identity_generation);
void wechat_voip_contacts_clear_identity_cache(uint32_t identity_generation);
void wechat_voip_contacts_load(const char *device_id, uint32_t identity_generation);
esp_err_t wechat_voip_contacts_remember_for_device(const char *device_id,
                                                    uint32_t identity_generation,
                                                    const wechat_voip_auth_user_t *user,
                                                    const char *source);
esp_err_t wechat_voip_contacts_replace_for_device(const char *device_id,
                                                   uint32_t identity_generation,
                                                   const wechat_voip_auth_user_t *users,
                                                   size_t count,
                                                   const char *source);
void wechat_voip_contacts_find(const char *openid, wechat_voip_auth_user_t *target);
void wechat_voip_contacts_get_snapshot(wechat_voip_contacts_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
