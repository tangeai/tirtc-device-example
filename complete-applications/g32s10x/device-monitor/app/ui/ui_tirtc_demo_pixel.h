#ifndef _UI_TIRTC_DEMO_PIXEL_H
#define _UI_TIRTC_DEMO_PIXEL_H

#include "tirtc_demo_embedded_assets.h"
#include "ai_chat_avatar_assets.h"
#include "tirtc_demo_cloud.h"
#if TIRTC_DEMO_USE_S3_TEXT_IMAGES
#include "text_assets.h"
#include "tirtc_demo_s3_text_extra.h"
#endif

LV_FONT_DECLARE(lv_font_tirtc_s3_18)

#define TIRTC_DEMO_PIXEL_ASSET(name) "S:/tirtc/pic/" name

#if TIRTC_DEMO_EMBED_S3_ASSETS
#define TIRTC_DEMO_PIXEL_SOURCE(symbol, name) (&(symbol))
#else
#define TIRTC_DEMO_PIXEL_SOURCE(symbol, name) TIRTC_DEMO_PIXEL_ASSET(name)
#endif

#define PIXEL_BG_COLOR 0xE8F3FA
#define PIXEL_HOME_BG_COLOR 0xF3F8FB
#define PIXEL_HEADER_COLOR 0xF7FBFF
#define PIXEL_CARD_COLOR 0xFFFFFF
#define PIXEL_BORDER_COLOR 0xD5E0EB
#define PIXEL_TEXT_COLOR 0x10243E
#define PIXEL_MUTED_COLOR 0x64758A
#define PIXEL_BLUE_COLOR 0x2F82D7
#define PIXEL_GREEN_COLOR 0x21C783
#define PIXEL_RED_COLOR 0xF15A5A

static lv_obj_t *g_pixel_canvas;
static lv_obj_t *g_pixel_home_time_label;
static lv_obj_t *g_pixel_wifi_bars[3];
static bool g_pixel_home_second_page;
static bool g_pixel_resources_initialized;
static bool g_pixel_resources_valid;
static lv_font_t g_pixel_s3_font;

static void tirtc_demo_pixel_child_back_callback(lv_event_t *e);
static void tirtc_demo_pixel_test_row(int y, const char *label,
                                      bool passed, bool running);

#if TIRTC_DEMO_USE_S3_TEXT_IMAGES
static const ui_text_asset_t *tirtc_demo_pixel_text_asset_find(
    const char *text, uint8_t size)
{
    const ui_text_asset_t *asset = ui_text_asset_find(text, size);

    return asset != NULL ? asset : ui_g32_text_asset_find(text, size);
}
#endif

static void tirtc_demo_pixel_resources_init(void)
{
    bool image_info_ok = true;
    bool image_decode_ok = false;
    bool font_ok = false;
    bool fallback_font_ok = false;
    bool text_image_ok = false;
    lv_img_decoder_dsc_t decoder;
    lv_img_header_t header;
    lv_font_glyph_dsc_t glyph;

    if (g_pixel_resources_initialized) {
        return;
    }
    g_pixel_resources_initialized = true;

#if TIRTC_DEMO_USE_S3_FONT
    g_pixel_s3_font = lv_font_tirtc_s3_18;
    g_pixel_s3_font.fallback = &jz_ui_font_16;
    font_ok = lv_font_get_glyph_dsc(&g_pixel_s3_font, &glyph, 0x8BBEU, 0U) &&
              !glyph.is_placeholder;
    fallback_font_ok =
        lv_font_get_glyph_dsc(&jz_ui_font_16, &glyph, 0x4F60U, 0U) &&
        !glyph.is_placeholder;
#else
    font_ok = true;
    fallback_font_ok = true;
#endif

#if TIRTC_DEMO_EMBED_S3_ASSETS
    for (size_t i = 0U; i < tirtc_demo_embedded_asset_count; ++i) {
        if (lv_img_decoder_get_info(tirtc_demo_embedded_assets[i].image,
                                    &header) != LV_RES_OK ||
            header.w == 0 || header.h == 0) {
            printf("[tirtc_ui] image info failed: %s\n",
                   tirtc_demo_embedded_assets[i].name);
            image_info_ok = false;
        }
    }
    memset(&decoder, 0, sizeof(decoder));
    if (lv_img_decoder_open(&decoder, &tirtc_s3_home_icon_view_img,
                            lv_color_white(), 0) == LV_RES_OK) {
        image_decode_ok = decoder.img_data != NULL;
        lv_img_decoder_close(&decoder);
    }
#else
    image_decode_ok = true;
#endif

#if TIRTC_DEMO_USE_S3_TEXT_IMAGES
    text_image_ok = tirtc_demo_pixel_text_asset_find("设备绑定", 16U) != NULL &&
                    tirtc_demo_pixel_text_asset_find("TiRTC 配置", 16U) != NULL &&
                    tirtc_demo_pixel_text_asset_find("微信通话", 16U) != NULL &&
                    tirtc_demo_pixel_text_asset_find("服务地址", 12U) != NULL;
#else
    text_image_ok = true;
#endif

    g_pixel_resources_valid = image_info_ok && image_decode_ok && font_ok &&
                              fallback_font_ok && text_image_ok;
    printf("[tirtc_ui] resources images=%u/%u decoder=%s "
           "text=%s font=%s fallback=%s status=%s\n",
           image_info_ok ? (unsigned)tirtc_demo_embedded_asset_count : 0U,
           (unsigned)tirtc_demo_embedded_asset_count,
           image_decode_ok ? "ok" : "failed",
#if TIRTC_DEMO_USE_S3_TEXT_IMAGES
           text_image_ok ? "s3-runtime-image" : "failed",
#else
           "font",
#endif
#if TIRTC_DEMO_USE_S3_FONT
           font_ok ? "s3-simhei-18" : "failed",
#else
           "vendor",
#endif
           fallback_font_ok ? "harmony16" : "failed",
           g_pixel_resources_valid ? "ok" : "failed");
}

static bool tirtc_demo_pixel_text_has_utf8(const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;

    if (cursor == NULL) {
        return false;
    }
    while (*cursor != '\0') {
        if ((*cursor & 0x80U) != 0U) {
            return true;
        }
        ++cursor;
    }
    return false;
}

static const lv_font_t *tirtc_demo_pixel_font(const char *text, uint8_t size)
{
#if TIRTC_DEMO_USE_S3_FONT
    if (tirtc_demo_pixel_text_has_utf8(text) &&
        g_pixel_resources_initialized) {
        return &g_pixel_s3_font;
    }
#endif
    if (size <= 12U) {
        return &lv_font_montserrat_12;
    }
    if (size >= 18U) {
        return &lv_font_montserrat_20;
    }
    return size >= 16U ? &lv_font_montserrat_16 : &lv_font_montserrat_14;
}

#if TIRTC_DEMO_USE_S3_TEXT_IMAGES
static lv_obj_t *tirtc_demo_pixel_text_image(
    lv_obj_t *parent, const ui_text_asset_t *asset, int x, int y, int width,
    uint32_t color, lv_text_align_t align)
{
    lv_obj_t *image;
    int image_x = x;
    int image_y;
    int image_width;
    int image_height;

    if (asset == NULL || asset->image == NULL) {
        return NULL;
    }

    image_width = (int)asset->image->header.w;
    image_height = (int)asset->image->header.h;
    image_y = y + asset->y_offset;
    if (align == LV_TEXT_ALIGN_CENTER) {
        image_x += (width - image_width) / 2;
    } else if (align == LV_TEXT_ALIGN_RIGHT) {
        image_x += width - image_width;
    } else {
        image_x += asset->x_offset;
    }

    image = lv_img_create(parent);
    lv_img_set_src(image, asset->image);
    lv_obj_set_pos(image, image_x, image_y);
    lv_obj_set_size(image, image_width, image_height);
    lv_obj_set_style_img_recolor(image, lv_color_hex(color), 0);
    lv_obj_set_style_img_recolor_opa(image, LV_OPA_COVER, 0);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return image;
}
#endif

static lv_obj_t *tirtc_demo_pixel_dynamic_label(
    lv_obj_t *parent, const char *text, int x, int y, int width,
    uint32_t color, uint8_t size, lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, text != NULL ? text : "");
    lv_obj_set_style_text_font(label, tirtc_demo_pixel_font(text, size), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    return label;
}

static lv_obj_t *tirtc_demo_pixel_label(lv_obj_t *parent, const char *text,
                                        int x, int y, int width,
                                        uint32_t color, uint8_t size,
                                        lv_text_align_t align)
{
#if TIRTC_DEMO_USE_S3_TEXT_IMAGES
    const ui_text_asset_t *asset = tirtc_demo_pixel_text_asset_find(text, size);

    if (asset != NULL) {
        return tirtc_demo_pixel_text_image(parent, asset, x, y, width, color,
                                            align);
    }
#endif

    return tirtc_demo_pixel_dynamic_label(parent, text, x, y, width, color,
                                           size, align);
}

static lv_obj_t *tirtc_demo_pixel_box(lv_obj_t *parent, int x, int y,
                                      int width, int height, uint32_t bg,
                                      uint32_t border, int radius)
{
    lv_obj_t *box = lv_obj_create(parent);

    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, width, height);
    lv_obj_set_style_radius(box, radius, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, border == bg ? 0 : 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(border), 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    return box;
}

static lv_obj_t *tirtc_demo_pixel_button(lv_obj_t *parent, int x, int y,
                                         int width, int height,
                                         uint32_t bg, uint32_t text_color,
                                         const char *text,
                                         lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_t *label;

    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, height <= 28 ? 5 : 8, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(button, lv_color_darken(lv_color_hex(bg), 24),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    if (cb != NULL) {
        lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user_data);
    }

    label = tirtc_demo_pixel_label(button, text, 2, 0, width - 4,
                                   text_color, height >= 34 ? 16 : 12,
                                   LV_TEXT_ALIGN_CENTER);
    lv_obj_center(label);

    return button;
}

static lv_obj_t *tirtc_demo_pixel_image(lv_obj_t *parent, const void *source,
                                        int x, int y)
{
    lv_obj_t *image = lv_img_create(parent);

    lv_img_set_src(image, source);
    lv_obj_set_pos(image, x, y);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    return image;
}

static lv_obj_t *tirtc_demo_pixel_qrcode(lv_obj_t *parent,
                                         const char *payload, int size)
{
    lv_obj_t *qrcode;

    if (payload == NULL || payload[0] == '\0' ||
        strcmp(payload, TIRTC_DEMO_UNCONFIGURED_TEXT) == 0) {
        return NULL;
    }
    qrcode = lv_qrcode_create(parent, size, lv_color_hex(0x10233B),
                              lv_color_hex(0xFFFFFF));
    if (qrcode == NULL ||
        lv_qrcode_update(qrcode, payload, (uint32_t)strlen(payload)) != LV_RES_OK) {
        if (qrcode != NULL) {
            lv_obj_del(qrcode);
        }
        return NULL;
    }
    lv_obj_clear_flag(qrcode, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return qrcode;
}

static void tirtc_demo_pixel_signal(lv_obj_t *parent, bool connected)
{
    static const uint8_t heights[3] = {6U, 10U, 14U};

    for (uint8_t i = 0U; i < 3U; ++i) {
        lv_obj_t *bar = tirtc_demo_pixel_box(parent, 294 + ((int)i * 7),
                                             22 - heights[i], 4, heights[i],
                                             connected ? 0x20BF7A : 0xBCCAD8,
                                             connected ? 0x20BF7A : 0xBCCAD8, 2);
        g_pixel_wifi_bars[i] = bar;
    }
    if (!connected) {
        tirtc_demo_pixel_label(parent, "x", 309, 2, 10, PIXEL_RED_COLOR, 12,
                               LV_TEXT_ALIGN_CENTER);
    }
}

static void tirtc_demo_pixel_header(const char *title, lv_event_cb_t back_cb,
                                    const char *action_text,
                                    lv_event_cb_t action_cb)
{
    tirtc_demo_runtime_t runtime;
    lv_obj_t *header = tirtc_demo_pixel_box(g_pixel_canvas, 0, 0, 320, 28,
                                            PIXEL_HEADER_COLOR,
                                            PIXEL_BORDER_COLOR, 0);

    if (back_cb != NULL) {
        lv_obj_t *back = tirtc_demo_pixel_button(header, 0, 0, 66, 28,
                                                 PIXEL_HEADER_COLOR,
                                                 PIXEL_TEXT_COLOR, "<",
                                                 back_cb, NULL);
        lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    }
    tirtc_demo_pixel_label(header, title, 66, 5, 188, PIXEL_TEXT_COLOR, 16,
                           LV_TEXT_ALIGN_CENTER);
    if (action_text != NULL) {
#if TIRTC_DEMO_EMBED_S3_ASSETS
        if (strcmp(action_text, "刷新") == 0) {
            lv_obj_t *refresh = lv_imgbtn_create(header);

            lv_obj_set_pos(refresh, 262, 3);
            lv_obj_set_size(refresh, 50, 22);
            lv_imgbtn_set_src(refresh, LV_IMGBTN_STATE_RELEASED, NULL,
                              &tirtc_s3_call_contacts_refresh_default_img,
                              NULL);
            lv_imgbtn_set_src(refresh, LV_IMGBTN_STATE_PRESSED, NULL,
                              &tirtc_s3_call_contacts_refresh_pressed_img,
                              NULL);
            lv_obj_add_event_cb(refresh, action_cb, LV_EVENT_CLICKED, NULL);
        } else
#endif
        {
            uint32_t action_color =
                strcmp(action_text, "开始") == 0 ||
                strcmp(action_text, "重测") == 0 ?
                    PIXEL_BLUE_COLOR : PIXEL_GREEN_COLOR;

            tirtc_demo_pixel_button(header, 262, 3, 50, 22,
                                    action_color, 0xFFFFFF,
                                    action_text, action_cb, NULL);
        }
    } else {
        tirtc_demo_app_get_runtime(&runtime);
        tirtc_demo_pixel_signal(header, runtime.network_ready);
    }
}

static void tirtc_demo_pixel_prepare_canvas(void)
{
    lv_obj_remove_style_all(g_page);
    lv_obj_set_size(g_page, get_screen_width(), get_screen_height());
    lv_obj_set_style_bg_color(g_page, lv_color_hex(0x17212B), 0);
    lv_obj_set_style_bg_opa(g_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_page, LV_OBJ_FLAG_SCROLLABLE);

    g_pixel_canvas = tirtc_demo_pixel_box(g_page, 0, 0, 320, 240,
                                          PIXEL_BG_COLOR, PIXEL_BG_COLOR, 0);
    lv_obj_align(g_pixel_canvas, LV_ALIGN_CENTER, 0, 0);
    g_pixel_home_time_label = NULL;
    memset(g_pixel_wifi_bars, 0, sizeof(g_pixel_wifi_bars));
}

static void tirtc_demo_pixel_refresh(void)
{
    tirtc_demo_runtime_t runtime;
    char time_text[9] = "--:--:--";
    struct rtc_time rtc;

    if (g_pixel_home_time_label != NULL &&
        lv_obj_is_valid(g_pixel_home_time_label)) {
        rtc_get_current_tm(&rtc);
        if (rtc_valid_tm(&rtc) == 0) {
            snprintf(time_text, sizeof(time_text), "%02d:%02d:%02d",
                     rtc.tm_hour, rtc.tm_min, rtc.tm_sec);
        }
        lv_label_set_text(g_pixel_home_time_label, time_text);
    }

    tirtc_demo_app_get_runtime(&runtime);
    for (uint8_t i = 0U; i < 3U; ++i) {
        if (g_pixel_wifi_bars[i] != NULL && lv_obj_is_valid(g_pixel_wifi_bars[i])) {
            bool active = runtime.network_ready &&
                          (runtime.network_rssi >= -80 + ((int)i * 10));
            lv_obj_set_style_bg_color(g_pixel_wifi_bars[i],
                                      lv_color_hex(active ? 0x20BF7A : 0xBCCAD8), 0);
        }
    }
}

static void tirtc_demo_pixel_home_page_callback(lv_event_t *e)
{
    g_pixel_home_second_page = (bool)(uintptr_t)lv_event_get_user_data(e);
    tirtc_demo_render_view(TIRTC_DEMO_VIEW_HOME);
}

static void tirtc_demo_pixel_home_tile(lv_obj_t *parent, int x,
                                       const void *icon_source,
                                       const void *title_source, int title_width,
                                       const void *subtitle_source,
                                       int subtitle_width,
                                       uint32_t accent, tirtc_demo_view_t view)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_t *bar;
    lv_obj_t *icon;

    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button, x, 8);
    lv_obj_set_size(button, 96, 172);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xF7FBFF), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(PIXEL_BORDER_COLOR), 0);
    lv_obj_set_style_translate_y(button, 1, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(button, tirtc_demo_view_callback, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)view);

    bar = tirtc_demo_pixel_box(button, 7, 18, 82, 5, accent, accent, 8);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    icon = tirtc_demo_pixel_image(button, icon_source, 22, 40);
    lv_img_set_pivot(icon, 26, 26);
    lv_img_set_zoom(icon, 192);
    tirtc_demo_pixel_image(button, title_source,
                           7 + ((82 - title_width) / 2), 102);
    tirtc_demo_pixel_image(button, subtitle_source,
                           7 + ((82 - subtitle_width) / 2), 130);
}

static void tirtc_demo_pixel_binding_overlay(const tirtc_demo_runtime_t *runtime)
{
    lv_obj_t *overlay;
    lv_obj_t *dialog;
    lv_obj_t *panel;
    lv_obj_t *icon;
    lv_obj_t *code_panel;
    lv_obj_t *code_label;
    lv_obj_t *qr_card;
    lv_obj_t *qrcode;
    lv_obj_t *button;
    const char *code;

    if (runtime->identity_ready) {
        return;
    }
    overlay = tirtc_demo_pixel_box(g_pixel_canvas, 0, 0, 320, 240,
                                   0x10243E, 0x10243E, 0);
    lv_obj_set_style_bg_opa(overlay, 82, 0);
    if (runtime->network_ready) {
        dialog = tirtc_demo_pixel_box(overlay, 22, 32, 276, 176,
                                      PIXEL_CARD_COLOR, PIXEL_BORDER_COLOR, 8);
        tirtc_demo_pixel_label(dialog, "设备绑定", 0, 8, 276,
                               PIXEL_TEXT_COLOR, 16, LV_TEXT_ALIGN_CENTER);
        code_panel = tirtc_demo_pixel_box(dialog, 15, 33, 136, 50,
                                          PIXEL_HEADER_COLOR,
                                          PIXEL_BORDER_COLOR, 6);
        tirtc_demo_pixel_label(code_panel, "绑定码", 0, 6, 136,
                               PIXEL_MUTED_COLOR, 10, LV_TEXT_ALIGN_CENTER);
        code = runtime->binding_waiting && runtime->binding_code != NULL &&
                       runtime->binding_code[0] != '\0' ?
                   runtime->binding_code : "------";
        code_label = tirtc_demo_pixel_dynamic_label(
            code_panel, code, 0, 19, 136, PIXEL_TEXT_COLOR, 20,
            LV_TEXT_ALIGN_CENTER);
        lv_obj_set_style_text_letter_space(code_label, 1, 0);
        tirtc_demo_pixel_label(dialog, "绑定网址", 15, 89, 136,
                               PIXEL_MUTED_COLOR, 10, LV_TEXT_ALIGN_LEFT);
        tirtc_demo_pixel_dynamic_label(dialog, TIRTC_DEMO_CLOUD_API_BASE,
                                       15, 104, 138, PIXEL_TEXT_COLOR, 8,
                                       LV_TEXT_ALIGN_LEFT);
        qr_card = tirtc_demo_pixel_box(dialog, 158, 30, 101, 101,
                                       PIXEL_CARD_COLOR,
                                       PIXEL_BORDER_COLOR, 5);
        qrcode = tirtc_demo_pixel_qrcode(qr_card,
                                         TIRTC_DEMO_CLOUD_API_BASE, 94);
        if (qrcode != NULL) {
            lv_obj_set_pos(qrcode, 3, 3);
        }
        button = tirtc_demo_pixel_button(dialog, 15, 147, 244, 22,
                                         PIXEL_CARD_COLOR,
                                         PIXEL_TEXT_COLOR, "刷新",
                                         tirtc_demo_binding_callback, NULL);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button,
                                      lv_color_hex(PIXEL_BORDER_COLOR), 0);
    } else {
        dialog = tirtc_demo_pixel_box(overlay, 24, 46, 272, 148,
                                      PIXEL_CARD_COLOR, PIXEL_BORDER_COLOR, 8);
        tirtc_demo_pixel_label(dialog, "设备绑定", 0, 11, 272,
                               PIXEL_TEXT_COLOR, 16, LV_TEXT_ALIGN_CENTER);
        panel = tirtc_demo_pixel_box(dialog, 15, 41, 240, 48,
                                     0xFFF1F1, 0xFFAEAE, 6);
        icon = tirtc_demo_pixel_box(panel, 17, 12, 22, 22,
                                    0xE41C1C, 0xE41C1C, 11);
        code_label = tirtc_demo_pixel_dynamic_label(icon, "!", 0, 2, 22,
                                                     0xFFFFFF, 14,
                                                     LV_TEXT_ALIGN_CENTER);
        lv_obj_center(code_label);
        tirtc_demo_pixel_label(panel, "请先连接 WiFi", 47, 7, 170,
                               PIXEL_TEXT_COLOR, 14, LV_TEXT_ALIGN_LEFT);
        tirtc_demo_pixel_label(panel, "联网后自动获取6位绑定码",
                               47, 27, 180, PIXEL_MUTED_COLOR, 10,
                               LV_TEXT_ALIGN_LEFT);
        tirtc_demo_pixel_button(dialog, 15, 107, 240, 28,
                                PIXEL_GREEN_COLOR, 0xFFFFFF, "设置WiFi",
                                tirtc_demo_vendor_page_callback,
                                (void *)(uintptr_t)TIRTC_DEMO_VENDOR_SETTINGS);
    }
}

static void tirtc_demo_pixel_create_home(void)
{
    tirtc_demo_runtime_t runtime;
    lv_obj_t *header;
    lv_obj_t *content;
    lv_obj_t *track;
    lv_obj_t *exit_hit;

    lv_obj_set_style_bg_color(g_pixel_canvas, lv_color_hex(PIXEL_HOME_BG_COLOR), 0);
    header = tirtc_demo_pixel_box(g_pixel_canvas, 0, 0, 320, 28,
                                  PIXEL_HEADER_COLOR, PIXEL_BORDER_COLOR, 0);
    g_pixel_home_time_label = tirtc_demo_pixel_dynamic_label(
        header, "--:--:--", 8, 6, 58, PIXEL_TEXT_COLOR, 12,
        LV_TEXT_ALIGN_LEFT);
    tirtc_demo_pixel_image(header,
                           TIRTC_DEMO_PIXEL_SOURCE(tirtc_s3_home_text_title_img,
                                                   "home_text_title.png"),
                           130, 6);
    tirtc_demo_app_get_runtime(&runtime);
    tirtc_demo_pixel_signal(header, runtime.network_ready);
    exit_hit = lv_btn_create(header);
    lv_obj_remove_style_all(exit_hit);
    lv_obj_set_pos(exit_hit, 0, 0);
    lv_obj_set_size(exit_hit, 66, 28);
    lv_obj_set_style_bg_opa(exit_hit, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(exit_hit, tirtc_demo_back_to_main_callback,
                        LV_EVENT_LONG_PRESSED, NULL);

    content = tirtc_demo_pixel_box(g_pixel_canvas, 0, 28, 320, 212,
                                   PIXEL_HOME_BG_COLOR, PIXEL_HOME_BG_COLOR, 0);
    if (!g_pixel_home_second_page) {
        tirtc_demo_pixel_home_tile(content, 8,
            TIRTC_DEMO_PIXEL_SOURCE(tirtc_s3_home_icon_view_img,
                                    "home_icon_view.png"),
            TIRTC_DEMO_PIXEL_SOURCE(tirtc_s3_home_text_view_img,
                                    "home_text_view.png"), 35,
            TIRTC_DEMO_PIXEL_SOURCE(tirtc_s3_home_text_view_desc_img,
                                    "home_text_view_desc.png"), 48,
            0xF6494C, TIRTC_DEMO_VIEW_DETAIL);
        tirtc_demo_pixel_home_tile(content, 112,
            TIRTC_DEMO_PIXEL_SOURCE(tirtc_s3_home_icon_call_img,
                                    "home_icon_call.png"),
            TIRTC_DEMO_PIXEL_SOURCE(tirtc_s3_home_text_call_img,
                                    "home_text_call.png"), 34,
            TIRTC_DEMO_PIXEL_SOURCE(tirtc_s3_home_text_call_desc_img,
                                    "home_text_call_desc.png"), 53,
            0x1296DB, TIRTC_DEMO_VIEW_CALL);
        tirtc_demo_pixel_home_tile(content, 216,
            TIRTC_DEMO_PIXEL_SOURCE(tirtc_s3_home_icon_wechat_img,
                                    "home_icon_wechat.png"),
            TIRTC_DEMO_PIXEL_SOURCE(tirtc_s3_home_text_wechat_img,
                                    "home_text_wechat.png"), 71,
            TIRTC_DEMO_PIXEL_SOURCE(tirtc_s3_home_text_wechat_desc_img,
                                    "home_text_wechat_desc.png"), 52,
            0x24DB5A, TIRTC_DEMO_VIEW_WECHAT);
    } else {
        tirtc_demo_pixel_home_tile(content, 8,
            TIRTC_DEMO_PIXEL_SOURCE(tirtc_s3_home_icon_ai_img,
                                    "home_icon_ai.png"),
            TIRTC_DEMO_PIXEL_SOURCE(tirtc_s3_home_text_ai_img,
                                    "home_text_ai.png"), 54,
            TIRTC_DEMO_PIXEL_SOURCE(tirtc_s3_home_text_ai_desc_img,
                                    "home_text_ai_desc.png"), 76,
            0x009D9A, TIRTC_DEMO_VIEW_AI_TALK);
        tirtc_demo_pixel_home_tile(content, 112,
            TIRTC_DEMO_PIXEL_SOURCE(tirtc_s3_home_icon_settings_img,
                                    "home_icon_settings.png"),
            TIRTC_DEMO_PIXEL_SOURCE(tirtc_s3_home_text_settings_img,
                                    "home_text_settings.png"), 35,
            TIRTC_DEMO_PIXEL_SOURCE(tirtc_s3_home_text_settings_desc_img,
                                    "home_text_settings_desc.png"), 74,
            0x64758A, TIRTC_DEMO_VIEW_SYSTEM);
    }

    track = tirtc_demo_pixel_box(g_pixel_canvas, 142, 220, 37, 8,
                                 PIXEL_HOME_BG_COLOR, PIXEL_HOME_BG_COLOR, 0);
    tirtc_demo_pixel_box(track, 0, 0, g_pixel_home_second_page ? 8 : 22, 8,
                         g_pixel_home_second_page ? 0xC7D5E3 : 0x1768B7,
                         g_pixel_home_second_page ? 0xC7D5E3 : 0x1768B7, 8);
    tirtc_demo_pixel_box(track, g_pixel_home_second_page ? 15 : 29, 0,
                         g_pixel_home_second_page ? 22 : 8, 8,
                         g_pixel_home_second_page ? 0x1768B7 : 0xC7D5E3,
                         g_pixel_home_second_page ? 0x1768B7 : 0xC7D5E3, 8);
    if (g_pixel_home_second_page) {
        tirtc_demo_pixel_button(g_pixel_canvas, 0, 190, 50, 50,
                                PIXEL_HOME_BG_COLOR, 0x1768B7, "<",
                                tirtc_demo_pixel_home_page_callback,
                                (void *)(uintptr_t)false);
    } else {
        tirtc_demo_pixel_button(g_pixel_canvas, 270, 190, 50, 50,
                                PIXEL_HOME_BG_COLOR, 0x1768B7, ">",
                                tirtc_demo_pixel_home_page_callback,
                                (void *)(uintptr_t)true);
    }
    tirtc_demo_pixel_refresh();
    tirtc_demo_pixel_binding_overlay(&runtime);
}

static void tirtc_demo_pixel_volume_row(lv_obj_t *parent, int y,
                                        const char *title,
                                        tirtc_demo_volume_t volume,
                                        tirtc_demo_volume_action_t down,
                                        tirtc_demo_volume_action_t up,
                                        tirtc_demo_volume_action_t mute)
{
    int value = tirtc_demo_app_get_volume(volume);
    bool muted = tirtc_demo_app_is_muted(volume);
    char value_text[8];
    lv_obj_t *row = tirtc_demo_pixel_box(parent, 8, y, 142, 66,
                                         PIXEL_CARD_COLOR, PIXEL_BORDER_COLOR, 8);

    snprintf(value_text, sizeof(value_text), "%d", value);
    tirtc_demo_pixel_label(row, title, 9, 7, 78, PIXEL_TEXT_COLOR, 12,
                           LV_TEXT_ALIGN_LEFT);
    tirtc_demo_pixel_label(row, value_text, 94, 7, 38,
                           muted ? PIXEL_RED_COLOR : PIXEL_GREEN_COLOR, 12,
                           LV_TEXT_ALIGN_RIGHT);
    tirtc_demo_pixel_button(row, 8, 32, 30, 26, 0xE9F5FF,
                            PIXEL_BLUE_COLOR, "-", tirtc_demo_volume_callback,
                            (void *)(uintptr_t)down);
    tirtc_demo_pixel_button(row, 43, 32, 30, 26, 0xE9F5FF,
                            PIXEL_BLUE_COLOR, "+", tirtc_demo_volume_callback,
                            (void *)(uintptr_t)up);
    tirtc_demo_pixel_button(row, 78, 32, 56, 26,
                            muted ? 0xE5FAF0 : 0xFFE7E7,
                            muted ? 0x0D8A59 : 0xE44747,
                            muted ? "开麦" : "静音",
                            tirtc_demo_volume_callback,
                            (void *)(uintptr_t)mute);
}

static void tirtc_demo_pixel_create_detail(void)
{
    tirtc_demo_runtime_t runtime;
    char device_id[TIRTC_DEMO_DEVICE_ID_MAX];
    lv_obj_t *status_card;
    lv_obj_t *qr_card;
    lv_obj_t *qr;

    tirtc_demo_app_get_runtime(&runtime);
    tirtc_demo_app_copy_device_id(device_id, sizeof(device_id));
    tirtc_demo_pixel_header("查看", tirtc_demo_back_to_home_callback, NULL, NULL);
    status_card = tirtc_demo_pixel_box(g_pixel_canvas, 8, 36, 142, 48,
                                       PIXEL_CARD_COLOR, PIXEL_BORDER_COLOR, 8);
    tirtc_demo_pixel_label(status_card, "连接状态", 8, 5, 66,
                           PIXEL_MUTED_COLOR, 12, LV_TEXT_ALIGN_LEFT);
    tirtc_demo_pixel_label(status_card,
                           runtime.peer_connected ? "已连接" : "未连接",
                           70, 5, 64,
                           runtime.peer_connected ? PIXEL_GREEN_COLOR : 0xF59E0B,
                           12, LV_TEXT_ALIGN_RIGHT);
    tirtc_demo_pixel_label(status_card, "开门指示", 8, 25, 66,
                           PIXEL_MUTED_COLOR, 12, LV_TEXT_ALIGN_LEFT);
    tirtc_demo_pixel_label(status_card, runtime.door_open ? "已开门" : "未开门",
                           70, 25, 64,
                           runtime.door_open ? PIXEL_GREEN_COLOR : 0xF59E0B,
                           12, LV_TEXT_ALIGN_RIGHT);
    tirtc_demo_pixel_volume_row(g_pixel_canvas, 91, "扬声器音量",
                                TIRTC_DEMO_VOLUME_RX, TIRTC_DEMO_VOL_RX_DOWN,
                                TIRTC_DEMO_VOL_RX_UP, TIRTC_DEMO_VOL_RX_MUTE);
    tirtc_demo_pixel_volume_row(g_pixel_canvas, 165, "麦克风音量",
                                TIRTC_DEMO_VOLUME_TX, TIRTC_DEMO_VOL_TX_DOWN,
                                TIRTC_DEMO_VOL_TX_UP, TIRTC_DEMO_VOL_TX_MUTE);
    qr_card = tirtc_demo_pixel_box(g_pixel_canvas, 158, 36, 154, 176,
                                   PIXEL_CARD_COLOR, PIXEL_BORDER_COLOR, 8);
    qr = tirtc_demo_pixel_qrcode(qr_card, device_id, 150);
    if (qr != NULL) {
        lv_obj_set_pos(qr, 2, 6);
    }
    tirtc_demo_pixel_label(qr_card, device_id, 4, 158, 146,
                           PIXEL_MUTED_COLOR, 12, LV_TEXT_ALIGN_CENTER);
}

static void tirtc_demo_pixel_menu_button(lv_obj_t *parent, int y,
                                         const char *line1, const char *line2,
                                         bool add, lv_event_cb_t cb,
                                         void *user_data)
{
    lv_obj_t *button = tirtc_demo_pixel_button(parent, 8, y, 104, 92,
                                               PIXEL_CARD_COLOR,
                                               PIXEL_TEXT_COLOR, "", cb,
                                               user_data);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(PIXEL_BORDER_COLOR), 0);
    tirtc_demo_pixel_label(button, add ? "+" : LV_SYMBOL_LIST, 28, 10, 48,
                           add ? PIXEL_GREEN_COLOR : PIXEL_BLUE_COLOR, 20,
                           LV_TEXT_ALIGN_CENTER);
    tirtc_demo_pixel_label(button, line1, 8, 50, 88, PIXEL_TEXT_COLOR, 12,
                           LV_TEXT_ALIGN_CENTER);
    if (line2 != NULL) {
        tirtc_demo_pixel_label(button, line2, 8, 68, 88, PIXEL_MUTED_COLOR, 12,
                               LV_TEXT_ALIGN_CENTER);
    }
}

static void tirtc_demo_pixel_call_home(bool wechat)
{
    char device_id[TIRTC_DEMO_DEVICE_ID_MAX];
    lv_obj_t *qr_card;
    lv_obj_t *qr;

    tirtc_demo_app_copy_device_id(device_id, sizeof(device_id));
    tirtc_demo_pixel_header(wechat ? "微信呼叫" : "呼叫",
                            tirtc_demo_back_to_home_callback, NULL, NULL);
    tirtc_demo_pixel_menu_button(g_pixel_canvas, 36,
                                 wechat ? "添加微信" : "添加联系人",
                                 wechat ? "联系人" : NULL, true,
                                 tirtc_demo_view_callback,
                                 (void *)(uintptr_t)(wechat ?
                                     TIRTC_DEMO_VIEW_WECHAT_ADD :
                                     TIRTC_DEMO_VIEW_ADD_CONTACT));
    tirtc_demo_pixel_menu_button(g_pixel_canvas, 140,
                                 wechat ? "微信" : "联系人列表",
                                 wechat ? "联系人" : NULL, false,
                                 tirtc_demo_view_callback,
                                 (void *)(uintptr_t)(wechat ?
                                     TIRTC_DEMO_VIEW_WECHAT_CONTACTS :
                                     TIRTC_DEMO_VIEW_CONTACTS));
    qr_card = tirtc_demo_pixel_box(g_pixel_canvas, 120, 36, 192, 196,
                                   0xF7FBFE, PIXEL_BORDER_COLOR, 8);
    qr = tirtc_demo_pixel_qrcode(qr_card, device_id, 160);
    if (qr != NULL) {
        lv_obj_set_pos(qr, 16, 4);
    }
    tirtc_demo_pixel_label(qr_card, device_id, 8, 170, 176,
                           PIXEL_MUTED_COLOR, 12, LV_TEXT_ALIGN_CENTER);
}

static lv_obj_t *tirtc_demo_pixel_input_row(lv_obj_t *parent, int y,
                                            const char *label,
                                            const char *value,
                                            uint32_t max_length,
                                            bool password)
{
    lv_obj_t *row = tirtc_demo_pixel_box(parent, 8, y, 304, 42,
                                         PIXEL_CARD_COLOR, PIXEL_BORDER_COLOR, 8);
    lv_obj_t *textarea = lv_textarea_create(row);

    tirtc_demo_pixel_label(row, label, 10, 12, 86, PIXEL_MUTED_COLOR, 12,
                           LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(textarea, 96, 6);
    lv_obj_set_size(textarea, 198, 30);
    lv_textarea_set_one_line(textarea, true);
    lv_textarea_set_max_length(textarea, max_length);
    lv_textarea_set_password_mode(textarea, password);
    lv_textarea_set_text(textarea, value != NULL ? value : "");
    lv_obj_set_style_text_font(textarea, &lv_font_montserrat_12, 0);
    lv_obj_set_style_radius(textarea, 6, 0);
    lv_obj_set_style_border_width(textarea, 1, 0);
    lv_obj_set_style_border_color(textarea, lv_color_hex(PIXEL_BORDER_COLOR), 0);
    lv_obj_set_style_pad_all(textarea, 5, 0);
    lv_obj_add_event_cb(textarea, tirtc_demo_textarea_callback, LV_EVENT_ALL, NULL);
    return textarea;
}

static void tirtc_demo_pixel_add_field_row(bool wechat)
{
    const char *value = wechat ? g_pending_wechat_open_id :
                                 g_pending_contact_device_id;
    lv_obj_t *row = tirtc_demo_pixel_button(
        g_pixel_canvas, 8, 42, 304, 42, PIXEL_CARD_COLOR,
        PIXEL_TEXT_COLOR, "", tirtc_demo_view_callback,
        (void *)(uintptr_t)(wechat ? TIRTC_DEMO_VIEW_WECHAT_ADD_EDIT :
                                     TIRTC_DEMO_VIEW_CALL_ADD_EDIT));

    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(PIXEL_BORDER_COLOR), 0);
    tirtc_demo_pixel_label(row, wechat ? "OpenID" : "设备 ID",
                           11, 13, 70, PIXEL_MUTED_COLOR, 12,
                           LV_TEXT_ALIGN_LEFT);
    tirtc_demo_pixel_dynamic_label(row,
                                   value[0] != '\0' ? value :
                                       (wechat ? "28位微信Open ID" : "Device ID"),
                                   85, 13, 176,
                                   value[0] != '\0' ? PIXEL_TEXT_COLOR :
                                                       0x8AA0B5,
                                   12, LV_TEXT_ALIGN_LEFT);
    tirtc_demo_pixel_label(row, ">", 270, 13, 20, PIXEL_MUTED_COLOR, 14,
                           LV_TEXT_ALIGN_CENTER);
}

static void tirtc_demo_pixel_create_add_contact(bool wechat)
{
    tirtc_demo_pixel_header(wechat ? "添加微信联系人" : "添加联系人",
                            tirtc_demo_pixel_child_back_callback, NULL, NULL);
    tirtc_demo_pixel_add_field_row(wechat);
    tirtc_demo_pixel_button(g_pixel_canvas, 8, 94, 148, 34,
                            wechat ? PIXEL_GREEN_COLOR : PIXEL_BLUE_COLOR,
                            0xFFFFFF,
                            wechat ? "扫码添加" : "扫码添加联系人",
                            tirtc_demo_vendor_page_callback,
                            (void *)(uintptr_t)TIRTC_DEMO_VENDOR_SCANCODE);
    tirtc_demo_pixel_button(g_pixel_canvas, 164, 94, 148, 34, 0xE9F5FF,
                            PIXEL_BLUE_COLOR, "查看扫码信息",
                            tirtc_demo_view_callback,
                            (void *)(uintptr_t)(wechat ?
                                TIRTC_DEMO_VIEW_WECHAT_SCAN_INFO :
                                TIRTC_DEMO_VIEW_CALL_SCAN_INFO));
    tirtc_demo_pixel_button(g_pixel_canvas, 8, 138, 304, 34,
                            wechat ? PIXEL_GREEN_COLOR : PIXEL_BLUE_COLOR,
                            0xFFFFFF, "确认添加",
                            wechat ? tirtc_demo_wechat_add_callback :
                                     tirtc_demo_add_contact_callback,
                            wechat ? NULL :
                                     (void *)(uintptr_t)TIRTC_DEMO_ADD_REQUEST);
}

static void tirtc_demo_pixel_create_add_contact_edit(bool wechat)
{
    const char *value = wechat ? g_pending_wechat_open_id :
                                 g_pending_contact_device_id;
    char length_text[12];
    lv_obj_t *textarea;

    snprintf(length_text, sizeof(length_text), "%u/%u",
             (unsigned int)strlen(value), wechat ? 28U : 12U);
    tirtc_demo_pixel_header(wechat ? "添加微信联系人" : "添加联系人",
                            tirtc_demo_pixel_child_back_callback, "保存",
                            wechat ? tirtc_demo_wechat_edit_save_callback :
                                     tirtc_demo_contact_edit_save_callback);
    tirtc_demo_pixel_label(g_pixel_canvas, wechat ? "OpenID" : "Device ID",
                           8, 36, 196, PIXEL_MUTED_COLOR, 12,
                           LV_TEXT_ALIGN_LEFT);
    tirtc_demo_pixel_dynamic_label(g_pixel_canvas, length_text,
                                   230, 36, 82, PIXEL_MUTED_COLOR, 12,
                                   LV_TEXT_ALIGN_RIGHT);
    textarea = lv_textarea_create(g_pixel_canvas);
    lv_obj_set_pos(textarea, 8, 55);
    lv_obj_set_size(textarea, 304, 31);
    lv_textarea_set_one_line(textarea, true);
    lv_textarea_set_max_length(textarea, wechat ? 28U : 12U);
    lv_textarea_set_placeholder_text(
        textarea, wechat ? "28位微信Open ID" : "Device ID");
    lv_textarea_set_text(textarea, value);
    lv_obj_set_style_text_font(textarea, &lv_font_montserrat_12, 0);
    lv_obj_set_style_radius(textarea, 8, 0);
    lv_obj_set_style_border_width(textarea, 1, 0);
    lv_obj_set_style_border_color(textarea, lv_color_hex(PIXEL_BORDER_COLOR), 0);
    lv_obj_set_style_bg_color(textarea, lv_color_hex(PIXEL_CARD_COLOR), 0);
    lv_obj_set_style_pad_left(textarea, 12, 0);
    lv_obj_set_style_pad_right(textarea, 12, 0);
    lv_obj_add_event_cb(textarea, tirtc_demo_textarea_callback,
                        LV_EVENT_ALL, NULL);
    if (wechat) {
        g_wechat_open_id_ta = textarea;
    } else {
        g_contact_device_id_ta = textarea;
    }
    tirtc_demo_pixel_label(g_pixel_canvas, "点击保存生效",
                           8, 91, 304, PIXEL_GREEN_COLOR, 12,
                           LV_TEXT_ALIGN_LEFT);
}

static void tirtc_demo_pixel_create_scan_info(bool wechat)
{
    lv_obj_t *overlay;
    lv_obj_t *dialog;
    lv_obj_t *field;

    tirtc_demo_pixel_create_add_contact(wechat);
    overlay = tirtc_demo_pixel_box(g_pixel_canvas, 0, 0, 320, 240,
                                   PIXEL_TEXT_COLOR, PIXEL_TEXT_COLOR, 0);
    lv_obj_set_style_bg_opa(overlay, 51, 0);
    dialog = tirtc_demo_pixel_box(overlay, 30, 52, 260, 140,
                                  PIXEL_CARD_COLOR, PIXEL_BORDER_COLOR, 9);
    lv_obj_set_style_shadow_width(dialog, 14, 0);
    lv_obj_set_style_shadow_ofs_y(dialog, 5, 0);
    lv_obj_set_style_shadow_color(dialog, lv_color_hex(PIXEL_TEXT_COLOR), 0);
    lv_obj_set_style_shadow_opa(dialog, 51, 0);
    tirtc_demo_pixel_label(dialog, wechat ? "扫码格式" : "QR Format",
                           12, 12, 236, PIXEL_TEXT_COLOR, 16,
                           LV_TEXT_ALIGN_CENTER);
    field = tirtc_demo_pixel_box(dialog, 12, 42, 236, wechat ? 66 : 40,
                                 0xF4F9FD, PIXEL_BORDER_COLOR, 6);
    tirtc_demo_pixel_dynamic_label(
        field, wechat ? "28-character WeChat Open ID" : "TIR588XN352C",
        9, wechat ? 24 : 11, 216, PIXEL_TEXT_COLOR, 12,
        wechat ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT);
    tirtc_demo_pixel_button(dialog, 92, wechat ? 110 : 94, 76, 24,
                            0xE9F5FF, PIXEL_BLUE_COLOR,
                            wechat ? "关闭" : "Close",
                            tirtc_demo_pixel_child_back_callback, NULL);
}

static void tirtc_demo_pixel_create_contacts(bool wechat)
{
    const tirtc_demo_contact_t *contacts = NULL;
    const tirtc_demo_wechat_contact_t *wechat_contacts = NULL;
    size_t count = wechat ? tirtc_demo_app_get_wechat_contacts(&wechat_contacts) :
                           tirtc_demo_app_get_contacts(&contacts);

    tirtc_demo_pixel_header(wechat ? "微信联系人" : "联系人列表",
                            tirtc_demo_pixel_child_back_callback, "刷新",
                            wechat ? tirtc_demo_wechat_refresh_callback :
                                     tirtc_demo_contacts_refresh_callback);
    if (count == 0U) {
        if (wechat) {
            tirtc_demo_pixel_dynamic_label(g_pixel_canvas,
                                           "No WeChat contacts",
                                           8, 104, 304, PIXEL_MUTED_COLOR,
                                           16, LV_TEXT_ALIGN_CENTER);
        }
        return;
    }
    if (count > 4U) {
        count = 4U;
    }
    for (size_t i = 0U; i < count; ++i) {
        int y = 36 + ((int)i * 50);
        lv_obj_t *row = tirtc_demo_pixel_box(g_pixel_canvas, 8, y, 304, 44,
                                             PIXEL_CARD_COLOR,
                                             PIXEL_BORDER_COLOR, 7);
        const char *id = wechat ? wechat_contacts[i].open_id : contacts[i].device_id;
        lv_obj_t *call;

        if (wechat) {
            tirtc_demo_pixel_dynamic_label(row, id, 11, 13, 206,
                                           PIXEL_TEXT_COLOR, 12,
                                           LV_TEXT_ALIGN_LEFT);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, tirtc_demo_wechat_delete_callback,
                                LV_EVENT_LONG_PRESSED,
                                (void *)(uintptr_t)i);
            call = tirtc_demo_pixel_button(
                row, 226, 5, 66, 34, 0xDDF8EA, 0x1FC985, "呼叫",
                tirtc_demo_wechat_call_callback, (void *)(uintptr_t)i);
            lv_obj_add_event_cb(call, tirtc_demo_wechat_delete_callback,
                                LV_EVENT_LONG_PRESSED,
                                (void *)(uintptr_t)i);
        } else {
            const char *subtitle = contacts[i].name != NULL &&
                                   contacts[i].name[0] != '\0' ?
                                       contacts[i].name :
                                       (contacts[i].online ? "Online" : "Offline");
            tirtc_demo_pixel_dynamic_label(row, id, 11, 6, 220,
                                           PIXEL_TEXT_COLOR, 12,
                                           LV_TEXT_ALIGN_LEFT);
            tirtc_demo_pixel_dynamic_label(row, subtitle, 11, 25, 190,
                                           PIXEL_MUTED_COLOR, 10,
                                           LV_TEXT_ALIGN_LEFT);
            call = tirtc_demo_pixel_button(
                row, 245, 9, 46, 26,
                contacts[i].online ? 0xDDF8EA : 0xEEF3F7,
                contacts[i].online ? 0x1FC985 : 0x8AA0B5,
                contacts[i].online ? "呼叫" : "Offline",
                tirtc_demo_call_contact_callback, (void *)(uintptr_t)i);
            if (!contacts[i].online) {
                lv_obj_add_state(call, LV_STATE_DISABLED);
            }
        }
    }
}

static void tirtc_demo_pixel_child_back_callback(lv_event_t *e)
{
    (void)e;

    switch (g_current_view) {
    case TIRTC_DEMO_VIEW_CALL_ADD_EDIT:
        tirtc_demo_delete_keyboard();
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_ADD_CONTACT);
        break;
    case TIRTC_DEMO_VIEW_CALL_SCAN_INFO:
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_ADD_CONTACT);
        break;
    case TIRTC_DEMO_VIEW_ADD_CONTACT:
    case TIRTC_DEMO_VIEW_CONTACTS:
    case TIRTC_DEMO_VIEW_ACTIVE_CALL:
    case TIRTC_DEMO_VIEW_CALL_INCOMING:
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_CALL);
        break;
    case TIRTC_DEMO_VIEW_WECHAT_SCAN_INFO:
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_WECHAT_ADD);
        break;
    case TIRTC_DEMO_VIEW_WECHAT_ADD_EDIT:
        tirtc_demo_delete_keyboard();
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_WECHAT_ADD);
        break;
    case TIRTC_DEMO_VIEW_WECHAT_ADD:
    case TIRTC_DEMO_VIEW_WECHAT_CONTACTS:
    case TIRTC_DEMO_VIEW_WECHAT_ACTIVE:
    case TIRTC_DEMO_VIEW_WECHAT_INCOMING:
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_WECHAT);
        break;
    case TIRTC_DEMO_VIEW_AI_SETTINGS:
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_AI_TALK);
        break;
    case TIRTC_DEMO_VIEW_CONFIG_EDIT:
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_CONFIG);
        break;
    case TIRTC_DEMO_VIEW_CONFIG:
    case TIRTC_DEMO_VIEW_DEVICE_INFO:
    case TIRTC_DEMO_VIEW_DEVICE_QR:
    case TIRTC_DEMO_VIEW_DIAGNOSTICS:
    case TIRTC_DEMO_VIEW_TIRTC_TEST:
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_SYSTEM);
        break;
    default:
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_HOME);
        break;
    }
}

static void tirtc_demo_pixel_call_volume_row(lv_obj_t *parent, int y,
                                             const char *title,
                                             tirtc_demo_volume_t volume,
                                             tirtc_demo_volume_action_t down,
                                             tirtc_demo_volume_action_t up)
{
    char value[8];
    lv_obj_t *row = tirtc_demo_pixel_box(parent, 8, y, 304, 34,
                                         PIXEL_CARD_COLOR, PIXEL_BORDER_COLOR, 7);
    lv_obj_t *pill;

    snprintf(value, sizeof(value), "%d", tirtc_demo_app_get_volume(volume));
    tirtc_demo_pixel_label(row, title, 12, 9, 132, PIXEL_TEXT_COLOR, 12,
                           LV_TEXT_ALIGN_LEFT);
    tirtc_demo_pixel_button(row, 152, 4, 32, 26, 0xEDF5FB,
                            PIXEL_TEXT_COLOR, "-", tirtc_demo_volume_callback,
                            (void *)(uintptr_t)down);
    pill = tirtc_demo_pixel_box(row, 192, 4, 34, 26,
                                0xEDF8F2, 0xEDF8F2, 6);
    tirtc_demo_pixel_dynamic_label(pill, value, 0, 6, 34,
                           tirtc_demo_app_is_muted(volume) ? PIXEL_RED_COLOR :
                                                            PIXEL_GREEN_COLOR,
                           12, LV_TEXT_ALIGN_CENTER);
    tirtc_demo_pixel_button(row, 234, 4, 32, 26, 0xEDF5FB,
                            PIXEL_TEXT_COLOR, "+", tirtc_demo_volume_callback,
                            (void *)(uintptr_t)up);
}

static void tirtc_demo_pixel_create_active_call(bool wechat)
{
    tirtc_demo_runtime_t runtime;
    char duration[16];
    lv_obj_t *duration_row;
    lv_obj_t *button;

    tirtc_demo_app_get_runtime(&runtime);
    snprintf(duration, sizeof(duration), "%lu:%02lu",
             (unsigned long)(runtime.call_duration_s / 60U),
             (unsigned long)(runtime.call_duration_s % 60U));
    tirtc_demo_pixel_header(wechat ? "微信通话" : "通话",
                            tirtc_demo_pixel_child_back_callback, NULL, NULL);
    duration_row = tirtc_demo_pixel_box(g_pixel_canvas, 8, 38, 304, 32,
                                        PIXEL_CARD_COLOR, PIXEL_BORDER_COLOR, 8);
    tirtc_demo_pixel_label(duration_row, "通话时长", 16, 8, 180,
                           PIXEL_TEXT_COLOR, 12, LV_TEXT_ALIGN_LEFT);
    g_call_duration_label = tirtc_demo_pixel_dynamic_label(
        duration_row, duration, 184, 7, 104, PIXEL_GREEN_COLOR, 16,
        LV_TEXT_ALIGN_RIGHT);
    tirtc_demo_pixel_call_volume_row(g_pixel_canvas, 80, "麦克风",
                                     TIRTC_DEMO_VOLUME_TX,
                                     TIRTC_DEMO_VOL_TX_DOWN,
                                     TIRTC_DEMO_VOL_TX_UP);
    tirtc_demo_pixel_call_volume_row(g_pixel_canvas, 124, "扬声器",
                                     TIRTC_DEMO_VOLUME_RX,
                                     TIRTC_DEMO_VOL_RX_DOWN,
                                     TIRTC_DEMO_VOL_RX_UP);
    if (runtime.incoming_call) {
        button = tirtc_demo_pixel_button(g_pixel_canvas, 8, 168, 148, 38,
                                         PIXEL_GREEN_COLOR, 0xFFFFFF, "接听",
                                         wechat ? tirtc_demo_wechat_answer_callback :
                                                  tirtc_demo_call_action_callback,
                                         (void *)(uintptr_t)(wechat ? true :
                                             TIRTC_DEMO_CALL_ACCEPT));
        (void)button;
        tirtc_demo_pixel_button(g_pixel_canvas, 164, 168, 148, 38,
                                PIXEL_RED_COLOR, 0xFFFFFF, "拒绝",
                                wechat ? tirtc_demo_wechat_answer_callback :
                                         tirtc_demo_call_action_callback,
                                (void *)(uintptr_t)(wechat ? false :
                                    TIRTC_DEMO_CALL_REJECT));
    } else {
        button = tirtc_demo_pixel_button(g_pixel_canvas, 8, 168, 304, 38,
                                         0xFFE7E7, 0xE44747, "挂断",
                                         wechat ? tirtc_demo_wechat_hangup_callback :
                                                  tirtc_demo_call_action_callback,
                                         wechat ? NULL :
                                             (void *)(uintptr_t)TIRTC_DEMO_CALL_HANGUP);
        if (!runtime.peer_connected &&
            strcmp(runtime.call_phase != NULL ? runtime.call_phase : "idle",
                   "outgoing") != 0 &&
            strcmp(runtime.call_phase != NULL ? runtime.call_phase : "idle",
                   "connecting") != 0) {
            lv_obj_add_state(button, LV_STATE_DISABLED);
        }
    }
}

static void tirtc_demo_pixel_ai_settings_callback(lv_event_t *e)
{
    (void)e;
    tirtc_demo_render_view(TIRTC_DEMO_VIEW_AI_SETTINGS);
}

static void tirtc_demo_pixel_ai_avatar_select_callback(lv_event_t *e)
{
    uint8_t avatar = (uint8_t)(uintptr_t)lv_event_get_user_data(e);

    if (avatar < TIRTC_DEMO_AI_AVATAR_COUNT) {
        (void)tirtc_demo_app_set_ai_avatar(avatar);
    }
    tirtc_demo_render_view(TIRTC_DEMO_VIEW_AI_SETTINGS);
}

static void tirtc_demo_pixel_create_ai(void)
{
    tirtc_demo_status_t status;
    tirtc_demo_runtime_t runtime;
    tirtc_demo_ai_message_t messages[TIRTC_DEMO_AI_MESSAGE_MAX];
    size_t count;
    lv_obj_t *header;
    lv_obj_t *avatar;
    lv_obj_t *caption_bar;
    lv_obj_t *caption_label;
    char caption[TIRTC_DEMO_AI_CAPTION_MAX];
    const char *state_text;
    uint32_t state_color;
    uint32_t caption_color;
    ai_chat_avatar_state_t avatar_state;
    bool show_new_chat;

    tirtc_demo_app_get_status(&status);
    tirtc_demo_app_get_runtime(&runtime);
    count = tirtc_demo_app_copy_ai_messages(messages, TIRTC_DEMO_AI_MESSAGE_MAX);
    tirtc_demo_app_copy_ai_caption(caption, sizeof(caption));
    show_new_chat = !runtime.ai_ready && !runtime.service_starting;
    if (status.state == TIRTC_DEMO_STATE_ERROR) {
        state_text = "异常";
        state_color = 0xE45757;
        avatar_state = AI_CHAT_AVATAR_STATE_ERROR;
    } else if (runtime.service_starting) {
        state_text = "连接";
        state_color = 0xF59E0B;
        avatar_state = AI_CHAT_AVATAR_STATE_THINKING;
    } else if (count > 0U && messages[count - 1U].caption_type != 0U) {
        state_text = "回复";
        state_color = PIXEL_GREEN_COLOR;
        avatar_state = AI_CHAT_AVATAR_STATE_SPEAKING;
    } else if (runtime.ai_ready) {
        state_text = "聆听";
        state_color = PIXEL_BLUE_COLOR;
        avatar_state = AI_CHAT_AVATAR_STATE_LISTENING;
    } else {
        state_text = "休息";
        state_color = PIXEL_MUTED_COLOR;
        avatar_state = AI_CHAT_AVATAR_STATE_RESTING;
    }
    caption_color = count > 0U && messages[count - 1U].caption_type == 0U ?
                        0x0D8A59 : 0x1768B7;

    header = tirtc_demo_pixel_box(g_pixel_canvas, 0, 0, 320, 28,
                                  PIXEL_HEADER_COLOR, PIXEL_BORDER_COLOR, 0);
    {
        lv_obj_t *back = tirtc_demo_pixel_button(
            header, 0, 0, 64, 28, PIXEL_HEADER_COLOR, PIXEL_TEXT_COLOR, "<",
            tirtc_demo_back_to_home_callback, NULL);
        lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    }
    tirtc_demo_pixel_label(header, state_text, 41, 5, 48,
                           state_color, 12, LV_TEXT_ALIGN_LEFT);
    tirtc_demo_pixel_label(header, "AI 对讲", 100, 4, 120,
                           PIXEL_TEXT_COLOR, 12, LV_TEXT_ALIGN_CENTER);
    tirtc_demo_pixel_button(header, 262, 3, 50, 22, 0xE9F5FF,
                            PIXEL_BLUE_COLOR, "设置",
                            tirtc_demo_pixel_ai_settings_callback, NULL);

    avatar = tirtc_demo_pixel_image(
        g_pixel_canvas,
        ai_chat_avatar_asset_get(runtime.ai_avatar, avatar_state),
        112, 40);
    lv_obj_clear_flag(avatar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    if (show_new_chat) {
        tirtc_demo_pixel_button(g_pixel_canvas, 71, 194, 178, 34,
                                PIXEL_GREEN_COLOR, 0xFFFFFF, "开始新对话",
                                tirtc_demo_ai_new_session_callback, NULL);
    } else {
        caption_bar = tirtc_demo_pixel_box(g_pixel_canvas, 8, 194, 304, 34,
                                            PIXEL_CARD_COLOR,
                                            PIXEL_BORDER_COLOR, 8);
        caption_label = tirtc_demo_pixel_dynamic_label(
            caption_bar,
            caption[0] != '\0' ? caption :
                (runtime.ai_ready ? "聆听中" : "正在连接"),
            12, 7, 280, caption_color, 16, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_style_text_font(caption_label, &jz_ui_font_16, 0);
    }
}

static void tirtc_demo_pixel_ai_setting_volume_row(
    int y, const char *title, tirtc_demo_volume_t volume,
    tirtc_demo_volume_action_t down, tirtc_demo_volume_action_t up)
{
    lv_obj_t *row;
    char value[8];

    row = tirtc_demo_pixel_box(g_pixel_canvas, 8, y, 304, 44,
                               PIXEL_CARD_COLOR, PIXEL_BORDER_COLOR, 8);
    snprintf(value, sizeof(value), "%d", tirtc_demo_app_get_volume(volume));
    tirtc_demo_pixel_label(row, title, 13, 13, 120, PIXEL_TEXT_COLOR, 12,
                           LV_TEXT_ALIGN_LEFT);
    tirtc_demo_pixel_button(row, 165, 7, 34, 28, 0xE9F5FF,
                            PIXEL_BLUE_COLOR, "-", tirtc_demo_volume_callback,
                            (void *)(uintptr_t)down);
    tirtc_demo_pixel_label(row, value, 207, 9, 44,
                           tirtc_demo_app_is_muted(volume) ? PIXEL_RED_COLOR :
                                                            PIXEL_GREEN_COLOR,
                           13, LV_TEXT_ALIGN_CENTER);
    tirtc_demo_pixel_button(row, 259, 7, 34, 28, 0xE9F5FF,
                            PIXEL_BLUE_COLOR, "+", tirtc_demo_volume_callback,
                            (void *)(uintptr_t)up);
}

static void tirtc_demo_pixel_create_ai_settings(void)
{
    tirtc_demo_runtime_t runtime;
    lv_obj_t *avatar_row;
    lv_obj_t *info;

    tirtc_demo_app_get_runtime(&runtime);
    tirtc_demo_pixel_header("AI 对讲设置",
                            tirtc_demo_pixel_child_back_callback, NULL, NULL);
    tirtc_demo_pixel_ai_setting_volume_row(
        38, "麦克风音量", TIRTC_DEMO_VOLUME_TX,
        TIRTC_DEMO_VOL_TX_DOWN, TIRTC_DEMO_VOL_TX_UP);
    tirtc_demo_pixel_ai_setting_volume_row(
        90, "扬声器音量", TIRTC_DEMO_VOLUME_RX,
        TIRTC_DEMO_VOL_RX_DOWN, TIRTC_DEMO_VOL_RX_UP);

    avatar_row = tirtc_demo_pixel_box(g_pixel_canvas, 8, 142, 304, 44,
                                      PIXEL_CARD_COLOR, PIXEL_BORDER_COLOR, 8);
    tirtc_demo_pixel_label(avatar_row, "角色形象", 13, 13, 92,
                           PIXEL_TEXT_COLOR, 12, LV_TEXT_ALIGN_LEFT);
    tirtc_demo_pixel_button(
        avatar_row, 142, 6, 76, 32,
        runtime.ai_avatar == 0U ? PIXEL_GREEN_COLOR : 0xE9F5FF,
        runtime.ai_avatar == 0U ? 0xFFFFFF : PIXEL_BLUE_COLOR,
        "伙伴", tirtc_demo_pixel_ai_avatar_select_callback,
        (void *)(uintptr_t)0U);
    tirtc_demo_pixel_button(
        avatar_row, 226, 6, 68, 32,
        runtime.ai_avatar == 1U ? PIXEL_GREEN_COLOR : 0xE9F5FF,
        runtime.ai_avatar == 1U ? 0xFFFFFF : PIXEL_BLUE_COLOR,
        "小芽", tirtc_demo_pixel_ai_avatar_select_callback,
        (void *)(uintptr_t)1U);

    info = tirtc_demo_pixel_box(g_pixel_canvas, 8, 194, 304, 34,
                                PIXEL_CARD_COLOR, PIXEL_BORDER_COLOR, 8);
    tirtc_demo_pixel_label(info, "对讲方式", 13, 10, 78,
                           PIXEL_TEXT_COLOR, 12, LV_TEXT_ALIGN_LEFT);
    tirtc_demo_pixel_label(info, "默认常听，直接说话。", 96, 10, 195,
                           PIXEL_MUTED_COLOR, 12, LV_TEXT_ALIGN_RIGHT);
}

static void tirtc_demo_pixel_setting_row(int y, const char *text,
                                         lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *row = tirtc_demo_pixel_button(g_pixel_canvas, 8, y, 304, 34,
                                            PIXEL_CARD_COLOR,
                                            PIXEL_TEXT_COLOR, "", cb,
                                            user_data);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(PIXEL_BORDER_COLOR), 0);
    tirtc_demo_pixel_label(row, text, 12, 9, 248, PIXEL_TEXT_COLOR, 12,
                           LV_TEXT_ALIGN_LEFT);
    tirtc_demo_pixel_label(row, ">", 268, 7, 24, PIXEL_MUTED_COLOR, 16,
                           LV_TEXT_ALIGN_CENTER);
}

static void tirtc_demo_pixel_create_system(void)
{
    tirtc_demo_pixel_header("设置", tirtc_demo_back_to_home_callback,
                            NULL, NULL);
    tirtc_demo_pixel_setting_row(42, "Wi-Fi 设置",
                                 tirtc_demo_vendor_page_callback,
                                 (void *)(uintptr_t)TIRTC_DEMO_VENDOR_SETTINGS);
    tirtc_demo_pixel_setting_row(80, "网络测试",
                                 tirtc_demo_view_callback,
                                 (void *)(uintptr_t)TIRTC_DEMO_VIEW_DIAGNOSTICS);
    tirtc_demo_pixel_setting_row(118, "TiRTC 配置",
                                 tirtc_demo_view_callback,
                                 (void *)(uintptr_t)TIRTC_DEMO_VIEW_CONFIG);
    tirtc_demo_pixel_setting_row(156, "TiRTC 测试",
                                 tirtc_demo_view_callback,
                                 (void *)(uintptr_t)TIRTC_DEMO_VIEW_TIRTC_TEST);
}

static void tirtc_demo_pixel_config_row(int y, const char *label,
                                        const char *value, uint32_t color)
{
    lv_obj_t *row = tirtc_demo_pixel_box(g_pixel_canvas, 8, y, 304, 38,
                                         PIXEL_CARD_COLOR, PIXEL_BORDER_COLOR, 6);
    tirtc_demo_pixel_label(row, label, 10, 5, 92, PIXEL_MUTED_COLOR, 12,
                           LV_TEXT_ALIGN_LEFT);
    tirtc_demo_pixel_label(row, value, 10, 20, 284, color, 12,
                           LV_TEXT_ALIGN_LEFT);
}

static void tirtc_demo_pixel_config_field(lv_obj_t *parent, int y,
                                          const char *label,
                                          const char *value)
{
    lv_obj_t *row = tirtc_demo_pixel_box(parent, 0, y, 304, 42,
                                         PIXEL_CARD_COLOR,
                                         PIXEL_BORDER_COLOR, 6);

    tirtc_demo_pixel_label(row, label, 10, 7, 250, PIXEL_MUTED_COLOR, 12,
                           LV_TEXT_ALIGN_LEFT);
    tirtc_demo_pixel_dynamic_label(row, value, 10, 22, 246,
                                   PIXEL_TEXT_COLOR, 12,
                                   LV_TEXT_ALIGN_LEFT);
}

static void tirtc_demo_pixel_create_config(void)
{
    tirtc_demo_runtime_t runtime;
    tirtc_demo_settings_t settings;
    lv_obj_t *fields;
    char binding[64];

    tirtc_demo_app_get_runtime(&runtime);
    tirtc_demo_app_get_settings(&settings);
    if (runtime.identity_ready) {
        snprintf(binding, sizeof(binding), "%s", "Bound");
    } else if (runtime.binding_waiting && runtime.binding_code != NULL &&
               runtime.binding_code[0] != '\0') {
        snprintf(binding, sizeof(binding), "Code %s", runtime.binding_code);
    } else if (runtime.binding_waiting) {
        snprintf(binding, sizeof(binding), "%s", "Waiting user");
    } else {
        snprintf(binding, sizeof(binding), "%s", "Idle");
    }

    tirtc_demo_pixel_header("TiRTC 配置",
                            tirtc_demo_pixel_child_back_callback, NULL, NULL);
    fields = tirtc_demo_pixel_box(g_pixel_canvas, 8, 40, 304, 154,
                                  PIXEL_BG_COLOR, PIXEL_BG_COLOR, 0);
    lv_obj_add_flag(fields, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(fields, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(fields, LV_SCROLLBAR_MODE_OFF);
    tirtc_demo_pixel_config_field(fields, 0, "Device ID",
                                  settings.peer_id[0] != '\0' ?
                                      settings.peer_id : "Unbound");
    tirtc_demo_pixel_config_field(fields, 46, "Binding", binding);
    tirtc_demo_pixel_config_field(fields, 92, "Token Subject", "Not set");
    tirtc_demo_pixel_config_field(fields, 138, "Token API",
                                  settings.endpoint[0] != '\0' ?
                                      settings.endpoint : "Service issued");
    tirtc_demo_pixel_config_field(fields, 184, "Credential",
                                  "Managed by binding");
    tirtc_demo_pixel_button(g_pixel_canvas, 8, 202, 304, 30,
                            0xFFE7E7, 0xE44747, "重置绑定",
                            tirtc_demo_reset_binding_callback, NULL);
}

static void tirtc_demo_pixel_create_config_edit(void)
{
    tirtc_demo_config_t config;

    memset(&config, 0, sizeof(config));
    tirtc_demo_app_get_config(&config);
    tirtc_demo_pixel_header("编辑 TiRTC 配置",
                            tirtc_demo_pixel_child_back_callback,
                            "保存", tirtc_demo_save_config_callback);
    g_config_endpoint_ta = tirtc_demo_pixel_input_row(
        g_pixel_canvas, 36, "服务地址", config.endpoint,
        TIRTC_DEMO_ENDPOINT_MAX - 1U, false);
    g_config_device_id_ta = tirtc_demo_pixel_input_row(
        g_pixel_canvas, 80, "设备 ID", config.device_id,
        TIRTC_DEMO_DEVICE_ID_MAX - 1U, false);
    g_config_secret_ta = tirtc_demo_pixel_input_row(
        g_pixel_canvas, 124, "设备密钥", config.device_secret,
        TIRTC_DEMO_DEVICE_SECRET_MAX - 1U, true);
    g_config_error_label = tirtc_demo_pixel_dynamic_label(
        g_pixel_canvas, "点击输入框可编辑，保存后自动重连。",
        8, 180, 304, PIXEL_MUTED_COLOR, 12, LV_TEXT_ALIGN_CENTER);
    memset(config.device_secret, 0, sizeof(config.device_secret));
}

static void tirtc_demo_pixel_create_device_info(void)
{
    tirtc_demo_runtime_t runtime;
    char device_id[TIRTC_DEMO_DEVICE_ID_MAX];
    char rssi[24];

    tirtc_demo_app_get_runtime(&runtime);
    tirtc_demo_app_copy_device_id(device_id, sizeof(device_id));
    if (runtime.network_ready) {
        snprintf(rssi, sizeof(rssi), "%d dBm", runtime.network_rssi);
    } else {
        snprintf(rssi, sizeof(rssi), "%s", "未连接");
    }
    tirtc_demo_pixel_header("设备信息",
                            tirtc_demo_pixel_child_back_callback, NULL, NULL);
    tirtc_demo_pixel_config_row(36, "芯片", "Ingenic G32S10X", PIXEL_TEXT_COLOR);
    tirtc_demo_pixel_config_row(76, "物理屏幕", "480 x 854", PIXEL_TEXT_COLOR);
    tirtc_demo_pixel_config_row(116, "S3 UI 画布", "320 x 240 / 1:1",
                                 PIXEL_GREEN_COLOR);
    tirtc_demo_pixel_config_row(156, "Wi-Fi", rssi,
                                 runtime.network_ready ? PIXEL_GREEN_COLOR :
                                                         0xF59E0B);
    tirtc_demo_pixel_label(g_pixel_canvas, device_id, 8, 202, 190,
                           PIXEL_MUTED_COLOR, 12, LV_TEXT_ALIGN_LEFT);
    tirtc_demo_pixel_button(g_pixel_canvas, 204, 200, 50, 30,
                            0xE9F5FF, PIXEL_BLUE_COLOR, "二维码",
                            tirtc_demo_view_callback,
                            (void *)(uintptr_t)TIRTC_DEMO_VIEW_DEVICE_QR);
    tirtc_demo_pixel_button(g_pixel_canvas, 260, 200, 52, 30,
                            PIXEL_GREEN_COLOR, 0xFFFFFF, "相机",
                            tirtc_demo_vendor_page_callback,
                            (void *)(uintptr_t)TIRTC_DEMO_VENDOR_CAMERA);
}

static void tirtc_demo_pixel_create_device_qr(void)
{
    char device_id[TIRTC_DEMO_DEVICE_ID_MAX];
    lv_obj_t *qr;

    tirtc_demo_app_copy_device_id(device_id, sizeof(device_id));
    tirtc_demo_pixel_header("本机二维码",
                            tirtc_demo_pixel_child_back_callback, NULL, NULL);
    qr = tirtc_demo_pixel_qrcode(g_pixel_canvas, device_id, 192);
    if (qr != NULL) {
        lv_obj_set_pos(qr, 64, 36);
    }
    tirtc_demo_pixel_label(g_pixel_canvas, device_id, 56, 218, 208,
                           PIXEL_MUTED_COLOR, 12, LV_TEXT_ALIGN_CENTER);
}

static void tirtc_demo_pixel_create_diagnostics(void)
{
    tirtc_demo_runtime_t runtime;
    tirtc_demo_media_stats_t media;
    lv_obj_t *summary;
    lv_obj_t *result;
    uint32_t packets;
    uint32_t dropped;
    uint32_t loss_percent;
    char wifi_text[32];
    char loss_text[16];
    uint32_t network_color;
    uint32_t service_color;

    tirtc_demo_app_get_runtime(&runtime);
    tirtc_demo_media_get_stats(&media);
    tirtc_demo_pixel_header("网络测试",
                            tirtc_demo_pixel_child_back_callback,
                            "重测", tirtc_demo_diagnostics_callback);
    snprintf(wifi_text, sizeof(wifi_text), "Wi-Fi %s",
             runtime.network_ready ? "OK" : "--");
    summary = tirtc_demo_pixel_box(g_pixel_canvas, 8, 34, 304, 24,
                                   0xE7F1FB, PIXEL_BORDER_COLOR, 6);
    tirtc_demo_pixel_dynamic_label(summary, wifi_text, 8, 4, 148,
                                   PIXEL_TEXT_COLOR, 12, LV_TEXT_ALIGN_LEFT);
    tirtc_demo_pixel_dynamic_label(summary, "IP --", 116, 4, 180,
                                   PIXEL_TEXT_COLOR, 12, LV_TEXT_ALIGN_LEFT);

    network_color = runtime.network_ready ? PIXEL_GREEN_COLOR : 0xF59E0B;
    service_color = runtime.cloud_online ? PIXEL_GREEN_COLOR : 0xF59E0B;
    tirtc_demo_pixel_test_row(63, "网关", runtime.network_ready,
                              runtime.diagnostic_running);
    tirtc_demo_pixel_test_row(91, "DNS", runtime.network_ready,
                              runtime.diagnostic_running);
    tirtc_demo_pixel_test_row(119, "外网", runtime.network_ready,
                              runtime.diagnostic_running);
    tirtc_demo_pixel_test_row(147, "TiRTC 服务", runtime.cloud_online,
                              runtime.diagnostic_running);

    dropped = media.tx_audio_dropped + media.rx_audio_dropped;
    packets = media.tx_audio_frames + media.rx_audio_frames + dropped;
    loss_percent = packets > 0U ? (dropped * 100U) / packets : 0U;
    snprintf(loss_text, sizeof(loss_text), "%lu%%",
             (unsigned long)loss_percent);
    {
        lv_obj_t *loss = tirtc_demo_pixel_box(g_pixel_canvas, 8, 175, 304, 24,
                                              PIXEL_CARD_COLOR,
                                              PIXEL_BORDER_COLOR, 6);
        tirtc_demo_pixel_label(loss, "丢包", 10, 6, 180,
                               PIXEL_TEXT_COLOR, 12, LV_TEXT_ALIGN_LEFT);
        tirtc_demo_pixel_dynamic_label(loss, loss_text, 204, 6, 90,
                                       loss_percent == 0U ? PIXEL_GREEN_COLOR :
                                                            0xF59E0B,
                                       12, LV_TEXT_ALIGN_RIGHT);
    }

    result = tirtc_demo_pixel_box(g_pixel_canvas, 8, 204, 304, 30,
                                  runtime.network_ready ? 0xE5FAF0 : 0xFFF2D8,
                                  runtime.network_ready ? PIXEL_GREEN_COLOR :
                                                          0xF59E0B,
                                  6);
    tirtc_demo_pixel_label(result,
                           runtime.network_ready ? "基础网络正常" : "基础网络待测",
                           10, 7, 150,
                           runtime.network_ready ? PIXEL_GREEN_COLOR : 0xF59E0B,
                           12, LV_TEXT_ALIGN_LEFT);
    tirtc_demo_pixel_dynamic_label(
        result,
        runtime.diagnostic_running ? "检测中" :
            (runtime.cloud_online ? "TiRTC 可达" : "点击重测"),
        174, 7, 120,
        runtime.cloud_online ? service_color : network_color,
        12, LV_TEXT_ALIGN_RIGHT);
}

static void tirtc_demo_pixel_test_start_callback(lv_event_t *e)
{
    (void)e;
    (void)tirtc_demo_app_restart();
    tirtc_demo_render_view(TIRTC_DEMO_VIEW_TIRTC_TEST);
}

static void tirtc_demo_pixel_test_row(int y, const char *label,
                                      bool passed, bool running)
{
    lv_obj_t *row;
    const char *value = passed ? "通过" : (running ? "进行中" : "等待");
    uint32_t color = passed ? PIXEL_GREEN_COLOR :
                     (running ? PIXEL_BLUE_COLOR : 0xF59E0B);

    row = tirtc_demo_pixel_box(g_pixel_canvas, 8, y, 304, 24,
                               PIXEL_CARD_COLOR, PIXEL_BORDER_COLOR, 6);
    tirtc_demo_pixel_label(row, label, 10, 6, 180, PIXEL_TEXT_COLOR, 12,
                           LV_TEXT_ALIGN_LEFT);
    tirtc_demo_pixel_label(row, value, 204, 6, 90, color, 12,
                           LV_TEXT_ALIGN_RIGHT);
}

static void tirtc_demo_pixel_create_tirtc_test(void)
{
    tirtc_demo_status_t status;
    tirtc_demo_runtime_t runtime;
    lv_obj_t *summary;
    lv_obj_t *result;
    bool running;

    tirtc_demo_app_get_status(&status);
    tirtc_demo_app_get_runtime(&runtime);
    running = runtime.service_starting || runtime.call_active;

    tirtc_demo_pixel_header("TiRTC 测试",
                            tirtc_demo_pixel_child_back_callback,
                            "开始", tirtc_demo_pixel_test_start_callback);
    summary = tirtc_demo_pixel_box(g_pixel_canvas, 8, 34, 304, 24,
                                   0xE7F1FB, PIXEL_BORDER_COLOR, 6);
    tirtc_demo_pixel_label(summary, "会话 TEST", 8, 6, 150,
                           PIXEL_TEXT_COLOR, 12, LV_TEXT_ALIGN_LEFT);
    tirtc_demo_pixel_label(summary, "音频链路", 196, 6, 100,
                           PIXEL_TEXT_COLOR, 12, LV_TEXT_ALIGN_RIGHT);

    tirtc_demo_pixel_test_row(63, "获取 Token", runtime.token_ready,
                              runtime.service_starting);
    tirtc_demo_pixel_test_row(91, "连接房间", runtime.sdk_ready,
                              runtime.service_starting);
    tirtc_demo_pixel_test_row(119, "发送音频", runtime.audio_tx_active,
                              running);
    tirtc_demo_pixel_test_row(147, "接收音频", runtime.audio_rx_active,
                              running);
    tirtc_demo_pixel_test_row(175, "断开测试", !runtime.call_active,
                              false);

    result = tirtc_demo_pixel_box(g_pixel_canvas, 8, 204, 304, 30,
                                  runtime.sdk_ready ? 0xE5FAF0 : 0xFFF2D8,
                                  runtime.sdk_ready ? PIXEL_GREEN_COLOR :
                                                      0xF59E0B,
                                  6);
    tirtc_demo_pixel_label(result,
                           runtime.sdk_ready ? "连接成功" :
                               (runtime.service_starting ? "测试中" : "测试就绪"),
                           10, 8, 100,
                           runtime.sdk_ready ? PIXEL_GREEN_COLOR : 0xF59E0B,
                           12, LV_TEXT_ALIGN_LEFT);
    tirtc_demo_pixel_dynamic_label(result,
                                   status.message != NULL ? status.message : "点击开始",
                                   116, 8, 178, PIXEL_MUTED_COLOR, 12,
                                   LV_TEXT_ALIGN_RIGHT);
}

static void tirtc_demo_pixel_render_view(tirtc_demo_view_t view)
{
    switch (view) {
    case TIRTC_DEMO_VIEW_DETAIL:
        tirtc_demo_pixel_create_detail();
        break;
    case TIRTC_DEMO_VIEW_CALL:
        tirtc_demo_pixel_call_home(false);
        break;
    case TIRTC_DEMO_VIEW_ACTIVE_CALL:
        tirtc_demo_pixel_create_active_call(false);
        break;
    case TIRTC_DEMO_VIEW_CALL_INCOMING:
        tirtc_demo_pixel_create_active_call(false);
        break;
    case TIRTC_DEMO_VIEW_ADD_CONTACT:
        tirtc_demo_pixel_create_add_contact(false);
        break;
    case TIRTC_DEMO_VIEW_CALL_ADD_EDIT:
        tirtc_demo_pixel_create_add_contact_edit(false);
        break;
    case TIRTC_DEMO_VIEW_CALL_SCAN_INFO:
        tirtc_demo_pixel_create_scan_info(false);
        break;
    case TIRTC_DEMO_VIEW_CONTACTS:
        tirtc_demo_pixel_create_contacts(false);
        break;
    case TIRTC_DEMO_VIEW_WECHAT:
        tirtc_demo_pixel_call_home(true);
        break;
    case TIRTC_DEMO_VIEW_WECHAT_ADD:
        tirtc_demo_pixel_create_add_contact(true);
        break;
    case TIRTC_DEMO_VIEW_WECHAT_ADD_EDIT:
        tirtc_demo_pixel_create_add_contact_edit(true);
        break;
    case TIRTC_DEMO_VIEW_WECHAT_SCAN_INFO:
        tirtc_demo_pixel_create_scan_info(true);
        break;
    case TIRTC_DEMO_VIEW_WECHAT_CONTACTS:
        tirtc_demo_pixel_create_contacts(true);
        break;
    case TIRTC_DEMO_VIEW_WECHAT_ACTIVE:
        tirtc_demo_pixel_create_active_call(true);
        break;
    case TIRTC_DEMO_VIEW_WECHAT_INCOMING:
        tirtc_demo_pixel_create_active_call(true);
        break;
    case TIRTC_DEMO_VIEW_AI_TALK:
        tirtc_demo_pixel_create_ai();
        break;
    case TIRTC_DEMO_VIEW_AI_SETTINGS:
        tirtc_demo_pixel_create_ai_settings();
        break;
    case TIRTC_DEMO_VIEW_SYSTEM:
        tirtc_demo_pixel_create_system();
        break;
    case TIRTC_DEMO_VIEW_CONFIG:
        tirtc_demo_pixel_create_config();
        break;
    case TIRTC_DEMO_VIEW_CONFIG_EDIT:
        tirtc_demo_pixel_create_config_edit();
        break;
    case TIRTC_DEMO_VIEW_DEVICE_INFO:
        tirtc_demo_pixel_create_device_info();
        break;
    case TIRTC_DEMO_VIEW_DEVICE_QR:
        tirtc_demo_pixel_create_device_qr();
        break;
    case TIRTC_DEMO_VIEW_DIAGNOSTICS:
        tirtc_demo_pixel_create_diagnostics();
        break;
    case TIRTC_DEMO_VIEW_TIRTC_TEST:
        tirtc_demo_pixel_create_tirtc_test();
        break;
    case TIRTC_DEMO_VIEW_HOME:
    default:
        tirtc_demo_pixel_create_home();
        break;
    }
}

#endif
