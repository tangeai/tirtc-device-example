#include "wechat_voip_service.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "network.h"
#include "tirtc_session.h"
#include "wechat_voip_config.h"
#include "wechat_voip_session.h"
#include "wechat_voip_thing.h"

static const char *TAG = "wx_voip_service";

#define WECHAT_VOIP_SERVICE_TASK_STACK    (8 * 1024)
#define WECHAT_VOIP_SERVICE_TASK_PRIORITY 4
#define WECHAT_VOIP_SERVICE_POLL_MS       500
#define WECHAT_VOIP_START_RETRY_LOG_MS    5000

static TaskHandle_t s_service_task;
static bool s_registered;
static volatile bool s_service_stop_requested;

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

    while (!s_service_stop_requested) {
        if (APP_CONFIG_WECHAT_VOIP_ENABLE && network_is_connected()) {
            tirtc_session_stats_t rtc_stats = {0};
            esp_err_t rtc_ret = ESP_OK;
            esp_err_t thing_ret = ESP_OK;

            tirtc_session_get_stats(&rtc_stats);
            if (!rtc_stats.sdk_started && rtc_stats.state != TIRTC_SESSION_STATE_STARTING) {
                rtc_ret = tirtc_session_prepare_sdk();
            }

            tirtc_session_get_stats(&rtc_stats);
            if (rtc_stats.sdk_started) {
                thing_ret = wechat_voip_thing_start();
            }

            if (rtc_ret != ESP_OK || thing_ret != ESP_OK) {
                TickType_t now = xTaskGetTickCount();
                if (last_start_log_tick == 0 ||
                    now - last_start_log_tick >= pdMS_TO_TICKS(WECHAT_VOIP_START_RETRY_LOG_MS)) {
                    last_start_log_tick = now;
                    ESP_LOGW(TAG,
                             "微信 VoIP 启动等待: rtc=%s thing=%s",
                             esp_err_to_name(rtc_ret),
                             esp_err_to_name(thing_ret));
                }
            }
        }

        wechat_voip_service_maintenance();
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WECHAT_VOIP_SERVICE_POLL_MS));
    }

    s_service_task = NULL;
    s_service_stop_requested = false;
    ESP_LOGI(TAG, "微信 VoIP 业务守护已停止");
    vTaskDelete(NULL);
}

esp_err_t wechat_voip_service_start(void)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        ESP_LOGI(TAG, "微信 VoIP 业务已关闭");
        return ESP_OK;
    }

    s_service_stop_requested = false;

    if (!s_registered) {
        const tirtc_session_observer_t observer = {
            .on_command = wechat_voip_service_on_command,
            .on_connection_error = wechat_voip_service_on_connection_error,
            .on_disconnected = wechat_voip_service_on_disconnected,
        };
        ESP_RETURN_ON_ERROR(tirtc_session_register_observer(&observer, NULL), TAG, "register rtc observer failed");
        s_registered = true;
    }

    if (s_service_task != NULL) {
        return ESP_OK;
    }

    /* The service start path may load contact data from NVS. Keep this task on
     * an internal-RAM stack so flash/NVS operations never run from PSRAM stack. */
    BaseType_t task_ret = xTaskCreate(wechat_voip_service_task,
                                      "wx_voip_svc",
                                      WECHAT_VOIP_SERVICE_TASK_STACK,
                                      NULL,
                                      WECHAT_VOIP_SERVICE_TASK_PRIORITY,
                                      &s_service_task);
    ESP_RETURN_ON_FALSE(task_ret == pdPASS, ESP_ERR_NO_MEM, TAG, "create wechat voip service task failed");
    ESP_LOGI(TAG, "微信 VoIP 业务守护已启动");
    return ESP_OK;
}

void wechat_voip_service_stop(void)
{
    TaskHandle_t service_task = s_service_task;

    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        return;
    }

    if (service_task != NULL) {
        s_service_stop_requested = true;
        xTaskNotifyGive(service_task);
    }

    if (!wechat_voip_session_is_idle()) {
        wechat_voip_session_hangup();
    }
    if (wechat_voip_thing_request_call_busy()) {
        wechat_voip_thing_cancel_pending_call();
    }
    wechat_voip_thing_stop();
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

esp_err_t wechat_voip_service_add_contact(const char *open_id)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        return ESP_ERR_INVALID_STATE;
    }
    return wechat_voip_thing_add_contact(open_id);
}

esp_err_t wechat_voip_service_remove_contact(const char *open_id)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        return ESP_ERR_INVALID_STATE;
    }
    return wechat_voip_thing_remove_contact(open_id);
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
