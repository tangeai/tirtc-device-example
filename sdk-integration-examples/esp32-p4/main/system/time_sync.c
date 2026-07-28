/*
 * 开机时间同步：保证 token 和服务鉴权使用有效 UTC 时间。
 */
#include "time_sync.h"

#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "time_sync";

#define TIME_SYNC_SNTP_SERVER "ntp.aliyun.com"
#define TIME_SYNC_TIMEOUT_MS 15000

esp_err_t time_sync_once(void)
{
    time_t now = 0;
    struct tm timeinfo = {0};
    time(&now);
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year >= (2024 - 1900))
    {
        ESP_LOGI(TAG, "系统时间已有效: %04d-%02d-%02d %02d:%02d:%02d",
                 timeinfo.tm_year + 1900,
                 timeinfo.tm_mon + 1,
                 timeinfo.tm_mday,
                 timeinfo.tm_hour,
                 timeinfo.tm_min,
                 timeinfo.tm_sec);
        return ESP_OK;
    }

    /* TiRTC 服务鉴权依赖时间戳，SDK 启动前先把系统时间校准。 */
    ESP_LOGI(TAG, "开始通过 SNTP 同步系统时间: %s", TIME_SYNC_SNTP_SERVER);

    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(TIME_SYNC_SNTP_SERVER);
    esp_err_t ret = esp_netif_sntp_init(&sntp_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SNTP 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(TIME_SYNC_TIMEOUT_MS));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "系统时间同步超时: %d ms", TIME_SYNC_TIMEOUT_MS);
        esp_netif_sntp_deinit();
        return ret;
    }

    time(&now);
    localtime_r(&now, &timeinfo);
    ESP_LOGI(TAG, "系统时间同步完成: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900,
             timeinfo.tm_mon + 1,
             timeinfo.tm_mday,
             timeinfo.tm_hour,
             timeinfo.tm_min,
             timeinfo.tm_sec);

    esp_netif_sntp_deinit();
    return ESP_OK;
}
