#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#define DISPLAY_DRIVER_WIDTH  ((lv_coord_t)display_driver_width())
#define DISPLAY_DRIVER_HEIGHT ((lv_coord_t)display_driver_height())

typedef struct {
    lv_disp_t *display;
    lv_indev_t *touch_indev;
} display_driver_handles_t;

esp_err_t display_driver_init(display_driver_handles_t *handles);
bool display_driver_is_initialized(void);
uint16_t display_driver_width(void);
uint16_t display_driver_height(void);
