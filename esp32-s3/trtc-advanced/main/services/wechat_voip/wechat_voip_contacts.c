#include "wechat_voip_contacts.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "platform_nvs_async.h"
#include "tirtc_session.h"

static const char *TAG = "wx_voip_contacts";
static const char *CONTACT_NVS_NAMESPACE = "wx_voip";
static const char *CONTACT_NVS_KEY = "contacts";

#define CONTACT_STORE_MAGIC   0x57585631U
#define CONTACT_STORE_VERSION 1U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t count;
    uint8_t reserved;
    char device_id[TIRTC_SESSION_DEVICE_ID_MAX_LEN];
    wechat_voip_auth_user_t contacts[WECHAT_VOIP_CONTACT_MAX];
} wx_contact_store_t;

typedef struct {
    SemaphoreHandle_t lock;
    bool contacts_loaded;
    char device_id[TIRTC_SESSION_DEVICE_ID_MAX_LEN];
    wechat_voip_auth_user_t cached_auth;
    wechat_voip_auth_user_t contacts[WECHAT_VOIP_CONTACT_MAX];
    uint8_t contact_count;
} wechat_voip_contacts_runtime_t;

/*
 * 1032B in the current map. Contact cache is ordinary app state; NVS reads
 * copy through platform_nvs_async's internal-RAM request buffer before touching
 * this cache, so the cache itself can live in PSRAM.
 */
static EXT_RAM_BSS_ATTR wechat_voip_contacts_runtime_t s_contacts;

static void copy_str(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
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

static bool contact_valid(const wechat_voip_auth_user_t *entry)
{
    return entry != NULL && entry->openid[0] != '\0' && entry->model_id[0] != '\0';
}

static bool auth_user_same(const wechat_voip_auth_user_t *a, const wechat_voip_auth_user_t *b)
{
    return str_same(a != NULL ? a->openid : NULL, b != NULL ? b->openid : NULL) &&
           str_same(a != NULL ? a->model_id : NULL, b != NULL ? b->model_id : NULL) &&
           str_same(a != NULL ? a->app_id : NULL, b != NULL ? b->app_id : NULL);
}

static bool remember_locked(const wechat_voip_auth_user_t *user)
{
    int existing = -1;
    uint8_t count = s_contacts.contact_count > WECHAT_VOIP_CONTACT_MAX ?
                    WECHAT_VOIP_CONTACT_MAX :
                    s_contacts.contact_count;

    for (uint8_t index = 0; index < count; ++index) {
        if (str_same(s_contacts.contacts[index].openid, user->openid)) {
            existing = (int)index;
            break;
        }
    }

    wechat_voip_auth_user_t next = {0};
    copy_str(next.openid, sizeof(next.openid), user->openid);
    copy_str(next.model_id,
             sizeof(next.model_id),
             user->model_id[0] != '\0' ? user->model_id :
             existing >= 0 ? s_contacts.contacts[existing].model_id : "");
    copy_str(next.app_id,
             sizeof(next.app_id),
             user->app_id[0] != '\0' ? user->app_id :
             existing >= 0 ? s_contacts.contacts[existing].app_id : "");

    bool changed = true;
    if (existing >= 0) {
        changed = !auth_user_same(&s_contacts.contacts[existing], &next);
    } else if (count < WECHAT_VOIP_CONTACT_MAX) {
        existing = count++;
        s_contacts.contact_count = count;
    } else {
        memmove(&s_contacts.contacts[1],
                &s_contacts.contacts[0],
                sizeof(s_contacts.contacts[0]) * (WECHAT_VOIP_CONTACT_MAX - 1));
        existing = 0;
        s_contacts.contact_count = WECHAT_VOIP_CONTACT_MAX;
    }

    s_contacts.contacts[existing] = next;
    return changed;
}

static bool remove_locked(const char *openid, wechat_voip_auth_user_t *removed)
{
    uint8_t count = s_contacts.contact_count > WECHAT_VOIP_CONTACT_MAX ?
                    WECHAT_VOIP_CONTACT_MAX :
                    s_contacts.contact_count;
    for (uint8_t index = 0; index < count; ++index) {
        if (!str_same(s_contacts.contacts[index].openid, openid)) {
            continue;
        }
        if (removed != NULL) {
            *removed = s_contacts.contacts[index];
        }
        if (index + 1 < count) {
            memmove(&s_contacts.contacts[index],
                    &s_contacts.contacts[index + 1],
                    sizeof(s_contacts.contacts[0]) * (count - index - 1));
        }
        memset(&s_contacts.contacts[count - 1], 0, sizeof(s_contacts.contacts[count - 1]));
        s_contacts.contact_count = count - 1;
        if (str_same(s_contacts.cached_auth.openid, openid)) {
            memset(&s_contacts.cached_auth, 0, sizeof(s_contacts.cached_auth));
        }
        return true;
    }
    return false;
}

static esp_err_t save_contacts_now(void)
{
    wx_contact_store_t store = {
        .magic = CONTACT_STORE_MAGIC,
        .version = CONTACT_STORE_VERSION,
    };

    xSemaphoreTake(s_contacts.lock, portMAX_DELAY);
    copy_str(store.device_id, sizeof(store.device_id), s_contacts.device_id);
    uint8_t count = s_contacts.contact_count > WECHAT_VOIP_CONTACT_MAX ?
                    WECHAT_VOIP_CONTACT_MAX :
                    s_contacts.contact_count;
    for (uint8_t index = 0; index < count && store.count < WECHAT_VOIP_CONTACT_MAX; ++index) {
        if (contact_valid(&s_contacts.contacts[index])) {
            store.contacts[store.count++] = s_contacts.contacts[index];
        }
    }
    xSemaphoreGive(s_contacts.lock);

    esp_err_t ret = platform_nvs_async_set_blob(CONTACT_NVS_NAMESPACE,
                                                CONTACT_NVS_KEY,
                                                &store,
                                                sizeof(store));
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "contacts save queued: count=%u", (unsigned)store.count);
    }
    return ret;
}

static void request_save(const char *reason)
{
    if (wechat_voip_contacts_init() != ESP_OK) {
        return;
    }

    /*
     * NVS writes are already serialized by platform_nvs_async. Do not create a
     * short-lived contact-save task here: entering WeChat can happen when the
     * internal heap is badly fragmented after IPC/AI, and FreeRTOS task creation
     * touches internal heap metadata even when the task stack itself is PSRAM.
     */
    esp_err_t ret = save_contacts_now();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "contact save queue failed: %s source=%s",
                 esp_err_to_name(ret),
                 reason != NULL ? reason : "update");
    }
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

void wechat_voip_contacts_reset_for_device(const char *device_id)
{
    if (wechat_voip_contacts_init() != ESP_OK) {
        return;
    }

    xSemaphoreTake(s_contacts.lock, portMAX_DELAY);
    if (!str_same(s_contacts.device_id, device_id)) {
        memset(s_contacts.contacts, 0, sizeof(s_contacts.contacts));
        memset(&s_contacts.cached_auth, 0, sizeof(s_contacts.cached_auth));
        s_contacts.contact_count = 0;
        s_contacts.contacts_loaded = false;
        copy_str(s_contacts.device_id, sizeof(s_contacts.device_id), device_id);
    }
    xSemaphoreGive(s_contacts.lock);
}

void wechat_voip_contacts_load(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0' || wechat_voip_contacts_init() != ESP_OK) {
        return;
    }

    xSemaphoreTake(s_contacts.lock, portMAX_DELAY);
    if (s_contacts.contacts_loaded) {
        xSemaphoreGive(s_contacts.lock);
        return;
    }
    s_contacts.contacts_loaded = true;
    copy_str(s_contacts.device_id, sizeof(s_contacts.device_id), device_id);
    xSemaphoreGive(s_contacts.lock);

    wx_contact_store_t store = {0};
    size_t store_len = sizeof(store);
    esp_err_t ret = platform_nvs_async_get_blob(CONTACT_NVS_NAMESPACE, CONTACT_NVS_KEY, &store, &store_len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    if (ret != ESP_OK || store_len != sizeof(store) ||
        store.magic != CONTACT_STORE_MAGIC ||
        store.version != CONTACT_STORE_VERSION) {
        ESP_LOGW(TAG, "contact nvs data invalid: %s", esp_err_to_name(ret));
        return;
    }
    if (!str_same(store.device_id, device_id)) {
        ESP_LOGD(TAG, "ignore contacts from other device");
        return;
    }

    xSemaphoreTake(s_contacts.lock, portMAX_DELAY);
    for (int index = (int)store.count - 1; index >= 0; --index) {
        if (contact_valid(&store.contacts[index])) {
            (void)remember_locked(&store.contacts[index]);
        }
    }
    if (s_contacts.cached_auth.openid[0] == '\0' && s_contacts.contact_count > 0) {
        s_contacts.cached_auth = s_contacts.contacts[0];
    }
    xSemaphoreGive(s_contacts.lock);
    ESP_LOGD(TAG, "contacts loaded: count=%u", (unsigned)store.count);
}

bool wechat_voip_contacts_remember(const wechat_voip_auth_user_t *user, const char *source)
{
    if (user == NULL || user->openid[0] == '\0' || wechat_voip_contacts_init() != ESP_OK) {
        return false;
    }
    if (user->model_id[0] == '\0') {
        ESP_LOGD(TAG, "skip contact without model_id: source=%s openid_len=%u",
                 source != NULL ? source : "unknown",
                 (unsigned)strlen(user->openid));
        return false;
    }

    bool changed = false;
    xSemaphoreTake(s_contacts.lock, portMAX_DELAY);
    wechat_voip_auth_user_t before = s_contacts.cached_auth;
    s_contacts.cached_auth = *user;
    changed = !auth_user_same(&before, &s_contacts.cached_auth);
    changed = remember_locked(user) || changed;
    xSemaphoreGive(s_contacts.lock);

    if (changed) {
        request_save(source);
        ESP_LOGD(TAG, "contact cached: source=%s openid_len=%u model_id_len=%u",
                 source != NULL ? source : "unknown",
                 (unsigned)strlen(user->openid),
                 (unsigned)strlen(user->model_id));
    }
    return changed;
}

bool wechat_voip_contacts_remove(const char *openid, wechat_voip_auth_user_t *removed)
{
    if (openid == NULL || openid[0] == '\0' || wechat_voip_contacts_init() != ESP_OK) {
        return false;
    }

    xSemaphoreTake(s_contacts.lock, portMAX_DELAY);
    bool found = remove_locked(openid, removed);
    xSemaphoreGive(s_contacts.lock);
    if (found) {
        request_save("remove contact");
    }
    return found;
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
    uint8_t count = s_contacts.contact_count > WECHAT_VOIP_CONTACT_MAX ?
                    WECHAT_VOIP_CONTACT_MAX :
                    s_contacts.contact_count;
    for (uint8_t index = 0; index < count; ++index) {
        if (str_same(s_contacts.contacts[index].openid, openid)) {
            *target = s_contacts.contacts[index];
            break;
        }
    }
    if (target->openid[0] == '\0' && str_same(s_contacts.cached_auth.openid, openid)) {
        *target = s_contacts.cached_auth;
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
        return;
    }

    xSemaphoreTake(s_contacts.lock, portMAX_DELAY);
    uint8_t count = s_contacts.contact_count > WECHAT_VOIP_CONTACT_MAX ?
                    WECHAT_VOIP_CONTACT_MAX :
                    s_contacts.contact_count;
    for (uint8_t index = 0; index < count && snapshot->count < WECHAT_VOIP_CONTACT_MAX; ++index) {
        if (!contact_valid(&s_contacts.contacts[index])) {
            continue;
        }
        copy_str(snapshot->contacts[snapshot->count].open_id,
                 sizeof(snapshot->contacts[snapshot->count].open_id),
                 s_contacts.contacts[index].openid);
        snapshot->count++;
    }
    xSemaphoreGive(s_contacts.lock);
}
