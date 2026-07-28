/*
 * Wi-Fi STA 接入：为 TiRTC 音视频链路准备稳定的 IP 网络。
 */
#include "wifi_sta.h"

#include <stdbool.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "app_config.h"

static const char *TAG = "wifi_sta";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_MAX_RETRY 8
#define WIFI_CONNECT_TIMEOUT_MS 30000
#define WIFI_ENABLE_PERFORMANCE_MODE 1
#define WIFI_ENABLE_HT40 1
#define WIFI_MAX_TX_POWER_QDBM 84

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    EventGroupHandle_t wifi_event_group = (EventGroupHandle_t)arg;
    static int retry_count;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (retry_count < WIFI_MAX_RETRY)
        {
            retry_count++;
            ESP_LOGW(TAG, "Wi-Fi 断开，正在重连 %d/%d", retry_count, WIFI_MAX_RETRY);
            esp_wifi_connect();
        }
        else
        {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
        return;
    }

    /* 只有拿到 IP 以后才让主流程继续启动 TiRTC。 */
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        retry_count = 0;
        ESP_LOGI(TAG, "Wi-Fi 已获取 IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_sta_connect(void)
{
    if (strlen(APP_WIFI_SSID) == 0)
    {
        ESP_LOGE(TAG, "Wi-Fi SSID 为空，请先配置 APP_WIFI_SSID");
        return ESP_ERR_INVALID_ARG;
    }

    /* 主流程只在拿到 IP 后继续启动 TiRTC。 */
    EventGroupHandle_t wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL)
    {
        ESP_LOGE(TAG, "创建 Wi-Fi 事件组失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        wifi_event_handler,
                                                        (void *)wifi_event_group,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler,
                                                        (void *)wifi_event_group,
                                                        NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .scan_method = WIFI_FAST_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };

    strlcpy((char *)wifi_cfg.sta.ssid, APP_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, APP_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

#if WIFI_ENABLE_PERFORMANCE_MODE
    /* 低延迟优先：关闭省电，尽量使用 11n/HT40 和较高发射功率。 */
    esp_err_t wifi_ret = esp_wifi_set_ps(WIFI_PS_NONE);
    if (wifi_ret == ESP_OK)
    {
        ESP_LOGI(TAG, "Wi-Fi 省电关闭已启用");
    }
    else
    {
        ESP_LOGW(TAG, "Wi-Fi 省电关闭设置失败: %s", esp_err_to_name(wifi_ret));
    }

    wifi_ret = esp_wifi_set_protocol(WIFI_IF_STA,
                                     WIFI_PROTOCOL_11B |
                                         WIFI_PROTOCOL_11G |
                                         WIFI_PROTOCOL_11N);
    if (wifi_ret == ESP_OK)
    {
        ESP_LOGI(TAG, "Wi-Fi 11b/g/n 协议已启用");
    }
    else
    {
        ESP_LOGW(TAG, "Wi-Fi 11b/g/n 协议设置失败: %s", esp_err_to_name(wifi_ret));
    }

#if WIFI_ENABLE_HT40
    wifi_ret = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40);
    if (wifi_ret == ESP_OK)
    {
        ESP_LOGI(TAG, "Wi-Fi HT40 带宽已启用");
    }
    else
    {
        ESP_LOGW(TAG, "Wi-Fi HT40 带宽设置失败: %s", esp_err_to_name(wifi_ret));
    }
#endif

#else
    ESP_LOGI(TAG, "Wi-Fi 性能模式未启用，使用 ESP-IDF 默认参数");
#endif

    ESP_ERROR_CHECK(esp_wifi_start());

#if WIFI_ENABLE_PERFORMANCE_MODE
    wifi_ret = esp_wifi_set_max_tx_power(WIFI_MAX_TX_POWER_QDBM);
    if (wifi_ret == ESP_OK)
    {
        ESP_LOGI(TAG, "Wi-Fi 最大发射功率已启用");
    }
    else
    {
        ESP_LOGW(TAG, "Wi-Fi 最大发射功率设置失败: %s", esp_err_to_name(wifi_ret));
    }
#endif

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if ((bits & WIFI_CONNECTED_BIT) != 0)
    {
        ESP_LOGI(TAG, "Wi-Fi 已连接: %s", APP_WIFI_SSID);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Wi-Fi 连接失败");
    return ESP_FAIL;
}
