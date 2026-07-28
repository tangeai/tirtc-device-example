#include "system_time.h"

#include <stdint.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "platform_task_reaper.h"

static const char *TAG = "system_time";

#define TIME_SYNC_MIN_VALID_UNIX_TIME 1704067200LL
#define TIME_SYNC_TIMEOUT_MS          800U
#define TIME_SYNC_RETRY_COUNT         5U
#define TIME_SYNC_SNTP_SERVER_COUNT   4U
#define TIME_SYNC_SNTP_SERVER_0       "ntp.aliyun.com"
#define TIME_SYNC_SNTP_SERVER_1       "ntp.tencent.com"
#define TIME_SYNC_SNTP_SERVER_2       "ntp.huaweicloud.com"
#define TIME_SYNC_SNTP_SERVER_3       "cn.pool.ntp.org"
#define TIME_SYNC_TASK_STACK_SIZE     (4U * 1024U)
#define TIME_SYNC_TASK_PRIORITY       3U

static portMUX_TYPE s_time_sync_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_time_sync_running;
static bool s_time_sync_force_requested;
static system_time_sync_cb_t s_time_sync_cb;
static void *s_time_sync_cb_ctx;

static void system_time_sync_task(void *ctx);
static void system_time_notify_sync_done(esp_err_t result);

bool system_time_has_valid_time(void)
{
    time_t now = 0;

    time(&now);
    return now >= (time_t)TIME_SYNC_MIN_VALID_UNIX_TIME;
}
void system_time_set_sync_cb(system_time_sync_cb_t cb, void *ctx)
{
    taskENTER_CRITICAL(&s_time_sync_lock);
    s_time_sync_cb = cb;
    s_time_sync_cb_ctx = ctx;
    taskEXIT_CRITICAL(&s_time_sync_lock);
}

esp_err_t system_time_request_sync(bool force_sync)
{
    BaseType_t task_ok = pdFALSE;

    if (!force_sync && system_time_has_valid_time()) {
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_time_sync_lock);
    if (force_sync) {
        s_time_sync_force_requested = true;
    }
    if (s_time_sync_running) {
        taskEXIT_CRITICAL(&s_time_sync_lock);
        return ESP_OK;
    }
    s_time_sync_running = true;
    taskEXIT_CRITICAL(&s_time_sync_lock);

    task_ok = xTaskCreateWithCaps(system_time_sync_task,
                                  "system_time_sync",
                                  TIME_SYNC_TASK_STACK_SIZE,
                                  NULL,
                                  TIME_SYNC_TASK_PRIORITY,
                                  NULL,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (task_ok != pdPASS) {
        taskENTER_CRITICAL(&s_time_sync_lock);
        s_time_sync_running = false;
        taskEXIT_CRITICAL(&s_time_sync_lock);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "system time sync scheduled%s", force_sync ? " (forced)" : "");
    return ESP_OK;
}

esp_err_t system_time_once(bool force_sync)
{
    esp_err_t ret = ESP_OK;
    time_t now = 0;
    esp_sntp_config_t sntp_config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(TIME_SYNC_SNTP_SERVER_COUNT,
                                               ESP_SNTP_SERVER_LIST(TIME_SYNC_SNTP_SERVER_0,
                                                                    TIME_SYNC_SNTP_SERVER_1,
                                                                    TIME_SYNC_SNTP_SERVER_2,
                                                                    TIME_SYNC_SNTP_SERVER_3));

    if (!force_sync && system_time_has_valid_time()) {
        return ESP_OK;
    }

    if (force_sync) {
        ESP_LOGI(TAG, "system time sync forced: primary=%s", TIME_SYNC_SNTP_SERVER_0);
    } else {
        ESP_LOGI(TAG, "system time invalid, syncing SNTP: primary=%s", TIME_SYNC_SNTP_SERVER_0);
    }

    esp_netif_sntp_deinit();
    ret = esp_netif_sntp_init(&sntp_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SNTP init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    for (uint32_t retry = 1; retry <= TIME_SYNC_RETRY_COUNT; ++retry) {
        ESP_LOGD(TAG,
                 "waiting for system time... (%lu/%lu)",
                 (unsigned long)retry,
                 (unsigned long)TIME_SYNC_RETRY_COUNT);

        ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(TIME_SYNC_TIMEOUT_MS));
        if (ret != ESP_ERR_TIMEOUT) {
            break;
        }
    }

    time(&now);
    esp_netif_sntp_deinit();

    if (ret == ESP_OK && system_time_has_valid_time()) {
        ESP_LOGI(TAG, "system time synchronized: unix=%lld", (long long)now);
        return ESP_OK;
    }

    ESP_LOGE(TAG,
             "system time sync failed: %s (%d), unix=%lld",
             esp_err_to_name(ret),
             ret,
             (long long)now);
    return ret == ESP_OK ? ESP_FAIL : ret;
}

static void system_time_notify_sync_done(esp_err_t result)
{
    system_time_sync_cb_t cb = NULL;
    void *cb_ctx = NULL;
    bool valid = system_time_has_valid_time();

    taskENTER_CRITICAL(&s_time_sync_lock);
    cb = s_time_sync_cb;
    cb_ctx = s_time_sync_cb_ctx;
    taskEXIT_CRITICAL(&s_time_sync_lock);

    if (cb != NULL) {
        cb(result, valid, cb_ctx);
    }
}

static void system_time_sync_task(void *ctx)
{
    (void)ctx;

    while (true) {
        bool force_sync = false;

        taskENTER_CRITICAL(&s_time_sync_lock);
        force_sync = s_time_sync_force_requested;
        s_time_sync_force_requested = false;
        taskEXIT_CRITICAL(&s_time_sync_lock);

        esp_err_t ret = system_time_once(force_sync);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "background system time sync failed: %s", esp_err_to_name(ret));
        }
        system_time_notify_sync_done(ret);

        taskENTER_CRITICAL(&s_time_sync_lock);
        if (!s_time_sync_force_requested) {
            s_time_sync_running = false;
            taskEXIT_CRITICAL(&s_time_sync_lock);
            break;
        }
        taskEXIT_CRITICAL(&s_time_sync_lock);
    }

    platform_task_reaper_delete_current_with_caps(TAG);
}
