#include "display_driver.h"

#include "bsp/esp32_p4_wifi6_touch_lcd_35.h"
#include "esp_check.h"
#include "esp_log.h"

#include "app_task_affinity.h"
#include "hardware_board.h"

static const char *TAG = "display_driver";

#define DISPLAY_DRIVER_ROTATION   LV_DISP_ROT_270
#define DISPLAY_DRIVER_DRAW_LINES 4
#define DISPLAY_DRIVER_TOUCH_SCROLL_LIMIT_PX  18
#define DISPLAY_DRIVER_TOUCH_SCROLL_THROW     0
#define DISPLAY_DRIVER_TOUCH_GESTURE_LIMIT_PX 45

static lv_disp_t *s_display;
static lv_indev_t *s_touch_indev;
static bool s_initialized;

static void display_driver_configure_touch(lv_indev_t *indev)
{
	if (indev == NULL || indev->driver == NULL) {
		return;
	}

	indev->driver->scroll_limit = DISPLAY_DRIVER_TOUCH_SCROLL_LIMIT_PX;
	indev->driver->scroll_throw = DISPLAY_DRIVER_TOUCH_SCROLL_THROW;
	indev->driver->gesture_limit = DISPLAY_DRIVER_TOUCH_GESTURE_LIMIT_PX;
	ESP_LOGI(TAG,
		 "touch input ready: scroll_limit=%u scroll_throw=%u gesture_limit=%u",
		 (unsigned)indev->driver->scroll_limit,
		 (unsigned)indev->driver->scroll_throw,
		 (unsigned)indev->driver->gesture_limit);
}

esp_err_t display_driver_init(display_driver_handles_t *handles)
{
	if (s_initialized) {
		if (handles != NULL) {
			handles->display = s_display;
			handles->touch_indev = s_touch_indev;
		}
		return ESP_OK;
	}

	bsp_display_cfg_t cfg = {
		.lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
		.buffer_size = BSP_LCD_V_RES * DISPLAY_DRIVER_DRAW_LINES,
		.double_buffer = false,
		.flags = {
			.buff_dma = false,
			.buff_spiram = true,
		},
	};
	cfg.lvgl_port_cfg.task_stack = 12 * 1024;
	cfg.lvgl_port_cfg.task_affinity = APP_TASK_CORE_UI;
	cfg.lvgl_port_cfg.task_stack_caps = APP_TASK_STACK_CAPS_INTERNAL;

	s_display = bsp_display_start_with_config(&cfg);
	ESP_RETURN_ON_FALSE(s_display != NULL, ESP_FAIL, TAG, "bsp display start failed");
	lv_disp_set_rotation(s_display, DISPLAY_DRIVER_ROTATION);

	s_touch_indev = bsp_display_get_input_dev();
	ESP_RETURN_ON_FALSE(s_touch_indev != NULL, ESP_FAIL, TAG, "bsp touch init failed");
	display_driver_configure_touch(s_touch_indev);
	ESP_RETURN_ON_ERROR(bsp_display_backlight_on(), TAG, "backlight on failed");

	s_initialized = true;
	if (handles != NULL) {
		handles->display = s_display;
		handles->touch_indev = s_touch_indev;
	}

	ESP_LOGI(TAG,
		 "display ready: physical=%dx%d ui=%ux%u rotation=%u",
		 BSP_LCD_H_RES,
		 BSP_LCD_V_RES,
		 display_driver_width(),
		 display_driver_height(),
		 (unsigned)DISPLAY_DRIVER_ROTATION);
	return ESP_OK;
}

bool display_driver_is_initialized(void)
{
	return s_initialized;
}

uint16_t display_driver_width(void)
{
	if (s_initialized && s_display != NULL) {
		return (uint16_t)lv_disp_get_hor_res(s_display);
	}
	return hardware_board_get_display_config()->width;
}

uint16_t display_driver_height(void)
{
	if (s_initialized && s_display != NULL) {
		return (uint16_t)lv_disp_get_ver_res(s_display);
	}
	return hardware_board_get_display_config()->height;
}
