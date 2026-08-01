#include "app.h"

#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_internal.h"
#include "device_call.h"
#include "network.h"
#include "qr_scanner.h"
#include "rtc_transport.h"

static const char *TAG = "app_call";
static const char *CALL_FLOW_TAG = "CALL_FLOW";

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
	const char *raw_payload = "";
	app_contact_scan_result_cb_t result_cb = s_contact_scan.result_cb;
	void *result_ctx = s_contact_scan.ctx;

	(void)ctx;

	if (contact != NULL && contact->raw_payload[0] != '\0') {
		raw_payload = contact->raw_payload;
	}
	if (result == ESP_OK) {
		if (contact == NULL ||
		    strlen(contact->device_id) != APP_CALL_CONTACT_DEVICE_ID_LENGTH) {
			result = ESP_ERR_INVALID_RESPONSE;
		} else {
			device_id = contact->device_id;
			ESP_LOGD(TAG, "contact QR accepted: device_id_len=%u", (unsigned)strlen(device_id));
		}
	}

	if (result_cb != NULL) {
		result_cb(result, device_id, raw_payload, result_ctx);
	}
	app_defer_contact_scan_resources(true);
	s_contact_scan = (app_contact_scan_state_t){0};
}

esp_err_t app_call_contact(const char *device_id)
{
    return app_call_contact_with_type(device_id, DEVICE_CALL_TYPE_AUDIO);
}

esp_err_t app_call_contact_with_type(const char *device_id, const char *call_type)
{
    if (device_id == NULL ||
        strlen(device_id) != APP_CALL_CONTACT_DEVICE_ID_LENGTH ||
        call_type == NULL ||
        (strcmp(call_type, DEVICE_CALL_TYPE_AUDIO) != 0 &&
         strcmp(call_type, DEVICE_CALL_TYPE_VIDEO) != 0)) {
        ESP_LOGW(CALL_FLOW_TAG, "stage=app_call_rejected reason=invalid_request");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=app_call_begin peer=%s type=%s active_app=%d network=%d",
             device_id,
             call_type,
             (int)app_get_active_app(),
             network_is_connected() ? 1 : 0);
    if (app_get_active_app() != APP_ID_CALL) {
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=app_call_rejected peer=%s reason=wrong_app",
                 device_id);
        return ESP_ERR_INVALID_STATE;
    }
    if (!network_is_connected()) {
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=app_call_rejected peer=%s reason=wifi_offline",
                 device_id);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t resource_ret = app_acquire_call_session_resources();
    if (resource_ret != ESP_OK) {
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=app_call_rejected peer=%s reason=resource_acquire ret=%s",
                 device_id,
                 esp_err_to_name(resource_ret));
        ESP_LOGE(TAG, "acquire call session resources failed: %s", esp_err_to_name(resource_ret));
        return resource_ret;
    }
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=app_call_resources_ready peer=%s",
             device_id);
    esp_err_t ret = device_call_request_with_type(device_id, call_type);
    if (ret != ESP_OK) {
        app_release_call_session_resources();
    }
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=app_call_queued peer=%s type=%s ret=%s",
             device_id,
             call_type,
             esp_err_to_name(ret));
    return ret;
}

esp_err_t app_scan_contact(void)
{
	qr_scanner_contact_t contact = {0};
	esp_err_t scan_ret = ESP_OK;
	esp_err_t resume_ret = ESP_OK;

	if (app_get_active_app() != APP_ID_CALL) {
		return ESP_ERR_INVALID_STATE;
	}

	ESP_RETURN_ON_ERROR(app_suspend_call_scan_resources(), TAG, "acquire contact scan camera failed");
	scan_ret = qr_scanner_scan_contact(&contact);
	resume_ret = app_resume_call_scan_resources();
	if (resume_ret != ESP_OK) {
		ESP_LOGW(TAG,
			 "restore call resources after scan failed: %s",
			 esp_err_to_name(resume_ret));
	}
	if (scan_ret != ESP_OK) {
		ESP_LOGW(TAG, "scan contact failed: %s", esp_err_to_name(scan_ret));
		return scan_ret;
	}
	if (resume_ret != ESP_OK) {
		return resume_ret;
	}
	if (strlen(contact.device_id) != APP_CALL_CONTACT_DEVICE_ID_LENGTH) {
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
	esp_err_t service_ret = ESP_OK;
	esp_err_t hangup_ret = ESP_OK;
	esp_err_t disconnect_ret = ESP_OK;

    if (app_get_active_app() != APP_ID_CALL) {
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=app_hangup_rejected reason=wrong_app active_app=%d",
                 (int)app_get_active_app());
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(CALL_FLOW_TAG, "stage=app_hangup_begin");

	service_ret = device_call_hangup();
	if (service_ret != ESP_OK && service_ret != ESP_ERR_INVALID_STATE) {
		ESP_LOGW(TAG, "device call hangup service failed: %s", esp_err_to_name(service_ret));
	}
	if (service_ret != ESP_OK) {
		hangup_ret = rtc_transport_hangup();
		if (hangup_ret != ESP_OK) {
			ESP_LOGW(TAG, "call hangup command failed: %s", esp_err_to_name(hangup_ret));
		}
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
    if (service_ret == ESP_OK || hangup_ret == ESP_OK || disconnect_ret == ESP_OK) {
        ESP_LOGI(CALL_FLOW_TAG,
                 "stage=app_hangup_done service=%s command=%s disconnect=%s ret=ESP_OK",
                 esp_err_to_name(service_ret),
                 esp_err_to_name(hangup_ret),
                 esp_err_to_name(disconnect_ret));
        return ESP_OK;
    }
    ESP_LOGW(CALL_FLOW_TAG,
             "stage=app_hangup_done service=%s command=%s disconnect=%s ret=failed",
             esp_err_to_name(service_ret),
             esp_err_to_name(hangup_ret),
             esp_err_to_name(disconnect_ret));
    return service_ret != ESP_ERR_INVALID_STATE ? service_ret : hangup_ret;
}

esp_err_t app_accept_call(void)
{
    esp_err_t ret = ESP_OK;
    bool service_pending = device_call_has_pending_incoming();

    if (app_get_active_app() != APP_ID_CALL) {
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=app_accept_rejected reason=wrong_app active_app=%d",
                 (int)app_get_active_app());
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(CALL_FLOW_TAG,
             "stage=app_accept_begin source=%s",
             service_pending ? "thing_connect" : "tirtc_command");

    ret = app_acquire_call_session_resources();
    if (ret != ESP_OK) {
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=app_accept_done source=%s ret=%s reason=resource_acquire",
                 service_pending ? "thing_connect" : "tirtc_command",
                 esp_err_to_name(ret));
        ESP_LOGE(TAG, "acquire call session resources failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (service_pending) {
        ret = device_call_accept_pending();
	} else {
		ret = rtc_transport_accept_incoming_call();
	}
    if (ret != ESP_OK) {
        app_release_call_session_resources();
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=app_accept_done source=%s ret=%s",
                 service_pending ? "thing_connect" : "tirtc_command",
                 esp_err_to_name(ret));
        return ret;
    }

	(void)app_state_sync_call_media_defaults(true, NULL);
    ret = app_apply_media_policy();
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=app_accept_done source=%s ret=%s",
             service_pending ? "thing_connect" : "tirtc_command",
             esp_err_to_name(ret));
    return ret;
}

esp_err_t app_reject_call(void)
{
    bool service_pending = device_call_has_pending_incoming();
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=app_reject_begin source=%s",
             service_pending ? "thing_connect" : "tirtc_command");
    esp_err_t ret = service_pending ?
                    device_call_reject_pending() :
                    rtc_transport_reject_incoming_call();
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=app_reject_done source=%s ret=%s",
             service_pending ? "thing_connect" : "tirtc_command",
             esp_err_to_name(ret));
    return ret;
}
