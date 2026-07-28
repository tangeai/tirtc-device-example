/*
 * 示例主流程：联网、同步时间、启动 TiRTC，再用 BOOT 键切换连接状态。
 */
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "app_version.h"
#include "boot_button.h"
#include "time_sync.h"
#include "tirtc_app.h"
#include "wifi_sta.h"

static const char *TAG = "app_main";

static void on_boot_button_pressed(void *user_data)
{
    (void)user_data;

    /* BOOT 键切换连接状态：未连接时主动连接，已连接时主动断开。 */
    ESP_LOGI(TAG, "收到 BOOT 键触发，切换 TiRTC 连接状态");
    (void)tirtc_toggle_connection();
}

void app_main(void)
{
    ESP_LOGI(TAG,
             "%s v%s release=%s sdk=%s",
             APP_DEMO_NAME,
             APP_DEMO_VERSION,
             APP_DEMO_RELEASE_DATE,
             APP_DEMO_TIRTC_SDK_VERSION);

    ESP_ERROR_CHECK(wifi_sta_connect());
    ESP_ERROR_CHECK(time_sync_once());

    esp_err_t ret = tirtc_start();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "TiRTC 启动失败，请保留完整串口输出给技术支持分析");
        return;
    }

    ESP_ERROR_CHECK(boot_button_start(on_boot_button_pressed, NULL));

    /* 本示例常驻运行；如果业务需要退出 TiRTC，可在退出前调用 tirtc_deinit()。 */
    while (true)
    {
        tirtc_log_status();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
