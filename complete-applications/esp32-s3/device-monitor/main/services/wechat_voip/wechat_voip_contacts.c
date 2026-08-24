#include "wechat_voip_contacts.h"

#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tirtc_session.h"

static const char *TAG = "wx_voip_contacts";

#define CONTACT_AUTH_ALLOC_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

typedef struct {
    SemaphoreHandle_t lock;
    bool server_synced;
    esp_err_t sync_error;
    uint32_t identity_generation;
    char device_id[TIRTC_SESSION_DEVICE_ID_MAX_LEN];
    wechat_voip_auth_user_t *auth_users;
    size_t auth_count;
} wechat_voip_contacts_runtime_t;

/* Authorization data is a server-owned runtime cache and has no DMA requirement. */
static EXT_RAM_BSS_ATTR wechat_voip_contacts_runtime_t s_contacts;

static void copy_str(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0U) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strlcpy(dst, src, dst_size);
}

static bool str_same(const char *a, const char *b)
{
    return strcmp(a != NULL ? a : "", b != NULL ? b : "") == 0;
}

bool wechat_voip_remark_is_valid(const char *remark)
{
    if (remark == NULL) {
        return false;
    }

    const unsigned char *cursor = (const unsigned char *)remark;
    size_t remaining = strlen(remark);
    size_t characters = 0U;
    while (remaining > 0U) {
        size_t width = 0U;
        if (cursor[0] <= 0x7FU) {
            width = 1U;
        } else if (cursor[0] >= 0xC2U && cursor[0] <= 0xDFU) {
            if (remaining < 2U || (cursor[1] & 0xC0U) != 0x80U) {
                return false;
            }
            width = 2U;
        } else if (cursor[0] >= 0xE0U && cursor[0] <= 0xEFU) {
            if (remaining < 3U ||
                (cursor[1] & 0xC0U) != 0x80U || (cursor[2] & 0xC0U) != 0x80U ||
                (cursor[0] == 0xE0U && cursor[1] < 0xA0U) ||
                (cursor[0] == 0xEDU && cursor[1] >= 0xA0U)) {
                return false;
            }
            width = 3U;
        } else if (cursor[0] >= 0xF0U && cursor[0] <= 0xF4U) {
            if (remaining < 4U ||
                (cursor[1] & 0xC0U) != 0x80U || (cursor[2] & 0xC0U) != 0x80U ||
                (cursor[3] & 0xC0U) != 0x80U ||
                (cursor[0] == 0xF0U && cursor[1] < 0x90U) ||
                (cursor[0] == 0xF4U && cursor[1] > 0x8FU)) {
                return false;
            }
            width = 4U;
        } else {
            return false;
        }

        cursor += width;
        remaining -= width;
        if (++characters > WECHAT_VOIP_REMARK_MAX_CHARS) {
            return false;
        }
    }
    return true;
}

static void copy_remark(char *dst, size_t dst_size, const char *src)
{
    copy_str(dst, dst_size, wechat_voip_remark_is_valid(src) ? src : "");
}

static bool auth_valid(const wechat_voip_auth_user_t *entry)
{
    return entry != NULL && entry->openid[0] != '\0' && entry->model_id[0] != '\0';
}

static void normalize_auth_user(wechat_voip_auth_user_t *dst,
                                const wechat_voip_auth_user_t *src)
{
    memset(dst, 0, sizeof(*dst));
    if (src == NULL) {
        return;
    }
    copy_str(dst->openid, sizeof(dst->openid), src->openid);
    copy_str(dst->model_id, sizeof(dst->model_id), src->model_id);
    copy_str(dst->app_id, sizeof(dst->app_id), src->app_id);
    copy_remark(dst->remark, sizeof(dst->remark), src->remark);
}

static int find_auth_index_locked(const char *openid)
{
    for (size_t index = 0U; index < s_contacts.auth_count; ++index) {
        if (str_same(s_contacts.auth_users[index].openid, openid)) {
            return (int)index;
        }
    }
    return -1;
}

esp_err_t wechat_voip_contacts_init(void)
{
    if (s_contacts.lock == NULL) {
        s_contacts.lock = xSemaphoreCreateMutexWithCaps(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (s_contacts.lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

void wechat_voip_contacts_reset_for_device(const char *device_id,
                                           uint32_t identity_generation)
{
    if (wechat_voip_contacts_init() != ESP_OK) {
        return;
    }

    wechat_voip_auth_user_t *old_auth = NULL;
    xSemaphoreTake(s_contacts.lock, portMAX_DELAY);
    if (!str_same(s_contacts.device_id, device_id) ||
        s_contacts.identity_generation != identity_generation) {
        old_auth = s_contacts.auth_users;
        s_contacts.auth_users = NULL;
        s_contacts.auth_count = 0U;
        s_contacts.server_synced = false;
        s_contacts.sync_error = ESP_OK;
        s_contacts.identity_generation = identity_generation;
        copy_str(s_contacts.device_id, sizeof(s_contacts.device_id), device_id);
    }
    xSemaphoreGive(s_contacts.lock);
    free(old_auth);
}

esp_err_t wechat_voip_contacts_replace_for_device(const char *device_id,
                                                   uint32_t identity_generation,
                                                   const wechat_voip_auth_user_t *users,
                                                   size_t count,
                                                   const char *source)
{
    if (device_id == NULL || device_id[0] == '\0' || (users == NULL && count > 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = wechat_voip_contacts_init();
    if (ret != ESP_OK) {
        return ret;
    }

    wechat_voip_auth_user_t *next = NULL;
    size_t next_count = 0U;
    if (count > 0U) {
        next = heap_caps_calloc(count, sizeof(*next), CONTACT_AUTH_ALLOC_CAPS);
        if (next == NULL) {
            return ESP_ERR_NO_MEM;
        }
        for (size_t index = 0U; index < count; ++index) {
            wechat_voip_auth_user_t normalized = {0};
            normalize_auth_user(&normalized, &users[index]);
            if (!auth_valid(&normalized)) {
                continue;
            }

            size_t existing = 0U;
            for (; existing < next_count; ++existing) {
                if (str_same(next[existing].openid, normalized.openid)) {
                    next[existing] = normalized;
                    break;
                }
            }
            if (existing == next_count) {
                next[next_count++] = normalized;
            }
        }
        if (next_count == 0U) {
            free(next);
            next = NULL;
        }
    }

    wechat_voip_auth_user_t *old_auth = NULL;
    xSemaphoreTake(s_contacts.lock, portMAX_DELAY);
    if (!str_same(s_contacts.device_id, device_id) ||
        s_contacts.identity_generation != identity_generation) {
        xSemaphoreGive(s_contacts.lock);
        free(next);
        return ESP_ERR_INVALID_STATE;
    }
    old_auth = s_contacts.auth_users;
    s_contacts.auth_users = next;
    s_contacts.auth_count = next_count;
    s_contacts.server_synced = true;
    s_contacts.sync_error = ESP_OK;
    xSemaphoreGive(s_contacts.lock);
    free(old_auth);

    ESP_LOGI(TAG,
             "authorized contacts refreshed: count=%u source=%s",
             (unsigned)next_count,
             source != NULL ? source : "server");
    return ESP_OK;
}

void wechat_voip_contacts_note_sync_error_for_device(const char *device_id,
                                                      uint32_t identity_generation,
                                                      esp_err_t error)
{
    if (device_id == NULL || device_id[0] == '\0' ||
        wechat_voip_contacts_init() != ESP_OK) {
        return;
    }
    xSemaphoreTake(s_contacts.lock, portMAX_DELAY);
    if (str_same(s_contacts.device_id, device_id) &&
        s_contacts.identity_generation == identity_generation) {
        s_contacts.sync_error = error;
    }
    xSemaphoreGive(s_contacts.lock);
}

esp_err_t wechat_voip_contacts_check_authorized(const char *openid)
{
    if (openid == NULL || openid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = wechat_voip_contacts_init();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(s_contacts.lock, portMAX_DELAY);
    bool found = find_auth_index_locked(openid) >= 0;
    bool server_synced = s_contacts.server_synced;
    xSemaphoreGive(s_contacts.lock);

    if (found) {
        return ESP_OK;
    }
    return server_synced ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
}

esp_err_t wechat_voip_contacts_update_remark(const char *openid,
                                             const char *remark,
                                             const char *source)
{
    if (openid == NULL || openid[0] == '\0' || !wechat_voip_remark_is_valid(remark)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = wechat_voip_contacts_init();
    if (ret != ESP_OK) {
        return ret;
    }

    bool changed = false;
    xSemaphoreTake(s_contacts.lock, portMAX_DELAY);
    int auth_index = find_auth_index_locked(openid);
    if (auth_index < 0) {
        xSemaphoreGive(s_contacts.lock);
        return ESP_ERR_NOT_FOUND;
    }
    changed = !str_same(s_contacts.auth_users[auth_index].remark, remark);
    copy_remark(s_contacts.auth_users[auth_index].remark,
                sizeof(s_contacts.auth_users[auth_index].remark),
                remark);
    xSemaphoreGive(s_contacts.lock);

    if (changed) {
        ESP_LOGD(TAG,
                 "runtime contact remark updated: source=%s",
                 source != NULL ? source : "server");
    }
    return ESP_OK;
}

void wechat_voip_contacts_find(const char *openid, wechat_voip_auth_user_t *target)
{
    if (target == NULL) {
        return;
    }
    memset(target, 0, sizeof(*target));
    if (openid == NULL || openid[0] == '\0' || wechat_voip_contacts_init() != ESP_OK) {
        return;
    }

    xSemaphoreTake(s_contacts.lock, portMAX_DELAY);
    int auth_index = find_auth_index_locked(openid);
    if (auth_index >= 0) {
        *target = s_contacts.auth_users[auth_index];
    }
    xSemaphoreGive(s_contacts.lock);
}

void wechat_voip_contacts_get_snapshot(wechat_voip_contacts_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (wechat_voip_contacts_init() != ESP_OK) {
        snapshot->last_error = ESP_ERR_NO_MEM;
        return;
    }

    xSemaphoreTake(s_contacts.lock, portMAX_DELAY);
    snapshot->ready = s_contacts.server_synced;
    snapshot->server_synced = s_contacts.server_synced;
    snapshot->last_error = s_contacts.sync_error;
    for (size_t index = 0U;
         index < s_contacts.auth_count && snapshot->count < WECHAT_VOIP_CONTACT_MAX;
         ++index) {
        if (!auth_valid(&s_contacts.auth_users[index])) {
            continue;
        }
        wechat_voip_contact_t *contact = &snapshot->contacts[snapshot->count++];
        copy_str(contact->open_id,
                 sizeof(contact->open_id),
                 s_contacts.auth_users[index].openid);
        copy_remark(contact->remark,
                    sizeof(contact->remark),
                    s_contacts.auth_users[index].remark);
    }
    xSemaphoreGive(s_contacts.lock);
}
