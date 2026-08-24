#include "app.h"

#include "esp_err.h"

void app_main(void)
{
    ESP_ERROR_CHECK(app_init());

    /*
     * All runtime work is owned by dedicated tasks, timers and event callbacks.
     * Returning lets ESP-IDF delete the main task and reclaim its larger,
     * boot-only internal-RAM stack after the full initialization pipeline.
     */
}
