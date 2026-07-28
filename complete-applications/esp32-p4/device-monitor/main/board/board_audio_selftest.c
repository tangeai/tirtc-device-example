#include "board_audio_selftest.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "es8311.h"

#include "board_profile.h"

static const char *TAG = "board_audio";

#define AUDIO_SELFTEST_I2C_NUM I2C_NUM_0
#define AUDIO_SELFTEST_I2S_NUM I2S_NUM_0
#define AUDIO_SELFTEST_SAMPLE_RATE 16000
#define AUDIO_SELFTEST_MCLK_MULTIPLE 384
#define AUDIO_SELFTEST_MCLK_HZ (AUDIO_SELFTEST_SAMPLE_RATE * AUDIO_SELFTEST_MCLK_MULTIPLE)
#define AUDIO_SELFTEST_CAPTURE_BYTES 2048
#define AUDIO_SELFTEST_TONE_FRAMES 160

static void *audio_selftest_calloc_prefer_psram(size_t count, size_t size)
{
    void *ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = calloc(count, size);
    }
    return ptr;
}
#define AUDIO_SELFTEST_TONE_LOOPS 20
#define AUDIO_SELFTEST_TONE_AMPLITUDE 1200

static i2s_chan_handle_t s_tx_handle;
static i2s_chan_handle_t s_rx_handle;
static bool s_i2c_ready;
static bool s_i2s_ready;
static bool s_codec_ready;

static esp_err_t audio_selftest_gpio_init(void)
{
    const board_pin_profile_t *pins = board_get_pin_profile();
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pins->speaker_pa),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "config speaker PA GPIO failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(pins->speaker_pa, 1), TAG, "enable speaker PA failed");
    ESP_LOGI(TAG, "Speaker PA enabled on GPIO%d", pins->speaker_pa);
    return ESP_OK;
}

static esp_err_t audio_selftest_i2c_init(void)
{
    const board_pin_profile_t *pins = board_get_pin_profile();

    if (s_i2c_ready)
    {
        return ESP_OK;
    }

    const i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = pins->i2c_sda,
        .scl_io_num = pins->i2c_scl,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };

    ESP_RETURN_ON_ERROR(i2c_param_config(AUDIO_SELFTEST_I2C_NUM, &i2c_cfg), TAG, "config I2C failed");
    esp_err_t ret = i2c_driver_install(AUDIO_SELFTEST_I2C_NUM, I2C_MODE_MASTER, 0, 0, 0);
    if (ret == ESP_ERR_INVALID_STATE)
    {
        ret = ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "install I2C driver failed");

    s_i2c_ready = true;
    ESP_LOGI(TAG, "I2C ready: port=%d SCL=%d SDA=%d", AUDIO_SELFTEST_I2C_NUM, pins->i2c_scl, pins->i2c_sda);
    return ESP_OK;
}

static esp_err_t audio_selftest_i2s_init(void)
{
    const board_pin_profile_t *pins = board_get_pin_profile();

    if (s_i2s_ready)
    {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_SELFTEST_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle), TAG, "create I2S channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SELFTEST_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = pins->i2s_mclk,
            .bclk = pins->i2s_bclk,
            .ws = pins->i2s_ws,
            .dout = pins->i2s_dout,
            .din = pins->i2s_din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = AUDIO_SELFTEST_MCLK_MULTIPLE;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &std_cfg), TAG, "init I2S TX failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_handle, &std_cfg), TAG, "init I2S RX failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "enable I2S TX failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_handle), TAG, "enable I2S RX failed");

    s_i2s_ready = true;
    ESP_LOGI(TAG,
             "I2S ready: port=%d sample_rate=%d MCLK=%d BCLK=%d WS=%d DOUT=%d DIN=%d",
             AUDIO_SELFTEST_I2S_NUM,
             AUDIO_SELFTEST_SAMPLE_RATE,
             pins->i2s_mclk,
             pins->i2s_bclk,
             pins->i2s_ws,
             pins->i2s_dout,
             pins->i2s_din);
    return ESP_OK;
}

static esp_err_t audio_selftest_codec_init(void)
{
    if (s_codec_ready)
    {
        return ESP_OK;
    }

    es8311_handle_t codec = es8311_create(AUDIO_SELFTEST_I2C_NUM, ES8311_ADDRRES_0);
    ESP_RETURN_ON_FALSE(codec != NULL, ESP_FAIL, TAG, "create ES8311 failed");

    const es8311_clock_config_t clk_cfg = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = AUDIO_SELFTEST_MCLK_HZ,
        .sample_frequency = AUDIO_SELFTEST_SAMPLE_RATE,
    };

    ESP_RETURN_ON_ERROR(es8311_init(codec, &clk_cfg, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16),
                        TAG,
                        "init ES8311 failed");
    ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(codec, AUDIO_SELFTEST_MCLK_HZ, AUDIO_SELFTEST_SAMPLE_RATE),
                        TAG,
                        "set ES8311 sample rate failed");
    ESP_RETURN_ON_ERROR(es8311_voice_volume_set(codec, 70, NULL), TAG, "set ES8311 volume failed");
    ESP_RETURN_ON_ERROR(es8311_microphone_config(codec, false), TAG, "enable ES8311 microphone failed");
    ESP_RETURN_ON_ERROR(es8311_microphone_gain_set(codec, ES8311_MIC_GAIN_30DB),
                        TAG,
                        "set ES8311 microphone gain failed");

    s_codec_ready = true;
    ESP_LOGI(TAG, "ES8311 ready: sample_rate=%d bits=16 channels=stereo", AUDIO_SELFTEST_SAMPLE_RATE);
    return ESP_OK;
}

static esp_err_t audio_selftest_capture_once(void)
{
    int16_t *buffer = audio_selftest_calloc_prefer_psram(1, AUDIO_SELFTEST_CAPTURE_BYTES);
    ESP_RETURN_ON_FALSE(buffer != NULL, ESP_ERR_NO_MEM, TAG, "alloc capture buffer failed");

    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(s_rx_handle, buffer, AUDIO_SELFTEST_CAPTURE_BYTES, &bytes_read, pdMS_TO_TICKS(1000));
    if (ret == ESP_OK)
    {
        uint32_t sample_count = bytes_read / sizeof(int16_t);
        uint32_t peak = 0;
        uint64_t abs_sum = 0;

        for (uint32_t i = 0; i < sample_count; ++i)
        {
            int32_t sample = buffer[i];
            uint32_t abs_value = (sample < 0) ? (uint32_t)(-sample) : (uint32_t)sample;
            if (abs_value > peak)
            {
                peak = abs_value;
            }
            abs_sum += abs_value;
        }

        ESP_LOGI(TAG,
                 "Capture ok: bytes=%u samples=%" PRIu32 " peak=%" PRIu32 " avg_abs=%" PRIu32,
                 (unsigned)bytes_read,
                 sample_count,
                 peak,
                 sample_count > 0 ? (uint32_t)(abs_sum / sample_count) : 0);
    }
    else
    {
        ESP_LOGE(TAG, "Capture failed: %s", esp_err_to_name(ret));
    }

    free(buffer);
    return ret;
}

static esp_err_t audio_selftest_play_tone(void)
{
    int16_t tone[AUDIO_SELFTEST_TONE_FRAMES * 2] = {0};

    for (uint32_t frame = 0; frame < AUDIO_SELFTEST_TONE_FRAMES; ++frame)
    {
        int16_t sample = ((frame / 8U) % 2U) ? AUDIO_SELFTEST_TONE_AMPLITUDE : -AUDIO_SELFTEST_TONE_AMPLITUDE;
        tone[frame * 2] = sample;
        tone[frame * 2 + 1] = sample;
    }

    size_t total_written = 0;
    for (uint32_t loop = 0; loop < AUDIO_SELFTEST_TONE_LOOPS; ++loop)
    {
        size_t bytes_written = 0;
        esp_err_t ret = i2s_channel_write(s_tx_handle, tone, sizeof(tone), &bytes_written, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Playback failed: %s", esp_err_to_name(ret));
            return ret;
        }
        total_written += bytes_written;
    }

    ESP_LOGI(TAG, "Playback ok: tone_bytes=%u", (unsigned)total_written);
    return ESP_OK;
}

esp_err_t board_audio_selftest_run(void)
{
    ESP_LOGI(TAG, "Audio self-test begin");

    esp_err_t ret = audio_selftest_gpio_init();
    if (ret == ESP_OK)
    {
        ret = audio_selftest_i2s_init();
    }
    if (ret == ESP_OK)
    {
        ret = audio_selftest_i2c_init();
    }
    if (ret == ESP_OK)
    {
        ret = audio_selftest_codec_init();
    }
    if (ret == ESP_OK)
    {
        ret = audio_selftest_capture_once();
    }
    if (ret == ESP_OK)
    {
        ret = audio_selftest_play_tone();
    }

    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "Audio self-test done");
    }
    else
    {
        ESP_LOGE(TAG, "Audio self-test failed: %s", esp_err_to_name(ret));
    }
    return ret;
}
