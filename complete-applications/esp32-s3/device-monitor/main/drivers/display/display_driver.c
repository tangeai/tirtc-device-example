#include "display_driver.h"

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "hardware_board.h"

static const char *TAG = "display_driver";

static esp_lcd_panel_handle_t s_panel_handle;
static esp_lcd_panel_io_handle_t s_panel_io_handle;
static esp_lcd_touch_handle_t s_touch_handle;
static lv_disp_t *s_display;
static lv_indev_t *s_touch_indev;
static bool s_initialized;

static esp_err_t display_driver_backlight_init(void)
{
    const hardware_display_config_t *display_config = hardware_board_get_display_config();
    const ledc_channel_config_t channel_cfg = {
        .gpio_num = display_config->backlight_gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = display_config->backlight_ledc_channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = display_config->backlight_ledc_timer,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = true,
    };
    const ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = display_config->backlight_ledc_timer,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "backlight timer init failed");
    return ledc_channel_config(&channel_cfg);
}

static esp_err_t display_driver_backlight_set(int brightness_percent)
{
    if (brightness_percent < 0) {
        brightness_percent = 0;
    }
    if (brightness_percent > 100) {
        brightness_percent = 100;
    }

    uint32_t duty_cycle = (1023U * (uint32_t)brightness_percent) / 100U;
    const hardware_display_config_t *display_config = hardware_board_get_display_config();
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE,
                                      display_config->backlight_ledc_channel,
                                      duty_cycle),
                        TAG,
                        "backlight duty failed");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, display_config->backlight_ledc_channel);
}

static esp_err_t display_driver_panel_init(void)
{
    const hardware_display_config_t *display_config = hardware_board_get_display_config();

    ESP_RETURN_ON_ERROR(display_driver_backlight_init(), TAG, "backlight init failed");

    const spi_bus_config_t bus_cfg = {
        .sclk_io_num = display_config->spi_clk_gpio,
        .mosi_io_num = display_config->spi_mosi_gpio,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = display_config->width * display_config->draw_buffer_height * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(display_config->spi_host, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "spi init failed");

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = display_config->dc_gpio,
        .cs_gpio_num = display_config->spi_cs_gpio,
        .pclk_hz = display_config->pixel_clock_hz,
        .lcd_cmd_bits = display_config->cmd_bits,
        .lcd_param_bits = display_config->param_bits,
        .spi_mode = 2,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)display_config->spi_host,
                                                 &io_cfg,
                                                 &s_panel_io_handle),
                        TAG,
                        "panel io init failed");

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = display_config->reset_gpio,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = display_config->bits_per_pixel,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_panel_io_handle, &panel_cfg, &s_panel_handle),
                        TAG,
                        "panel driver init failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel_handle), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(hardware_board_set_lcd_chip_select(true), TAG, "panel cs assert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel_handle), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel_handle, true), TAG, "invert color failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel_handle, true), TAG, "swap xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel_handle, true, false), TAG, "mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel_handle, true), TAG, "display on failed");
    return display_driver_backlight_set(100);
}

static esp_err_t display_driver_touch_init(void)
{
    const hardware_display_config_t *display_config = hardware_board_get_display_config();
    esp_lcd_panel_io_handle_t touch_io_handle = NULL;
    esp_lcd_touch_config_t touch_cfg = {
        .x_max = display_config->height,
        .y_max = display_config->width,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 1,
            .mirror_x = 1,
            .mirror_y = 0,
        },
    };
    esp_lcd_panel_io_i2c_config_t touch_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(hardware_board_get_i2c_bus_handle(),
                                                 &touch_io_cfg,
                                                 &touch_io_handle),
                        TAG,
                        "touch io init failed");
    return esp_lcd_touch_new_i2c_ft5x06(touch_io_handle, &touch_cfg, &s_touch_handle);
}

esp_err_t display_driver_init(display_driver_handles_t *handles)
{
    const hardware_display_config_t *display_config = hardware_board_get_display_config();

    if (s_initialized) {
        if (handles != NULL) {
            handles->display = s_display;
            handles->touch_indev = s_touch_indev;
        }
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(hardware_board_init(), TAG, "board init failed");

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack = 16 * 1024;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl port init failed");
    ESP_RETURN_ON_ERROR(display_driver_panel_init(), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(display_driver_touch_init(), TAG, "touch init failed");

    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = s_panel_io_handle,
        .panel_handle = s_panel_handle,
        .buffer_size = display_config->width * display_config->height,
        .double_buffer = false,
        .trans_size = display_config->width * display_config->draw_buffer_height,
        .hres = display_config->width,
        .vres = display_config->height,
        .monochrome = false,
        .rotation = {
            .swap_xy = true,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
        },
    };
    s_display = lvgl_port_add_disp(&display_cfg);
    ESP_RETURN_ON_FALSE(s_display != NULL, ESP_FAIL, TAG, "lvgl display add failed");

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = s_display,
        .handle = s_touch_handle,
    };
    s_touch_indev = lvgl_port_add_touch(&touch_cfg);
    ESP_RETURN_ON_FALSE(s_touch_indev != NULL, ESP_FAIL, TAG, "lvgl touch add failed");

    s_initialized = true;
    if (handles != NULL) {
        handles->display = s_display;
        handles->touch_indev = s_touch_indev;
    }
    return ESP_OK;
}

bool display_driver_is_initialized(void)
{
    return s_initialized;
}

uint16_t display_driver_width(void)
{
    return hardware_board_get_display_config()->width;
}

uint16_t display_driver_height(void)
{
    return hardware_board_get_display_config()->height;
}
