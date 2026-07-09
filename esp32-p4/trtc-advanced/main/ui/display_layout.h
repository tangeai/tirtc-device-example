#pragma once

#include "lvgl.h"

#define DISPLAY_DESIGN_WIDTH  320
#define DISPLAY_DESIGN_HEIGHT 240

lv_coord_t display_scale_x(lv_coord_t value);
lv_coord_t display_scale_y(lv_coord_t value);
lv_coord_t display_scale_square(lv_coord_t value);
void display_obj_set_design_pos(lv_obj_t *obj, lv_coord_t x, lv_coord_t y);
void display_obj_set_design_size(lv_obj_t *obj, lv_coord_t width, lv_coord_t height);
