#include "app.h"

#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_internal.h"
#include "network.h"
#include "qr_scanner.h"
#include "rtc_transport.h"

static const char *TAG = "app_call";

#define APP_CALL_SCAN_RESTORE_TASK_STACK_SIZE 4096
#define APP_CALL_SCAN_RESTORE_TASK_PRIORITY   3

typedef struct {
	app_scan_preview_cb_t preview_cb;
	app_contact_scan_result_cb_t result_cb;
	void *ctx;
	bool resources_suspended;
} app_contact_scan_state_t;

static app_contact_scan_state_t s_contact_scan;

static void app_contact_scan_restore_task(void *arg)
{
	bool restore = (bool)(uintptr_t)arg;

	if (restore && app_get_active_app() == APP_ID_CALL) {
		esp_err_t ret = app_resume_call_scan_resources();
		if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "resume call resources after scan failed: %s", esp_err_to_name(ret));
		} else {
			ESP_LOGD(TAG, "contact scan resources resumed: %s", esp_err_to_name(ret));
		}
	}
	vTaskDelete(NULL);
}

static void app_restore_contact_scan_resources(bool restore)
{
	bool resources_suspended = s_contact_scan.resources_suspended;

	s_contact_scan.resources_suspended = false;
	if (restore && resources_suspended && app_get_active_app() == APP_ID_CALL) {
		esp_err_t ret = app_resume_call_scan_resources();
		if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "resume call resources after scan failed: %s", esp_err_to_name(ret));
		}
	}
}

static void app_defer_contact_scan_resources(bool restore)
{
	bool resources_suspended = s_contact_scan.resources_suspended;

	s_contact_scan.resources_suspended = false;
	if (!restore || !resources_suspended) {
		return;
	}

	BaseType_t task_ret = xTaskCreate(app_contact_scan_restore_task,
					  "call_scan_res",
					  APP_CALL_SCAN_RESTORE_TASK_STACK_SIZE,
					  (void *)(uintptr_t)restore,
					  APP_CALL_SCAN_RESTORE_TASK_PRIORITY,
					  NULL);
	if (task_ret != pdPASS) {
		ESP_LOGW(TAG, "defer contact scan resource resume failed; resuming inline");
		esp_err_t ret = app_resume_call_scan_resources();
		if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "inline contact scan resource resume failed: %s", esp_err_to_name(ret));
		}
	}
}

static void app_contact_scan_preview_cb(const uint16_t *rgb565_pixels,
					uint16_t width,
					uint16_t height,
					void *ctx)
{
	(void)ctx;

	if (s_contact_scan.preview_cb != NULL) {
		s_contact_scan.preview_cb(rgb565_pixels, width, height, s_contact_scan.ctx);
	}
}

static void app_contact_scan_result_cb(esp_err_t result,
				       const qr_scanner_contact_t *contact,
				       void *ctx)
{
	const char *device_id = "";
	const char *pair_key = "";
	const char *raw_payload = "";
	app_contact_scan_result_cb_t result_cb = s_contact_scan.result_cb;
	void *result_ctx = s_contact_scan.ctx;

	(void)ctx;

	if (contact != NULL && contact->raw_payload[0] != '\0') {
		raw_payload = contact->raw_payload;
	}
	if (result == ESP_OK) {
		if (contact == NULL || contact->device_id[0] == '\0' || contact->pair_key[0] == '\0') {
			result = ESP_ERR_INVALID_RESPONSE;
		} else {
			device_id = contact->device_id;
			pair_key = contact->pair_key;
			ESP_LOGD(TAG, "contact QR accepted: device_id_len=%u", (unsigned)strlen(device_id));
		}
	}

	if (result_cb != NULL) {
		result_cb(result, device_id, pair_key, raw_payload, result_ctx);
	}
	app_defer_contact_scan_resources(true);
	s_contact_scan = (app_contact_scan_state_t){0};
}

esp_err_t app_call_contact(const char *device_id, const char *pair_key)
{
	if (device_id == NULL || device_id[0] == '\0' || pair_key == NULL || pair_key[0] == '\0') {
		return ESP_ERR_INVALID_ARG;
	}
	if (app_get_active_app() != APP_ID_CALL) {
		return ESP_ERR_INVALID_STATE;
	}
	if (!network_is_connected()) {
		return ESP_ERR_INVALID_STATE;
	}

	ESP_RETURN_ON_ERROR(app_acquire_call_session_resources(), TAG, "acquire call session resources failed");
	return rtc_transport_connect_peer(device_id, pair_key);
}

esp_err_t app_scan_contact(void)
{
	qr_scanner_contact_t contact = {0};

	if (app_get_active_app() != APP_ID_CALL) {
		return ESP_ERR_INVALID_STATE;
	}

	ESP_RETURN_ON_ERROR(qr_scanner_scan_contact(&contact), TAG, "scan contact failed");
	if (contact.device_id[0] == '\0' || contact.pair_key[0] == '\0') {
		return ESP_ERR_INVALID_RESPONSE;
	}

	ESP_LOGD(TAG, "contact QR accepted: device_id_len=%u", (unsigned)strlen(contact.device_id));
	return ESP_OK;
}

esp_err_t app_start_contact_scan(app_scan_preview_cb_t preview_cb,
				 app_contact_scan_result_cb_t result_cb,
				 void *ctx)
{
	esp_err_t ret = ESP_OK;
	app_id_t active_app = app_get_active_app();

	if (active_app != APP_ID_CALL) {
		ESP_LOGW(TAG, "start contact scan rejected: active_app=%d", (int)active_app);
		return ESP_ERR_INVALID_STATE;
	}

	ret = app_suspend_call_scan_resources();
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "start contact scan failed while suspending resources: %s", esp_err_to_name(ret));
		return ret;
	}

	s_contact_scan = (app_contact_scan_state_t){
		.preview_cb = preview_cb,
		.result_cb = result_cb,
		.ctx = ctx,
		.resources_suspended = true,
	};

	ret = qr_scanner_start_contact(app_contact_scan_preview_cb, app_contact_scan_result_cb, NULL);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "start contact scan failed while starting qr scanner: %s", esp_err_to_name(ret));
		app_restore_contact_scan_resources(true);
		s_contact_scan = (app_contact_scan_state_t){0};
	} else {
		ESP_LOGD(TAG, "contact scan started");
	}
	return ret;
}

esp_err_t app_stop_contact_scan(void)
{
	esp_err_t ret = qr_scanner_stop();

	if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
		app_restore_contact_scan_resources(true);
		s_contact_scan = (app_contact_scan_state_t){0};
	}
	return ret;
}

void app_cancel_contact_scan_for_lifecycle(void)
{
	esp_err_t ret = qr_scanner_stop();

	if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
		ESP_LOGW(TAG, "cancel contact scan failed: %s", esp_err_to_name(ret));
	}
	app_restore_contact_scan_resources(false);
	s_contact_scan = (app_contact_scan_state_t){0};
}

esp_err_t app_hangup_call(void)
{
	esp_err_t hangup_ret = ESP_OK;
	esp_err_t disconnect_ret = ESP_OK;

	if (app_get_active_app() != APP_ID_CALL) {
		return ESP_ERR_INVALID_STATE;
	}

	hangup_ret = rtc_transport_hangup();
	if (hangup_ret != ESP_OK) {
		ESP_LOGW(TAG, "call hangup command failed: %s", esp_err_to_name(hangup_ret));
	}
	rtc_transport_flush_remote_media();

	disconnect_ret = rtc_transport_disconnect();
	if (disconnect_ret != ESP_OK && disconnect_ret != ESP_ERR_INVALID_STATE) {
		ESP_LOGW(TAG, "call disconnect after hangup failed: %s", esp_err_to_name(disconnect_ret));
		rtc_transport_flush_remote_media();
		return disconnect_ret;
	}
	rtc_transport_flush_remote_media();

	(void)app_state_sync_call_media_defaults(false, NULL);
	app_release_call_session_resources();
	return (hangup_ret == ESP_OK || disconnect_ret == ESP_OK) ? ESP_OK : hangup_ret;
}

esp_err_t app_accept_call(void)
{
	esp_err_t ret = ESP_OK;

	if (app_get_active_app() != APP_ID_CALL) {
		return ESP_ERR_INVALID_STATE;
	}

	ESP_RETURN_ON_ERROR(app_acquire_call_session_resources(), TAG, "acquire call session resources failed");
	ret = rtc_transport_accept_incoming_call();
	if (ret != ESP_OK) {
		return ret;
	}

	(void)app_state_sync_call_media_defaults(true, NULL);
	return app_apply_media_policy();
}

esp_err_t app_reject_call(void)
{
	return rtc_transport_reject_incoming_call();
}
