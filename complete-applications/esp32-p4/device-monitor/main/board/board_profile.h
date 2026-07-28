#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    const char *name;
    const char *soc;
    const char *wifi_transport;
    uint8_t flash_mb;
    uint8_t psram_mb;
    bool has_camera;
    bool has_audio_codec;
    bool has_lcd;
    bool has_touch;
} board_profile_t;

typedef struct
{
    int i2c_scl;
    int i2c_sda;
    int i2s_mclk;
    int i2s_bclk;
    int i2s_ws;
    int i2s_dout;
    int i2s_din;
    int speaker_pa;
    int wifi_reset;
    int wifi_sdio_clk;
    int wifi_sdio_cmd;
    int wifi_sdio_d0;
    int wifi_sdio_d1;
    int wifi_sdio_d2;
    int wifi_sdio_d3;
} board_pin_profile_t;

const board_profile_t *board_get_profile(void);
const board_pin_profile_t *board_get_pin_profile(void);
void board_log_profile(void);
