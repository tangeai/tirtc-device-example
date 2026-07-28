#include "board_profile.h"

#include "esp_log.h"

static const char *TAG = "board";

static const board_profile_t s_board_profile = {
    .name = "Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5",
    .soc = "ESP32-P4NRW32",
    .wifi_transport = "ESP32-C6H8 SDIO 4-bit ESP-Hosted",
    .flash_mb = 16,
    .psram_mb = 32,
    .has_camera = true,
    .has_audio_codec = true,
    .has_lcd = true,
    .has_touch = true,
};

static const board_pin_profile_t s_pin_profile = {
    .i2c_scl = 8,
    .i2c_sda = 7,
    .i2s_mclk = 13,
    .i2s_bclk = 12,
    .i2s_ws = 10,
    .i2s_dout = 9,
    .i2s_din = 11,
    .speaker_pa = 53,
    .wifi_reset = 54,
    .wifi_sdio_clk = 18,
    .wifi_sdio_cmd = 19,
    .wifi_sdio_d0 = 14,
    .wifi_sdio_d1 = 15,
    .wifi_sdio_d2 = 16,
    .wifi_sdio_d3 = 17,
};

const board_profile_t *board_get_profile(void)
{
    return &s_board_profile;
}

const board_pin_profile_t *board_get_pin_profile(void)
{
    return &s_pin_profile;
}

void board_log_profile(void)
{
    ESP_LOGI(TAG,
             "Board: %s, SoC=%s, Flash=%uMB, PSRAM=%uMB, Wi-Fi=%s",
             s_board_profile.name,
             s_board_profile.soc,
             s_board_profile.flash_mb,
             s_board_profile.psram_mb,
             s_board_profile.wifi_transport);
    ESP_LOGI(TAG,
             "Board pins: I2C SCL=%d SDA=%d, I2S MCLK=%d BCLK=%d WS=%d DOUT=%d DIN=%d, PA=%d",
             s_pin_profile.i2c_scl,
             s_pin_profile.i2c_sda,
             s_pin_profile.i2s_mclk,
             s_pin_profile.i2s_bclk,
             s_pin_profile.i2s_ws,
             s_pin_profile.i2s_dout,
             s_pin_profile.i2s_din,
             s_pin_profile.speaker_pa);
    ESP_LOGI(TAG,
             "Board Wi-Fi SDIO: reset=%d clk=%d cmd=%d d0=%d d1=%d d2=%d d3=%d",
             s_pin_profile.wifi_reset,
             s_pin_profile.wifi_sdio_clk,
             s_pin_profile.wifi_sdio_cmd,
             s_pin_profile.wifi_sdio_d0,
             s_pin_profile.wifi_sdio_d1,
             s_pin_profile.wifi_sdio_d2,
             s_pin_profile.wifi_sdio_d3);
    ESP_LOGI(TAG,
             "Board media: camera=%d audio=%d lcd=%d touch=%d",
             s_board_profile.has_camera ? 1 : 0,
             s_board_profile.has_audio_codec ? 1 : 0,
             s_board_profile.has_lcd ? 1 : 0,
             s_board_profile.has_touch ? 1 : 0);
}
