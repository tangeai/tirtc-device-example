#include "app.h"

#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "platform_storage.h"

static const char *TAG = "app_call_contacts";

#define APP_CALL_CONTACT_NVS_NAMESPACE "call_ct"
#define APP_CALL_CONTACT_NVS_KEY       "contacts"
#define APP_CALL_CONTACT_STORE_MAGIC   0x4C4C4143U
#define APP_CALL_CONTACT_STORE_VERSION 1U
#define APP_CALL_CONTACT_MIN_VALID_UNIX_TIME 1672531200LL

typedef struct {
	uint32_t magic;
	uint16_t version;
	uint16_t reserved;
	uint8_t count;
	uint8_t reserved2[3];
	app_call_contact_t contacts[APP_CALL_CONTACT_MAX];
} app_call_contact_store_t;

static portMUX_TYPE s_call_contact_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR app_call_contact_t s_call_contacts[APP_CALL_CONTACT_MAX];
static uint8_t s_call_contact_count;
static bool s_call_contacts_loaded;

static bool app_call_contact_entry_valid(const app_call_contact_t *contact)
{
	return contact != NULL &&
	       contact->device_id[0] != '\0' &&
	       contact->pair_key[0] != '\0';
}

static void app_call_contact_set_time(char *out, size_t out_size)
{
	if (out == NULL || out_size == 0) {
		return;
	}

	strlcpy(out, "Just now", out_size);
	time_t now = time(NULL);
	if (now >= APP_CALL_CONTACT_MIN_VALID_UNIX_TIME) {
		struct tm time_info = {0};
		localtime_r(&now, &time_info);
		strftime(out, out_size, "%Y-%m-%d %H:%M", &time_info);
	}
}

static void app_call_contacts_load_from_store(const app_call_contact_store_t *store)
{
	if (store == NULL) {
		return;
	}

	taskENTER_CRITICAL(&s_call_contact_lock);
	memset(s_call_contacts, 0, sizeof(s_call_contacts));
	s_call_contact_count = 0;
	uint8_t count = store->count > APP_CALL_CONTACT_MAX ? APP_CALL_CONTACT_MAX : store->count;
	for (uint8_t index = 0; index < count; ++index) {
		if (!app_call_contact_entry_valid(&store->contacts[index])) {
			continue;
		}
		s_call_contacts[s_call_contact_count++] = store->contacts[index];
	}
	taskEXIT_CRITICAL(&s_call_contact_lock);
}

static void app_call_contacts_ensure_loaded(void)
{
	bool should_load = false;

	taskENTER_CRITICAL(&s_call_contact_lock);
	if (!s_call_contacts_loaded) {
		s_call_contacts_loaded = true;
		should_load = true;
	}
	taskEXIT_CRITICAL(&s_call_contact_lock);

	if (!should_load) {
		return;
	}

	esp_err_t ret = platform_storage_init();
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "call contacts nvs init failed: %s", esp_err_to_name(ret));
		taskENTER_CRITICAL(&s_call_contact_lock);
		s_call_contacts_loaded = false;
		taskEXIT_CRITICAL(&s_call_contact_lock);
		return;
	}

	nvs_handle_t nvs_handle = 0;
	ret = nvs_open(APP_CALL_CONTACT_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
	if (ret == ESP_ERR_NVS_NOT_FOUND) {
		return;
	}
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "call contacts nvs open failed: %s", esp_err_to_name(ret));
		taskENTER_CRITICAL(&s_call_contact_lock);
		s_call_contacts_loaded = false;
		taskEXIT_CRITICAL(&s_call_contact_lock);
		return;
	}

	app_call_contact_store_t store = {0};
	size_t store_len = sizeof(store);
	ret = nvs_get_blob(nvs_handle, APP_CALL_CONTACT_NVS_KEY, &store, &store_len);
	nvs_close(nvs_handle);
	if (ret == ESP_ERR_NVS_NOT_FOUND) {
		return;
	}
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "call contacts nvs read failed: %s", esp_err_to_name(ret));
		taskENTER_CRITICAL(&s_call_contact_lock);
		s_call_contacts_loaded = false;
		taskEXIT_CRITICAL(&s_call_contact_lock);
		return;
	}
	if (store_len != sizeof(store) ||
	    store.magic != APP_CALL_CONTACT_STORE_MAGIC ||
	    store.version != APP_CALL_CONTACT_STORE_VERSION) {
		ESP_LOGW(TAG, "call contacts nvs data ignored: version mismatch");
		return;
	}

	app_call_contacts_load_from_store(&store);
	ESP_LOGD(TAG, "call contacts loaded from nvs: count=%u", (unsigned)store.count);
}

static esp_err_t app_call_contacts_save_current(void)
{
	app_call_contact_store_t store = {
		.magic = APP_CALL_CONTACT_STORE_MAGIC,
		.version = APP_CALL_CONTACT_STORE_VERSION,
	};

	taskENTER_CRITICAL(&s_call_contact_lock);
	uint8_t count = s_call_contact_count > APP_CALL_CONTACT_MAX ? APP_CALL_CONTACT_MAX : s_call_contact_count;
	for (uint8_t index = 0; index < count; ++index) {
		if (!app_call_contact_entry_valid(&s_call_contacts[index])) {
			continue;
		}
		store.contacts[store.count++] = s_call_contacts[index];
	}
	taskEXIT_CRITICAL(&s_call_contact_lock);

	ESP_RETURN_ON_ERROR(platform_storage_init(), TAG, "nvs init failed");

	nvs_handle_t nvs_handle = 0;
	esp_err_t ret = nvs_open(APP_CALL_CONTACT_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
	if (ret != ESP_OK) {
		return ret;
	}

	if (store.count == 0) {
		ret = nvs_erase_key(nvs_handle, APP_CALL_CONTACT_NVS_KEY);
		if (ret == ESP_ERR_NVS_NOT_FOUND) {
			ret = ESP_OK;
		}
	} else {
		ret = nvs_set_blob(nvs_handle, APP_CALL_CONTACT_NVS_KEY, &store, sizeof(store));
	}
	if (ret == ESP_OK) {
		ret = nvs_commit(nvs_handle);
	}
	nvs_close(nvs_handle);

	if (ret == ESP_OK) {
		ESP_LOGD(TAG, "call contacts saved to nvs: count=%u", (unsigned)store.count);
	} else {
		ESP_LOGW(TAG, "call contacts save failed: %s", esp_err_to_name(ret));
	}
	return ret;
}

esp_err_t app_add_call_contact(const char *device_id, const char *pair_key)
{
	if (device_id == NULL || device_id[0] == '\0' ||
	    pair_key == NULL || pair_key[0] == '\0') {
		return ESP_ERR_INVALID_ARG;
	}
	if (strlen(device_id) >= APP_CALL_CONTACT_DEVICE_ID_MAX ||
	    strlen(pair_key) >= APP_CALL_CONTACT_PAIR_KEY_MAX) {
		return ESP_ERR_INVALID_SIZE;
	}

	app_call_contacts_ensure_loaded();

	app_call_contact_t contact = {0};
	strlcpy(contact.device_id, device_id, sizeof(contact.device_id));
	strlcpy(contact.pair_key, pair_key, sizeof(contact.pair_key));
	app_call_contact_set_time(contact.last_time, sizeof(contact.last_time));

	taskENTER_CRITICAL(&s_call_contact_lock);
	uint8_t existing_index = APP_CALL_CONTACT_MAX;
	for (uint8_t index = 0; index < s_call_contact_count; ++index) {
		if (strcmp(s_call_contacts[index].device_id, device_id) == 0) {
			existing_index = index;
			break;
		}
	}

	if (existing_index < s_call_contact_count) {
		for (uint8_t index = existing_index; index > 0; --index) {
			s_call_contacts[index] = s_call_contacts[index - 1U];
		}
	} else if (s_call_contact_count < APP_CALL_CONTACT_MAX) {
		++s_call_contact_count;
		for (uint8_t index = s_call_contact_count - 1U; index > 0; --index) {
			s_call_contacts[index] = s_call_contacts[index - 1U];
		}
	} else {
		for (uint8_t index = APP_CALL_CONTACT_MAX - 1U; index > 0; --index) {
			s_call_contacts[index] = s_call_contacts[index - 1U];
		}
	}
	s_call_contacts[0] = contact;
	taskEXIT_CRITICAL(&s_call_contact_lock);

	return app_call_contacts_save_current();
}

esp_err_t app_remove_call_contact(const char *device_id)
{
	if (device_id == NULL || device_id[0] == '\0') {
		return ESP_ERR_INVALID_ARG;
	}

	app_call_contacts_ensure_loaded();

	bool found = false;
	taskENTER_CRITICAL(&s_call_contact_lock);
	for (uint8_t index = 0; index < s_call_contact_count; ++index) {
		if (strcmp(s_call_contacts[index].device_id, device_id) != 0) {
			continue;
		}
		found = true;
		for (uint8_t move = index; move + 1U < s_call_contact_count; ++move) {
			s_call_contacts[move] = s_call_contacts[move + 1U];
		}
		if (s_call_contact_count > 0) {
			--s_call_contact_count;
			memset(&s_call_contacts[s_call_contact_count], 0, sizeof(s_call_contacts[s_call_contact_count]));
		}
		break;
	}
	taskEXIT_CRITICAL(&s_call_contact_lock);

	if (!found) {
		return ESP_ERR_NOT_FOUND;
	}
	return app_call_contacts_save_current();
}

void app_get_call_contacts(app_call_contacts_snapshot_t *snapshot)
{
	if (snapshot == NULL) {
		return;
	}

	memset(snapshot, 0, sizeof(*snapshot));
	app_call_contacts_ensure_loaded();

	taskENTER_CRITICAL(&s_call_contact_lock);
	uint8_t count = s_call_contact_count > APP_CALL_CONTACT_MAX ? APP_CALL_CONTACT_MAX : s_call_contact_count;
	snapshot->count = count;
	for (uint8_t index = 0; index < count; ++index) {
		snapshot->contacts[index] = s_call_contacts[index];
	}
	taskEXIT_CRITICAL(&s_call_contact_lock);
}
