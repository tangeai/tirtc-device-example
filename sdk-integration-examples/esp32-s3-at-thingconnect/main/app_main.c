#include "app_at_commands.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "app_main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS layout changed; erasing NVS partition");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = app_at_commands_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AT application start failed: %s",
                 esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG,
             "AT control ready on native USB Serial/JTAG; logs use UART0");
}
