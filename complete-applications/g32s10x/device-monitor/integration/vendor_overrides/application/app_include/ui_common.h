#ifndef UI_COMMON_H
#define UI_COMMON_H

#include "third_party/lvgl/lvgl/lvgl.h"
#include "printf.h"

extern lv_font_t jz_ui_font_12;
extern lv_font_t jz_ui_font_16;
extern lv_font_t jz_ui_font_20;

#define UI_PATH "S:/"
#define GET_UI_PATH(ui) UI_PATH ui

#define JZ_FS_ROOT_PATH     "/fs"           /* 文件系统根路径 */

typedef enum {
    PAGE_MAIN = 0,
    PAGE_CAM,
    PAGE_AUDIO,
    PAGE_SETTING,
    PAGE_MULTIOBJ_DET,
    PAGE_SCANCODE,
#ifdef CONFIG_APPLICATION_TIRTC_DEMO
    PAGE_TIRTC_DEMO,
#endif
    PAGE_COUNT
} page_id_t;

uint16_t get_screen_width(void);
uint16_t get_screen_height(void);

lv_obj_t *ui_get_page(int page_id);
int ui_set_page(lv_obj_t *page, int page_id);

void ui_switch_page(page_id_t page_id);

#endif // UI_COMMON_H
