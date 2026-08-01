#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cJSON.h"

#include "app_ai_device_action.h"
#include "app_ai_chat_config.h"
#include "app_audio_config.h"
#include "app_config.h"
#include "app_internal.h"
#include "app_rtc_config.h"
#include "app_ui.h"
#include "ai_chat.h"
#include "ai_chat_token.h"
#include "audio_device.h"
#include "camera_driver.h"
#include "device.h"
#include "device_binding.h"
#include "device_call.h"
#include "device_identity.h"
#include "device_online.h"
#include "display.h"
#include "network.h"
#include "ota.h"
#include "media_sink.h"
#include "platform/app_task_affinity.h"
#include "platform_nvs_async.h"
#include "platform_task_reaper.h"
#include "rtc_media_bridge.h"
#include "rtc_transport.h"
#include "sender_test.h"
#include "system_time.h"
#include "thing_mqtt_client.h"
#include "thing_service_registry.h"
#include "wechat_voip_config.h"
#include "wechat_voip_service.h"

#if APP_CONFIG_DEBUG_SCREEN_SERVER_ENABLE
#include "screen_debug_server.h"
#endif

static const char *TAG = "app";
static const char *CALL_FLOW_TAG = "CALL_FLOW";

#define APP_CONTROL_TASK_STACK_SIZE 8192
#define APP_CONTROL_TASK_PRIORITY   2
#define APP_CONTROL_QUEUE_LENGTH    4
#define APP_RTC_RECONFIGURE_REASON_MAX 32
#define APP_RTC_PREPARE_POLL_MS        100U
#define APP_RTC_IDENTITY_RESET_WAIT_MS 15000U
#define APP_LIFECYCLE_TASK_STACK_SIZE 6144
#define APP_LIFECYCLE_TASK_PRIORITY   4
#define APP_LIFECYCLE_QUEUE_LENGTH    4
#define APP_DEVICE_IPC_SEND_VOLUME_PERCENT     80U
#define APP_DEVICE_IPC_PLAYBACK_VOLUME_PERCENT 60U
#define APP_DEVICE_IPC_CAPTURE_CODEC_GAIN_PERCENT 45U
#define APP_DEVICE_IPC_CAPTURE_UPLOAD_GAIN_PERCENT APP_DEVICE_IPC_SEND_VOLUME_PERCENT
#define APP_DEVICE_IPC_CAPTURE_AUTO_GAIN_MAX_PERCENT 200U
#define APP_DEVICE_IPC_CAPTURE_NOISE_GATE_OPEN_PEAK 240U
#define APP_DEVICE_IPC_CAPTURE_NOISE_GATE_CLOSE_PEAK 120U
#define APP_DEVICE_IPC_CAPTURE_NOISE_GATE_ATTENUATION_PERCENT 20U
#define APP_AI_CHAT_DEFAULT_CAPTURE_GAIN_PERCENT 50U
#define APP_AI_CHAT_DEFAULT_SPEAKER_VOLUME_PERCENT 90U
#define APP_AI_CHAT_START_REPORT_DEFER_MS 4000U
#define APP_AI_CHAT_TOKEN_PREFETCH_DELAY_MS 200U
#define APP_AI_CHAT_TOKEN_PREFETCH_MIN_INTERVAL_MS 60000U
#define APP_AI_CHAT_TOKEN_PREFETCH_TASK_STACK_SIZE (8 * 1024)
#define APP_AI_CHAT_TOKEN_PREFETCH_TASK_PRIORITY   1
#define APP_THING_BOOTSTRAP_TASK_STACK_SIZE        (12 * 1024)
#define APP_THING_BOOTSTRAP_TASK_PRIORITY          2
#define APP_DEVICE_UNBIND_ACK_WAIT_MS               2000U
#define APP_DEVICE_ONLINE_STOP_WAIT_MS              12000U

typedef enum {
	APP_CONTROL_EVENT_SPEAKER_VOLUME = 1,
	APP_CONTROL_EVENT_RTC_CREDENTIALS_UPDATE,
	APP_CONTROL_EVENT_RTC_RECONFIGURE,
	APP_CONTROL_EVENT_RTC_PREPARE_AFTER_IDENTITY,
	APP_CONTROL_EVENT_RTC_IDENTITY_CONFLICT,
	APP_CONTROL_EVENT_DEVICE_UNBIND,
	APP_CONTROL_EVENT_DEVICE_BINDING_VERIFY,
	APP_CONTROL_EVENT_DEVICE_REBIND_REQUIRED,
	APP_CONTROL_EVENT_DEVICE_ONLINE_READY,
	APP_CONTROL_EVENT_CALL_SESSION_ENDED,
} app_control_event_type_t;

typedef enum {
	APP_LIFECYCLE_EVENT_ENTER_APP = 1,
	APP_LIFECYCLE_EVENT_RETURN_HOME,
	APP_LIFECYCLE_EVENT_START_APP_SERVICES,
	APP_LIFECYCLE_EVENT_AI_CHAT_CALL_CONTACT,
} app_lifecycle_event_type_t;

typedef struct {
	app_control_event_type_t type;
	uint8_t percent;
	char reason[APP_RTC_RECONFIGURE_REASON_MAX];
	char rtc_device_id[APP_RTC_CONFIG_TEXT_MAX];
	char rtc_device_secret[APP_RTC_CONFIG_TEXT_MAX];
	char rtc_client_id[APP_RTC_CONFIG_TEXT_MAX];
} app_control_event_t;

typedef struct {
	app_lifecycle_event_type_t type;
	app_id_t app_id;
	ai_chat_device_action_route_t call_route;
	char call_target_id[AI_CHAT_DEVICE_ACTION_TARGET_MAX];
	char call_type[AI_CHAT_DEVICE_ACTION_CALL_TYPE_MAX];
} app_lifecycle_event_t;

typedef struct {
	char reason[APP_RTC_RECONFIGURE_REASON_MAX];
} app_thing_bootstrap_context_t;

static portMUX_TYPE s_app_lifecycle_lock = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_app_transition_mutex_buffer;
static SemaphoreHandle_t s_app_transition_mutex;
static QueueHandle_t s_app_control_queue;
static TaskHandle_t s_app_control_task;
static QueueHandle_t s_app_lifecycle_queue;
static TaskHandle_t s_app_lifecycle_task;
static app_id_t s_active_app = APP_ID_HOME;
static uint32_t s_active_resources;
static bool s_door_open;
static bool s_rtc_runtime_initialized;
static bool s_rtc_runtime_init_in_progress;
static bool s_rtc_sdk_prepared;
static bool s_rtc_identity_conflict_handled;
static char s_rtc_identity_conflict_device_id[APP_RTC_CONFIG_TEXT_MAX];
static char s_rtc_identity_conflict_client_id[APP_RTC_CONFIG_TEXT_MAX];
static portMUX_TYPE s_online_report_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_online_report_defer_timer;
static int64_t s_online_report_defer_until_us;
static bool s_online_report_pending;
static char s_online_report_pending_reason[DEVICE_ONLINE_STATUS_REASON_MAX];
static TaskHandle_t s_ai_chat_token_prefetch_task;
static int64_t s_ai_chat_token_last_prefetch_us;
static bool s_thing_bootstrap_running;
static bool s_device_binding_control_pending;
static bool s_rtc_identity_reconfigure_pending;

static void app_configure_log_timezone(void)
{
	setenv("TZ", "CST-8", 1);
	tzset();
}

static esp_err_t app_configure_thing_service_registry(void)
{
	const thing_service_registry_config_t config = {
		.discovery_url = APP_CONFIG_THING_SERVICE_DISCOVERY_URL,
		.device_api_base = APP_CONFIG_DEVICE_BINDING_API_BASE,
		.voip_api_base = APP_CONFIG_WECHAT_VOIP_API_BASE,
		.ai_api_base = APP_CONFIG_DEVICE_BINDING_API_BASE,
		.call_api_base = APP_CONFIG_DEVICE_BINDING_API_BASE,
		.mqtt_uri = APP_CONFIG_DEVICE_BINDING_MQTT_URI,
		.tirtc_endpoint = APP_CONFIG_RTC_SERVICE_ENDPOINT,
	};

	return thing_service_registry_init(&config);
}

static esp_err_t app_set_speaker_volume_internal(uint8_t percent, bool persist);
static esp_err_t app_enter_app_locked(app_id_t app_id);
static esp_err_t app_return_home_locked(void);
static esp_err_t app_enter_app_sync(app_id_t app_id);
static esp_err_t app_return_home_sync(void);
static esp_err_t app_enqueue_lifecycle_event(app_lifecycle_event_type_t type, app_id_t app_id);
static esp_err_t app_enqueue_ai_chat_call_lifecycle_event(ai_chat_device_action_route_t route,
							 const char *target_id,
							 const char *call_type);
static esp_err_t app_start_app_services(app_id_t app_id);
static esp_err_t app_prepare_rtc_after_time_sync(const char *reason);
static esp_err_t app_prepare_rtc_after_config_if_ready(const char *reason);
static esp_err_t app_reconfigure_tirtc_after_settings_change(const char *reason);
static void app_request_rtc_reconfigure_after_settings_change(const char *reason);
static void app_request_rtc_prepare_after_identity(const char *reason);
static void app_request_rtc_identity_conflict(int error,
					      const char *device_id,
					      const char *client_id,
					      void *ctx);
static bool app_rtc_identity_conflict_mark_pending(const char *device_id, const char *client_id);
static void app_rtc_identity_conflict_clear_if_new_credentials(const char *device_id);
static bool app_rtc_device_credentials_available(void);
static bool app_device_binding_ready_for_rtc(const char *reason);
static void app_prepare_rtc_when_identity_ready(const char *reason);
static esp_err_t app_start_device_identity_services(const char *reason);
static esp_err_t app_start_device_online_if_ready(const char *reason);
static void app_start_device_identity_ingress(void);
static void app_device_online_ready_cb(void *ctx);
static void app_suspend_device_identity_services(const char *reason);
static esp_err_t app_apply_pending_rtc_identity_config(const char *reason);
static esp_err_t app_handle_device_unbind(const char *reason);
static void app_request_device_unbind(const char *reason);
static void app_clear_device_binding_control_pending(void);
static void app_schedule_thing_bootstrap(const char *reason);
static void app_time_sync_cb(esp_err_t result, bool time_valid, void *ctx);
static void app_device_binding_bound_cb(const char *device_id, void *ctx);
static void app_restart_identity_after_credentials(const char *device_id, const char *reason);
static esp_err_t app_verify_device_binding_internal(const char *reason);
static esp_err_t app_reconcile_binding_with_retained_credentials(const char *reason);
static esp_err_t app_report_device_state_async(const char *reason);
static bool app_device_call_incoming_allowed(void *ctx);
static bool app_wechat_incoming_allowed(void *ctx);
static esp_err_t app_configure_incoming_session_policy(void);
static void app_device_call_session_ended(void *ctx);
static void app_release_call_session_resources_if_idle(void);
static esp_err_t app_ai_chat_device_action_cb(const ai_chat_device_action_t *action,
					      ai_chat_device_action_result_t *result,
					      void *ctx);
static esp_err_t app_ai_chat_device_action_committed_cb(const ai_chat_device_action_t *action,
						       const ai_chat_device_action_result_t *result,
						       void *ctx);
static void app_handle_ai_chat_call_contact(ai_chat_device_action_route_t route,
					    const char *target_id,
					    const char *call_type);
static void app_request_ai_chat_start_if_idle(const char *reason);
static void app_begin_ai_chat_start_window(void);
static void app_schedule_ai_chat_token_prefetch(const char *reason);
static void app_reset_ai_chat_token_prefetch_throttle(void);
static esp_err_t app_apply_call_audio_profile(void);

enum {
	APP_RESOURCE_RTC = 1U << 0,
	APP_RESOURCE_AUDIO = 1U << 1,
	APP_RESOURCE_CAMERA = 1U << 2,
};

typedef struct {
	app_id_t app_id;
	uint32_t resources;
} app_resource_profile_t;

static const app_resource_profile_t s_app_resource_profiles[] = {
	{ APP_ID_HOME, 0 },
	{ APP_ID_DEVICE, APP_RESOURCE_RTC | APP_RESOURCE_AUDIO },
	{ APP_ID_CALL, 0 },
	{ APP_ID_WECHAT, APP_RESOURCE_RTC | APP_RESOURCE_AUDIO },
	{ APP_ID_AI_CHAT, APP_RESOURCE_RTC | APP_RESOURCE_AUDIO },
	{ APP_ID_SYSTEM, 0 },
};

static const char *app_id_name(app_id_t app_id)
{
	switch (app_id) {
	case APP_ID_DEVICE:
		return "device";
	case APP_ID_CALL:
		return "call";
	case APP_ID_WECHAT:
		return "wechat";
	case APP_ID_AI_CHAT:
		return "ai_chat";
	case APP_ID_SYSTEM:
		return "system";
	case APP_ID_HOME:
	default:
		return "home";
	}
}

static void app_flush_deferred_online_report(void)
{
	char reason[DEVICE_ONLINE_STATUS_REASON_MAX] = {0};
	bool pending = false;

	taskENTER_CRITICAL(&s_online_report_lock);
	pending = s_online_report_pending;
	if (pending) {
		strlcpy(reason,
			s_online_report_pending_reason[0] != '\0' ? s_online_report_pending_reason : "deferred",
			sizeof(reason));
		s_online_report_pending = false;
		s_online_report_pending_reason[0] = '\0';
	}
	taskEXIT_CRITICAL(&s_online_report_lock);

	if (pending) {
		(void)device_online_report_state_async(reason);
	}
}

static void app_online_report_defer_timer_cb(void *ctx)
{
	(void)ctx;
	app_flush_deferred_online_report();
}

static esp_err_t app_init_online_report_defer_timer(void)
{
	if (s_online_report_defer_timer != NULL) {
		return ESP_OK;
	}

	const esp_timer_create_args_t timer_args = {
		.callback = app_online_report_defer_timer_cb,
		.name = "online_report_def",
	};
	return esp_timer_create(&timer_args, &s_online_report_defer_timer);
}

static bool app_online_report_defer_active(void)
{
	int64_t now_us = esp_timer_get_time();
	bool active = false;

	taskENTER_CRITICAL(&s_online_report_lock);
	active = s_online_report_defer_until_us > now_us;
	taskEXIT_CRITICAL(&s_online_report_lock);
	return active;
}

static void app_begin_ai_chat_start_window(void)
{
	int64_t now_us = esp_timer_get_time();
	int64_t defer_until_us = now_us + (int64_t)APP_AI_CHAT_START_REPORT_DEFER_MS * 1000LL;

	taskENTER_CRITICAL(&s_online_report_lock);
	s_online_report_defer_until_us = defer_until_us;
	taskEXIT_CRITICAL(&s_online_report_lock);

	if (s_online_report_defer_timer != NULL) {
		(void)esp_timer_stop(s_online_report_defer_timer);
		(void)esp_timer_start_once(s_online_report_defer_timer,
					   (uint64_t)APP_AI_CHAT_START_REPORT_DEFER_MS * 1000ULL);
	}
}

static esp_err_t app_report_device_state_async(const char *reason)
{
	const char *safe_reason = reason != NULL && reason[0] != '\0' ? reason : "state";

	if (!app_online_report_defer_active()) {
		app_flush_deferred_online_report();
		return device_online_report_state_async(safe_reason);
	}

	taskENTER_CRITICAL(&s_online_report_lock);
	s_online_report_pending = true;
	strlcpy(s_online_report_pending_reason, safe_reason, sizeof(s_online_report_pending_reason));
	taskEXIT_CRITICAL(&s_online_report_lock);
	return ESP_OK;
}

static uint32_t app_resource_mask_for_app(app_id_t app_id)
{
	for (size_t index = 0; index < sizeof(s_app_resource_profiles) / sizeof(s_app_resource_profiles[0]); ++index) {
		if (s_app_resource_profiles[index].app_id == app_id) {
			return s_app_resource_profiles[index].resources;
		}
	}
	return 0;
}

app_id_t app_get_active_app(void)
{
	app_id_t app_id = APP_ID_HOME;

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	app_id = s_active_app;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	return app_id;
}

bool app_is_door_open(void)
{
	bool open = false;

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	open = s_door_open;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	return open;
}

static void app_set_active_app(app_id_t app_id)
{
	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	s_active_app = app_id;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	(void)app_report_device_state_async("app");
}

static uint32_t app_get_active_resources(void)
{
	uint32_t resources = 0;

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	resources = s_active_resources;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	return resources;
}

static void app_set_active_resources(uint32_t resources)
{
	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	s_active_resources = resources;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
}

esp_err_t app_configure_tirtc(void)
{
	rtc_transport_config_t *rtc_config = calloc(1, sizeof(*rtc_config));
	if (rtc_config == NULL) {
		return ESP_ERR_NO_MEM;
	}

	esp_err_t ret = app_build_rtc_transport_config(rtc_config);
	if (ret == ESP_OK) {
		ret = rtc_transport_configure(rtc_config);
	}
	free(rtc_config);
	return ret;
}

static esp_err_t app_build_ai_chat_config(ai_chat_config_t *config)
{
	app_rtc_config_snapshot_t rtc_settings = {0};
	device_binding_identity_t identity = {0};

	if (config == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	app_get_rtc_config_snapshot(&rtc_settings);
	memset(config, 0, sizeof(*config));
	config->enabled = APP_CONFIG_AI_CHAT_ENABLE != 0;
	strlcpy(config->device_id, rtc_settings.device_id, sizeof(config->device_id));
	strlcpy(config->user_id, APP_CONFIG_AI_CHAT_USER_ID, sizeof(config->user_id));
	strlcpy(config->role_id, APP_CONFIG_AI_CHAT_ROLE_ID, sizeof(config->role_id));
	strlcpy(config->device_key, rtc_settings.device_secret, sizeof(config->device_key));
	strlcpy(config->token_api_base,
		thing_service_registry_ai_api_base(),
		sizeof(config->token_api_base));
	config->on_device_action = app_ai_chat_device_action_cb;
	config->on_device_action_committed = app_ai_chat_device_action_committed_cb;
	config->device_action_ctx = NULL;
	if (device_identity_get(&identity) == ESP_OK) {
		strlcpy(config->device_mac, identity.mac, sizeof(config->device_mac));
	}
	if (config->device_id[0] == '\0' || config->device_key[0] == '\0') {
		config->enabled = false;
	}
	return ESP_OK;
}

static void app_ai_chat_token_prefetch_task(void *ctx)
{
	(void)ctx;
	vTaskDelay(pdMS_TO_TICKS(APP_AI_CHAT_TOKEN_PREFETCH_DELAY_MS));

	esp_err_t ret = ESP_ERR_INVALID_STATE;
	bool attempted = false;

	if (app_get_active_app() == APP_ID_HOME &&
	    network_is_connected() &&
	    system_time_has_valid_time() &&
	    app_rtc_device_credentials_available()) {
		ai_chat_config_t config = {0};
		ret = app_build_ai_chat_config(&config);
		if (ret == ESP_OK && config.enabled) {
			attempted = true;
			ret = ai_chat_token_prefetch_join(&config);
		}
		if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE && ret != ESP_ERR_NOT_FOUND) {
			ESP_LOGD(TAG, "AI Chat token prefetch skipped: %s", esp_err_to_name(ret));
		}
	}

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	if (attempted && ret != ESP_OK) {
		s_ai_chat_token_last_prefetch_us = 0;
	}
	s_ai_chat_token_prefetch_task = NULL;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	vTaskDeleteWithCaps(NULL);
}

static void app_reset_ai_chat_token_prefetch_throttle(void)
{
	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	s_ai_chat_token_last_prefetch_us = 0;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
}

static void app_schedule_ai_chat_token_prefetch(const char *reason)
{
	(void)reason;

	if (!network_is_connected() ||
	    !system_time_has_valid_time() ||
	    !app_rtc_device_credentials_available() ||
	    app_get_active_app() != APP_ID_HOME) {
		return;
	}

	int64_t now_us = esp_timer_get_time();
	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	bool running = s_ai_chat_token_prefetch_task != NULL;
	bool throttled = s_ai_chat_token_last_prefetch_us > 0 &&
			 now_us - s_ai_chat_token_last_prefetch_us <
			     (int64_t)APP_AI_CHAT_TOKEN_PREFETCH_MIN_INTERVAL_MS * 1000LL;
	if (!running && !throttled) {
		s_ai_chat_token_last_prefetch_us = now_us;
	}
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	if (running || throttled) {
		return;
	}

	TaskHandle_t task = NULL;
	BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(app_ai_chat_token_prefetch_task,
							      "ai_tok_pref",
							      APP_AI_CHAT_TOKEN_PREFETCH_TASK_STACK_SIZE,
							      NULL,
							      APP_AI_CHAT_TOKEN_PREFETCH_TASK_PRIORITY,
							      &task,
							      APP_TASK_CORE_BACKGROUND,
							      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (task_ret != pdPASS) {
		taskENTER_CRITICAL(&s_app_lifecycle_lock);
		s_ai_chat_token_last_prefetch_us = 0;
		taskEXIT_CRITICAL(&s_app_lifecycle_lock);
		return;
	}

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	s_ai_chat_token_prefetch_task = task;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
}

static esp_err_t app_configure_ai_chat(void)
{
	ai_chat_config_t config = {0};

	ESP_RETURN_ON_ERROR(app_build_ai_chat_config(&config), TAG, "build ai chat config failed");
	return ai_chat_configure(&config);
}

static esp_err_t app_ai_chat_device_action_cb(const ai_chat_device_action_t *action,
					      ai_chat_device_action_result_t *result,
					      void *ctx)
{
	(void)ctx;
	if (result == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	if (s_app_lifecycle_queue == NULL ||
	    s_app_transition_mutex == NULL ||
	    app_get_active_app() != APP_ID_AI_CHAT) {
		memset(result, 0, sizeof(*result));
		strlcpy(result->status, "busy", sizeof(result->status));
		strlcpy(result->message, "应用正在切换，请稍后再试", sizeof(result->message));
		return ESP_ERR_INVALID_STATE;
	}
	return app_ai_device_action_execute(action, result);
}

static esp_err_t app_ai_chat_device_action_committed_cb(const ai_chat_device_action_t *action,
						       const ai_chat_device_action_result_t *result,
						       void *ctx)
{
	(void)action;
	(void)ctx;
	if (result == NULL || !result->ok ||
	    result->call_route == AI_CHAT_DEVICE_ACTION_ROUTE_NONE ||
	    result->target_id[0] == '\0' ||
	    result->call_type[0] == '\0') {
		return ESP_ERR_INVALID_ARG;
	}

	return app_enqueue_ai_chat_call_lifecycle_event(result->call_route,
							result->target_id,
							result->call_type);
}

static const char *app_ai_call_route_name(ai_chat_device_action_route_t route)
{
	switch (route) {
	case AI_CHAT_DEVICE_ACTION_ROUTE_DEVICE_CALL:
		return "device";
	case AI_CHAT_DEVICE_ACTION_ROUTE_WECHAT_VOIP:
		return "wechat";
	case AI_CHAT_DEVICE_ACTION_ROUTE_NONE:
	default:
		return "none";
	}
}

static bool app_ai_call_target_valid(ai_chat_device_action_route_t route, const char *target_id)
{
	if (target_id == NULL || target_id[0] == '\0') {
		return false;
	}
	switch (route) {
	case AI_CHAT_DEVICE_ACTION_ROUTE_DEVICE_CALL:
		return strlen(target_id) == APP_CALL_CONTACT_DEVICE_ID_LENGTH;
	case AI_CHAT_DEVICE_ACTION_ROUTE_WECHAT_VOIP:
		return strlen(target_id) < APP_WECHAT_OPEN_ID_MAX;
	case AI_CHAT_DEVICE_ACTION_ROUTE_NONE:
	default:
		return false;
	}
}

static bool app_ai_call_type_valid(ai_chat_device_action_route_t route, const char *call_type)
{
	if (call_type == NULL) {
		return false;
	}
	if (strcmp(call_type, DEVICE_CALL_TYPE_AUDIO) == 0) {
		return true;
	}
	return route == AI_CHAT_DEVICE_ACTION_ROUTE_DEVICE_CALL &&
	       strcmp(call_type, DEVICE_CALL_TYPE_VIDEO) == 0;
}

static void app_handle_ai_chat_call_contact(ai_chat_device_action_route_t route,
					    const char *target_id,
					    const char *call_type)
{
	esp_err_t ret = ESP_OK;
	app_id_t target_app = route == AI_CHAT_DEVICE_ACTION_ROUTE_WECHAT_VOIP ?
				      APP_ID_WECHAT :
				      APP_ID_CALL;

	if (!app_ai_call_target_valid(route, target_id) ||
	    !app_ai_call_type_valid(route, call_type)) {
		ESP_LOGW(CALL_FLOW_TAG,
			 "stage=ai_call_handoff_rejected route=%s type=%s reason=invalid_request",
			 app_ai_call_route_name(route),
			 call_type != NULL ? call_type : "(null)");
		return;
	}
	if (s_app_transition_mutex == NULL ||
	    xSemaphoreTake(s_app_transition_mutex, portMAX_DELAY) != pdTRUE) {
		ESP_LOGE(CALL_FLOW_TAG,
			 "stage=ai_call_handoff_rejected route=%s reason=transition_lock",
			 app_ai_call_route_name(route));
		return;
	}

	if (app_get_active_app() != APP_ID_AI_CHAT) {
		ESP_LOGW(CALL_FLOW_TAG,
			 "stage=ai_call_handoff_rejected route=%s reason=wrong_owner active=%s",
			 app_ai_call_route_name(route),
			 app_id_name(app_get_active_app()));
		ret = ESP_ERR_INVALID_STATE;
	} else {
		/*
		 * This function runs only on app_lifecycle_task. Keep enter + call
		 * submission in one transition transaction so no other synchronous
		 * caller can interleave a home/app switch between the two operations.
		 */
		ESP_LOGI(CALL_FLOW_TAG,
			 "stage=ai_call_handoff_begin route=%s target_len=%u type=%s",
			 app_ai_call_route_name(route),
			 (unsigned)strlen(target_id),
			 call_type);
		ret = app_enter_app_locked(target_app);
		if (ret == ESP_OK) {
			ret = route == AI_CHAT_DEVICE_ACTION_ROUTE_WECHAT_VOIP ?
				      app_wechat_call_contact(target_id) :
				      app_call_contact_with_type(target_id, call_type);
		}
	}
	xSemaphoreGive(s_app_transition_mutex);

	if (ret == ESP_OK) {
		esp_err_t display_ret = route == AI_CHAT_DEVICE_ACTION_ROUTE_WECHAT_VOIP ?
					    display_open_wechat_active_page_async() :
					    display_open_call_active_page_async();
		if (display_ret != ESP_OK) {
			ESP_LOGW(CALL_FLOW_TAG,
				 "stage=ai_call_page_failed route=%s page=active ret=%s",
				 app_ai_call_route_name(route),
				 esp_err_to_name(display_ret));
		}
	} else if (app_get_active_app() == target_app) {
		if (route == AI_CHAT_DEVICE_ACTION_ROUTE_WECHAT_VOIP) {
			(void)display_open_wechat_page_async();
		} else {
			(void)display_open_call_page_async();
		}
	}
	ESP_LOGI(CALL_FLOW_TAG,
		 "stage=ai_call_handoff_done route=%s type=%s ret=%s",
		 app_ai_call_route_name(route),
		 call_type,
		 esp_err_to_name(ret));
}

static bool app_rtc_device_credentials_available(void)
{
	app_rtc_config_snapshot_t settings = {0};

	app_get_rtc_config_snapshot(&settings);
	return settings.device_id[0] != '\0' && settings.device_secret[0] != '\0';
}

static bool app_rtc_identity_conflict_mark_pending(const char *device_id, const char *client_id)
{
	const char *safe_device_id = device_id != NULL ? device_id : "";
	const char *safe_client_id = client_id != NULL ? client_id : "";
	bool duplicate = false;

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	duplicate = s_rtc_identity_conflict_handled &&
		    strcmp(s_rtc_identity_conflict_device_id, safe_device_id) == 0 &&
		    strcmp(s_rtc_identity_conflict_client_id, safe_client_id) == 0;
	if (!duplicate) {
		s_rtc_identity_conflict_handled = true;
		strlcpy(s_rtc_identity_conflict_device_id, safe_device_id, sizeof(s_rtc_identity_conflict_device_id));
		strlcpy(s_rtc_identity_conflict_client_id, safe_client_id, sizeof(s_rtc_identity_conflict_client_id));
	}
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);

	return !duplicate;
}

static void app_rtc_identity_conflict_clear_if_new_credentials(const char *device_id)
{
	if (device_id == NULL || device_id[0] == '\0') {
		return;
	}

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	if (s_rtc_identity_conflict_handled &&
	    strcmp(s_rtc_identity_conflict_device_id, device_id) != 0) {
		s_rtc_identity_conflict_handled = false;
		s_rtc_identity_conflict_device_id[0] = '\0';
		s_rtc_identity_conflict_client_id[0] = '\0';
	}
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
}

static esp_err_t app_device_binding_save_credentials(const char *device_id,
						     const char *device_key,
						     void *ctx)
{
	(void)ctx;

	if (device_id == NULL || device_key == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	return app_set_rtc_device_credentials(device_id, device_key);
}

static void app_restart_identity_after_credentials(const char *device_id, const char *reason)
{
	const char *safe_reason = reason != NULL && reason[0] != '\0' ? reason : "credentials";
	bool network_ready = network_is_connected() && system_time_has_valid_time();

	ai_chat_token_invalidate_cache();
	app_rtc_identity_conflict_clear_if_new_credentials(device_id);
	app_suspend_device_identity_services(safe_reason);
	if (network_ready) {
		device_online_set_network_ready(true);
		esp_err_t online_ret = device_online_notify_credentials_changed(safe_reason);
		if (online_ret != ESP_OK) {
			ESP_LOGW(TAG,
				 "device online restart after credential change failed: reason=%s ret=%s",
				 safe_reason,
				 esp_err_to_name(online_ret));
		}
	} else {
		device_online_set_network_ready(false);
		device_online_invalidate_cache();
	}

	/* Register business listeners before the formal MQTT connection completes. */
	app_start_device_identity_ingress();
	(void)app_configure_ai_chat();
	if (network_ready) {
		app_request_rtc_prepare_after_identity(safe_reason);
	}
}

static void app_device_binding_bound_cb(const char *device_id, void *ctx)
{
	(void)ctx;
	app_restart_identity_after_credentials(device_id, "binding-complete");
}

static esp_err_t app_device_binding_load_credentials(device_binding_credentials_t *credentials,
						     void *ctx)
{
	app_rtc_config_snapshot_t settings = {0};

	(void)ctx;

	if (credentials == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	memset(credentials, 0, sizeof(*credentials));
	app_get_rtc_config_snapshot(&settings);
	if (settings.device_id[0] == '\0' || settings.device_secret[0] == '\0') {
		return ESP_ERR_NOT_FOUND;
	}

	strlcpy(credentials->device_id, settings.device_id, sizeof(credentials->device_id));
	strlcpy(credentials->device_key, settings.device_secret, sizeof(credentials->device_key));
	return ESP_OK;
}

static esp_err_t app_configure_device_binding(void)
{
	device_binding_config_t config = {
		.enabled = APP_CONFIG_DEVICE_BINDING_ENABLE != 0,
		.api_base = thing_service_registry_device_api_base(),
		.mqtt_uri = thing_service_registry_mqtt_uri(),
		.wait_timeout_ms = APP_CONFIG_DEVICE_BINDING_WAIT_TIMEOUT_MS,
		.load_credentials = app_device_binding_load_credentials,
		.save_credentials = app_device_binding_save_credentials,
		.on_bound = app_device_binding_bound_cb,
		.ctx = NULL,
		.bound_ctx = NULL,
	};

	return device_binding_init(&config);
}

static esp_err_t app_device_online_load_credentials(device_online_credentials_t *credentials,
						    void *ctx)
{
	app_rtc_config_snapshot_t settings = {0};

	(void)ctx;

	if (credentials == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	app_get_rtc_config_snapshot(&settings);
	if (settings.device_id[0] == '\0' || settings.device_secret[0] == '\0') {
		return ESP_ERR_NOT_FOUND;
	}

	strlcpy(credentials->device_id, settings.device_id, sizeof(credentials->device_id));
	strlcpy(credentials->device_key, settings.device_secret, sizeof(credentials->device_key));
	return ESP_OK;
}

static bool app_device_online_payload_is_unbind(const char *payload, size_t payload_len)
{
	bool unbind = false;
	cJSON *root = NULL;
	const cJSON *type = NULL;

	if (payload == NULL || payload_len == 0) {
		return false;
	}

	root = cJSON_ParseWithLength(payload, payload_len);
	if (root == NULL) {
		return false;
	}

	type = cJSON_GetObjectItemCaseSensitive(root, "type");
	unbind = cJSON_IsString(type) && strcmp(type->valuestring, "unbind") == 0;
	cJSON_Delete(root);
	return unbind;
}

static esp_err_t app_queue_retained_binding_reconcile(const char *reason)
{
	const char *safe_reason = reason != NULL && reason[0] != '\0' ? reason : "rebind";
	bool already_pending = false;

	if (s_app_control_queue == NULL) {
		ESP_LOGW(TAG, "retained binding reconcile dropped: control queue not ready");
		return ESP_ERR_INVALID_STATE;
	}
	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	already_pending = s_device_binding_control_pending;
	if (!already_pending) {
		s_device_binding_control_pending = true;
	}
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	if (already_pending) {
		ESP_LOGI(TAG, "retained binding reconcile already pending: reason=%s", safe_reason);
		return ESP_OK;
	}
	app_control_event_t event = {
		.type = APP_CONTROL_EVENT_DEVICE_REBIND_REQUIRED,
	};
	strlcpy(event.reason, safe_reason, sizeof(event.reason));
	if (xQueueSendToBack(s_app_control_queue, &event, 0) != pdTRUE) {
		taskENTER_CRITICAL(&s_app_lifecycle_lock);
		s_device_binding_control_pending = false;
		taskEXIT_CRITICAL(&s_app_lifecycle_lock);
		ESP_LOGW(TAG, "retained binding reconcile dropped: control queue full reason=%s", safe_reason);
		return ESP_ERR_NO_MEM;
	}
	return ESP_OK;
}

static esp_err_t app_queue_device_binding_verification(const char *reason)
{
	const char *safe_reason = reason != NULL && reason[0] != '\0' ? reason : "binding-check";
	bool already_pending = false;

	if (s_app_control_queue == NULL) {
		ESP_LOGW(TAG, "device binding verification dropped: control queue not ready");
		return ESP_ERR_INVALID_STATE;
	}
	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	already_pending = s_device_binding_control_pending;
	if (!already_pending) {
		s_device_binding_control_pending = true;
	}
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	if (already_pending) {
		ESP_LOGI(TAG, "device binding verification already pending: reason=%s", safe_reason);
		return ESP_OK;
	}

	app_control_event_t event = {
		.type = APP_CONTROL_EVENT_DEVICE_BINDING_VERIFY,
	};
	strlcpy(event.reason, safe_reason, sizeof(event.reason));
	if (xQueueSendToBack(s_app_control_queue, &event, 0) != pdTRUE) {
		app_clear_device_binding_control_pending();
		ESP_LOGW(TAG, "device binding verification dropped: control queue full reason=%s", safe_reason);
		return ESP_ERR_NO_MEM;
	}
	return ESP_OK;
}

static void app_device_online_message_cb(const char *topic,
					 const char *payload,
					 size_t payload_len,
					 void *ctx)
{
	(void)ctx;

	if (app_device_online_payload_is_unbind(payload, payload_len)) {
		ESP_LOGI(TAG, "device unbind command received");
		app_request_device_unbind("mqtt-unbind");
	}
}

static esp_err_t app_device_online_build_status(char *buffer,
						size_t buffer_size,
						const char *reason,
						uint32_t seq,
						void *ctx)
{
	network_state_t network = {0};
	rtc_transport_stats_t rtc = {0};
	app_rtc_config_snapshot_t rtc_settings = {0};
	device_online_snapshot_t online = {0};
	app_id_t active_app = app_get_active_app();
	bool door_open = app_is_door_open();
	const char *safe_reason = reason != NULL && reason[0] != '\0' ? reason : "state";
	const char *payload_type = strcmp(safe_reason, "heartbeat") == 0 ? "heartbeat" : "status";

	(void)ctx;

	if (buffer == NULL || buffer_size == 0) {
		return ESP_ERR_INVALID_ARG;
	}

	network_get_state(&network);
	rtc_transport_get_stats(&rtc);
	app_get_rtc_config_snapshot(&rtc_settings);
	device_online_get_snapshot(&online);

	int written = snprintf(buffer,
			       buffer_size,
			       "{\"type\":\"%s\",\"reason\":\"%s\",\"seq\":%lu,"
			       "\"ts\":%lld,\"app\":\"%s\",\"network\":%d,"
			       "\"mqtt\":%d,\"bound\":%d,\"ip\":\"%s\",\"rssi\":%d,"
			       "\"rtc_sdk\":%d,"
			       "\"rtc_connected\":%d,\"call_active\":%d,"
			       "\"incoming\":%d,\"door_open\":%d,"
			       "\"device_id\":\"%s\"}",
			       payload_type,
			       safe_reason,
			       (unsigned long)seq,
			       (long long)time(NULL),
			       app_id_name(active_app),
			       network.connected ? 1 : 0,
			       online.mqtt_connected ? 1 : 0,
			       online.bound ? 1 : 0,
			       network.ip_addr,
			       (int)network.rssi,
			       rtc.sdk_initialized ? 1 : 0,
			       rtc.active_connection ? 1 : 0,
			       rtc.call_active ? 1 : 0,
			       rtc.incoming_call_pending ? 1 : 0,
			       door_open ? 1 : 0,
			       rtc_settings.device_id);
	if (written <= 0 || written >= (int)buffer_size) {
		buffer[0] = '\0';
		return ESP_ERR_NO_MEM;
	}
	return ESP_OK;
}

static void app_device_online_rebind_required_cb(void *ctx)
{
	(void)ctx;

	ESP_LOGI(TAG, "device token reset reported by server");
	(void)app_queue_retained_binding_reconcile("token-reset");
}

static void app_device_online_ready_cb(void *ctx)
{
	(void)ctx;

	if (s_app_control_queue == NULL) {
		ESP_LOGW(TAG, "device online event dropped: control queue not ready");
		return;
	}

	const app_control_event_t event = {
		.type = APP_CONTROL_EVENT_DEVICE_ONLINE_READY,
	};
	if (xQueueSendToBack(s_app_control_queue, &event, 0) != pdTRUE) {
		ESP_LOGW(TAG, "device online event dropped: control queue full");
	}
}

static esp_err_t app_handle_device_unbind(const char *reason)
{
	const char *safe_reason = reason != NULL && reason[0] != '\0' ? reason : "mqtt-unbind";
	esp_err_t ack_ret = thing_mqtt_client_wait_last_command_ack(APP_DEVICE_UNBIND_ACK_WAIT_MS);

	if (ack_ret == ESP_OK) {
		ESP_LOGI(TAG, "device unbind command ACK confirmed by broker");
	} else {
		ESP_LOGW(TAG,
			 "device unbind command ACK not confirmed before reconcile: ret=%s",
			 esp_err_to_name(ack_ret));
	}

	ESP_LOGI(TAG, "device unbind command starts signed rebind with retained identity: reason=%s", safe_reason);
	return app_reconcile_binding_with_retained_credentials(safe_reason);
}

static void app_request_device_unbind(const char *reason)
{
	bool already_pending = false;

	if (s_app_control_queue == NULL) {
		ESP_LOGW(TAG, "device unbind event dropped: control queue not ready");
		return;
	}
	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	already_pending = s_device_binding_control_pending;
	if (!already_pending) {
		s_device_binding_control_pending = true;
	}
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	if (already_pending) {
		ESP_LOGI(TAG, "device unbind reset already pending: reason=%s", reason != NULL ? reason : "mqtt-unbind");
		return;
	}

	app_control_event_t event = {
		.type = APP_CONTROL_EVENT_DEVICE_UNBIND,
	};
	strlcpy(event.reason, reason != NULL ? reason : "mqtt-unbind", sizeof(event.reason));

	if (xQueueSendToBack(s_app_control_queue, &event, 0) != pdTRUE) {
		taskENTER_CRITICAL(&s_app_lifecycle_lock);
		s_device_binding_control_pending = false;
		taskEXIT_CRITICAL(&s_app_lifecycle_lock);
		ESP_LOGW(TAG, "device unbind event dropped: queue full reason=%s", event.reason);
	}
}

static void app_clear_device_binding_control_pending(void)
{
	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	s_device_binding_control_pending = false;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
}

static esp_err_t app_configure_device_online(void)
{
	device_online_config_t config = {
		.enabled = APP_CONFIG_DEVICE_BINDING_ENABLE != 0,
		.api_base = thing_service_registry_device_api_base(),
		.mqtt_uri = thing_service_registry_mqtt_uri(),
		.heartbeat_interval_ms = 0,
		.load_credentials = app_device_online_load_credentials,
		.on_message = app_device_online_message_cb,
		.build_status = app_device_online_build_status,
		.on_rebind_required = app_device_online_rebind_required_cb,
		.on_online_ready = app_device_online_ready_cb,
		.ctx = NULL,
		.status_ctx = NULL,
		.rebind_ctx = NULL,
		.online_ready_ctx = NULL,
	};

	return device_online_init(&config);
}

static esp_err_t app_configure_device_call(void)
{
	device_call_config_t config = {
		.enabled = APP_CONFIG_DEVICE_BINDING_ENABLE != 0,
		.api_base = thing_service_registry_call_api_base(),
		.incoming_allowed = app_device_call_incoming_allowed,
		.on_session_ended = app_device_call_session_ended,
		.ctx = NULL,
	};

	return device_call_init(&config);
}

static bool app_ai_chat_session_busy(void)
{
	ai_chat_status_t status = {0};

	ai_chat_get_status(&status);
	return status.state == AI_CHAT_STATE_STARTING ||
	       status.state == AI_CHAT_STATE_TOKEN ||
	       status.state == AI_CHAT_STATE_CONNECTING ||
	       status.state == AI_CHAT_STATE_CONNECTED ||
	       status.state == AI_CHAT_STATE_STARTING_SESSION ||
	       status.state == AI_CHAT_STATE_IN_SESSION ||
	       status.state == AI_CHAT_STATE_STOPPING;
}

static bool app_incoming_session_allowed(app_id_t incoming_app)
{
	app_id_t active_app = app_get_active_app();
	device_call_snapshot_t call = {0};
	wechat_voip_call_state_t wechat = wechat_voip_service_get_call_state();
	rtc_transport_stats_t rtc = {0};
	bool ai_busy = app_ai_chat_session_busy();
	bool call_busy = false;
	bool rtc_busy = false;

	device_call_get_snapshot(&call);
	rtc_transport_get_stats(&rtc);
	call_busy = call.pending_incoming ||
	            call.state == DEVICE_CALL_STATE_OUTGOING ||
	            call.state == DEVICE_CALL_STATE_INCOMING ||
	            call.state == DEVICE_CALL_STATE_CONNECTING ||
	            call.state == DEVICE_CALL_STATE_IN_CALL;

	if ((incoming_app != APP_ID_AI_CHAT && ai_busy) ||
	    (incoming_app != APP_ID_CALL && call_busy) ||
	    (incoming_app != APP_ID_WECHAT && wechat != WECHAT_VOIP_CALL_STATE_IDLE)) {
		ESP_LOGW(TAG,
			 "incoming session rejected busy: incoming=%s owner=%s ai=%d call=%u pending=%d wechat=%u",
			 app_id_name(incoming_app),
			 app_id_name(active_app),
			 ai_busy ? 1 : 0,
			 (unsigned)call.state,
			 call.pending_incoming ? 1 : 0,
			 (unsigned)wechat);
		return false;
	}

	rtc_busy = rtc.active_connection || rtc.call_active || rtc.incoming_call_pending ||
	           rtc.state == RTC_TRANSPORT_STATE_CONNECTED ||
	           rtc.state == RTC_TRANSPORT_STATE_MEDIA_BOOTSTRAPPING ||
	           rtc.state == RTC_TRANSPORT_STATE_DISCONNECTING;
	if (rtc_busy && active_app != APP_ID_DEVICE && active_app != incoming_app) {
		ESP_LOGW(TAG,
			 "incoming session rejected by RTC owner: incoming=%s owner=%s state=%u active=%d call=%d pending=%d",
			 app_id_name(incoming_app),
			 app_id_name(active_app),
			 (unsigned)rtc.state,
			 rtc.active_connection ? 1 : 0,
			 rtc.call_active ? 1 : 0,
			 rtc.incoming_call_pending ? 1 : 0);
		return false;
	}
	return true;
}

static bool app_device_call_incoming_allowed(void *ctx)
{
	(void)ctx;
	return app_incoming_session_allowed(APP_ID_CALL);
}

static bool app_wechat_incoming_allowed(void *ctx)
{
	(void)ctx;
	return app_incoming_session_allowed(APP_ID_WECHAT);
}

static esp_err_t app_configure_incoming_session_policy(void)
{
	return wechat_voip_service_set_incoming_policy(app_wechat_incoming_allowed, NULL);
}

static void app_device_call_session_ended(void *ctx)
{
	(void)ctx;

	if (s_app_control_queue == NULL) {
		ESP_LOGW(TAG, "device call resource release dropped: control queue not ready");
		return;
	}
	const app_control_event_t event = {
		.type = APP_CONTROL_EVENT_CALL_SESSION_ENDED,
	};
	if (xQueueSendToBack(s_app_control_queue, &event, 0) != pdTRUE) {
		ESP_LOGW(TAG, "device call resource release dropped: control queue full");
	} else {
		ESP_LOGI(CALL_FLOW_TAG, "stage=app_session_ended_queued");
	}
}

static void app_control_task(void *arg)
{
	(void)arg;
	app_control_event_t event = {0};

	while (true) {
		if (xQueueReceive(s_app_control_queue, &event, portMAX_DELAY) != pdTRUE) {
			continue;
		}

		switch (event.type) {
		case APP_CONTROL_EVENT_SPEAKER_VOLUME:
		{
			esp_err_t ret = app_set_speaker_volume_internal(event.percent, false);
			if (ret != ESP_OK) {
				ESP_LOGW(TAG,
					 "remote speaker volume apply failed: volume=%u ret=%s",
					 event.percent,
					 esp_err_to_name(ret));
			}
			break;
		}
		case APP_CONTROL_EVENT_RTC_CREDENTIALS_UPDATE:
		{
			const char *reason = event.reason[0] != '\0' ? event.reason : "credential-update";
			esp_err_t ret = app_set_rtc_device_credentials(event.rtc_device_id, event.rtc_device_secret);
			if (ret != ESP_OK) {
				ESP_LOGW(TAG,
					 "rtc credential save failed: reason=%s ret=%s",
					 reason,
					 esp_err_to_name(ret));
				break;
			}
			ESP_LOGD(TAG,
				 "rtc credential saved: reason=%s device_id_len=%u",
				 reason,
				 (unsigned)strlen(event.rtc_device_id));
			app_restart_identity_after_credentials(event.rtc_device_id, reason);
			ESP_LOGD(TAG,
				 "rtc credential update worker stack_hwm=%u",
				 (unsigned)uxTaskGetStackHighWaterMark(NULL));
			break;
		}
		case APP_CONTROL_EVENT_RTC_RECONFIGURE:
		{
			const char *reason = event.reason[0] != '\0' ? event.reason : "settings";
			ESP_LOGD(TAG, "rtc config apply begin: reason=%s", reason);
			esp_err_t ret = app_reconfigure_tirtc_after_settings_change(reason);
			if (ret != ESP_OK) {
				ESP_LOGW(TAG,
					 "rtc config apply failed: reason=%s ret=%s",
					 reason,
					 esp_err_to_name(ret));
			} else {
				ESP_LOGD(TAG, "rtc config apply done: reason=%s", reason);
			}
			break;
		}
		case APP_CONTROL_EVENT_RTC_PREPARE_AFTER_IDENTITY:
		{
			const char *reason = event.reason[0] != '\0' ? event.reason : "identity-ready";
			app_prepare_rtc_when_identity_ready(reason);
			break;
		}
		case APP_CONTROL_EVENT_RTC_IDENTITY_CONFLICT:
		{
			if (!app_rtc_identity_conflict_mark_pending(event.rtc_device_id, event.rtc_client_id)) {
				ESP_LOGW(TAG,
					 "rtc device ownership conflict already reported: device_id=%s physical_client_id=%s, binding preserved",
					 event.rtc_device_id,
					 event.rtc_client_id);
				break;
			}
			ESP_LOGW(TAG,
				 "rtc device ownership conflict: device_id=%s is registered to another client_id; physical_client_id=%s, preserve binding and wait for server mapping repair",
				 event.rtc_device_id,
				 event.rtc_client_id);
			(void)app_report_device_state_async("rtc-client-conflict");
			break;
		}
		case APP_CONTROL_EVENT_DEVICE_UNBIND:
		{
			const char *reason = event.reason[0] != '\0' ? event.reason : "mqtt-unbind";
			esp_err_t ret = app_handle_device_unbind(reason);
			app_clear_device_binding_control_pending();
			if (ret != ESP_OK) {
				ESP_LOGW(TAG,
					 "device unbind flow failed: reason=%s ret=%s",
					 reason,
					 esp_err_to_name(ret));
			}
			break;
		}
		case APP_CONTROL_EVENT_DEVICE_BINDING_VERIFY:
		{
			const char *reason = event.reason[0] != '\0' ? event.reason : "binding-check";
			esp_err_t ret = app_verify_device_binding_internal(reason);
			app_clear_device_binding_control_pending();
			if (ret != ESP_OK) {
				ESP_LOGW(TAG,
					 "device binding verification failed: reason=%s ret=%s",
					 reason,
					 esp_err_to_name(ret));
			}
			break;
		}
		case APP_CONTROL_EVENT_DEVICE_REBIND_REQUIRED:
		{
			const char *reason = event.reason[0] != '\0' ? event.reason : "token-reset";
			esp_err_t ret = app_reconcile_binding_with_retained_credentials(reason);
			app_clear_device_binding_control_pending();
			if (ret != ESP_OK) {
				ESP_LOGW(TAG,
					 "device retained rebind flow failed: reason=%s ret=%s",
					 reason,
					 esp_err_to_name(ret));
			}
			break;
		}
		case APP_CONTROL_EVENT_DEVICE_ONLINE_READY:
		{
			/* Registration happens before connect so no cmd is missed.  Re-run
			 * startup recovery only after formal MQTT is confirmed online. */
			app_rtc_config_snapshot_t rtc_settings = {0};
			app_id_t active_app = app_get_active_app();
			app_get_rtc_config_snapshot(&rtc_settings);
			(void)device_binding_confirm_online(rtc_settings.device_id, "formal mqtt online");
			app_start_device_identity_ingress();
			app_prepare_rtc_when_identity_ready("mqtt-online");
			if (active_app == APP_ID_AI_CHAT) {
				app_request_ai_chat_start_if_idle("mqtt-online");
			} else if (active_app == APP_ID_DEVICE || active_app == APP_ID_WECHAT) {
				(void)app_enqueue_lifecycle_event(APP_LIFECYCLE_EVENT_START_APP_SERVICES,
							  active_app);
			}
			break;
		}
		case APP_CONTROL_EVENT_CALL_SESSION_ENDED:
			app_release_call_session_resources_if_idle();
			ESP_LOGI(CALL_FLOW_TAG, "stage=app_session_ended_handled");
			break;
		default:
			ESP_LOGW(TAG, "unknown app control event: type=%u", (unsigned)event.type);
			break;
		}
	}
}

static void app_lifecycle_task(void *arg)
{
	(void)arg;
	app_lifecycle_event_t event = {0};

	while (true) {
		if (xQueueReceive(s_app_lifecycle_queue, &event, portMAX_DELAY) != pdTRUE) {
			continue;
		}

		esp_err_t ret = ESP_OK;
		switch (event.type) {
		case APP_LIFECYCLE_EVENT_ENTER_APP:
			ret = app_enter_app_sync(event.app_id);
			if (ret != ESP_OK) {
				ESP_LOGW(TAG,
					 "lifecycle enter failed: app=%s ret=%s",
					 app_id_name(event.app_id),
					 esp_err_to_name(ret));
			}
			break;
		case APP_LIFECYCLE_EVENT_RETURN_HOME:
			ret = app_return_home_sync();
			if (ret != ESP_OK) {
				ESP_LOGW(TAG, "lifecycle return home failed: %s", esp_err_to_name(ret));
			}
			break;
		case APP_LIFECYCLE_EVENT_START_APP_SERVICES:
			if (app_get_active_app() == event.app_id) {
				ret = app_start_app_services(event.app_id);
				if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
					ESP_LOGW(TAG,
						 "lifecycle start service failed: app=%s ret=%s",
						 app_id_name(event.app_id),
						 esp_err_to_name(ret));
				}
			}
			break;
		case APP_LIFECYCLE_EVENT_AI_CHAT_CALL_CONTACT:
			app_handle_ai_chat_call_contact(event.call_route,
							event.call_target_id,
							event.call_type);
			break;
		default:
			ESP_LOGW(TAG, "unknown lifecycle event: type=%u", (unsigned)event.type);
			break;
		}
	}
}

static esp_err_t app_start_control_task(void)
{
	if (s_app_transition_mutex == NULL) {
		s_app_transition_mutex = xSemaphoreCreateMutexStatic(&s_app_transition_mutex_buffer);
		if (s_app_transition_mutex == NULL) {
			return ESP_ERR_NO_MEM;
		}
	}

	if (s_app_control_queue == NULL) {
		s_app_control_queue = xQueueCreateWithCaps(APP_CONTROL_QUEUE_LENGTH,
							   sizeof(app_control_event_t),
							   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
		if (s_app_control_queue == NULL) {
			return ESP_ERR_NO_MEM;
		}
	}

	if (s_app_control_task == NULL) {
		BaseType_t task_ret = xTaskCreateWithCaps(app_control_task,
							  "app_ctrl",
							  APP_CONTROL_TASK_STACK_SIZE,
							  NULL,
							  APP_CONTROL_TASK_PRIORITY,
							  &s_app_control_task,
							  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
		if (task_ret != pdPASS) {
			vQueueDeleteWithCaps(s_app_control_queue);
			s_app_control_queue = NULL;
			return ESP_ERR_NO_MEM;
		}
	}

	if (s_app_lifecycle_queue == NULL) {
		s_app_lifecycle_queue = xQueueCreateWithCaps(APP_LIFECYCLE_QUEUE_LENGTH,
							     sizeof(app_lifecycle_event_t),
							     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
		if (s_app_lifecycle_queue == NULL) {
			return ESP_ERR_NO_MEM;
		}
	}

	if (s_app_lifecycle_task == NULL) {
		BaseType_t task_ret = xTaskCreateWithCaps(app_lifecycle_task,
							  "app_lifecycle",
							  APP_LIFECYCLE_TASK_STACK_SIZE,
							  NULL,
							  APP_LIFECYCLE_TASK_PRIORITY,
							  &s_app_lifecycle_task,
							  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
		if (task_ret != pdPASS) {
			vQueueDeleteWithCaps(s_app_lifecycle_queue);
			s_app_lifecycle_queue = NULL;
			return ESP_ERR_NO_MEM;
		}
	}

	return ESP_OK;
}

static esp_err_t app_enqueue_lifecycle_event(app_lifecycle_event_type_t type, app_id_t app_id)
{
	if (s_app_lifecycle_queue == NULL) {
		return ESP_ERR_INVALID_STATE;
	}

	app_lifecycle_event_t event = {
		.type = type,
		.app_id = app_id,
	};

	/*
	 * The lifecycle queue is the serialization boundary. Resetting it here can
	 * silently discard an already accepted home/app/action transition.
	 */
	return xQueueSendToBack(s_app_lifecycle_queue, &event, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t app_enqueue_ai_chat_call_lifecycle_event(ai_chat_device_action_route_t route,
							 const char *target_id,
							 const char *call_type)
{
	if (s_app_lifecycle_queue == NULL ||
	    !app_ai_call_target_valid(route, target_id) ||
	    !app_ai_call_type_valid(route, call_type)) {
		return ESP_ERR_INVALID_STATE;
	}

	app_lifecycle_event_t event = {
		.type = APP_LIFECYCLE_EVENT_AI_CHAT_CALL_CONTACT,
		.app_id = route == AI_CHAT_DEVICE_ACTION_ROUTE_WECHAT_VOIP ?
			      APP_ID_WECHAT :
			      APP_ID_CALL,
		.call_route = route,
	};
	strlcpy(event.call_target_id, target_id, sizeof(event.call_target_id));
	strlcpy(event.call_type, call_type, sizeof(event.call_type));

	/*
	 * A successful JSON-RPC action response already promised that this
	 * asynchronous handoff was accepted. Wait for queue space rather than
	 * dropping that committed transition.
	 */
	return xQueueSendToBack(s_app_lifecycle_queue, &event, portMAX_DELAY) == pdTRUE ?
		       ESP_OK :
		       ESP_ERR_TIMEOUT;
}

static void app_request_rtc_reconfigure_after_settings_change(const char *reason)
{
	if (s_app_control_queue == NULL) {
		ESP_LOGW(TAG, "rtc config saved; apply skipped because app control queue is not ready");
		return;
	}

	app_control_event_t event = {
		.type = APP_CONTROL_EVENT_RTC_RECONFIGURE,
	};
	strlcpy(event.reason, reason != NULL ? reason : "settings", sizeof(event.reason));

	if (xQueueSendToBack(s_app_control_queue, &event, 0) != pdTRUE) {
		ESP_LOGW(TAG,
			 "rtc config saved; apply event dropped because app control queue is full: reason=%s",
			 event.reason);
	}
}

static bool app_ai_chat_can_auto_start(void)
{
	ai_chat_status_t status = {0};

	ai_chat_get_status(&status);
	return status.state == AI_CHAT_STATE_IDLE && status.last_error == 0;
}

static void app_request_ai_chat_start_if_idle(const char *reason)
{
	if (app_get_active_app() != APP_ID_AI_CHAT || !app_ai_chat_can_auto_start()) {
		return;
	}

	esp_err_t ret = app_enqueue_lifecycle_event(APP_LIFECYCLE_EVENT_START_APP_SERVICES, APP_ID_AI_CHAT);
	if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
		ESP_LOGW(TAG,
			 "queue AI Chat start failed: reason=%s ret=%s",
			 reason != NULL ? reason : "unknown",
			 esp_err_to_name(ret));
	}
}

static esp_err_t app_enqueue_speaker_volume(uint8_t percent)
{
	if (s_app_control_queue == NULL) {
		return ESP_ERR_INVALID_STATE;
	}

	if (percent > 100U) {
		percent = 100U;
	}

	app_control_event_t event = {
		.type = APP_CONTROL_EVENT_SPEAKER_VOLUME,
		.percent = percent,
	};

	if (xQueueSendToBack(s_app_control_queue, &event, 0) == pdTRUE) {
		return ESP_OK;
	}

	ESP_LOGW(TAG, "speaker volume command dropped: control queue full volume=%u", (unsigned)percent);
	return ESP_ERR_TIMEOUT;
}

static esp_err_t app_rtc_set_speaker_volume(uint8_t percent, void *ctx)
{
	(void)ctx;
	return app_enqueue_speaker_volume(percent);
}

static esp_err_t app_rtc_set_door_open(bool open, void *ctx)
{
	(void)ctx;

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	s_door_open = open;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);

	ESP_LOGD(TAG, "door command accepted: state=%s hardware_driver=not_configured", open ? "open" : "locked");
	(void)app_report_device_state_async("door");
	return ESP_OK;
}

static void app_register_rtc_control_ops(void)
{
	const rtc_transport_control_ops_t ops = {
		.set_speaker_volume = app_rtc_set_speaker_volume,
		.set_door_open = app_rtc_set_door_open,
	};

	rtc_transport_set_control_ops(&ops, NULL);
}

static esp_err_t app_register_rtc_observer(void)
{
	const rtc_transport_observer_t observer = {
		.on_start_error = app_request_rtc_identity_conflict,
	};

	return rtc_transport_register_observer(&observer, NULL);
}

static bool app_rtc_runtime_is_initialized(void)
{
	bool initialized = false;

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	initialized = s_rtc_runtime_initialized;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	return initialized;
}

static bool app_rtc_runtime_begin_init(void)
{
	bool should_init = false;

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	if (!s_rtc_runtime_initialized && !s_rtc_runtime_init_in_progress) {
		s_rtc_runtime_init_in_progress = true;
		should_init = true;
	}
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	return should_init;
}

static void app_rtc_runtime_finish_init(esp_err_t result)
{
	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	if (result == ESP_OK) {
		s_rtc_runtime_initialized = true;
	}
	s_rtc_runtime_init_in_progress = false;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
}

static void app_rtc_sdk_set_prepared(bool prepared)
{
	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	s_rtc_sdk_prepared = prepared;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
}

static bool app_rtc_sdk_is_prepared(void)
{
	bool prepared = false;

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	prepared = s_rtc_sdk_prepared;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	return prepared;
}

static void app_mark_rtc_identity_reconfigure_pending(void)
{
	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	s_rtc_identity_reconfigure_pending = true;
	s_rtc_sdk_prepared = false;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
}

static bool app_rtc_identity_reconfigure_is_pending(void)
{
	bool pending = false;

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	pending = s_rtc_identity_reconfigure_pending;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	return pending;
}

static void app_clear_rtc_identity_reconfigure_pending(void)
{
	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	s_rtc_identity_reconfigure_pending = false;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
}

static void app_suspend_device_identity_services(const char *reason)
{
	const char *safe_reason = reason != NULL && reason[0] != '\0' ? reason : "identity-change";

	wechat_voip_service_suspend_ingress();
	device_call_reset_identity_state();
	app_mark_rtc_identity_reconfigure_pending();
	rtc_transport_set_identity_ready(false);
	(void)rtc_transport_disconnect();
	(void)app_state_sync_call_media_defaults(false, NULL);
	ESP_LOGI(TAG, "device identity services suspended: reason=%s", safe_reason);
}

static esp_err_t app_apply_pending_rtc_identity_config(const char *reason)
{
	const char *safe_reason = reason != NULL && reason[0] != '\0' ? reason : "identity-ready";
	uint32_t waited_ms = 0;

	if (!app_rtc_identity_reconfigure_is_pending()) {
		return ESP_OK;
	}

	if (!app_rtc_runtime_is_initialized()) {
		app_clear_rtc_identity_reconfigure_pending();
		return ESP_OK;
	}

	while (waited_ms < APP_RTC_IDENTITY_RESET_WAIT_MS) {
		rtc_transport_stats_t stats = {0};
		rtc_transport_get_stats(&stats);
		if (!stats.sdk_initialized && !stats.sdk_started && !stats.active_connection) {
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(APP_RTC_PREPARE_POLL_MS));
		waited_ms += APP_RTC_PREPARE_POLL_MS;
	}

	rtc_transport_stats_t stats = {0};
	rtc_transport_get_stats(&stats);
	if (stats.sdk_initialized || stats.sdk_started || stats.active_connection) {
		ESP_LOGW(TAG,
			 "rtc identity reset wait timed out: reason=%s waited_ms=%u sdk_init=%d sdk_started=%d conn=%d state=%d",
			 safe_reason,
			 (unsigned)waited_ms,
			 stats.sdk_initialized ? 1 : 0,
			 stats.sdk_started ? 1 : 0,
			 stats.active_connection ? 1 : 0,
			 (int)stats.state);
		return ESP_ERR_TIMEOUT;
	}

	esp_err_t ret = app_configure_tirtc();
	if (ret != ESP_OK) {
		ESP_LOGW(TAG,
			 "rtc identity config apply failed: reason=%s waited_ms=%u ret=%s",
			 safe_reason,
			 (unsigned)waited_ms,
			 esp_err_to_name(ret));
		return ret;
	}

	app_clear_rtc_identity_reconfigure_pending();
	ESP_LOGI(TAG,
		 "rtc identity config applied after full reset: reason=%s waited_ms=%u",
		 safe_reason,
		 (unsigned)waited_ms);
	return ESP_OK;
}

static esp_err_t app_init_rtc_transport(void)
{
	if (app_rtc_runtime_is_initialized()) {
		return ESP_OK;
	}
	if (!app_rtc_runtime_begin_init()) {
		return ESP_ERR_INVALID_STATE;
	}

	rtc_transport_config_t *rtc_config = calloc(1, sizeof(*rtc_config));
	if (rtc_config == NULL) {
		app_rtc_runtime_finish_init(ESP_ERR_NO_MEM);
		return ESP_ERR_NO_MEM;
	}

	esp_err_t ret = app_build_rtc_transport_config(rtc_config);
	if (ret == ESP_OK) {
		ret = rtc_transport_init(rtc_config);
		if (ret == ESP_OK) {
			app_register_rtc_control_ops();
			ret = app_register_rtc_observer();
		}
	}
	free(rtc_config);
	app_rtc_runtime_finish_init(ret);
	return ret;
}

static network_config_t app_make_network_config(void)
{
	network_config_t config = {
		.enabled = APP_CONFIG_WIFI_ENABLE != 0,
		.auto_connect = APP_CONFIG_WIFI_AUTO_CONNECT != 0,
		.default_ssid = APP_CONFIG_WIFI_SSID,
		.default_password = APP_CONFIG_WIFI_PASSWORD,
	};

	return config;
}

static ota_config_t app_make_ota_config(void)
{
	ota_config_t config = {
		.default_url = APP_CONFIG_OTA_DEFAULT_URL,
	};

	return config;
}

static esp_err_t app_start_network_baseline(void)
{
	network_config_t network_config = app_make_network_config();
	esp_err_t ret = network_prepare(&network_config);
	if (ret != ESP_OK) {
		return ret;
	}

	return ESP_OK;
}

static esp_err_t app_prepare_rtc_if_network_ready(void)
{
	if (!network_is_connected()) {
		return ESP_OK;
	}

	if (system_time_has_valid_time()) {
		if (!thing_service_registry_is_ready()) {
			app_schedule_thing_bootstrap("prepare");
			return ESP_ERR_INVALID_STATE;
		}
		esp_err_t identity_ret = app_start_device_identity_services("prepare");
		if (identity_ret != ESP_OK && identity_ret != ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "device identity start before rtc prepare failed: %s", esp_err_to_name(identity_ret));
		}
		if (!app_device_binding_ready_for_rtc("prepare")) {
			return ESP_ERR_INVALID_STATE;
		}
	}

	esp_err_t prepare_ret = app_prepare_rtc_after_time_sync("prepare");
	if (prepare_ret != ESP_OK) {
		if (prepare_ret == ESP_ERR_INVALID_STATE) {
			ESP_LOGD(TAG, "rtc prepare waits for network time/sdk init");
		}
		return prepare_ret;
	}

	rtc_transport_network_state_t rtc_network = {
		.connected = true,
	};
	rtc_transport_on_network_state_changed(&rtc_network);
	return ESP_OK;
}

static esp_err_t app_prepare_rtc_after_time_sync(const char *reason)
{
	if (!network_is_connected()) {
		return ESP_ERR_INVALID_STATE;
	}
	if (!system_time_has_valid_time()) {
		esp_err_t time_ret = system_time_request_sync(false);
		if (time_ret != ESP_OK) {
			ESP_LOGW(TAG, "schedule system time sync failed: %s", esp_err_to_name(time_ret));
		}
		return ESP_ERR_INVALID_STATE;
	}

#if APP_CONFIG_DEVICE_BINDING_ENABLE != 0
	device_online_snapshot_t online = {0};

	if (!device_online_is_online()) {
		device_online_get_snapshot(&online);
		ESP_LOGW(TAG,
			 "rtc prepare waits for ThingConnect online identity: reason=%s state=%d running=%d mqtt=%d",
			 reason != NULL ? reason : "time",
			 (int)online.state,
			 online.running ? 1 : 0,
			 online.mqtt_connected ? 1 : 0);
		return ESP_ERR_INVALID_STATE;
	}
#endif
	esp_err_t ret = app_apply_pending_rtc_identity_config(reason);
	if (ret != ESP_OK) {
		return ret;
	}
	rtc_transport_set_identity_ready(true);

	ret = app_init_rtc_transport();
	if (ret != ESP_OK) {
		return ret;
	}

	rtc_transport_network_state_t rtc_network = {
		.connected = true,
	};
	rtc_transport_on_network_state_changed(&rtc_network);

	bool was_prepared = app_rtc_sdk_is_prepared();
	ret = rtc_transport_prepare_sdk();
	if (ret == ESP_OK) {
		rtc_transport_stats_t rtc_stats = {0};
		rtc_transport_get_stats(&rtc_stats);
		if (!rtc_stats.sdk_initialized) {
			app_rtc_sdk_set_prepared(false);
			return ESP_ERR_INVALID_STATE;
		}
		app_rtc_sdk_set_prepared(true);
		if (!was_prepared) {
			ESP_LOGI(TAG, "rtc sdk initialized after time sync: reason=%s", reason != NULL ? reason : "time");
		}
		return ESP_OK;
	}
	if (ret != ESP_ERR_INVALID_STATE) {
		app_rtc_sdk_set_prepared(false);
		ESP_LOGW(TAG, "rtc sdk init after time sync failed: %s", esp_err_to_name(ret));
	}
	return ret;
}

static esp_err_t app_prepare_rtc_after_config_if_ready(const char *reason)
{
	if (!network_is_connected() || !system_time_has_valid_time()) {
		return ESP_OK;
	}

	esp_err_t ret = app_prepare_rtc_after_time_sync(reason != NULL ? reason : "settings");
	if (ret == ESP_ERR_INVALID_STATE) {
		return ESP_OK;
	}
	return ret;
}

static esp_err_t app_start_device_binding_reconcile_if_needed(const char *reason)
{
	device_binding_snapshot_t binding = {0};

	if (!network_is_connected() || !system_time_has_valid_time()) {
		return ESP_ERR_INVALID_STATE;
	}
	if (!app_rtc_device_credentials_available()) {
		return device_binding_start_async(reason != NULL ? reason : "auto");
	}

	device_binding_get_snapshot(&binding);
	if (binding.running ||
	    binding.state == DEVICE_BINDING_STATE_WAITING_USER) {
		return ESP_OK;
	}

	/*
	 * Bound devices follow the Python reference flow: try /device/token first.
	 * A 6006 token response starts signed Report from the retained credentials
	 * path; do not proactively consume a new verification code on every boot.
	 */
	return ESP_OK;
}

static bool app_device_binding_ready_for_rtc(const char *reason)
{
	device_binding_snapshot_t binding = {0};

	if (!app_rtc_device_credentials_available()) {
		ESP_LOGD(TAG,
			 "rtc prepare skipped before binding: reason=%s",
			 reason != NULL ? reason : "identity-ready");
		return false;
	}

	device_binding_get_snapshot(&binding);
	if (binding.state == DEVICE_BINDING_STATE_REPORTING ||
	    binding.state == DEVICE_BINDING_STATE_WAITING_USER) {
		ESP_LOGW(TAG,
			 "rtc prepare waits for device binding reconciliation: reason=%s running=%d state=%d",
			 reason != NULL ? reason : "identity-ready",
			 binding.running ? 1 : 0,
			 (int)binding.state);
		return false;
	}
	if (binding.state == DEVICE_BINDING_STATE_ERROR && !device_online_is_online()) {
		ESP_LOGW(TAG,
			 "rtc prepare waits for formal identity verification after binding error: reason=%s ret=%s",
			 reason != NULL ? reason : "identity-ready",
			 esp_err_to_name(binding.last_error));
		return false;
	}

	ESP_LOGD(TAG,
		 "rtc prepare binding gate passed: reason=%s state=%d",
		 reason != NULL ? reason : "identity-ready",
		 (int)binding.state);
	return true;
}

static bool app_device_binding_ready_for_online_services(const char *reason)
{
	device_binding_snapshot_t binding = {0};

	if (!app_rtc_device_credentials_available()) {
		ESP_LOGW(TAG,
			 "online service waits for device binding: service=%s reason=no-credentials",
			 reason != NULL ? reason : "unknown");
		return false;
	}

	device_binding_get_snapshot(&binding);
	if (binding.state == DEVICE_BINDING_STATE_REPORTING ||
	    binding.state == DEVICE_BINDING_STATE_WAITING_USER) {
		ESP_LOGW(TAG,
			 "online service waits for device binding: service=%s state=%d running=%d ret=%s",
			 reason != NULL ? reason : "unknown",
			 (int)binding.state,
			 binding.running ? 1 : 0,
			 esp_err_to_name(binding.last_error));
		return false;
	}
	if (!device_online_is_online()) {
		ESP_LOGD(TAG,
			 "online service waits for formal MQTT: service=%s binding_state=%d",
			 reason != NULL ? reason : "unknown",
			 (int)binding.state);
		return false;
	}
	return true;
}

static void app_prepare_rtc_when_identity_ready(const char *reason)
{
	device_online_snapshot_t online = {0};

	if (!app_device_binding_ready_for_rtc(reason)) {
		return;
	}

	device_online_get_snapshot(&online);
	ESP_LOGD(TAG,
		 "rtc prepare after identity gate: reason=%s online_state=%d online_running=%d mqtt=%d",
		 reason != NULL ? reason : "identity-ready",
		 (int)online.state,
		 online.running ? 1 : 0,
		 online.mqtt_connected ? 1 : 0);
	if (!device_online_is_online()) {
		ESP_LOGW(TAG,
			 "rtc prepare skipped: ThingConnect identity is not online reason=%s state=%d running=%d mqtt=%d",
			 reason != NULL ? reason : "identity-ready",
			 (int)online.state,
			 online.running ? 1 : 0,
			 online.mqtt_connected ? 1 : 0);
		return;
	}

	esp_err_t ret = app_prepare_rtc_after_time_sync(reason != NULL ? reason : "identity-ready");
	if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
		ESP_LOGW(TAG, "prepare rtc after identity gate failed: %s", esp_err_to_name(ret));
	} else if (ret == ESP_OK) {
		app_schedule_ai_chat_token_prefetch(reason);
	}
}

static void app_request_rtc_prepare_after_identity(const char *reason)
{
	if (s_app_control_queue == NULL) {
		return;
	}

	app_control_event_t event = {
		.type = APP_CONTROL_EVENT_RTC_PREPARE_AFTER_IDENTITY,
	};
	strlcpy(event.reason, reason != NULL ? reason : "identity-ready", sizeof(event.reason));
	if (xQueueSend(s_app_control_queue, &event, 0) != pdTRUE) {
		ESP_LOGW(TAG, "rtc prepare request dropped: reason=%s", event.reason);
	}
}

static void app_request_rtc_identity_conflict(int error,
					      const char *device_id,
					      const char *client_id,
					      void *ctx)
{
	(void)ctx;

	if (error != TIRTC_SESSION_SERVICE_CODE_CLIENT_ID_CONFLICT) {
		return;
	}
	if (s_app_control_queue == NULL) {
		ESP_LOGW(TAG,
			 "rtc device ownership conflict detected before control queue ready: device_id=%s physical_client_id=%s",
			 device_id != NULL ? device_id : "",
			 client_id != NULL ? client_id : "");
		return;
	}

	app_control_event_t event = {
		.type = APP_CONTROL_EVENT_RTC_IDENTITY_CONFLICT,
	};
	strlcpy(event.reason, "rtc-client-conflict", sizeof(event.reason));
	strlcpy(event.rtc_device_id, device_id != NULL ? device_id : "", sizeof(event.rtc_device_id));
	strlcpy(event.rtc_client_id, client_id != NULL ? client_id : "", sizeof(event.rtc_client_id));
	if (xQueueSendToBack(s_app_control_queue, &event, pdMS_TO_TICKS(200)) != pdTRUE) {
		ESP_LOGW(TAG,
			 "rtc device ownership conflict event dropped: device_id=%s physical_client_id=%s",
			 event.rtc_device_id,
			 event.rtc_client_id);
	}
}

static bool app_should_prepare_rtc_for_active_app(app_id_t app_id)
{
	(void)app_id;
	return true;
}

static void app_thing_bootstrap_task(void *arg)
{
	app_thing_bootstrap_context_t *context = (app_thing_bootstrap_context_t *)arg;
	char reason[APP_RTC_RECONFIGURE_REASON_MAX] = "thing-bootstrap";

	if (context != NULL && context->reason[0] != '\0') {
		strlcpy(reason, context->reason, sizeof(reason));
	}

	if (network_is_connected() && system_time_has_valid_time()) {
		esp_err_t discovery_ret = thing_service_registry_refresh();
		if (discovery_ret != ESP_OK) {
			ESP_LOGW(TAG,
				 "service discovery failed, using configured fallback endpoints: %s",
				 esp_err_to_name(discovery_ret));
		}

		esp_err_t call_ret = device_call_set_api_base(thing_service_registry_call_api_base());
		if (call_ret != ESP_OK) {
			ESP_LOGW(TAG, "device call endpoint update failed: %s", esp_err_to_name(call_ret));
		}

		if (network_is_connected() && system_time_has_valid_time()) {
			app_id_t active_app = app_get_active_app();
			esp_err_t identity_ret = app_start_device_identity_services(reason);
			if (identity_ret != ESP_OK && identity_ret != ESP_ERR_INVALID_STATE) {
				ESP_LOGW(TAG,
					 "device identity start after service discovery failed: %s",
					 esp_err_to_name(identity_ret));
			}
			(void)app_report_device_state_async(reason);
			if (app_should_prepare_rtc_for_active_app(active_app)) {
				app_request_rtc_prepare_after_identity(reason);
			}
			if (active_app == APP_ID_AI_CHAT) {
				app_request_ai_chat_start_if_idle(reason);
			}
		}
	}

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	s_thing_bootstrap_running = false;
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	free(context);
	vTaskDeleteWithCaps(NULL);
}

static void app_schedule_thing_bootstrap(const char *reason)
{
	app_thing_bootstrap_context_t *context = NULL;
	bool already_running = false;

	if (!network_is_connected() || !system_time_has_valid_time()) {
		return;
	}

	context = heap_caps_calloc(1,
					  sizeof(*context),
					  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (context == NULL) {
		context = calloc(1, sizeof(*context));
	}
	if (context == NULL) {
		ESP_LOGW(TAG, "service bootstrap context allocation failed");
		return;
	}
	strlcpy(context->reason,
		reason != NULL && reason[0] != '\0' ? reason : "thing-bootstrap",
		sizeof(context->reason));

	taskENTER_CRITICAL(&s_app_lifecycle_lock);
	already_running = s_thing_bootstrap_running;
	if (!already_running) {
		s_thing_bootstrap_running = true;
	}
	taskEXIT_CRITICAL(&s_app_lifecycle_lock);
	if (already_running) {
		free(context);
		return;
	}

	BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(app_thing_bootstrap_task,
							     "thing_bootstrap",
							     APP_THING_BOOTSTRAP_TASK_STACK_SIZE,
							     context,
							     APP_THING_BOOTSTRAP_TASK_PRIORITY,
							     NULL,
							     APP_TASK_CORE_BACKGROUND,
							     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (task_ret != pdPASS) {
		taskENTER_CRITICAL(&s_app_lifecycle_lock);
		s_thing_bootstrap_running = false;
		taskEXIT_CRITICAL(&s_app_lifecycle_lock);
		free(context);
		ESP_LOGW(TAG, "service bootstrap task create failed");
	}
}

static void app_time_sync_cb(esp_err_t result, bool time_valid, void *ctx)
{
	(void)ctx;

	if (result != ESP_OK || !time_valid) {
		ESP_LOGW(TAG,
			 "system time sync callback: result=%s valid=%d",
			 esp_err_to_name(result),
			 time_valid ? 1 : 0);
		return;
	}

	app_schedule_thing_bootstrap("time-sync");
}

static esp_err_t app_acquire_rtc_resource(void)
{
	ESP_RETURN_ON_ERROR(app_apply_media_policy(), TAG, "apply media policy failed");
	return ESP_OK;
}

static void app_release_rtc_resource(void)
{
	rtc_transport_flush_remote_media();
	esp_err_t disconnect_ret = rtc_transport_disconnect();
	if (disconnect_ret != ESP_OK && disconnect_ret != ESP_ERR_INVALID_STATE) {
		ESP_LOGW(TAG, "app lifecycle rtc disconnect failed: %s", esp_err_to_name(disconnect_ret));
	}
	rtc_transport_flush_remote_media();
	(void)app_state_sync_call_media_defaults(false, NULL);
}

static bool app_rtc_session_needs_hangup(const rtc_transport_stats_t *stats)
{
	if (stats == NULL) {
		return false;
	}

	return stats->active_connection || stats->call_active || stats->incoming_call_pending ||
	       stats->state == RTC_TRANSPORT_STATE_CONNECTED ||
	       stats->state == RTC_TRANSPORT_STATE_MEDIA_BOOTSTRAPPING ||
	       stats->state == RTC_TRANSPORT_STATE_DISCONNECTING;
}

static esp_err_t app_hangup_rtc_session_if_active(app_id_t owner)
{
	rtc_transport_stats_t stats = {0};
	esp_err_t hangup_ret = ESP_OK;
	esp_err_t disconnect_ret = ESP_OK;

	if (!app_rtc_runtime_is_initialized()) {
		return ESP_OK;
	}

	rtc_transport_get_stats(&stats);
	if (!app_rtc_session_needs_hangup(&stats)) {
		return ESP_OK;
	}

	ESP_LOGI(TAG,
		 "app lifecycle hangup: app=%s state=%u active=%d call=%d incoming=%d",
		 app_id_name(owner),
		 (unsigned)stats.state,
		 stats.active_connection ? 1 : 0,
		 stats.call_active ? 1 : 0,
		 stats.incoming_call_pending ? 1 : 0);

	if (stats.active_connection && stats.state != RTC_TRANSPORT_STATE_DISCONNECTING) {
		hangup_ret = rtc_transport_hangup();
		if (hangup_ret != ESP_OK && hangup_ret != ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "app lifecycle hangup command failed: %s", esp_err_to_name(hangup_ret));
		}
	}
	rtc_transport_flush_remote_media();

	disconnect_ret = rtc_transport_disconnect();
	if (disconnect_ret != ESP_OK && disconnect_ret != ESP_ERR_INVALID_STATE) {
		ESP_LOGW(TAG, "app lifecycle disconnect after hangup failed: %s", esp_err_to_name(disconnect_ret));
	}
	rtc_transport_flush_remote_media();

	(void)app_state_sync_call_media_defaults(false, NULL);
	if (disconnect_ret != ESP_OK && disconnect_ret != ESP_ERR_INVALID_STATE) {
		return disconnect_ret;
	}
	if (hangup_ret != ESP_OK && hangup_ret != ESP_ERR_INVALID_STATE) {
		return hangup_ret;
	}
	return ESP_OK;
}

static esp_err_t app_acquire_audio_resource(void)
{
	return audio_device_prepare();
}

static void app_release_audio_resource(void)
{
	(void)microphone_set_enabled(false);
	speaker_stop_playback();
	audio_device_release();
}

static esp_err_t app_acquire_camera_resource(void)
{
	if (!camera_driver_is_configured()) {
		return ESP_OK;
	}
	return camera_driver_init();
}

static void app_release_camera_resource(void)
{
	esp_err_t ret = camera_driver_deinit();
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "camera release failed: %s", esp_err_to_name(ret));
	}
}

static void app_release_resources(uint32_t resources);

static esp_err_t app_acquire_resources(uint32_t resources)
{
	uint32_t acquired_resources = 0U;
	esp_err_t ret = ESP_OK;

	if ((resources & APP_RESOURCE_RTC) != 0U) {
		ret = app_acquire_rtc_resource();
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "acquire rtc failed: %s", esp_err_to_name(ret));
			goto rollback;
		}
		acquired_resources |= APP_RESOURCE_RTC;
	}
	if ((resources & APP_RESOURCE_AUDIO) != 0U) {
		ret = app_acquire_audio_resource();
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "acquire audio failed: %s", esp_err_to_name(ret));
			goto rollback;
		}
		acquired_resources |= APP_RESOURCE_AUDIO;
	}
	if ((resources & APP_RESOURCE_CAMERA) != 0U) {
		ret = app_acquire_camera_resource();
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "acquire camera failed: %s", esp_err_to_name(ret));
			goto rollback;
		}
		acquired_resources |= APP_RESOURCE_CAMERA;
	}
	return ESP_OK;

rollback:
	app_release_resources(acquired_resources);
	return ret;
}

static void app_release_resources(uint32_t resources)
{
	if ((resources & APP_RESOURCE_CAMERA) != 0U) {
		app_release_camera_resource();
	}
	if ((resources & APP_RESOURCE_AUDIO) != 0U) {
		app_release_audio_resource();
	}
	if ((resources & APP_RESOURCE_RTC) != 0U) {
		app_release_rtc_resource();
	}
}

static esp_err_t app_switch_resources(uint32_t target_resources)
{
	uint32_t current_resources = app_get_active_resources();
	uint32_t acquire_resources = target_resources & ~current_resources;
	uint32_t release_resources = current_resources & ~target_resources;

	esp_err_t ret = app_acquire_resources(acquire_resources);
	if (ret != ESP_OK) {
		return ret;
	}

	app_release_resources(release_resources);
	app_set_active_resources(target_resources);
	return ESP_OK;
}

static void app_network_state_cb(const network_state_t *state, void *ctx)
{
	(void)ctx;

	if (state == NULL) {
		return;
	}

	(void)app_report_device_state_async(state->connected ? "network-up" : "network-down");

	if (state->connected) {
		esp_err_t time_ret = system_time_request_sync(false);
		if (time_ret != ESP_OK) {
			ESP_LOGW(TAG, "schedule system time sync failed: %s", esp_err_to_name(time_ret));
		}
		if (system_time_has_valid_time()) {
			app_schedule_thing_bootstrap("network-ready");
		}
	} else {
		device_online_set_network_ready(false);
	}

	if (app_rtc_runtime_is_initialized()) {
		rtc_transport_network_state_t rtc_network = {
			.connected = state->connected,
		};
		rtc_transport_on_network_state_changed(&rtc_network);
	}

}

static bool app_rtc_test_video_active(void *ctx)
{
	(void)ctx;

	return sender_test_is_mode_active(SENDER_TEST_MODE_VIDEO);
}

static bool app_rtc_test_audio_active(void *ctx)
{
	(void)ctx;

	return sender_test_is_mode_active(SENDER_TEST_MODE_AUDIO);
}

static void app_rtc_request_test_audio_restart(void *ctx)
{
	(void)ctx;

	sender_test_request_audio_restart();
}

static esp_err_t app_apply_audio_preferences(void)
{
	app_audio_config_t audio_config = {0};

	ESP_RETURN_ON_ERROR(app_audio_config_load(&audio_config), TAG, "load audio config failed");
	ESP_RETURN_ON_ERROR(speaker_set_volume_percent(audio_config.speaker_volume_percent),
			    TAG,
			    "apply speaker volume failed");
	ESP_RETURN_ON_ERROR(microphone_set_gain_percent(audio_config.capture_gain_percent),
			    TAG,
			    "apply capture gain failed");
	return ESP_OK;
}

static bool app_capture_uplink_allowed(void)
{
	audio_stats_t audio = {0};

	audio_device_get_stats(&audio);
	return audio.capture_gain_percent > 0U;
}

static esp_err_t app_apply_call_audio_profile(void)
{
	audio_stats_t audio = {0};

	/* Ordinary device calls own the built-in microphone and speaker path. */
	media_sink_set_audio_profile(MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY);
	ESP_RETURN_ON_ERROR(rtc_transport_set_remote_audio_stream_id(RTC_TRANSPORT_DEVICE_AUDIO_STREAM_ID),
			    TAG,
			    "select device call audio stream failed");
	ESP_RETURN_ON_ERROR(rtc_transport_use_builtin_media(), TAG, "restore call media owner failed");
	ESP_RETURN_ON_ERROR(app_apply_audio_preferences(), TAG, "apply call audio preferences failed");

	audio_device_get_stats(&audio);
	ESP_LOGI(TAG,
		 "call audio owner prepared: owner=tirtc device_ready=%d mic_gain=%u speaker_volume=%u",
		 audio.ready ? 1 : 0,
		 (unsigned)audio.capture_gain_percent,
		 (unsigned)audio.speaker_volume_percent);
	return ESP_OK;
}

static esp_err_t app_apply_device_ipc_audio_profile(void)
{
	/*
	 * IPC viewing uses the TiRTC built-in media bridge: the primary microphone
	 * callback is enabled only when TiRTC owns the capture path and publishes a
	 * local audio stream.  AI Chat and WeChat VoIP use observer/external-audio
	 * paths, so entering IPC must explicitly restore the built-in owner and
	 * re-arm local send intent instead of inheriting the previous app's state.
	 */
	app_state_set_audio_enabled(true);
	app_state_set_video_enabled(true);
	ESP_RETURN_ON_ERROR(rtc_transport_set_remote_audio_stream_id(RTC_TRANSPORT_H5_TALKBACK_STREAM_ID),
			    TAG,
			    "select H5 talkback audio stream failed");
	ESP_RETURN_ON_ERROR(rtc_transport_use_builtin_media(), TAG, "restore IPC media owner failed");
	ESP_RETURN_ON_ERROR(speaker_set_volume_percent(APP_DEVICE_IPC_PLAYBACK_VOLUME_PERCENT),
			    TAG,
			    "apply IPC playback volume failed");

	const audio_capture_processing_config_t ipc_capture_config = {
		.send_volume_percent = APP_DEVICE_IPC_SEND_VOLUME_PERCENT,
		.codec_gain_percent = APP_DEVICE_IPC_CAPTURE_CODEC_GAIN_PERCENT,
		.upload_gain_percent = APP_DEVICE_IPC_CAPTURE_UPLOAD_GAIN_PERCENT,
		.auto_gain_max_percent = APP_DEVICE_IPC_CAPTURE_AUTO_GAIN_MAX_PERCENT,
		.noise_gate_enabled = true,
		.noise_gate_open_peak = APP_DEVICE_IPC_CAPTURE_NOISE_GATE_OPEN_PEAK,
		.noise_gate_close_peak = APP_DEVICE_IPC_CAPTURE_NOISE_GATE_CLOSE_PEAK,
		.noise_gate_attenuation_percent = APP_DEVICE_IPC_CAPTURE_NOISE_GATE_ATTENUATION_PERCENT,
	};
	ESP_RETURN_ON_ERROR(microphone_set_processing_config(&ipc_capture_config),
			    TAG,
			    "apply IPC send volume failed");
	ESP_RETURN_ON_ERROR(app_apply_media_policy(), TAG, "apply IPC media policy failed");

	audio_stats_t audio = {0};
	rtc_transport_stats_t rtc = {0};
	audio_device_get_stats(&audio);
	rtc_transport_get_stats(&rtc);
	ESP_LOGI(TAG,
		 "IPC audio uplink armed: send=%u codec=%u upload=%u auto_max=%u noise_gate=%d speaker=%u call=%d stream=%u capture=%d tx_audio=%lu",
		 (unsigned)audio.capture_gain_percent,
		 (unsigned)audio.capture_codec_gain_percent,
		 (unsigned)audio.capture_upload_gain_percent,
		 (unsigned)audio.capture_auto_gain_max_percent,
		 audio.capture_noise_gate_enabled ? 1 : 0,
		 (unsigned)audio.speaker_volume_percent,
		 rtc.call_active ? 1 : 0,
		 (unsigned)rtc.local_audio_stream_id,
		 audio.capture_enabled ? 1 : 0,
		 (unsigned long)rtc.tx_audio_frames);
	return ESP_OK;
}

static esp_err_t app_apply_ai_chat_audio_profile(void)
{
	ESP_RETURN_ON_ERROR(speaker_set_volume_percent(APP_AI_CHAT_DEFAULT_SPEAKER_VOLUME_PERCENT),
			    TAG,
			    "apply AI Chat speaker volume failed");
	ESP_RETURN_ON_ERROR(microphone_set_gain_percent(APP_AI_CHAT_DEFAULT_CAPTURE_GAIN_PERCENT),
			    TAG,
			    "apply AI Chat capture gain failed");
	return ESP_OK;
}

static void app_stop_app_services(app_id_t app_id)
{
	switch (app_id) {
	case APP_ID_CALL:
	{
		app_cancel_contact_scan_for_lifecycle();
		esp_err_t call_ret = device_call_hangup();
		if (call_ret != ESP_OK && call_ret != ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "app lifecycle device call close failed: %s", esp_err_to_name(call_ret));
		}
		(void)rtc_transport_set_remote_audio_stream_id(RTC_TRANSPORT_H5_TALKBACK_STREAM_ID);
		esp_err_t hangup_ret = app_hangup_rtc_session_if_active(app_id);
		if (hangup_ret != ESP_OK) {
			ESP_LOGW(TAG, "app lifecycle rtc session close failed: %s", esp_err_to_name(hangup_ret));
		}
		break;
	}
	case APP_ID_AI_CHAT:
		(void)ai_chat_close();
		media_sink_set_audio_profile(MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY);
		break;
	case APP_ID_WECHAT:
		app_cancel_wechat_contact_scan_for_lifecycle();
		wechat_voip_service_stop();
		break;
	case APP_ID_SYSTEM:
	{
		app_cancel_tirtc_config_scan_for_lifecycle();
		network_cancel_ping();
		sender_test_stop();
		esp_err_t hangup_ret = app_hangup_rtc_session_if_active(app_id);
		if (hangup_ret != ESP_OK) {
			ESP_LOGW(TAG, "app lifecycle rtc session close failed: %s", esp_err_to_name(hangup_ret));
		}
		break;
	}
	case APP_ID_DEVICE:
	{
		media_sink_set_audio_profile(MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY);
		esp_err_t hangup_ret = app_hangup_rtc_session_if_active(app_id);
		if (hangup_ret != ESP_OK) {
			ESP_LOGW(TAG, "app lifecycle rtc session close failed: %s", esp_err_to_name(hangup_ret));
		}
		esp_err_t audio_ret = app_apply_audio_preferences();
		if (audio_ret != ESP_OK) {
			ESP_LOGW(TAG, "restore audio preferences after IPC view failed: %s", esp_err_to_name(audio_ret));
		}
		break;
	}
	case APP_ID_HOME:
	{
		esp_err_t hangup_ret = app_hangup_rtc_session_if_active(app_id);
		if (hangup_ret != ESP_OK) {
			ESP_LOGW(TAG, "app lifecycle rtc session close failed: %s", esp_err_to_name(hangup_ret));
		}
		break;
	}
	default:
		break;
	}
}

static esp_err_t app_start_app_services(app_id_t app_id)
{
	switch (app_id) {
	case APP_ID_WECHAT:
	{
		media_sink_set_audio_profile(MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY);
		if (!app_device_binding_ready_for_online_services("wechat")) {
			return ESP_OK;
		}
		esp_err_t ret = app_prepare_rtc_if_network_ready();
		if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
			return ret;
		}
		return wechat_voip_service_start();
	}
	case APP_ID_AI_CHAT:
		media_sink_set_audio_profile(MEDIA_SINK_AUDIO_PROFILE_JITTER_SAFE);
		ESP_RETURN_ON_ERROR(app_apply_ai_chat_audio_profile(), TAG, "apply AI Chat audio profile failed");
		app_reset_ai_chat_token_prefetch_throttle();
		if (!network_is_connected()) {
			ESP_LOGD(TAG, "AI Chat waits for network connection");
			return ESP_OK;
		}
	{
		app_begin_ai_chat_start_window();
		esp_err_t ret = app_open_ai_chat();
		if (ret == ESP_ERR_INVALID_STATE) {
			ESP_LOGD(TAG, "AI Chat waits for RTC/time readiness");
			return ESP_OK;
		}
		return ret;
	}
	case APP_ID_DEVICE:
	{
		media_sink_set_audio_profile(MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY);
		ESP_RETURN_ON_ERROR(app_apply_device_ipc_audio_profile(), TAG, "apply IPC audio profile failed");
		esp_err_t ret = app_prepare_rtc_if_network_ready();
		if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
			return ret;
		}
		ESP_RETURN_ON_ERROR(app_apply_device_ipc_audio_profile(), TAG, "refresh IPC audio profile failed");
		return ESP_OK;
	}
	case APP_ID_CALL:
		return ESP_OK;
	case APP_ID_SYSTEM:
	case APP_ID_HOME:
	default:
		return ESP_OK;
	}
}

esp_err_t app_acquire_call_session_resources(void)
{
	esp_err_t ret = ESP_OK;

	if (app_get_active_app() != APP_ID_CALL) {
		return ESP_ERR_INVALID_STATE;
	}

	ret = app_switch_resources(APP_RESOURCE_RTC | APP_RESOURCE_AUDIO);
	if (ret != ESP_OK) {
		return ret;
	}
	ret = app_apply_call_audio_profile();
	if (ret == ESP_OK) {
		ret = app_prepare_rtc_after_time_sync("call-session");
	}
	if (ret == ESP_OK) {
		return ESP_OK;
	}

	ESP_LOGW(TAG, "call session prepare failed, releasing acquired resources: %s", esp_err_to_name(ret));
	esp_err_t rollback_ret = app_switch_resources(app_resource_mask_for_app(APP_ID_CALL));
	if (rollback_ret != ESP_OK) {
		ESP_LOGW(TAG, "call session resource rollback failed: %s", esp_err_to_name(rollback_ret));
	}
	return ret;
}

void app_release_call_session_resources(void)
{
	if (app_get_active_app() != APP_ID_CALL) {
		return;
	}

	esp_err_t ret = app_switch_resources(app_resource_mask_for_app(APP_ID_CALL));
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "release call session resources failed: %s", esp_err_to_name(ret));
	}
}

static void app_release_call_session_resources_if_idle(void)
{
	device_call_snapshot_t snapshot = {0};

	if (app_get_active_app() != APP_ID_CALL) {
		return;
	}
	device_call_get_snapshot(&snapshot);
	if (snapshot.state == DEVICE_CALL_STATE_OUTGOING ||
	    snapshot.state == DEVICE_CALL_STATE_CONNECTING ||
	    snapshot.state == DEVICE_CALL_STATE_IN_CALL) {
		ESP_LOGD(TAG, "defer call resource release: state=%u", (unsigned)snapshot.state);
		return;
	}
	app_release_call_session_resources();
}

esp_err_t app_suspend_call_scan_resources(void)
{
	if (app_get_active_app() != APP_ID_CALL) {
		return ESP_ERR_INVALID_STATE;
	}

	return app_switch_resources(APP_RESOURCE_CAMERA);
}

esp_err_t app_resume_call_scan_resources(void)
{
	esp_err_t ret = ESP_OK;

	if (app_get_active_app() != APP_ID_CALL) {
		return ESP_ERR_INVALID_STATE;
	}

	ret = app_switch_resources(app_resource_mask_for_app(APP_ID_CALL));
	if (ret != ESP_OK) {
		return ret;
	}
	return app_start_app_services(APP_ID_CALL);
}

esp_err_t app_suspend_wechat_scan_resources(void)
{
	if (app_get_active_app() != APP_ID_WECHAT) {
		return ESP_ERR_INVALID_STATE;
	}

	return app_switch_resources(APP_RESOURCE_CAMERA);
}

esp_err_t app_resume_wechat_scan_resources(void)
{
	esp_err_t ret = ESP_OK;

	if (app_get_active_app() != APP_ID_WECHAT) {
		return ESP_ERR_INVALID_STATE;
	}

	ret = app_switch_resources(app_resource_mask_for_app(APP_ID_WECHAT));
	if (ret != ESP_OK) {
		return ret;
	}
	return app_start_app_services(APP_ID_WECHAT);
}

esp_err_t app_acquire_tirtc_config_scan_resources(void)
{
	if (app_get_active_app() != APP_ID_SYSTEM) {
		return ESP_ERR_INVALID_STATE;
	}

	return app_switch_resources(app_resource_mask_for_app(APP_ID_SYSTEM) | APP_RESOURCE_CAMERA);
}

void app_release_tirtc_config_scan_resources(void)
{
	if (app_get_active_app() != APP_ID_SYSTEM) {
		return;
	}

	esp_err_t ret = app_switch_resources(app_resource_mask_for_app(APP_ID_SYSTEM));
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "release tirtc config scan resources failed: %s", esp_err_to_name(ret));
	}
}

esp_err_t app_init(void)
{
	display_actions_t display_actions = {0};
	ota_config_t ota_config = app_make_ota_config();
	const int64_t boot_begin_us = esp_timer_get_time();
	int64_t after_device_us = 0;
	int64_t after_services_us = 0;
	int64_t after_network_us = 0;
	int64_t after_display_us = 0;

	app_ui_configure_display_actions(&display_actions);
	app_configure_log_timezone();

	ESP_LOGI(TAG, "system init start");
	const esp_app_desc_t *app_desc = esp_app_get_description();
	ESP_LOGI(TAG,
		 "firmware version: %s project=%s built=%s %s",
		 app_desc != NULL ? app_desc->version : "unknown",
		 app_desc != NULL ? app_desc->project_name : "unknown",
		 app_desc != NULL ? app_desc->date : "unknown",
		 app_desc != NULL ? app_desc->time : "unknown");
	ESP_RETURN_ON_ERROR(device_init(app_on_boot_button_changed, NULL), TAG, "device init failed");
	app_preload_rtc_config();
	after_device_us = esp_timer_get_time();
	ESP_RETURN_ON_ERROR(app_start_control_task(), TAG, "app control worker init failed");
	ESP_RETURN_ON_ERROR(app_configure_thing_service_registry(), TAG, "thing service registry init failed");
	ESP_RETURN_ON_ERROR(app_configure_device_binding(), TAG, "device binding init failed");
	ESP_RETURN_ON_ERROR(app_configure_device_online(), TAG, "device online init failed");
	ESP_RETURN_ON_ERROR(app_configure_device_call(), TAG, "device call init failed");
	ESP_RETURN_ON_ERROR(app_configure_incoming_session_policy(), TAG, "incoming session policy init failed");
	ESP_RETURN_ON_ERROR(app_init_online_report_defer_timer(), TAG, "online report defer timer init failed");
	ESP_RETURN_ON_ERROR(rtc_transport_set_media_bridge(rtc_media_bridge_get_ops(),
							   rtc_media_bridge_get_context()),
			    TAG,
			    "rtc media bridge configure failed");
	app_ai_chat_config_t ai_chat_ui_config = {0};
	esp_err_t ai_chat_ui_ret = app_ai_chat_config_load(&ai_chat_ui_config);
	if (ai_chat_ui_ret != ESP_OK) {
		ESP_LOGW(TAG, "AI Chat UI preference init failed: %s", esp_err_to_name(ai_chat_ui_ret));
	}
	esp_err_t audio_pref_ret = app_apply_audio_preferences();
	if (audio_pref_ret != ESP_OK) {
		ESP_LOGW(TAG, "audio preference init failed: %s", esp_err_to_name(audio_pref_ret));
	}
	after_services_us = esp_timer_get_time();
	ESP_RETURN_ON_ERROR(platform_nvs_async_init(), TAG, "nvs async worker init failed");
	ESP_RETURN_ON_ERROR(platform_task_reaper_init(), TAG, "task reaper init failed");
	system_time_set_sync_cb(app_time_sync_cb, NULL);
	network_set_state_cb(app_network_state_cb, NULL);
	ESP_RETURN_ON_ERROR(app_start_network_baseline(), TAG, "network baseline init failed");
	after_network_us = esp_timer_get_time();

	rtc_transport_hooks_t rtc_hooks = {
		.is_test_video_active = app_rtc_test_video_active,
		.is_test_audio_active = app_rtc_test_audio_active,
		.request_test_audio_restart = app_rtc_request_test_audio_restart,
	};
	rtc_transport_set_hooks(&rtc_hooks, NULL);

	ESP_RETURN_ON_ERROR(app_switch_resources(app_resource_mask_for_app(APP_ID_HOME)),
			    TAG,
			    "acquire home resources failed");
	ESP_RETURN_ON_ERROR(ota_init(&ota_config), TAG, "ota init failed");
	display_set_snapshot_provider(app_ui_fill_display_status, NULL);
	ESP_RETURN_ON_ERROR(display_init(&display_actions), TAG, "display init failed");
	after_display_us = esp_timer_get_time();
#if APP_CONFIG_DEBUG_SCREEN_SERVER_ENABLE
	esp_err_t screen_debug_ret = screen_debug_server_start();
	if (screen_debug_ret != ESP_OK) {
		ESP_LOGW(TAG, "screen debug server start failed: %s", esp_err_to_name(screen_debug_ret));
	}
#endif

	ESP_LOGI(TAG,
		 "boot timeline: total=%ums device=%ums services=%ums network=%ums display=%ums",
		 (unsigned)((esp_timer_get_time() - boot_begin_us) / 1000LL),
		 (unsigned)((after_device_us - boot_begin_us) / 1000LL),
		 (unsigned)((after_services_us - after_device_us) / 1000LL),
		 (unsigned)((after_network_us - after_services_us) / 1000LL),
		 (unsigned)((after_display_us - after_network_us) / 1000LL));
	ESP_LOGI(TAG, "system ready: ESP32-S3 TiRTC dashboard");
	return ESP_OK;
}

void app_run(void)
{
	while (true) {
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}

static esp_err_t app_enter_app_locked(app_id_t app_id)
{
	if (app_id < APP_ID_HOME || app_id > APP_ID_SYSTEM) {
		return ESP_ERR_INVALID_ARG;
	}

	app_id_t current = app_get_active_app();
	if (current == app_id) {
		return ESP_OK;
	}

	if (current != APP_ID_HOME) {
		ESP_LOGI(TAG, "app release: %s", app_id_name(current));
		app_stop_app_services(current);
		esp_err_t release_ret = app_switch_resources(app_resource_mask_for_app(APP_ID_HOME));
		if (release_ret != ESP_OK) {
			ESP_LOGW(TAG, "app release failed: %s ret=%s", app_id_name(current), esp_err_to_name(release_ret));
			app_set_active_app(APP_ID_HOME);
			return release_ret;
		}
		app_set_active_app(APP_ID_HOME);
	}

	ESP_LOGI(TAG, "app enter: %s", app_id_name(app_id));
	uint32_t target_resources = app_resource_mask_for_app(app_id);
	esp_err_t ret = app_switch_resources(target_resources);
	if (ret == ESP_OK) {
		ret = app_start_app_services(app_id);
	}
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "app enter failed: %s ret=%s", app_id_name(app_id), esp_err_to_name(ret));
		app_stop_app_services(app_id);
		(void)app_switch_resources(app_resource_mask_for_app(APP_ID_HOME));
		app_set_active_app(APP_ID_HOME);
		return ret;
	}

	app_set_active_app(app_id);
	return ESP_OK;
}

static esp_err_t app_return_home_locked(void)
{
	app_id_t previous = app_get_active_app();

	if (previous != APP_ID_HOME) {
		ESP_LOGI(TAG, "app return home: release %s", app_id_name(previous));
		app_stop_app_services(previous);
	}
	ESP_RETURN_ON_ERROR(app_switch_resources(app_resource_mask_for_app(APP_ID_HOME)),
			    TAG,
			    "return home resources failed");
	app_set_active_app(APP_ID_HOME);
	app_schedule_ai_chat_token_prefetch("home");
	return ESP_OK;
}

static esp_err_t app_enter_app_sync(app_id_t app_id)
{
	if (s_app_transition_mutex == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	if (xSemaphoreTake(s_app_transition_mutex, portMAX_DELAY) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}
	esp_err_t ret = app_enter_app_locked(app_id);
	xSemaphoreGive(s_app_transition_mutex);
	return ret;
}

static esp_err_t app_return_home_sync(void)
{
	if (s_app_transition_mutex == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	if (xSemaphoreTake(s_app_transition_mutex, portMAX_DELAY) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}
	esp_err_t ret = app_return_home_locked();
	xSemaphoreGive(s_app_transition_mutex);
	return ret;
}

esp_err_t app_enter_app(app_id_t app_id)
{
	return app_enter_app_sync(app_id);
}

esp_err_t app_return_home(void)
{
	return app_return_home_sync();
}

esp_err_t app_request_enter_app(app_id_t app_id)
{
	if (app_id < APP_ID_HOME || app_id > APP_ID_SYSTEM) {
		return ESP_ERR_INVALID_ARG;
	}

	return app_enqueue_lifecycle_event(APP_LIFECYCLE_EVENT_ENTER_APP, app_id);
}

esp_err_t app_request_return_home(void)
{
	return app_enqueue_lifecycle_event(APP_LIFECYCLE_EVENT_RETURN_HOME, APP_ID_HOME);
}

static void app_parse_ping_target(char *target, size_t target_len)
{
	rtc_transport_config_t rtc_config = {0};
	const char *endpoint = NULL;
	const char *start = NULL;
	size_t length = 0;

	rtc_transport_get_config(&rtc_config);
	endpoint = rtc_config.service_endpoint;
	if (endpoint == NULL || endpoint[0] == '\0') {
		strlcpy(target, "223.5.5.5", target_len);
		return;
	}

	start = strstr(endpoint, "://");
	start = (start != NULL) ? (start + 3) : endpoint;
	while (start[length] != '\0' && start[length] != '/' && start[length] != ':' && length < target_len - 1) {
		length++;
	}
	if (length == 0) {
		strlcpy(target, "223.5.5.5", target_len);
		return;
	}
	memcpy(target, start, length);
	target[length] = '\0';
}

esp_err_t app_connect_wifi(const char *ssid, const char *password)
{
	return network_connect(ssid, password);
}

esp_err_t app_request_wifi_scan(void)
{
	return network_request_scan();
}

esp_err_t app_update_device_uuid(const char *uuid)
{
	esp_err_t ret = device_set_uuid(uuid);
	if (ret != ESP_OK) {
		return ret;
	}

	if (!app_rtc_runtime_is_initialized()) {
		return ESP_OK;
	}
	return app_configure_tirtc();
}

esp_err_t app_start_ping_test(void)
{
	char target[NETWORK_PING_TARGET_MAX] = {0};

	app_parse_ping_target(target, sizeof(target));
	return network_start_ping(target);
}

esp_err_t app_start_rtc(void)
{
	if (!network_is_connected()) {
		return ESP_ERR_INVALID_STATE;
	}

	ESP_RETURN_ON_ERROR(app_prepare_rtc_after_time_sync("manual-start"), TAG, "prepare rtc failed");
	return ESP_OK;
}

esp_err_t app_disconnect_rtc(void)
{
	esp_err_t ret = rtc_transport_disconnect();

	if (ret != ESP_OK) {
		return ret;
	}

	(void)app_state_sync_call_media_defaults(false, NULL);
	return ESP_OK;
}

static esp_err_t app_reconfigure_tirtc_after_settings_change(const char *reason)
{
	esp_err_t ret = ESP_OK;

	(void)app_configure_ai_chat();
	if (!app_rtc_runtime_is_initialized()) {
		return app_prepare_rtc_after_config_if_ready(reason);
	}

	rtc_transport_state_t rtc_state = rtc_transport_get_state();
	if (rtc_state == RTC_TRANSPORT_STATE_STOPPED ||
	    rtc_state == RTC_TRANSPORT_STATE_READY ||
	    rtc_state == RTC_TRANSPORT_STATE_ERROR) {
		ret = app_configure_tirtc();
		if (ret == ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "rtc config saved; endpoint changes need reboot or explicit full reset after SDK init");
			return ESP_OK;
		}
		if (ret != ESP_OK) {
			return ret;
		}
		return app_prepare_rtc_after_config_if_ready(reason);
	}

	ESP_LOGI(TAG,
		 "rtc config changed while session is active; it will apply after disconnect or reboot: %s",
		 reason != NULL ? reason : "settings");
	return ESP_OK;
}

esp_err_t app_update_rtc_config_field(app_rtc_config_field_t field, const char *value)
{
	ESP_RETURN_ON_ERROR(app_set_rtc_config_field(field, value), TAG, "rtc config save failed");

	app_request_rtc_reconfigure_after_settings_change("field");
	return ESP_OK;
}

esp_err_t app_update_rtc_device_credentials(const char *device_id, const char *device_secret)
{
	ESP_RETURN_ON_ERROR(app_set_rtc_device_credentials(device_id, device_secret),
			    TAG,
			    "rtc device credentials save failed");

	app_restart_identity_after_credentials(device_id, "credentials");
	return ESP_OK;
}

static esp_err_t app_start_device_online_if_ready(const char *reason)
{
	if (!network_is_connected() || !system_time_has_valid_time()) {
		device_online_set_network_ready(false);
		return ESP_ERR_INVALID_STATE;
	}
	device_online_set_network_ready(true);
	if (!app_rtc_device_credentials_available()) {
		return ESP_ERR_NOT_FOUND;
	}

	esp_err_t ret = device_online_start_async(reason != NULL ? reason : "auto");
	if (ret == ESP_OK) {
		app_start_device_identity_ingress();
	}
	return ret;
}

static void app_start_device_identity_ingress(void)
{
	esp_err_t wechat_ret = wechat_voip_service_start_ingress();
	if (wechat_ret != ESP_OK) {
		ESP_LOGW(TAG, "wechat ingress start failed: %s", esp_err_to_name(wechat_ret));
	}

	esp_err_t call_ret = device_call_start();
	if (call_ret != ESP_OK && call_ret != ESP_ERR_INVALID_STATE) {
		ESP_LOGW(TAG, "device call listener start failed: %s", esp_err_to_name(call_ret));
	}
}

static esp_err_t app_start_device_identity_services(const char *reason)
{
	if (!thing_service_registry_is_ready()) {
		return ESP_ERR_INVALID_STATE;
	}

	esp_err_t binding_ret = app_start_device_binding_reconcile_if_needed(reason);
	esp_err_t ret = app_start_device_online_if_ready(reason);
	if (ret == ESP_OK) {
		return ESP_OK;
	}
	if (ret != ESP_ERR_NOT_FOUND) {
		return ret;
	}
	return binding_ret;
}

esp_err_t app_start_device_binding(void)
{
	if (!network_is_connected() || !system_time_has_valid_time()) {
		return ESP_ERR_INVALID_STATE;
	}
	if (!thing_service_registry_is_ready()) {
		app_schedule_thing_bootstrap("manual");
		return ESP_ERR_INVALID_STATE;
	}

	return app_start_device_identity_services("manual");
}

static esp_err_t app_reconcile_binding_with_retained_credentials(const char *reason)
{
	esp_err_t ret = ESP_OK;
	const char *safe_reason = reason != NULL && reason[0] != '\0' ? reason : "reset-binding";

	if (!app_rtc_device_credentials_available()) {
		ESP_LOGW(TAG, "device binding reconcile needs retained credentials: reason=%s", safe_reason);
		return ESP_ERR_NOT_FOUND;
	}

	ESP_LOGI(TAG, "device binding reconcile begin: reason=%s identity=retained", safe_reason);
	app_suspend_device_identity_services(safe_reason);
	ret = device_online_stop_and_wait(APP_DEVICE_ONLINE_STOP_WAIT_MS);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG,
			 "device binding reconcile could not stop formal MQTT: reason=%s ret=%s",
			 safe_reason,
			 esp_err_to_name(ret));
		return ret;
	}
	device_online_invalidate_cache();
	ai_chat_token_invalidate_cache();
	ret = device_binding_reset_state(safe_reason);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG,
			 "device binding reconcile could not clear pending session: reason=%s ret=%s",
			 safe_reason,
			 esp_err_to_name(ret));
		return ret;
	}

	if (network_is_connected() && system_time_has_valid_time()) {
		ret = device_binding_start_async(safe_reason);
		if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "signed binding reconcile start failed: %s", esp_err_to_name(ret));
			return ret;
		}
	}

	ESP_LOGI(TAG, "device binding reconcile queued: reason=%s identity=retained", safe_reason);
	return ESP_OK;
}

static esp_err_t app_verify_device_binding_internal(const char *reason)
{
	const char *safe_reason = reason != NULL && reason[0] != '\0' ? reason : "binding-check";

	if (!network_is_connected() || !system_time_has_valid_time()) {
		return ESP_ERR_INVALID_STATE;
	}
	if (!thing_service_registry_is_ready()) {
		app_schedule_thing_bootstrap(safe_reason);
		return ESP_ERR_INVALID_STATE;
	}
	if (!app_rtc_device_credentials_available()) {
		return device_binding_start_async(safe_reason);
	}

	/*
	 * A device has no user JWT and therefore cannot call the platform's
	 * DELETE /v1/user/device/reset endpoint.  The local button only forces a
	 * fresh /v1/device/token check.  A 6006 response is the authoritative
	 * signal that cloud unbind completed; the online service then queues the
	 * signed Report flow through on_rebind_required.
	 */
	ESP_LOGI(TAG, "device binding verification begin: reason=%s", safe_reason);
	app_suspend_device_identity_services(safe_reason);
	esp_err_t ret = device_online_notify_credentials_changed(safe_reason);
	if (ret != ESP_OK) {
		return ret;
	}
	app_start_device_identity_ingress();
	app_request_rtc_prepare_after_identity(safe_reason);
	ESP_LOGI(TAG, "device binding verification queued: reason=%s", safe_reason);
	return ESP_OK;
}

esp_err_t app_reset_device_binding(void)
{
	return app_queue_device_binding_verification("manual-reset-check");
}

esp_err_t app_request_update_rtc_device_credentials(const char *device_id, const char *device_secret)
{
	if (s_app_control_queue == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	if (device_id == NULL || device_id[0] == '\0' ||
	    device_secret == NULL || device_secret[0] == '\0') {
		return ESP_ERR_INVALID_ARG;
	}
	if (strlen(device_id) >= APP_RTC_CONFIG_TEXT_MAX ||
	    strlen(device_secret) >= APP_RTC_CONFIG_TEXT_MAX) {
		return ESP_ERR_INVALID_SIZE;
	}

	app_control_event_t event = {
		.type = APP_CONTROL_EVENT_RTC_CREDENTIALS_UPDATE,
	};
	strlcpy(event.reason, "credential-scan", sizeof(event.reason));
	strlcpy(event.rtc_device_id, device_id, sizeof(event.rtc_device_id));
	strlcpy(event.rtc_device_secret, device_secret, sizeof(event.rtc_device_secret));

	if (xQueueSendToBack(s_app_control_queue, &event, 0) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}

	ESP_LOGD(TAG, "rtc credential save queued: device_id_len=%u", (unsigned)strlen(device_id));
	return ESP_OK;
}

esp_err_t app_set_rtc_server_env(app_rtc_server_env_t env)
{
	ESP_RETURN_ON_ERROR(app_set_rtc_config_server_env(env), TAG, "rtc server save failed");

	app_request_rtc_reconfigure_after_settings_change("server");
	return ESP_OK;
}

esp_err_t app_start_sender_video_test(void)
{
	return sender_test_start(SENDER_TEST_MODE_VIDEO);
}

esp_err_t app_start_sender_audio_test(void)
{
	return sender_test_start(SENDER_TEST_MODE_AUDIO);
}

esp_err_t app_start_ota(void)
{
	device_state_t device = {0};

	if (!network_is_connected()) {
		return ESP_ERR_INVALID_STATE;
	}

	device_get_state(&device);
	return ota_start_default(device.uuid);
}

void app_restart_for_ota(void)
{
	ota_restart();
}

esp_err_t app_open_ai_chat(void)
{
	esp_err_t ret = ESP_OK;
	ai_chat_config_t ai_chat_config = {0};

	if (!network_is_connected()) {
		return ESP_ERR_INVALID_STATE;
	}
	if (!app_device_binding_ready_for_online_services("ai_chat")) {
		return ESP_ERR_INVALID_STATE;
	}

	ret = app_prepare_rtc_if_network_ready();
	if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
		ESP_LOGW(TAG, "prepare rtc for AI Chat failed: %s", esp_err_to_name(ret));
		return ret;
	}
	ESP_RETURN_ON_ERROR(app_build_ai_chat_config(&ai_chat_config), TAG, "build AI Chat config failed");
	ESP_RETURN_ON_ERROR(ai_chat_init(&ai_chat_config), TAG, "init AI Chat failed");
	return ai_chat_open();
}

esp_err_t app_request_start_ai_chat(void)
{
	if (app_get_active_app() != APP_ID_AI_CHAT) {
		return ESP_ERR_INVALID_STATE;
	}
	if (!network_is_connected()) {
		return ESP_ERR_INVALID_STATE;
	}

	return app_enqueue_lifecycle_event(APP_LIFECYCLE_EVENT_START_APP_SERVICES, APP_ID_AI_CHAT);
}

esp_err_t app_close_ai_chat(void)
{
	return ai_chat_close();
}

esp_err_t app_clear_ai_chat_messages(void)
{
	return ai_chat_clear_messages();
}

esp_err_t app_handle_ai_chat_button(bool pressed)
{
	return ai_chat_handle_control_button(pressed);
}

esp_err_t app_set_ai_chat_avatar(uint8_t avatar)
{
	esp_err_t save_ret = app_ai_chat_config_set_avatar(avatar);
	if (save_ret != ESP_OK) {
		ESP_LOGW(TAG, "save AI Chat avatar failed: %s", esp_err_to_name(save_ret));
	}
	return save_ret;
}

static esp_err_t app_set_speaker_volume_internal(uint8_t percent, bool persist)
{
	esp_err_t ret = speaker_set_volume_percent(percent);
	if (ret != ESP_OK) {
		return ret;
	}

	if (!persist) {
		return ESP_OK;
	}

	esp_err_t save_ret = app_audio_config_save_speaker_volume(percent);
	if (save_ret != ESP_OK) {
		ESP_LOGW(TAG, "save speaker volume failed: %s", esp_err_to_name(save_ret));
	}
	return ESP_OK;
}

esp_err_t app_set_speaker_volume(uint8_t percent)
{
	return app_set_speaker_volume_internal(percent, true);
}

esp_err_t app_set_capture_gain(uint8_t percent)
{
	esp_err_t ret = microphone_set_gain_percent(percent);
	if (ret != ESP_OK) {
		return ret;
	}

	esp_err_t save_ret = app_audio_config_save_capture_gain(percent);
	if (save_ret != ESP_OK) {
		ESP_LOGW(TAG, "save capture gain failed: %s", esp_err_to_name(save_ret));
	}
	return app_apply_media_policy();
}

esp_err_t app_set_local_video_enabled(bool enabled)
{
	if (!app_state_is_call_active()) {
		return ESP_ERR_INVALID_STATE;
	}

	app_state_set_video_enabled(enabled);
	return app_apply_media_policy();
}

esp_err_t app_set_local_audio_enabled(bool enabled)
{
	if (!app_state_is_call_active()) {
		return ESP_ERR_INVALID_STATE;
	}

	app_state_set_audio_enabled(enabled);
	return app_apply_media_policy();
}

void app_on_boot_button_changed(bool pressed, void *ctx)
{
	(void)ctx;
	if (app_get_active_app() == APP_ID_AI_CHAT) {
		if (ai_chat_owns_control_button()) {
			esp_err_t ret = ai_chat_handle_control_button(pressed);
			if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
				ESP_LOGW(TAG, "AI Chat boot action failed: %s", esp_err_to_name(ret));
			}
		} else if (pressed) {
			esp_err_t ret = app_enqueue_lifecycle_event(APP_LIFECYCLE_EVENT_START_APP_SERVICES, APP_ID_AI_CHAT);
			if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
				ESP_LOGW(TAG, "AI Chat boot restart failed: %s", esp_err_to_name(ret));
			}
		}
		return;
	}
}

void app_get_snapshot(app_snapshot_t *snapshot)
{
	app_snapshot_get(snapshot);
}

esp_err_t app_apply_media_policy(void)
{
	app_control_state_t control = {0};
	bool enable_audio_send = true;
	esp_err_t video_ret = ESP_OK;
	esp_err_t audio_ret = ESP_OK;

	control = app_state_get_control();
	enable_audio_send = control.audio_enabled &&
				    app_capture_uplink_allowed();

	video_ret = rtc_transport_set_local_video_send_enabled(control.video_enabled);
	audio_ret = rtc_transport_set_local_audio_send_enabled(enable_audio_send);
	if (video_ret != ESP_OK || audio_ret != ESP_OK) {
		ESP_LOGW(TAG,
			 "apply media policy failed: video=%s audio=%s",
			 esp_err_to_name(video_ret),
			 esp_err_to_name(audio_ret));
	}

	return video_ret != ESP_OK ? video_ret : audio_ret;
}
