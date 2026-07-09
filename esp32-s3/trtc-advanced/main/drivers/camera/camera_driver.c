#include "camera_driver.h"

#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_camera.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sensor.h"
#include "sdkconfig.h"

#include "hardware_board.h"

static const char *TAG = "camera";

#if CONFIG_CAMERA_PSRAM_DMA
#define CAMERA_DRIVER_PSRAM_DMA_ENABLED 1
#else
#define CAMERA_DRIVER_PSRAM_DMA_ENABLED 0
#endif

static bool s_camera_initialized;
static bool s_first_frame_logged;

static void camera_driver_log_memory(const char *stage)
{
	ESP_LOGD(TAG,
		 "camera memory %s: dma_largest=%u internal_free=%u psram_free=%u",
		 stage,
		 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
		 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
		 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static bool camera_driver_pin_is_set(gpio_num_t gpio)
{
	return gpio != GPIO_NUM_NC;
}

static bool camera_driver_has_required_pins(const hardware_camera_config_t *config)
{
	if (config == NULL || !config->enabled) {
		return false;
	}
	if (!camera_driver_pin_is_set(config->xclk_gpio) ||
	    !camera_driver_pin_is_set(config->sccb_scl_gpio) ||
	    !camera_driver_pin_is_set(config->vsync_gpio) ||
	    !camera_driver_pin_is_set(config->href_gpio) ||
	    !camera_driver_pin_is_set(config->pclk_gpio)) {
		return false;
	}
	for (size_t index = 0; index < sizeof(config->data_gpio) / sizeof(config->data_gpio[0]); ++index) {
		if (!camera_driver_pin_is_set(config->data_gpio[index])) {
			return false;
		}
	}
	return true;
}

bool camera_driver_is_configured(void)
{
	return camera_driver_has_required_pins(hardware_board_get_camera_config());
}

esp_err_t camera_driver_init(void)
{
	const hardware_camera_config_t *board_camera = hardware_board_get_camera_config();

	if (s_camera_initialized) {
		return ESP_OK;
	}
	if (!camera_driver_has_required_pins(board_camera)) {
		return ESP_ERR_NOT_SUPPORTED;
	}
	ESP_RETURN_ON_ERROR(hardware_board_set_camera_power(true), TAG, "camera power on failed");
	ESP_LOGI(TAG,
		 "camera init begin: frame=QVGA format=rgb565 fb_count=%u xclk=%" PRIu32 " dma_max=%d psram_dma=%s",
		 board_camera->frame_buffer_count,
		 board_camera->xclk_freq_hz,
		 CONFIG_CAMERA_DMA_BUFFER_SIZE_MAX,
		 CAMERA_DRIVER_PSRAM_DMA_ENABLED ? "on" : "off");
	camera_driver_log_memory("before-init");

	camera_config_t config = {
		.pin_pwdn = board_camera->pwdn_gpio,
		.pin_reset = board_camera->reset_gpio,
		.pin_xclk = board_camera->xclk_gpio,
		.pin_sccb_sda = camera_driver_pin_is_set(board_camera->sccb_sda_gpio) ?
				 board_camera->sccb_sda_gpio : -1,
		.pin_sccb_scl = board_camera->sccb_scl_gpio,
		.sccb_i2c_port = hardware_board_get_i2c_config()->port,
		.pin_d7 = board_camera->data_gpio[7],
		.pin_d6 = board_camera->data_gpio[6],
		.pin_d5 = board_camera->data_gpio[5],
		.pin_d4 = board_camera->data_gpio[4],
		.pin_d3 = board_camera->data_gpio[3],
		.pin_d2 = board_camera->data_gpio[2],
		.pin_d1 = board_camera->data_gpio[1],
		.pin_d0 = board_camera->data_gpio[0],
		.pin_vsync = board_camera->vsync_gpio,
		.pin_href = board_camera->href_gpio,
		.pin_pclk = board_camera->pclk_gpio,
		.xclk_freq_hz = board_camera->xclk_freq_hz,
		.ledc_timer = board_camera->xclk_ledc_timer,
		.ledc_channel = board_camera->xclk_ledc_channel,
		.pixel_format = PIXFORMAT_RGB565,
		.frame_size = FRAMESIZE_QVGA,
		.jpeg_quality = 12,
		.fb_count = board_camera->frame_buffer_count > 0 ? board_camera->frame_buffer_count : 1,
#if CONFIG_SPIRAM
		.fb_location = CAMERA_FB_IN_PSRAM,
#else
		.fb_location = CAMERA_FB_IN_DRAM,
#endif
		.grab_mode = CAMERA_GRAB_WHEN_EMPTY,
	};

	esp_err_t ret = esp_camera_init(&config);
	camera_driver_log_memory("after-init");
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "camera init failed: %s", esp_err_to_name(ret));
		(void)esp_camera_deinit();
		(void)hardware_board_set_camera_power(false);
		camera_driver_log_memory("after-failed-init-cleanup");
		return ret;
	}

	sensor_t *sensor = esp_camera_sensor_get();
	if (sensor != NULL) {
		if (sensor->set_hmirror != NULL) {
			(void)sensor->set_hmirror(sensor, board_camera->hmirror ? 1 : 0);
		}
		if (sensor->set_vflip != NULL) {
			(void)sensor->set_vflip(sensor, board_camera->vflip ? 1 : 0);
		}
		ESP_LOGI(TAG,
			 "camera sensor orientation: pid=0x%04x hmirror=%u vflip=%u",
			 (unsigned)sensor->id.PID,
			 board_camera->hmirror ? 1U : 0U,
			 board_camera->vflip ? 1U : 0U);
	}

	s_first_frame_logged = false;
	s_camera_initialized = true;
	ESP_LOGI(TAG, "camera ready: frame=QVGA format=rgb565");
	return ESP_OK;
}

esp_err_t camera_driver_capture(camera_driver_frame_t *frame)
{
	camera_fb_t *fb = NULL;

	ESP_RETURN_ON_FALSE(frame != NULL, ESP_ERR_INVALID_ARG, TAG, "frame is null");
	ESP_RETURN_ON_ERROR(camera_driver_init(), TAG, "camera init failed");

	memset(frame, 0, sizeof(*frame));
	fb = esp_camera_fb_get();
	if (fb == NULL) {
		return ESP_ERR_TIMEOUT;
	}
	if (fb->format != PIXFORMAT_GRAYSCALE && fb->format != PIXFORMAT_RGB565) {
		esp_camera_fb_return(fb);
		return ESP_ERR_NOT_SUPPORTED;
	}

	frame->data = fb->buf;
	frame->data_len = fb->len;
	frame->width = (uint16_t)fb->width;
	frame->height = (uint16_t)fb->height;
	frame->pixel_format = fb->format == PIXFORMAT_RGB565 ?
		CAMERA_DRIVER_PIXEL_FORMAT_RGB565 : CAMERA_DRIVER_PIXEL_FORMAT_GRAYSCALE;
	frame->owner = fb;
	if (!s_first_frame_logged) {
		s_first_frame_logged = true;
		ESP_LOGD(TAG,
			 "camera first frame: %ux%u len=%u format=%s",
			 frame->width,
			 frame->height,
			 (unsigned)frame->data_len,
			 frame->pixel_format == CAMERA_DRIVER_PIXEL_FORMAT_RGB565 ? "rgb565" : "grayscale");
	}
	return ESP_OK;
}

void camera_driver_release(camera_driver_frame_t *frame)
{
	if (frame == NULL || frame->owner == NULL) {
		return;
	}

	esp_camera_fb_return((camera_fb_t *)frame->owner);
	memset(frame, 0, sizeof(*frame));
}

esp_err_t camera_driver_deinit(void)
{
	if (!s_camera_initialized) {
		return ESP_OK;
	}

	esp_err_t ret = esp_camera_deinit();
	if (ret == ESP_OK) {
		s_camera_initialized = false;
		s_first_frame_logged = false;
		ESP_RETURN_ON_ERROR(hardware_board_set_camera_power(false), TAG, "camera power off failed");
	}
	return ret;
}
