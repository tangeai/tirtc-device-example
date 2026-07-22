#include "app_log_policy.h"

#include <stddef.h>

static const char *TAG = "log_policy";

void app_log_policy_apply(void)
{
#if CONFIG_APP_VERBOSE_RUNTIME_LOGS
    ESP_LOGI(TAG, "runtime log profile: diagnostic");
#else
    static const char *const warning_only_tags[] = {
        "CALL_FLOW",
        "H_API",
        "H_SDIO_DRV",
        "sdio_wrapper",
        "transport",
        "hci_stub_drv",
        "esp_wifi_remote",
        "esp_netif_handlers",
        "esp-x509-crt-bundle",
        "mqtt_client",
        "thing_http",
        "binding_http",
        "binding_mqtt",
        "st7796",
        "st7796_general",
        "LVGL",
        "ESP32_P4_35",
        "csi_video",
        "H264_DEC.SW",
        "video_convert",
        "media_sink",
        "rtc_media_bridge",
        "tirtc_sdk",
        "I2S_IF",
        "i2s_std",
    };

    for (size_t index = 0; index < sizeof(warning_only_tags) / sizeof(warning_only_tags[0]); ++index) {
        esp_log_level_set(warning_only_tags[index], ESP_LOG_WARN);
    }
    ESP_LOGI(TAG, "runtime log profile: concise");
#endif
}
