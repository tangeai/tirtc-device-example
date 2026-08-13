#include "app_at_commands.h"
#include "app_user_log.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "app_main";

void app_main(void)
{
    app_user_log_init();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 布局已变化，正在清空 NVS 分区");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = app_at_commands_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AT 应用启动失败（代码=%d）", err);
        return;
    }
}
