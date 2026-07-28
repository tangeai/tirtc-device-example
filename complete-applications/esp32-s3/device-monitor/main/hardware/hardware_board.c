#include "hardware_board.h"

#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "freertos/FreeRTOS.h"

#define HARDWARE_BOARD_IO_EXPANDER_INIT_RETRIES  5
#define HARDWARE_BOARD_IO_EXPANDER_INIT_DELAY_MS 10

static const hardware_board_t s_board = {
	.type = HARDWARE_BOARD_TYPE,
	.i2c = {
		.port = HARDWARE_BOARD_I2C_NUM,
		.sda_gpio = HARDWARE_BOARD_I2C_SDA,
		.scl_gpio = HARDWARE_BOARD_I2C_SCL,
		.freq_hz = HARDWARE_BOARD_I2C_FREQ_HZ,
	},
	.display = {
		.spi_host = HARDWARE_BOARD_LCD_SPI_HOST,
		.spi_mosi_gpio = HARDWARE_BOARD_LCD_SPI_MOSI,
		.spi_clk_gpio = HARDWARE_BOARD_LCD_SPI_CLK,
		.spi_cs_gpio = HARDWARE_BOARD_LCD_SPI_CS,
		.dc_gpio = HARDWARE_BOARD_LCD_DC,
		.reset_gpio = HARDWARE_BOARD_LCD_RST,
		.backlight_gpio = HARDWARE_BOARD_LCD_BACKLIGHT,
		.backlight_ledc_timer = HARDWARE_BOARD_LCD_BACKLIGHT_LEDC_TIMER,
		.backlight_ledc_channel = HARDWARE_BOARD_LCD_BACKLIGHT_LEDC_CHANNEL,
		.pixel_clock_hz = HARDWARE_BOARD_LCD_PIXEL_CLOCK_HZ,
		.width = HARDWARE_BOARD_LCD_WIDTH,
		.height = HARDWARE_BOARD_LCD_HEIGHT,
		.draw_buffer_height = HARDWARE_BOARD_LCD_DRAW_BUF_HEIGHT,
		.cmd_bits = HARDWARE_BOARD_LCD_CMD_BITS,
		.param_bits = HARDWARE_BOARD_LCD_PARAM_BITS,
		.bits_per_pixel = HARDWARE_BOARD_LCD_BITS_PER_PIXEL,
	},
	.audio = {
		.i2s_port = HARDWARE_BOARD_AUDIO_I2S_PORT,
		.lrck_gpio = HARDWARE_BOARD_AUDIO_LRCK,
		.mclk_gpio = HARDWARE_BOARD_AUDIO_MCLK,
		.bclk_gpio = HARDWARE_BOARD_AUDIO_BCLK,
		.din_gpio = HARDWARE_BOARD_AUDIO_DIN,
		.dout_gpio = HARDWARE_BOARD_AUDIO_DOUT,
		.pa_gpio = HARDWARE_BOARD_AUDIO_PA_GPIO,
		.sample_rate_hz = HARDWARE_BOARD_AUDIO_SAMPLE_RATE_HZ,
		.bits_per_sample = HARDWARE_BOARD_AUDIO_BITS_PER_SAMPLE,
		.channels = HARDWARE_BOARD_AUDIO_CHANNELS,
		.adc_channels = HARDWARE_BOARD_AUDIO_ADC_CHANNELS,
		.default_volume_percent = HARDWARE_BOARD_AUDIO_DEFAULT_VOLUME,
		.default_adc_gain_db = HARDWARE_BOARD_AUDIO_DEFAULT_ADC_GAIN_DB,
		.speaker_codec = HARDWARE_AUDIO_CODEC_ES8311,
		.microphone_codec = HARDWARE_AUDIO_CODEC_ES7210,
		.speaker_codec_i2c_addr = ES8311_CODEC_DEFAULT_ADDR,
		.microphone_codec_i2c_addr = HARDWARE_BOARD_AUDIO_ES7210_ADDR,
		.microphone_select_mask = ES7120_SEL_MIC1 | ES7120_SEL_MIC2 |
					   ES7120_SEL_MIC3 | ES7120_SEL_MIC4,
	},
	.camera = {
		.enabled = HARDWARE_BOARD_CAMERA_ENABLED != 0,
		.pwdn_gpio = HARDWARE_BOARD_CAMERA_PWDN,
		.reset_gpio = HARDWARE_BOARD_CAMERA_RESET,
		.xclk_gpio = HARDWARE_BOARD_CAMERA_XCLK,
		.sccb_sda_gpio = HARDWARE_BOARD_CAMERA_SIOD,
		.sccb_scl_gpio = HARDWARE_BOARD_CAMERA_SIOC,
		.data_gpio = {
			HARDWARE_BOARD_CAMERA_D0,
			HARDWARE_BOARD_CAMERA_D1,
			HARDWARE_BOARD_CAMERA_D2,
			HARDWARE_BOARD_CAMERA_D3,
			HARDWARE_BOARD_CAMERA_D4,
			HARDWARE_BOARD_CAMERA_D5,
			HARDWARE_BOARD_CAMERA_D6,
			HARDWARE_BOARD_CAMERA_D7,
		},
		.vsync_gpio = HARDWARE_BOARD_CAMERA_VSYNC,
		.href_gpio = HARDWARE_BOARD_CAMERA_HREF,
		.pclk_gpio = HARDWARE_BOARD_CAMERA_PCLK,
		.xclk_ledc_timer = HARDWARE_BOARD_CAMERA_XCLK_LEDC_TIMER,
		.xclk_ledc_channel = HARDWARE_BOARD_CAMERA_XCLK_LEDC_CHANNEL,
		.xclk_freq_hz = HARDWARE_BOARD_CAMERA_XCLK_FREQ_HZ,
		.frame_buffer_count = HARDWARE_BOARD_CAMERA_FB_COUNT,
		.hmirror = HARDWARE_BOARD_CAMERA_HMIRROR != 0,
		.vflip = HARDWARE_BOARD_CAMERA_VFLIP != 0,
	},
};

static bool s_i2c_initialized;
static bool s_io_expander_initialized;
static i2c_master_bus_handle_t s_i2c_bus_handle;
static i2c_master_dev_handle_t s_pca9557_dev_handle;
static bool s_pca9557_output_cached_valid;
static uint8_t s_pca9557_output_cached = HARDWARE_BOARD_IO_EXPANDER_OUTPUT_INIT;

const hardware_board_t *hardware_board_get(void)
{
	return &s_board;
}

const hardware_i2c_config_t *hardware_board_get_i2c_config(void)
{
	return &s_board.i2c;
}

const hardware_display_config_t *hardware_board_get_display_config(void)
{
	return &s_board.display;
}

const hardware_audio_config_t *hardware_board_get_audio_config(void)
{
	return &s_board.audio;
}

const hardware_camera_config_t *hardware_board_get_camera_config(void)
{
	return &s_board.camera;
}

static esp_err_t hardware_board_pca9557_init_device(void)
{
	if (s_pca9557_dev_handle != NULL) {
		return ESP_OK;
	}

	i2c_device_config_t dev_cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = HARDWARE_BOARD_PCA9557_ADDRESS,
		.scl_speed_hz = s_board.i2c.freq_hz,
	};
	return i2c_master_bus_add_device(s_i2c_bus_handle, &dev_cfg, &s_pca9557_dev_handle);
}

static esp_err_t hardware_board_pca9557_write(uint8_t reg_addr, uint8_t value)
{
	ESP_RETURN_ON_ERROR(hardware_board_pca9557_init_device(), "hardware", "init pca9557 failed");
	uint8_t write_buf[2] = {reg_addr, value};
	return i2c_master_transmit(s_pca9557_dev_handle,
				   write_buf,
				   sizeof(write_buf),
				   pdMS_TO_TICKS(100));
}

static esp_err_t hardware_board_pca9557_write_with_retry(uint8_t reg_addr, uint8_t value)
{
	esp_err_t last_err = ESP_FAIL;

	for (int attempt = 0; attempt < HARDWARE_BOARD_IO_EXPANDER_INIT_RETRIES; ++attempt) {
		last_err = hardware_board_pca9557_write(reg_addr, value);
		if (last_err == ESP_OK) {
			return ESP_OK;
		}
		vTaskDelay(pdMS_TO_TICKS(HARDWARE_BOARD_IO_EXPANDER_INIT_DELAY_MS));
	}

	return last_err;
}

static esp_err_t hardware_board_pca9557_set_output(uint8_t gpio_mask, bool high_level)
{
	uint8_t output_value = s_pca9557_output_cached_valid ?
			       s_pca9557_output_cached :
			       HARDWARE_BOARD_IO_EXPANDER_OUTPUT_INIT;

	if (high_level) {
		output_value |= gpio_mask;
	} else {
		output_value &= (uint8_t)~gpio_mask;
	}

	ESP_RETURN_ON_ERROR(hardware_board_pca9557_write_with_retry(0x01, output_value),
			    "hardware",
			    "write io expander failed");
	s_pca9557_output_cached = output_value;
	s_pca9557_output_cached_valid = true;
	return ESP_OK;
}

static esp_err_t hardware_board_write_io_expander_defaults(void)
{
	esp_err_t last_err = ESP_FAIL;

	for (int attempt = 0; attempt < HARDWARE_BOARD_IO_EXPANDER_INIT_RETRIES; ++attempt) {
		last_err = hardware_board_pca9557_write(0x03, 0xf8);
		if (last_err == ESP_OK) {
			last_err = hardware_board_pca9557_write(0x01, HARDWARE_BOARD_IO_EXPANDER_OUTPUT_INIT);
		}
		if (last_err == ESP_OK) {
			s_pca9557_output_cached = HARDWARE_BOARD_IO_EXPANDER_OUTPUT_INIT;
			s_pca9557_output_cached_valid = true;
			return ESP_OK;
		}

		vTaskDelay(pdMS_TO_TICKS(HARDWARE_BOARD_IO_EXPANDER_INIT_DELAY_MS));
	}

	return last_err;
}

esp_err_t hardware_board_init_i2c(void)
{
	if (s_i2c_initialized) {
		return ESP_OK;
	}

	i2c_master_bus_config_t i2c_cfg = {
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.i2c_port = s_board.i2c.port,
		.scl_io_num = s_board.i2c.scl_gpio,
		.sda_io_num = s_board.i2c.sda_gpio,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};

	ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c_bus_handle),
			    "hardware",
			    "create i2c bus failed");

	s_i2c_initialized = true;
	return ESP_OK;
}

i2c_master_bus_handle_t hardware_board_get_i2c_bus_handle(void)
{
	return s_i2c_bus_handle;
}

esp_err_t hardware_board_init_io_expander(void)
{
	if (s_io_expander_initialized) {
		return ESP_OK;
	}

	ESP_RETURN_ON_ERROR(hardware_board_init_i2c(), "hardware", "i2c not ready");
	ESP_RETURN_ON_ERROR(hardware_board_write_io_expander_defaults(),
			    "hardware",
			    "set io expander defaults failed");

	s_io_expander_initialized = true;
	return ESP_OK;
}

esp_err_t hardware_board_set_lcd_chip_select(bool active)
{
	ESP_RETURN_ON_ERROR(hardware_board_init_io_expander(), "hardware", "io expander not ready");
	return hardware_board_pca9557_set_output(HARDWARE_BOARD_LCD_CS_GPIO_MASK, !active);
}

esp_err_t hardware_board_set_audio_power(bool enable)
{
	ESP_RETURN_ON_ERROR(hardware_board_init_io_expander(), "hardware", "io expander not ready");
	return hardware_board_pca9557_set_output(HARDWARE_BOARD_PA_EN_GPIO_MASK, enable);
}

esp_err_t hardware_board_set_camera_power(bool enable)
{
	ESP_RETURN_ON_ERROR(hardware_board_init_io_expander(), "hardware", "io expander not ready");
	return hardware_board_pca9557_set_output(HARDWARE_BOARD_CAMERA_PWDN_GPIO_MASK, !enable);
}

esp_err_t hardware_board_init(void)
{
	ESP_RETURN_ON_ERROR(hardware_board_init_i2c(), "hardware", "i2c init failed");
	ESP_RETURN_ON_ERROR(hardware_board_init_io_expander(), "hardware", "io expander init failed");
	return ESP_OK;
}
