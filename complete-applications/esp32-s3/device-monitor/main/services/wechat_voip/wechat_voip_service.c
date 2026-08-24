#include "wechat_voip_service.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "network.h"
#include "tirtc_session.h"
#include "wechat_voip_config.h"
#include "wechat_voip_session.h"
#include "wechat_voip_thing.h"

static const char *TAG = "wx_voip_service";

#define WECHAT_VOIP_SERVICE_TASK_STACK    (32 * 1024)
#define WECHAT_VOIP_SERVICE_TASK_PRIORITY 4
#define WECHAT_VOIP_SERVICE_POLL_MS       500
#define WECHAT_VOIP_RTC_PREPARE_RETRY_MS  5000
#define WECHAT_VOIP_START_RETRY_LOG_MS    5000
#define WECHAT_VOIP_SERVICE_TASK_CAPS     (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static TaskHandle_t s_service_task;
static bool s_registered;
static volatile bool s_ingress_enabled;
static volatile bool s_session_enabled;
static bool s_service_task_starting;
static portMUX_TYPE s_service_state_lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t wechat_voip_service_set_incoming_policy(wechat_voip_incoming_allowed_cb_t callback,
                                                  void *ctx)
{
    return wechat_voip_thing_set_incoming_policy(callback, ctx);
}
static bool wechat_voip_service_on_command(tirtc_conn_t conn,
                                           uint32_t cmdw,
                                           const void *data,
                                           uint32_t data_len,
                                           void *ctx)
{
    (void)ctx;
    return wechat_voip_session_on_command(conn, cmdw, data, data_len);
}

static void wechat_voip_service_on_connection_error(tirtc_conn_t conn, int error, void *ctx)
{
    (void)ctx;
    (void)wechat_voip_session_on_conn_error(conn, error);
}

static void wechat_voip_service_on_disconnected(tirtc_conn_t conn, void *ctx)
{
    (void)ctx;
    (void)wechat_voip_session_on_disconnected(conn);
}

static void wechat_voip_service_task(void *ctx)
{
    (void)ctx;
    TickType_t last_start_log_tick = 0;
    TickType_t last_rtc_prepare_tick = 0;

    while (true) {
        if (APP_CONFIG_WECHAT_VOIP_ENABLE && s_ingress_enabled && network_is_connected()) {
            tirtc_session_stats_t rtc_stats = {0};
            esp_err_t rtc_ret = ESP_OK;
            esp_err_t thing_ret = wechat_voip_thing_start();

            if (!s_ingress_enabled) {
                wechat_voip_thing_stop();
            }

            if (s_session_enabled) {
                tirtc_session_get_stats(&rtc_stats);
                if (rtc_stats.sdk_started) {
                    last_rtc_prepare_tick = 0;
                } else if (rtc_stats.state != TIRTC_SESSION_STATE_STARTING) {
                    TickType_t now = xTaskGetTickCount();
                    bool prepare_due =
                        last_rtc_prepare_tick == 0 ||
                        now - last_rtc_prepare_tick >= pdMS_TO_TICKS(WECHAT_VOIP_RTC_PREPARE_RETRY_MS);
                    if (prepare_due) {
                        last_rtc_prepare_tick = now;
                        rtc_ret = tirtc_session_prepare_sdk();
                    }
                }
            } else {
                last_rtc_prepare_tick = 0;
            }

            if (rtc_ret != ESP_OK || thing_ret != ESP_OK) {
                TickType_t now = xTaskGetTickCount();
                if (last_start_log_tick == 0 ||
                    now - last_start_log_tick >= pdMS_TO_TICKS(WECHAT_VOIP_START_RETRY_LOG_MS)) {
                    last_start_log_tick = now;
                    bool waiting_for_prerequisite =
                        (thing_ret == ESP_OK || thing_ret == ESP_ERR_INVALID_STATE) &&
                        (rtc_ret == ESP_OK || rtc_ret == ESP_ERR_INVALID_STATE);
                    if (waiting_for_prerequisite) {
                        ESP_LOGI(TAG,
                                 "微信 VoIP 等待依赖就绪: ingress=%s rtc=%s page_session=%s",
                                 esp_err_to_name(thing_ret),
                                 esp_err_to_name(rtc_ret),
                                 s_session_enabled ? "enabled" : "disabled");
                    } else {
                        ESP_LOGW(TAG,
                                 "微信 VoIP 启动失败: ingress=%s rtc=%s page_session=%s",
                                 esp_err_to_name(thing_ret),
                                 esp_err_to_name(rtc_ret),
                                 s_session_enabled ? "enabled" : "disabled");
                    }
                }
            }
        }

        wechat_voip_service_maintenance();
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WECHAT_VOIP_SERVICE_POLL_MS));
    }
}

esp_err_t wechat_voip_service_start_ingress(void)
{
    TaskHandle_t service_task = NULL;
    bool create_task = false;

    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        ESP_LOGI(TAG, "微信 VoIP 业务已关闭");
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_service_state_lock);
    s_ingress_enabled = true;
    service_task = s_service_task;
    if (service_task == NULL && !s_service_task_starting) {
        s_service_task_starting = true;
        create_task = true;
    }
    taskEXIT_CRITICAL(&s_service_state_lock);

    if (service_task != NULL) {
        xTaskNotifyGive(service_task);
        return ESP_OK;
    }
    if (!create_task) {
        return ESP_OK;
    }

    /* MQTT ingress is device-level and stays alive outside the WeChat page. */
    BaseType_t task_ret = xTaskCreateWithCaps(wechat_voip_service_task,
                                              "wx_voip_svc",
                                              WECHAT_VOIP_SERVICE_TASK_STACK,
                                              NULL,
                                              WECHAT_VOIP_SERVICE_TASK_PRIORITY,
                                              &s_service_task,
                                              WECHAT_VOIP_SERVICE_TASK_CAPS);
    taskENTER_CRITICAL(&s_service_state_lock);
    s_service_task_starting = false;
    taskEXIT_CRITICAL(&s_service_state_lock);
    ESP_RETURN_ON_FALSE(task_ret == pdPASS, ESP_ERR_NO_MEM, TAG, "create wechat voip service task failed");
    ESP_LOGI(TAG, "微信 VoIP 常驻入口任务已启动，等待正式 MQTT");
    return ESP_OK;
}

esp_err_t wechat_voip_service_start(void)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        ESP_LOGI(TAG, "微信 VoIP 业务已关闭");
        return ESP_OK;
    }

    if (!s_registered) {
        const tirtc_session_observer_t observer = {
            .on_command = wechat_voip_service_on_command,
            .on_connection_error = wechat_voip_service_on_connection_error,
            .on_disconnected = wechat_voip_service_on_disconnected,
        };
        ESP_RETURN_ON_ERROR(tirtc_session_register_observer(&observer, NULL), TAG, "register rtc observer failed");
        s_registered = true;
    }

    s_session_enabled = true;
    return wechat_voip_service_start_ingress();
}

void wechat_voip_service_stop(void)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        return;
    }

    s_session_enabled = false;

    if (!wechat_voip_session_is_idle()) {
        wechat_voip_session_hangup();
    }
    if (wechat_voip_thing_request_call_busy()) {
        wechat_voip_thing_cancel_pending_call();
    }
    if (s_service_task != NULL) {
        xTaskNotifyGive(s_service_task);
    }
}

void wechat_voip_service_suspend_ingress(void)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        return;
    }

    s_session_enabled = false;
    s_ingress_enabled = false;
    /* Stop and drain the identity-scoped MQTT dispatcher before clearing the
     * RTC session, otherwise an already dequeued old-identity message can
     * recreate a pending session after hangup returns. */
    wechat_voip_thing_stop();
    if (!wechat_voip_session_is_idle()) {
        wechat_voip_session_hangup();
    }
    if (wechat_voip_thing_request_call_busy()) {
        wechat_voip_thing_cancel_pending_call();
    }
    if (s_service_task != NULL) {
        xTaskNotifyGive(s_service_task);
    }
}

esp_err_t wechat_voip_service_answer(void)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        return ESP_ERR_INVALID_STATE;
    }
    return wechat_voip_session_answer();
}

esp_err_t wechat_voip_service_reject_or_hangup(void)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!wechat_voip_session_is_idle()) {
        wechat_voip_session_hangup();
        return ESP_OK;
    }
    if (wechat_voip_thing_request_call_busy()) {
        wechat_voip_thing_cancel_pending_call();
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}

esp_err_t wechat_voip_service_request_call(const char *open_id)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        return ESP_ERR_INVALID_STATE;
    }
    return wechat_voip_thing_request_call(open_id);
}

esp_err_t wechat_voip_service_refresh_contacts_async(void)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        return ESP_ERR_INVALID_STATE;
    }
    return wechat_voip_thing_refresh_contacts_async();
}

bool wechat_voip_service_is_enabled(void)
{
    return APP_CONFIG_WECHAT_VOIP_ENABLE != 0;
}

bool wechat_voip_service_is_connected(void)
{
    return wechat_voip_service_is_enabled() && wechat_voip_thing_is_connected();
}

esp_err_t wechat_voip_service_add_contact(const char *open_id)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        return ESP_ERR_INVALID_STATE;
    }
    return wechat_voip_thing_add_contact(open_id);
}

esp_err_t wechat_voip_service_update_contact_remark(const char *open_id, const char *remark)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        return ESP_ERR_INVALID_STATE;
    }
    return wechat_voip_thing_update_contact_remark_async(open_id, remark);
}

bool wechat_voip_service_has_incoming_call(void)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        return false;
    }
    return wechat_voip_session_has_incoming_call();
}

wechat_voip_call_state_t wechat_voip_service_get_call_state(void)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        return WECHAT_VOIP_CALL_STATE_IDLE;
    }

    switch (wechat_voip_session_get_state()) {
    case WECHAT_VOIP_SESSION_STATE_RINGING:
        return WECHAT_VOIP_CALL_STATE_INCOMING;
    case WECHAT_VOIP_SESSION_STATE_CONNECTING:
    case WECHAT_VOIP_SESSION_STATE_AWAITING_CONNECTED:
        return WECHAT_VOIP_CALL_STATE_CONNECTING;
    case WECHAT_VOIP_SESSION_STATE_IN_CALL:
        return WECHAT_VOIP_CALL_STATE_IN_CALL;
    case WECHAT_VOIP_SESSION_STATE_CLOSING:
        return WECHAT_VOIP_CALL_STATE_CLOSING;
    case WECHAT_VOIP_SESSION_STATE_IDLE:
    default:
        break;
    }

    if (wechat_voip_thing_request_call_busy()) {
        return WECHAT_VOIP_CALL_STATE_CALLING;
    }

    return WECHAT_VOIP_CALL_STATE_IDLE;
}

void wechat_voip_service_maintenance(void)
{
    wechat_voip_session_maintenance();
    wechat_voip_thing_maintenance();
}

void wechat_voip_service_get_contacts(wechat_voip_contacts_snapshot_t *snapshot)
{
    wechat_voip_thing_get_contacts(snapshot);
}
