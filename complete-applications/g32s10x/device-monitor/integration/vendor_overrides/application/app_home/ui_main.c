#include <time.h>
#include <dfs.h>
#include <dfs_fs.h>
#include <dfs_file.h>
#include <dfs_posix.h>
#include "third_party/lvgl/lvgl_ingenic.h"
#include "third_party/lvgl/lvgl/lvgl.h"
#include "application/app_include/ui_common.h"
#include "application/app_include/ui_config.h"
#include "third_party/lvgl/jz_lvgl_common/jz_lvgl_common.h"
#include "hmi_service.h"
#include "ui_status_bar.h"
#ifdef CONFIG_APPLICATION_IVS_FACE
#include "ui_video.h"
#endif
#ifdef CONFIG_APPLICATION_SETTING_PAGE
#include "ui_setting.h"
#endif
#ifdef CONFIG_APPLICATION_AUDIO
#include "ui_audio.h"
#endif
#ifdef CONFIG_APPLICATION_TIRTC_DEMO
#include "ui_tirtc_demo.h"
#endif

#ifdef CONFIG_APPLICATION_MULTI_OBJ_DET
#include "ui_multi_obj.h"
#endif
#ifdef CONFIG_APPLICATION_SCANCODE
#include "ui_scancode.h"
#endif
/* 声明图片数据 */
#ifdef CONFIG_G32S10X_MIPI_TFT050_V30A05T
#define UI_BG_ICON "home/pic/desktop_480x854.png"
#else
#define UI_BG_ICON "home/pic/desktop_320x480.png"
#endif

#define UI_VIDEO_ICON "home/pic/video_100x100.png"
#define UI_AUDIO_ICON "home/pic/audio_100x100.png"
#define UI_SETTING_ICON "home/pic/setting_100x100.png"
#define UI_MULTI_OBJ_ICON "home/pic/multi_obj_det_100x100.png"
#define UI_SCANCODE_ICON "home/pic/scancode_100x100.png"
#define UI_TIRTC_ICON "tirtc/pic/tirtc_100x100.png"

/* 声明字体 */
LV_FONT_DECLARE(lv_font_montserrat_14);
#define UI_FONT_12 "S:/setting/fonts/lv_font_HarmonyOS_12.bin"
#define UI_FONT_16 "S:/setting/fonts/lv_font_HarmonyOS_16.bin"
#define UI_FONT_20 "S:/setting/fonts/lv_font_HarmonyOS_20.bin"

static lv_font_t *jz_font_12;
static lv_font_t *jz_font_16;
static lv_font_t *jz_font_20;

lv_font_t jz_ui_font_12;
lv_font_t jz_ui_font_16;
lv_font_t jz_ui_font_20;

// 屏幕方向、按钮配置等定义保持不变
typedef enum { SCREEN_ORIENT_PORTRAIT, SCREEN_ORIENT_LANDSCAPE } screen_orient_t;

// 全局变量保持不变
static screen_orient_t current_orient = SCREEN_ORIENT_PORTRAIT;
static lv_obj_t *pages[PAGE_COUNT] = {NULL};
static page_id_t current_page = PAGE_MAIN;

// 前向声明
static void create_sub_page(page_id_t page_id);
void page_button_event_handler(lv_event_t *e);

// 按钮配置保持不变
static jz_obj_config_t page_btn_configs[UI_MAIN_MAX_BUTTONS] = {
    {PAGE_CAM, JZ_OBJ_TYPE_BUTTON, true, 0, 0, 100, 100, LV_OPA_0, 0, UI_BTN_DEF_COLOR, UI_VIDEO_ICON, NULL, NULL,
     page_button_event_handler},
    {PAGE_AUDIO, JZ_OBJ_TYPE_BUTTON, true, 0, 0, 100, 100, LV_OPA_0, 0, UI_BTN_DEF_COLOR, UI_AUDIO_ICON, NULL, NULL,
     page_button_event_handler},
    {PAGE_SETTING, JZ_OBJ_TYPE_BUTTON, true, 0, 0, 100, 100, LV_OPA_0, 0, UI_BTN_DEF_COLOR, UI_SETTING_ICON, NULL, NULL,
     page_button_event_handler},
     {PAGE_MULTIOBJ_DET, JZ_OBJ_TYPE_BUTTON, true, 0, 0, 100, 100, LV_OPA_0, 0, UI_BTN_DEF_COLOR, UI_MULTI_OBJ_ICON, NULL, NULL,
        page_button_event_handler},
     {PAGE_SCANCODE, JZ_OBJ_TYPE_BUTTON, true, 0, 0, 100, 100, LV_OPA_0, 0, UI_BTN_DEF_COLOR, UI_SCANCODE_ICON, NULL, NULL,
        page_button_event_handler},
#ifdef CONFIG_APPLICATION_TIRTC_DEMO
     {PAGE_TIRTC_DEMO, JZ_OBJ_TYPE_BUTTON, true, 0, 0, 100, 100, LV_OPA_0, 0, UI_BTN_DEF_COLOR, UI_TIRTC_ICON, NULL, NULL,
        page_button_event_handler},
#endif
};

static uint16_t screen_width = UI_LCD_WIDTH;
static uint16_t screen_height = UI_LCD_HEIGHT;

// 屏幕尺寸管理函数保持不变
uint16_t get_screen_width(void)
{
    return screen_width;
}
uint16_t get_screen_height(void)
{
    return screen_height;
}

int ui_get_current_page_id(void)
{
    return current_page;
}

lv_obj_t *ui_get_page(int page_id)
{
    return pages[page_id];
}

int ui_set_page(lv_obj_t *page, int page_id)
{
    pages[page_id] = page;

    return 0;
}

void page_button_event_handler(lv_event_t *e)
{
    uint8_t *subpage = (uint8_t *)lv_event_get_user_data(e);
    printf("page_button_event_handler  \n");
    ui_switch_page(*subpage);
}

// 主页面创建函数
void creat_main_page(void)
{
    // 1. 先创建主页面容器
    pages[PAGE_MAIN] = lv_obj_create(NULL);
    lv_obj_set_size(pages[PAGE_MAIN], get_screen_width(), get_screen_height());
    lv_obj_set_style_bg_opa(pages[PAGE_MAIN], LV_OPA_COVER, 0); // 透明背景
    lv_obj_set_style_bg_color(pages[PAGE_MAIN], lv_color_hex(UI_MAIN_BG_COLOR), LV_PART_MAIN);

    // 2. 创建背景图片（确保在最底层）
    if (UI_MAIN_BG_PATH) {
        lv_obj_t *bg_img = lv_img_create(pages[PAGE_MAIN]);
        if (bg_img) {
            lv_img_set_src(bg_img, GET_UI_PATH(UI_BG_ICON));
            lv_obj_set_size(bg_img, get_screen_width(), get_screen_height());
            lv_obj_align(bg_img, LV_ALIGN_CENTER, 0, 0);
        }
    }

    // 3. 再创建状态栏（在背景图上方）
    create_status_bar(pages[PAGE_MAIN]);

    // 4. 然后创建旋转按钮（在状态栏下方，背景图上方）
    /*
    lv_obj_t *rotate_btn = lv_btn_create(pages[PAGE_MAIN]);
    lv_obj_set_size(rotate_btn, 30, 30);
    lv_obj_align(rotate_btn, LV_ALIGN_TOP_RIGHT, -5, UI_STATUS_BAR_HEIGHT + 5);
    lv_obj_set_style_radius(rotate_btn, 5, 0);
    lv_obj_set_style_bg_color(rotate_btn, lv_color_hex(0x555555), 0);

    lv_obj_t *rotate_label = lv_label_create(rotate_btn);
    lv_label_set_text(rotate_label, "↺");
    lv_obj_center(rotate_label);

    lv_obj_add_event_cb(rotate_btn, screen_set_orient, LV_EVENT_CLICKED, NULL);
    */

    // 5. 最后创建按钮容器（在所有背景之上）
    lv_obj_t *btn_container = lv_obj_create(pages[PAGE_MAIN]);
    lv_obj_align(btn_container, LV_ALIGN_DEFAULT, 0, UI_STATUS_BAR_HEIGHT);

    int btn_container_height = get_screen_height() - UI_BTN_CONTAINER_START_HEIGHT;
    lv_obj_set_size(btn_container, get_screen_width(), btn_container_height);
    lv_obj_set_style_bg_opa(btn_container, LV_OPA_0, 0);
    lv_obj_set_style_border_opa(btn_container, LV_OPA_0, 0);
    lv_obj_align(btn_container, LV_ALIGN_TOP_MID, 0, UI_STATUS_BAR_HEIGHT);

    uint8_t visible_count = 0;
    for (int i = 0; i < UI_MAIN_MAX_BUTTONS; i++) {
        if (page_btn_configs[i].visible)
            visible_count++;
    }

    uint8_t cols = UI_MAIN_GRID_COLUMNS;//current_orient == SCREEN_ORIENT_LANDSCAPE ? (visible_count > 4 ? 4 : visible_count)
                     //                                        : (visible_count > 4 ? 2 : 2);
    uint8_t rows = UI_MAIN_GRID_ROWS;//(visible_count + cols - 1) / cols;

    printf("btn_container visible_count:%d cols:%d rows:%d\n", visible_count, cols, rows);

    // 列描述符
    static lv_coord_t col_dsc[UI_MAIN_MAX_BUTTONS + 1];
    for (uint8_t i = 0; i < cols; i++) {
        col_dsc[i] = LV_GRID_FR(1);
    }
    col_dsc[cols] = LV_GRID_TEMPLATE_LAST;

    // 行描述符
    static lv_coord_t row_dsc[UI_MAIN_MAX_BUTTONS + 1];
    for (uint8_t i = 0; i < rows; i++) {
        row_dsc[i] = LV_GRID_FR(1);
    }
    row_dsc[rows] = LV_GRID_TEMPLATE_LAST;

    // 应用网格布局
    lv_obj_set_layout(btn_container, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(btn_container, col_dsc, row_dsc);
    lv_obj_set_style_pad_row(btn_container, 10, 0);
    lv_obj_set_style_pad_column(btn_container, 10, 0);

    // 创建可见按钮（最后创建，确保在最上层）
    for (int i = 0; i < UI_MAIN_MAX_BUTTONS; i++) {
        if (page_btn_configs[i].visible) {
            lv_obj_t *btn = jz_create_obj(btn_container, &page_btn_configs[i]);
            lv_obj_add_event_cb(btn, page_btn_configs[i].event_cb, LV_EVENT_CLICKED, &page_btn_configs[i].id);
            lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_CENTER, i % UI_MAIN_GRID_COLUMNS, 1, LV_GRID_ALIGN_CENTER,
                                 i / UI_MAIN_GRID_COLUMNS, 1);
        }
    }

    if (pages[PAGE_MAIN] == NULL)
        update_status_bar();

    lv_scr_load(pages[PAGE_MAIN]);
}

// 主界面回调函数
static void main_page_callback(lv_event_t *e)
{
    page_id_t page_id = (page_id_t)lv_event_get_user_data(e);
    ui_switch_page(PAGE_MAIN);
    ui_set_page(NULL, page_id);
}

// 子页面创建函数保持不变（遵循创建顺序控制层级）当前作为demo保留，后续各个子page独立写一个独自的
static void create_sub_page(page_id_t page_id)
{
    pages[page_id] = lv_scr_act();
    lv_obj_clean(pages[page_id]);

    ui_set_page(NULL, PAGE_MAIN);

    lv_obj_set_size(pages[page_id], get_screen_width(), get_screen_height() - UI_STATUS_BAR_HEIGHT);
    lv_obj_set_style_bg_color(pages[page_id], lv_color_hex(0xf0f0f0), 0);

    create_status_bar(pages[page_id]);

    const char *titles[PAGE_COUNT] = {
        "",
        "页面1",
        "音频",
        "设置",
        "目标检测",
        "扫码",
#ifdef CONFIG_APPLICATION_TIRTC_DEMO
        "TiRTC Demo",
#endif
    };

    lv_obj_t *title = lv_label_create(pages[page_id]);
    lv_label_set_text(title, titles[page_id]);
    lv_obj_set_style_text_font(title, &jz_ui_font_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, UI_STATUS_BAR_HEIGHT + 20);

    lv_obj_t *back_btn = lv_btn_create(pages[page_id]);
    lv_obj_set_size(back_btn, 80, 40);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_radius(back_btn, 5, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x3b72c8), 0);

    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, UI_GENERAL_BACK);
    lv_obj_set_style_text_font(back_label, &jz_ui_font_16, 0);
    lv_obj_set_style_text_color(back_label, lv_color_hex(0xffffff), 0);
    lv_obj_center(back_label);

    lv_obj_add_event_cb(back_btn, main_page_callback, LV_EVENT_PRESSED, (void *)page_id);

    lv_obj_t *content = lv_obj_create(pages[page_id]);
    lv_obj_set_size(content, get_screen_width() - 40, get_screen_height() - UI_STATUS_BAR_HEIGHT - 120);
    lv_obj_set_style_bg_color(content, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_radius(content, 8, 0);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, UI_STATUS_BAR_HEIGHT + 60);

    lv_obj_t *content_label = lv_label_create(content);
    lv_label_set_text_fmt(content_label, UI_GENERAL_UNSUPPORTED);
    lv_obj_set_style_text_font(content_label, &jz_ui_font_16, 0);
    lv_obj_align(content_label, LV_ALIGN_CENTER, 0, 0);

    if (pages[PAGE_MAIN] == NULL)
        update_status_bar();
}

// 屏幕方向切换函数保持不变
static void screen_set_orient(screen_orient_t orient)
{
    int width;
    int height;

    if (current_orient == orient)
        return;

    uint16_t temp = get_screen_width();
    width = get_screen_height();
    height = temp;
    current_orient = orient;

    lv_disp_t *disp = lv_disp_get_default();
    if (disp) {
        disp->driver->hor_res = width;
        disp->driver->ver_res = height;
        lv_disp_set_rotation(disp, orient == SCREEN_ORIENT_LANDSCAPE ? LV_DISP_ROT_90 : LV_DISP_ROT_NONE);
    }

    for (int i = 0; i < PAGE_COUNT; i++) {
        if (pages[i]) {
            lv_obj_del(pages[i]);
            pages[i] = NULL;
        }
    }

    if (current_page == PAGE_MAIN) {
        creat_main_page();
    } else {
        create_sub_page(current_page);
    }
    lv_scr_load(pages[current_page]);
    // lcd_hw_set_orientation(orient);
}

// 页面切换函数保持不变
void ui_switch_page(page_id_t page_id)
{
    int res = 0;
    if (page_id >= PAGE_COUNT)
        return;

    if (pages[page_id] == NULL) {
        if (page_id == PAGE_MAIN)
            creat_main_page();
        else if (page_id == PAGE_CAM) {
            #ifdef CONFIG_APPLICATION_IVS_FACE
                create_video_page(page_id);
            #endif
        } else if (page_id == PAGE_SETTING) {
#ifdef CONFIG_APPLICATION_SETTING_PAGE
            create_setting_page(page_id);
#else
            create_sub_page(page_id);
#endif
        } else if (page_id == PAGE_AUDIO) {
#ifdef CONFIG_APPLICATION_AUDIO
            res = create_audio_page(page_id);
#else
            create_sub_page(page_id);
#endif
        } else if (page_id == PAGE_MULTIOBJ_DET) {
#ifdef CONFIG_APPLICATION_MULTI_OBJ_DET
            create_multi_obj_page(page_id);
#else
            create_sub_page(page_id);
#endif
        } else if (page_id == PAGE_SCANCODE) {
#ifdef CONFIG_APPLICATION_SCANCODE
            create_scancode_page(page_id);
#else
            create_sub_page(page_id);
#endif
#ifdef CONFIG_APPLICATION_TIRTC_DEMO
        } else if (page_id == PAGE_TIRTC_DEMO) {
            res = create_tirtc_demo_page(page_id);
#endif
        } else
            create_sub_page(page_id);
    }

    if (res == 0) {
        lv_scr_load(pages[page_id]);
        lv_obj_invalidate(pages[page_id]);
        if (pages[current_page] != NULL) {
            lv_obj_del(pages[current_page]);
        }
        current_page = page_id;
    }
}

static void ui_fonts_init(void)
{
    jz_font_12 = lv_font_load(UI_FONT_12);
    if (jz_font_12 == NULL) {
        jz_font_12 = (lv_font_t *)&lv_font_montserrat_14;
    }

    jz_font_16 = lv_font_load(UI_FONT_16);
    if (jz_font_16 == NULL) {
        jz_font_16 = (lv_font_t *)&lv_font_montserrat_14;
    }

    jz_font_20 = lv_font_load(UI_FONT_20);
    if (jz_font_20 == NULL) {
        jz_font_20 = (lv_font_t *)&lv_font_montserrat_14;
    }

    jz_ui_font_12 = *jz_font_12;
    jz_ui_font_16 = *jz_font_16;
    jz_ui_font_20 = *jz_font_20;
}

static void ui_fonts_free(void)
{
    lv_font_free(jz_font_12);
    lv_font_free(jz_font_16);
    lv_font_free(jz_font_20);
}

void ui_main(void)
{
    ui_fonts_init();
    creat_main_page();
}
