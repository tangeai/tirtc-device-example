#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "third_party/lvgl/lvgl/lvgl.h"

#include "driver/rtc.h"
#include "application/app_include/ui_common.h"
#include "application/app_include/ui_config.h"
#include "application/app_status_bar/ui_status_bar.h"
#include "tirtc_demo_app.h"
#include "tirtc_demo_media.h"
#include "ui_tirtc_demo.h"

#define TIRTC_DEMO_BG_COLOR 0xE8F3FA
#define TIRTC_DEMO_SURFACE_COLOR 0xF7FBFE
#define TIRTC_DEMO_CARD_COLOR 0xFFFFFF
#define TIRTC_DEMO_BORDER_COLOR 0xD6E4EF
#define TIRTC_DEMO_TEXT_COLOR 0x10233B
#define TIRTC_DEMO_MUTED_COLOR 0x65768A
#define TIRTC_DEMO_PRIMARY_COLOR 0x2F82D7
#define TIRTC_DEMO_GREEN_COLOR 0x21C783
#define TIRTC_DEMO_RED_COLOR 0xD85D47
#define TIRTC_DEMO_WARNING_COLOR 0xF59E0B
#define TIRTC_DEMO_PURPLE_COLOR 0x7C5CFF

#define TIRTC_DEMO_PIXEL_UI_WIDTH 320
#define TIRTC_DEMO_PIXEL_UI_HEIGHT 240

#ifndef TIRTC_DEMO_USE_S3_PIXEL_UI
#define TIRTC_DEMO_USE_S3_PIXEL_UI 1
#endif

#ifndef TIRTC_DEMO_EMBED_S3_ASSETS
#define TIRTC_DEMO_EMBED_S3_ASSETS 1
#endif

#ifndef TIRTC_DEMO_USE_S3_FONT
#define TIRTC_DEMO_USE_S3_FONT 1
#endif

#ifndef TIRTC_DEMO_USE_S3_TEXT_IMAGES
#define TIRTC_DEMO_USE_S3_TEXT_IMAGES 1
#endif

typedef enum {
    TIRTC_DEMO_VIEW_HOME = 0,
    TIRTC_DEMO_VIEW_DETAIL,
    TIRTC_DEMO_VIEW_CALL,
    TIRTC_DEMO_VIEW_ACTIVE_CALL,
    TIRTC_DEMO_VIEW_ADD_CONTACT,
    TIRTC_DEMO_VIEW_CALL_ADD_EDIT,
    TIRTC_DEMO_VIEW_CONTACTS,
    TIRTC_DEMO_VIEW_WECHAT,
    TIRTC_DEMO_VIEW_WECHAT_ADD,
    TIRTC_DEMO_VIEW_WECHAT_ADD_EDIT,
    TIRTC_DEMO_VIEW_WECHAT_CONTACTS,
    TIRTC_DEMO_VIEW_WECHAT_ACTIVE,
    TIRTC_DEMO_VIEW_AI_TALK,
    TIRTC_DEMO_VIEW_SYSTEM,
    TIRTC_DEMO_VIEW_CONFIG,
    TIRTC_DEMO_VIEW_CONFIG_EDIT,
    TIRTC_DEMO_VIEW_DEVICE_INFO,
    TIRTC_DEMO_VIEW_DEVICE_QR,
    TIRTC_DEMO_VIEW_DIAGNOSTICS,
    TIRTC_DEMO_VIEW_CALL_SCAN_INFO,
    TIRTC_DEMO_VIEW_CALL_INCOMING,
    TIRTC_DEMO_VIEW_WECHAT_SCAN_INFO,
    TIRTC_DEMO_VIEW_WECHAT_INCOMING,
    TIRTC_DEMO_VIEW_AI_SETTINGS,
    TIRTC_DEMO_VIEW_WIFI,
    TIRTC_DEMO_VIEW_WIFI_CONNECT,
    TIRTC_DEMO_VIEW_CONFIG_SCAN,
    TIRTC_DEMO_VIEW_TIRTC_TEST,
} tirtc_demo_view_t;

typedef enum {
    TIRTC_DEMO_VOL_RX_DOWN = 0,
    TIRTC_DEMO_VOL_RX_UP,
    TIRTC_DEMO_VOL_RX_MUTE,
    TIRTC_DEMO_VOL_TX_DOWN,
    TIRTC_DEMO_VOL_TX_UP,
    TIRTC_DEMO_VOL_TX_MUTE,
} tirtc_demo_volume_action_t;

typedef enum {
    TIRTC_DEMO_ADD_REQUEST = 0,
} tirtc_demo_add_action_t;

typedef enum {
    TIRTC_DEMO_VENDOR_SETTINGS = PAGE_SETTING,
    TIRTC_DEMO_VENDOR_AUDIO = PAGE_AUDIO,
    TIRTC_DEMO_VENDOR_CAMERA = PAGE_CAM,
    TIRTC_DEMO_VENDOR_SCANCODE = PAGE_SCANCODE,
} tirtc_demo_vendor_page_t;

static lv_obj_t *g_page;
static lv_obj_t *g_state_label;
static lv_obj_t *g_stage_label;
static lv_obj_t *g_message_label;
static lv_obj_t *g_network_pill;
static lv_obj_t *g_identity_pill;
static lv_obj_t *g_peer_pill;
static lv_obj_t *g_call_duration_label;
static lv_obj_t *g_config_endpoint_ta;
static lv_obj_t *g_config_device_id_ta;
static lv_obj_t *g_config_secret_ta;
static lv_obj_t *g_config_error_label;
static lv_obj_t *g_contact_device_id_ta;
static lv_obj_t *g_wechat_open_id_ta;
static lv_obj_t *g_keyboard;
static lv_obj_t *g_remote_video_image;
static lv_obj_t *g_remote_video_placeholder;
static lv_timer_t *g_refresh_timer;
static tirtc_demo_view_t g_current_view = TIRTC_DEMO_VIEW_HOME;
static bool g_visible_incoming_call;
static size_t g_visible_contact_count;
static size_t g_visible_wechat_contact_count;
static uint32_t g_visible_cloud_generation;
static uint32_t g_visible_ai_generation;
static uint32_t g_visible_diagnostic_generation;
static tirtc_demo_state_t g_visible_status_state;
static bool g_visible_network_ready;
static bool g_visible_peer_connected;
static char g_visible_call_phase[24];
static char g_visible_ai_caption[256];
static char g_pending_contact_device_id[TIRTC_DEMO_DEVICE_ID_MAX];
static char g_pending_wechat_open_id[TIRTC_DEMO_WECHAT_OPENID_MAX];
static uint32_t g_remote_video_token;
static lv_img_dsc_t g_remote_video_dsc;

static void tirtc_demo_render_view(tirtc_demo_view_t view);
static void tirtc_demo_pixel_refresh(void);
static int tirtc_demo_content_width(void);
static lv_obj_t *tirtc_demo_create_config_field(lv_obj_t *content,
                                                 const char *label_text,
                                                 const char *value,
                                                 uint32_t max_length,
                                                 bool password);

static void tirtc_demo_delete_keyboard(void)
{
    if (g_keyboard != NULL) {
        lv_obj_del(g_keyboard);
        g_keyboard = NULL;
    }
}

static void tirtc_demo_stop_refresh_timer(void)
{
    if (g_refresh_timer != NULL) {
        lv_timer_del(g_refresh_timer);
        g_refresh_timer = NULL;
    }
}

static void tirtc_demo_release_remote_video(void)
{
    if (g_remote_video_token != 0U) {
        lv_img_cache_invalidate_src(&g_remote_video_dsc);
        tirtc_demo_media_release_remote_video(g_remote_video_token);
        g_remote_video_token = 0U;
        memset(&g_remote_video_dsc, 0, sizeof(g_remote_video_dsc));
    }
    g_remote_video_image = NULL;
    g_remote_video_placeholder = NULL;
}

static void tirtc_demo_update_remote_video(void)
{
    tirtc_demo_remote_video_t video;
    lv_img_header_t header;
    uint32_t previous_token;
    uint32_t zoom;
    int target_width;
    int target_height = 236;

    if (g_remote_video_image == NULL ||
        !tirtc_demo_media_acquire_remote_video(&video)) {
        return;
    }
    previous_token = g_remote_video_token;
    if (previous_token != 0U) {
        lv_img_cache_invalidate_src(&g_remote_video_dsc);
    }
    memset(&g_remote_video_dsc, 0, sizeof(g_remote_video_dsc));
    g_remote_video_dsc.header.cf = LV_IMG_CF_RAW;
    g_remote_video_dsc.data_size = (uint32_t)video.length;
    g_remote_video_dsc.data = video.data;
    if (lv_img_decoder_get_info(&g_remote_video_dsc, &header) != LV_RES_OK ||
        header.w == 0U || header.h == 0U) {
        lv_obj_add_flag(g_remote_video_image, LV_OBJ_FLAG_HIDDEN);
        tirtc_demo_media_release_remote_video(video.token);
        if (previous_token != 0U) {
            tirtc_demo_media_release_remote_video(previous_token);
        }
        g_remote_video_token = 0U;
        if (g_remote_video_placeholder != NULL) {
            lv_obj_clear_flag(g_remote_video_placeholder, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    target_width = tirtc_demo_content_width() - 48;
    zoom = (uint32_t)(target_width * 256 / header.w);
    if ((uint32_t)(header.h * zoom / 256U) > (uint32_t)target_height) {
        zoom = (uint32_t)(target_height * 256 / header.h);
    }
    if (zoom > 256U) {
        zoom = 256U;
    } else if (zoom < 16U) {
        zoom = 16U;
    }
    lv_img_set_src(g_remote_video_image, &g_remote_video_dsc);
    lv_img_set_zoom(g_remote_video_image, (uint16_t)zoom);
    lv_obj_center(g_remote_video_image);
    lv_obj_clear_flag(g_remote_video_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(g_remote_video_image);
    if (g_remote_video_placeholder != NULL) {
        lv_obj_add_flag(g_remote_video_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
    g_remote_video_token = video.token;
    if (previous_token != 0U) {
        tirtc_demo_media_release_remote_video(previous_token);
    }
}

static int tirtc_demo_content_width(void)
{
    int width = (int)get_screen_width() - 24;

    return width > 0 ? width : (int)get_screen_width();
}

static uint32_t tirtc_demo_ready_color(int value)
{
    return value ? TIRTC_DEMO_GREEN_COLOR : TIRTC_DEMO_WARNING_COLOR;
}

static lv_obj_t *tirtc_demo_create_label(lv_obj_t *parent, const char *text,
                                         const lv_font_t *font, uint32_t color,
                                         int width, lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, align, 0);
    if (width > 0) {
        lv_obj_set_width(label, width);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    }

    return label;
}

static lv_obj_t *tirtc_demo_create_button(lv_obj_t *parent, const char *text,
                                          int width, int height, uint32_t color,
                                          lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, 6, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);

    lv_obj_t *label = tirtc_demo_create_label(button, text, &jz_ui_font_16,
                                              0xFFFFFF, width - 8,
                                              LV_TEXT_ALIGN_CENTER);
    lv_obj_center(label);

    if (cb != NULL) {
        lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user_data);
    }

    return button;
}

static lv_obj_t *tirtc_demo_create_card(lv_obj_t *parent, int height)
{
    lv_obj_t *card = lv_obj_create(parent);

    lv_obj_set_size(card, tirtc_demo_content_width(), height);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(TIRTC_DEMO_CARD_COLOR), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(TIRTC_DEMO_BORDER_COLOR), 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    return card;
}

static lv_obj_t *tirtc_demo_create_pill(lv_obj_t *parent, const char *text,
                                        int x, int y, uint32_t color)
{
    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_set_size(pill, 86, 28);
    lv_obj_set_pos(pill, x, y);
    lv_obj_set_style_radius(pill, 14, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(pill, 0, 0);
    lv_obj_set_style_pad_all(pill, 0, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = tirtc_demo_create_label(pill, text, &jz_ui_font_12,
                                              0xFFFFFF, 78,
                                              LV_TEXT_ALIGN_CENTER);
    lv_obj_center(label);

    return pill;
}

static void tirtc_demo_update_status_labels(void)
{
    tirtc_demo_status_t status;
    tirtc_demo_runtime_t runtime;
    lv_obj_t *label;

    tirtc_demo_app_get_status(&status);
    tirtc_demo_app_get_runtime(&runtime);

    if (g_state_label != NULL) {
        lv_label_set_text_fmt(g_state_label, "状态: %s", tirtc_demo_app_state_name(status.state));
    }

    if (g_stage_label != NULL) {
        lv_label_set_text_fmt(g_stage_label, "服务: %s", status.stage);
    }

    if (g_message_label != NULL) {
        lv_label_set_text(g_message_label, status.message);
    }

    if (g_network_pill != NULL) {
        label = lv_obj_get_child(g_network_pill, 0);
        lv_label_set_text(label, runtime.network_ready ? "网络就绪" : "网络待连");
        lv_obj_set_style_bg_color(g_network_pill,
                                  lv_color_hex(tirtc_demo_ready_color(runtime.network_ready)), 0);
    }
    if (g_identity_pill != NULL) {
        label = lv_obj_get_child(g_identity_pill, 0);
        lv_label_set_text(label, runtime.identity_ready ? "凭据已配" : "凭据未配");
        lv_obj_set_style_bg_color(g_identity_pill,
                                  lv_color_hex(tirtc_demo_ready_color(runtime.identity_ready)), 0);
    }
    if (g_peer_pill != NULL) {
        label = lv_obj_get_child(g_peer_pill, 0);
        lv_label_set_text(label, runtime.peer_connected ? "对端已连" : "等待对端");
        lv_obj_set_style_bg_color(g_peer_pill,
                                  lv_color_hex(tirtc_demo_ready_color(runtime.peer_connected)), 0);
    }
}

static void tirtc_demo_refresh_timer_callback(lv_timer_t *timer)
{
    tirtc_demo_status_t status;
    tirtc_demo_runtime_t runtime;
    size_t contact_count;
    size_t wechat_contact_count;
    bool incoming_started;
    bool call_changed;
    bool contacts_changed;
    bool wechat_contacts_changed;
    bool cloud_changed;
    bool ai_caption_changed;
    bool diagnostic_changed;
    bool connection_changed;
    char ai_caption[TIRTC_DEMO_AI_CAPTION_MAX];

    (void)timer;
    tirtc_demo_app_get_status(&status);
    tirtc_demo_app_get_runtime(&runtime);
    tirtc_demo_app_copy_ai_caption(ai_caption, sizeof(ai_caption));
#if TIRTC_DEMO_USE_S3_PIXEL_UI
    tirtc_demo_pixel_refresh();
#endif
    if (g_call_duration_label != NULL) {
        lv_label_set_text_fmt(g_call_duration_label, "%lu:%02lu",
                              (unsigned long)(runtime.call_duration_s / 60U),
                              (unsigned long)(runtime.call_duration_s % 60U));
    }
    contact_count = tirtc_demo_app_get_contacts(NULL);
    wechat_contact_count = tirtc_demo_app_get_wechat_contacts(NULL);
    incoming_started = runtime.incoming_call && !g_visible_incoming_call;
    call_changed = runtime.incoming_call != g_visible_incoming_call ||
                   strcmp(runtime.call_phase, g_visible_call_phase) != 0;
    contacts_changed = contact_count != g_visible_contact_count;
    wechat_contacts_changed =
        wechat_contact_count != g_visible_wechat_contact_count;
    cloud_changed = runtime.cloud_generation != g_visible_cloud_generation;
    ai_caption_changed = runtime.ai_generation != g_visible_ai_generation ||
        strcmp(ai_caption, g_visible_ai_caption) != 0;
    diagnostic_changed = runtime.diagnostic_generation !=
                         g_visible_diagnostic_generation;
    connection_changed = runtime.network_ready != g_visible_network_ready ||
                         runtime.peer_connected != g_visible_peer_connected ||
                         status.state != g_visible_status_state;
    g_visible_incoming_call = runtime.incoming_call;
    g_visible_contact_count = contact_count;
    g_visible_wechat_contact_count = wechat_contact_count;
    g_visible_cloud_generation = runtime.cloud_generation;
    g_visible_ai_generation = runtime.ai_generation;
    g_visible_diagnostic_generation = runtime.diagnostic_generation;
    g_visible_status_state = status.state;
    g_visible_network_ready = runtime.network_ready;
    g_visible_peer_connected = runtime.peer_connected;
    snprintf(g_visible_call_phase, sizeof(g_visible_call_phase), "%s",
             runtime.call_phase != NULL ? runtime.call_phase : "idle");
    snprintf(g_visible_ai_caption, sizeof(g_visible_ai_caption), "%s",
             ai_caption);

    if (incoming_started) {
        tirtc_demo_render_view(runtime.wechat_incoming ?
                                  TIRTC_DEMO_VIEW_WECHAT_INCOMING :
                                  TIRTC_DEMO_VIEW_CALL_INCOMING);
        return;
    }
    if (g_current_view == TIRTC_DEMO_VIEW_ACTIVE_CALL) {
        tirtc_demo_update_remote_video();
    }
    if (connection_changed &&
        (g_current_view == TIRTC_DEMO_VIEW_DETAIL ||
         g_current_view == TIRTC_DEMO_VIEW_CALL ||
         g_current_view == TIRTC_DEMO_VIEW_ACTIVE_CALL ||
         g_current_view == TIRTC_DEMO_VIEW_SYSTEM ||
         g_current_view == TIRTC_DEMO_VIEW_DEVICE_INFO ||
         g_current_view == TIRTC_DEMO_VIEW_DIAGNOSTICS ||
         g_current_view == TIRTC_DEMO_VIEW_TIRTC_TEST)) {
        tirtc_demo_render_view(g_current_view);
        return;
    }
    if (call_changed &&
        (g_current_view == TIRTC_DEMO_VIEW_CALL ||
         g_current_view == TIRTC_DEMO_VIEW_ACTIVE_CALL ||
         g_current_view == TIRTC_DEMO_VIEW_CALL_INCOMING ||
         g_current_view == TIRTC_DEMO_VIEW_WECHAT ||
         g_current_view == TIRTC_DEMO_VIEW_WECHAT_ACTIVE ||
         g_current_view == TIRTC_DEMO_VIEW_WECHAT_INCOMING ||
         g_current_view == TIRTC_DEMO_VIEW_AI_TALK)) {
        tirtc_demo_view_t target = g_current_view;

        if (!runtime.incoming_call &&
            strcmp(runtime.call_phase, "idle") == 0) {
            if (target == TIRTC_DEMO_VIEW_ACTIVE_CALL) {
                target = TIRTC_DEMO_VIEW_CALL;
            } else if (target == TIRTC_DEMO_VIEW_CALL_INCOMING) {
                target = TIRTC_DEMO_VIEW_CALL;
            } else if (target == TIRTC_DEMO_VIEW_WECHAT_ACTIVE) {
                target = TIRTC_DEMO_VIEW_WECHAT;
            } else if (target == TIRTC_DEMO_VIEW_WECHAT_INCOMING) {
                target = TIRTC_DEMO_VIEW_WECHAT;
            }
        }
        tirtc_demo_render_view(target);
        return;
    }
    if (((contacts_changed || cloud_changed) &&
         g_current_view == TIRTC_DEMO_VIEW_CONTACTS) ||
        ((wechat_contacts_changed || cloud_changed) &&
         g_current_view == TIRTC_DEMO_VIEW_WECHAT_CONTACTS) ||
        (ai_caption_changed && g_current_view == TIRTC_DEMO_VIEW_AI_TALK) ||
        (diagnostic_changed &&
         g_current_view == TIRTC_DEMO_VIEW_DIAGNOSTICS) ||
        (cloud_changed &&
         (g_current_view == TIRTC_DEMO_VIEW_HOME ||
          g_current_view == TIRTC_DEMO_VIEW_CONFIG))) {
        tirtc_demo_render_view(g_current_view);
        return;
    }
    tirtc_demo_update_status_labels();
}

static void tirtc_demo_back_to_main_callback(lv_event_t *e)
{
    (void)e;

    g_current_view = TIRTC_DEMO_VIEW_HOME;
    tirtc_demo_release_remote_video();
    tirtc_demo_delete_keyboard();
    tirtc_demo_stop_refresh_timer();
    tirtc_demo_app_exit();
    ui_switch_page(PAGE_MAIN);
    ui_set_page(NULL, PAGE_TIRTC_DEMO);
    g_page = NULL;
}

static void tirtc_demo_back_to_home_callback(lv_event_t *e)
{
    (void)e;

    if (g_current_view == TIRTC_DEMO_VIEW_CONFIG_EDIT) {
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_CONFIG);
        return;
    }
    (void)tirtc_demo_app_return_home();
    tirtc_demo_release_remote_video();
    tirtc_demo_render_view(TIRTC_DEMO_VIEW_HOME);
}

static void tirtc_demo_view_callback(lv_event_t *e)
{
    tirtc_demo_view_t view = (tirtc_demo_view_t)(uintptr_t)lv_event_get_user_data(e);

    if (view == TIRTC_DEMO_VIEW_AI_TALK) {
        tirtc_demo_runtime_t runtime;

        tirtc_demo_app_get_runtime(&runtime);
        if (!runtime.ai_ready && !runtime.service_starting &&
            (runtime.session_type == NULL ||
             strcmp(runtime.session_type, "ai") != 0)) {
            (void)tirtc_demo_app_handle_ai(TIRTC_DEMO_AI_CONNECT);
        }
    }
    tirtc_demo_render_view(view);
}

static void tirtc_demo_call_action_callback(lv_event_t *e)
{
    tirtc_demo_call_action_t action =
        (tirtc_demo_call_action_t)(uintptr_t)lv_event_get_user_data(e);

    int result = tirtc_demo_app_handle_call(action);

    if (result != 0) {
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_CALL);
    } else if (action == TIRTC_DEMO_CALL_REJECT ||
               action == TIRTC_DEMO_CALL_HANGUP) {
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_CALL);
    } else {
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_ACTIVE_CALL);
    }
}

static void tirtc_demo_ai_action_callback(lv_event_t *e)
{
    tirtc_demo_ai_action_t action =
        (tirtc_demo_ai_action_t)(uintptr_t)lv_event_get_user_data(e);

    (void)tirtc_demo_app_handle_ai(action);
    tirtc_demo_render_view(TIRTC_DEMO_VIEW_AI_TALK);
}

static void tirtc_demo_ai_new_session_callback(lv_event_t *e)
{
    (void)e;

    (void)tirtc_demo_app_handle_ai(TIRTC_DEMO_AI_NEW_CHAT);
    (void)tirtc_demo_app_handle_ai(TIRTC_DEMO_AI_CONNECT);
    tirtc_demo_render_view(TIRTC_DEMO_VIEW_AI_TALK);
}

static void tirtc_demo_volume_callback(lv_event_t *e)
{
    tirtc_demo_volume_action_t action =
        (tirtc_demo_volume_action_t)(uintptr_t)lv_event_get_user_data(e);

    switch (action) {
    case TIRTC_DEMO_VOL_RX_DOWN:
        (void)tirtc_demo_app_adjust_volume(TIRTC_DEMO_VOLUME_RX, -10);
        break;
    case TIRTC_DEMO_VOL_RX_UP:
        (void)tirtc_demo_app_adjust_volume(TIRTC_DEMO_VOLUME_RX, 10);
        break;
    case TIRTC_DEMO_VOL_RX_MUTE:
        (void)tirtc_demo_app_toggle_mute(TIRTC_DEMO_VOLUME_RX);
        break;
    case TIRTC_DEMO_VOL_TX_DOWN:
        (void)tirtc_demo_app_adjust_volume(TIRTC_DEMO_VOLUME_TX, -10);
        break;
    case TIRTC_DEMO_VOL_TX_UP:
        (void)tirtc_demo_app_adjust_volume(TIRTC_DEMO_VOLUME_TX, 10);
        break;
    case TIRTC_DEMO_VOL_TX_MUTE:
        (void)tirtc_demo_app_toggle_mute(TIRTC_DEMO_VOLUME_TX);
        break;
    default:
        break;
    }

    tirtc_demo_render_view(g_current_view);
}

static void tirtc_demo_open_vendor_page(page_id_t page)
{
    bool resource_sensitive = page == PAGE_AUDIO || page == PAGE_CAM ||
                              page == PAGE_SCANCODE;

    if (resource_sensitive && tirtc_demo_app_prepare_external_media() != 0) {
        tirtc_demo_render_view(g_current_view);
        return;
    }
    tirtc_demo_release_remote_video();
    tirtc_demo_delete_keyboard();
    tirtc_demo_stop_refresh_timer();
    tirtc_demo_app_exit();
    ui_switch_page(page);
    ui_set_page(NULL, PAGE_TIRTC_DEMO);
    g_page = NULL;
}

static void tirtc_demo_vendor_page_callback(lv_event_t *e)
{
    tirtc_demo_open_vendor_page((page_id_t)(uintptr_t)lv_event_get_user_data(e));
}

static void tirtc_demo_add_contact_callback(lv_event_t *e)
{
    tirtc_demo_add_action_t action = (tirtc_demo_add_action_t)(uintptr_t)
        lv_event_get_user_data(e);

    if (action == TIRTC_DEMO_ADD_REQUEST) {
        if (tirtc_demo_app_request_contact(g_pending_contact_device_id) == 0) {
            g_pending_contact_device_id[0] = '\0';
            tirtc_demo_delete_keyboard();
            tirtc_demo_render_view(TIRTC_DEMO_VIEW_CONTACTS);
        } else {
            tirtc_demo_render_view(TIRTC_DEMO_VIEW_ADD_CONTACT);
        }
    }
}

static void tirtc_demo_contact_edit_save_callback(lv_event_t *e)
{
    const char *value;

    (void)e;
    if (g_contact_device_id_ta == NULL) {
        return;
    }
    value = lv_textarea_get_text(g_contact_device_id_ta);
    if (value == NULL || value[0] == '\0') {
        return;
    }
    snprintf(g_pending_contact_device_id,
             sizeof(g_pending_contact_device_id), "%s", value);
    tirtc_demo_delete_keyboard();
    tirtc_demo_render_view(TIRTC_DEMO_VIEW_ADD_CONTACT);
}

static void tirtc_demo_contacts_refresh_callback(lv_event_t *e)
{
    (void)e;
    (void)tirtc_demo_app_refresh_contacts();
    tirtc_demo_render_view(TIRTC_DEMO_VIEW_CONTACTS);
}

static void tirtc_demo_network_refresh_callback(lv_event_t *e)
{
    (void)e;

    (void)tirtc_demo_app_refresh_network();
    tirtc_demo_render_view(g_current_view);
}

static void tirtc_demo_binding_callback(lv_event_t *e)
{
    int result;

    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    printf("[tirtc_ui] binding refresh clicked view=%d\n",
           (int)g_current_view);
    result = tirtc_demo_app_request_binding();
    printf("[tirtc_ui] binding refresh result=%d stay=home\n", result);
    tirtc_demo_render_view(TIRTC_DEMO_VIEW_HOME);
}

static void tirtc_demo_reset_binding_callback(lv_event_t *e)
{
    int result;

    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    printf("[tirtc_ui] binding reset clicked\n");
    result = tirtc_demo_app_reset_binding();
    printf("[tirtc_ui] binding reset result=%d stay=config\n", result);
    tirtc_demo_render_view(TIRTC_DEMO_VIEW_CONFIG);
}

static void tirtc_demo_diagnostics_callback(lv_event_t *e)
{
    (void)e;

    (void)tirtc_demo_app_run_diagnostics();
    tirtc_demo_render_view(TIRTC_DEMO_VIEW_DIAGNOSTICS);
}

static void tirtc_demo_ai_avatar_callback(lv_event_t *e)
{
    tirtc_demo_runtime_t runtime;

    (void)e;
    tirtc_demo_app_get_runtime(&runtime);
    (void)tirtc_demo_app_set_ai_avatar(
        (uint8_t)((runtime.ai_avatar + 1U) % TIRTC_DEMO_AI_AVATAR_COUNT));
    tirtc_demo_render_view(TIRTC_DEMO_VIEW_AI_TALK);
}

static void tirtc_demo_restart_callback(lv_event_t *e)
{
    (void)e;

    (void)tirtc_demo_app_restart();
    tirtc_demo_render_view(TIRTC_DEMO_VIEW_CONFIG);
}

static void tirtc_demo_keyboard_callback(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        tirtc_demo_delete_keyboard();
    }
}

static void tirtc_demo_textarea_callback(lv_event_t *e)
{
    lv_obj_t *textarea = lv_event_get_target(e);

    if (lv_event_get_code(e) != LV_EVENT_FOCUSED) {
        return;
    }

    if (g_keyboard == NULL) {
        g_keyboard = lv_keyboard_create(g_page != NULL ? g_page : lv_layer_top());
#if TIRTC_DEMO_USE_S3_PIXEL_UI
        lv_obj_set_size(g_keyboard, TIRTC_DEMO_PIXEL_UI_WIDTH, 104);
        lv_obj_align(g_keyboard, LV_ALIGN_CENTER, 0, 68);
#else
        lv_obj_set_size(g_keyboard, LV_HOR_RES, LV_VER_RES / 3);
        lv_obj_align(g_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
#endif
        lv_obj_set_style_text_font(g_keyboard, &lv_font_montserrat_14, LV_PART_ITEMS);
        lv_obj_add_event_cb(g_keyboard, tirtc_demo_keyboard_callback, LV_EVENT_ALL, NULL);
    }
    lv_keyboard_set_textarea(g_keyboard, textarea);
}

static void tirtc_demo_save_config_callback(lv_event_t *e)
{
    tirtc_demo_config_t config = {0};
    int result;

    (void)e;
    if (g_config_endpoint_ta == NULL || g_config_device_id_ta == NULL ||
        g_config_secret_ta == NULL) {
        return;
    }

    snprintf(config.endpoint, sizeof(config.endpoint), "%s",
             lv_textarea_get_text(g_config_endpoint_ta));
    snprintf(config.device_id, sizeof(config.device_id), "%s",
             lv_textarea_get_text(g_config_device_id_ta));
    snprintf(config.device_secret, sizeof(config.device_secret), "%s",
             lv_textarea_get_text(g_config_secret_ta));
    result = tirtc_demo_app_save_config(&config);
    memset(config.device_secret, 0, sizeof(config.device_secret));

    if (result == 0) {
        tirtc_demo_delete_keyboard();
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_CONFIG);
    } else if (g_config_error_label != NULL) {
        lv_label_set_text(g_config_error_label, "保存失败，请检查服务地址和设备凭据");
        lv_obj_set_style_text_color(g_config_error_label,
                                    lv_color_hex(TIRTC_DEMO_RED_COLOR), 0);
    }
}

static void tirtc_demo_wechat_hangup_callback(lv_event_t *e)
{
    (void)e;

    (void)tirtc_demo_app_handle_call(TIRTC_DEMO_CALL_HANGUP);
    tirtc_demo_render_view(TIRTC_DEMO_VIEW_WECHAT);
}

static void tirtc_demo_wechat_refresh_callback(lv_event_t *e)
{
    (void)e;
    (void)tirtc_demo_app_refresh_wechat();
    tirtc_demo_render_view(TIRTC_DEMO_VIEW_WECHAT_CONTACTS);
}

static void tirtc_demo_wechat_add_callback(lv_event_t *e)
{
    (void)e;
    if (g_pending_wechat_open_id[0] == '\0') {
        return;
    }
    if (tirtc_demo_app_add_wechat_contact(g_pending_wechat_open_id) == 0) {
        g_pending_wechat_open_id[0] = '\0';
        tirtc_demo_delete_keyboard();
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_WECHAT_CONTACTS);
    } else {
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_WECHAT_ADD);
    }
}

static void tirtc_demo_wechat_edit_save_callback(lv_event_t *e)
{
    const char *value;

    (void)e;
    if (g_wechat_open_id_ta == NULL) {
        return;
    }
    value = lv_textarea_get_text(g_wechat_open_id_ta);
    if (value == NULL || value[0] == '\0') {
        return;
    }
    snprintf(g_pending_wechat_open_id,
             sizeof(g_pending_wechat_open_id), "%s", value);
    tirtc_demo_delete_keyboard();
    tirtc_demo_render_view(TIRTC_DEMO_VIEW_WECHAT_ADD);
}

static void tirtc_demo_wechat_call_callback(lv_event_t *e)
{
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(e);

    if (tirtc_demo_app_call_wechat(index) == 0) {
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_WECHAT_ACTIVE);
    } else {
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_WECHAT_CONTACTS);
    }
}

static void tirtc_demo_wechat_delete_callback(lv_event_t *e)
{
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(e);

    (void)tirtc_demo_app_delete_wechat_contact(index);
    tirtc_demo_render_view(TIRTC_DEMO_VIEW_WECHAT_CONTACTS);
}

static void tirtc_demo_wechat_answer_callback(lv_event_t *e)
{
    bool accept = (bool)(uintptr_t)lv_event_get_user_data(e);
    int result = accept ? tirtc_demo_app_accept_wechat() :
                          tirtc_demo_app_reject_wechat();

    if (result == 0 && accept) {
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_WECHAT_ACTIVE);
    } else {
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_WECHAT);
    }
}

static void tirtc_demo_call_contact_callback(lv_event_t *e)
{
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(e);

    if (tirtc_demo_app_call_contact(index) == 0) {
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_ACTIVE_CALL);
    } else {
        tirtc_demo_render_view(TIRTC_DEMO_VIEW_CONTACTS);
    }
}

static lv_obj_t *tirtc_demo_create_content(void)
{
    int top = UI_STATUS_BAR_HEIGHT + 72;
    lv_obj_t *content = lv_obj_create(g_page);

    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, tirtc_demo_content_width(), (int)get_screen_height() - top - 12);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content, 12, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);

    return content;
}

static void tirtc_demo_create_top_bar(const char *title, int home)
{
    lv_obj_t *bar = lv_obj_create(g_page);
    lv_obj_set_size(bar, tirtc_demo_content_width(), 52);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, UI_STATUS_BAR_HEIGHT + 8);
    lv_obj_set_style_radius(bar, 8, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(TIRTC_DEMO_SURFACE_COLOR), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(TIRTC_DEMO_BORDER_COLOR), 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = tirtc_demo_create_button(bar, "返回", 64, 34, 0x60758C,
                                              home ? tirtc_demo_back_to_main_callback :
                                                     tirtc_demo_back_to_home_callback,
                                              NULL);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t *title_label = tirtc_demo_create_label(bar, title, &jz_ui_font_20,
                                                    TIRTC_DEMO_TEXT_COLOR,
                                                    tirtc_demo_content_width() - 160,
                                                    LV_TEXT_ALIGN_CENTER);
    lv_obj_center(title_label);
}

static void tirtc_demo_create_status_strip(lv_obj_t *parent)
{
    tirtc_demo_runtime_t runtime;
    lv_obj_t *card = tirtc_demo_create_card(parent, 142);

    tirtc_demo_app_get_runtime(&runtime);

    g_state_label = tirtc_demo_create_label(card, "", &jz_ui_font_16,
                                            TIRTC_DEMO_TEXT_COLOR, 0,
                                            LV_TEXT_ALIGN_LEFT);
    lv_obj_align(g_state_label, LV_ALIGN_TOP_LEFT, 12, 10);

    g_stage_label = tirtc_demo_create_label(card, "", &jz_ui_font_12,
                                            TIRTC_DEMO_MUTED_COLOR,
                                            tirtc_demo_content_width() - 48,
                                            LV_TEXT_ALIGN_LEFT);
    lv_obj_align(g_stage_label, LV_ALIGN_TOP_LEFT, 12, 38);

    g_message_label = tirtc_demo_create_label(card, "", &jz_ui_font_12,
                                              TIRTC_DEMO_MUTED_COLOR,
                                              tirtc_demo_content_width() - 48,
                                              LV_TEXT_ALIGN_LEFT);
    lv_obj_align(g_message_label, LV_ALIGN_TOP_LEFT, 12, 62);

    g_network_pill = tirtc_demo_create_pill(card,
                                            runtime.network_ready ? "网络就绪" : "网络待连",
                                            12, 100,
                                            tirtc_demo_ready_color(runtime.network_ready));
    g_identity_pill = tirtc_demo_create_pill(card,
                                             runtime.identity_ready ? "凭据已配" : "凭据未配",
                                             106, 100,
                                             tirtc_demo_ready_color(runtime.identity_ready));
    g_peer_pill = tirtc_demo_create_pill(card,
                                         runtime.peer_connected ? "对端已连" : "等待对端",
                                         200, 100,
                                         tirtc_demo_ready_color(runtime.peer_connected));

    tirtc_demo_update_status_labels();
}

static void tirtc_demo_create_feature_tile(lv_obj_t *parent, int x, int y, int width,
                                           const char *icon, const char *title,
                                           const char *desc, uint32_t color,
                                           tirtc_demo_view_t view)
{
    lv_obj_t *tile = lv_btn_create(parent);
    lv_obj_set_size(tile, width, 118);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_style_radius(tile, 8, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(TIRTC_DEMO_CARD_COLOR), 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(TIRTC_DEMO_BORDER_COLOR), 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_add_event_cb(tile, tirtc_demo_view_callback, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)view);

    lv_obj_t *chip = lv_obj_create(tile);
    lv_obj_set_size(chip, 42, 42);
    lv_obj_align(chip, LV_ALIGN_TOP_LEFT, 14, 12);
    lv_obj_set_style_radius(chip, 21, 0);
    lv_obj_set_style_bg_color(chip, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(chip, 0, 0);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon_label = tirtc_demo_create_label(chip, icon,
                                                   &lv_font_montserrat_16,
                                                   0xFFFFFF, 38, LV_TEXT_ALIGN_CENTER);
    lv_obj_center(icon_label);

    lv_obj_t *title_label = tirtc_demo_create_label(tile, title, &jz_ui_font_16,
                                                    TIRTC_DEMO_TEXT_COLOR,
                                                    width - 28, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 14, 62);

    lv_obj_t *desc_label = tirtc_demo_create_label(tile, desc, &jz_ui_font_12,
                                                   TIRTC_DEMO_MUTED_COLOR,
                                                   width - 28, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(desc_label, LV_ALIGN_TOP_LEFT, 14, 88);
}

static lv_obj_t *tirtc_demo_create_info_row(lv_obj_t *parent, int y,
                                            const char *name, const char *value,
                                            uint32_t value_color)
{
    int width = tirtc_demo_content_width();
    lv_obj_t *name_label = tirtc_demo_create_label(parent, name, &jz_ui_font_16,
                                                   TIRTC_DEMO_MUTED_COLOR,
                                                   142, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(name_label, LV_ALIGN_TOP_LEFT, 12, y);

    lv_obj_t *value_label = tirtc_demo_create_label(parent, value, &jz_ui_font_16,
                                                    value_color, width - 192,
                                                    LV_TEXT_ALIGN_RIGHT);
    lv_obj_align(value_label, LV_ALIGN_TOP_RIGHT, -12, y);
    return value_label;
}

static lv_obj_t *tirtc_demo_create_qrcode(lv_obj_t *parent,
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
        lv_qrcode_update(qrcode, payload, (uint32_t)strlen(payload)) !=
            LV_RES_OK) {
        if (qrcode != NULL) {
            lv_obj_del(qrcode);
        }
        return NULL;
    }
    lv_obj_clear_flag(qrcode, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return qrcode;
}

static void tirtc_demo_create_device_qr_card(lv_obj_t *content, bool full)
{
    char device_id[TIRTC_DEMO_DEVICE_ID_MAX];
    int size = full ? 320 : 196;
    int height = size + (full ? 104 : 132);
    int width = tirtc_demo_content_width();
    lv_obj_t *card = tirtc_demo_create_card(content, height);
    lv_obj_t *qrcode;
    lv_obj_t *label;

    tirtc_demo_app_copy_device_id(device_id, sizeof(device_id));

    label = tirtc_demo_create_label(card,
                                    full ? "本机设备二维码" : "我的设备",
                                    &jz_ui_font_16, TIRTC_DEMO_TEXT_COLOR,
                                    width - 48, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 4);
    qrcode = tirtc_demo_create_qrcode(card, device_id, size);
    if (qrcode != NULL) {
        lv_obj_align(qrcode, LV_ALIGN_TOP_MID, 0, 34);
    } else {
        label = tirtc_demo_create_label(card, "完成设备绑定后显示二维码",
                                        &jz_ui_font_16,
                                        TIRTC_DEMO_MUTED_COLOR,
                                        width - 48, LV_TEXT_ALIGN_CENTER);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 104);
    }
    label = tirtc_demo_create_label(card, device_id, &jz_ui_font_16,
                                    TIRTC_DEMO_TEXT_COLOR, width - 48,
                                    LV_TEXT_ALIGN_CENTER);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, size + 44);
    if (!full) {
        lv_obj_t *button = tirtc_demo_create_button(
            card, "全屏显示", width - 48, 40, TIRTC_DEMO_PRIMARY_COLOR,
            tirtc_demo_view_callback,
            (void *)(uintptr_t)TIRTC_DEMO_VIEW_DEVICE_QR);
        lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -10);
    }
}

static void tirtc_demo_create_binding_card(lv_obj_t *content,
                                            const tirtc_demo_runtime_t *runtime)
{
    int width = tirtc_demo_content_width();
    lv_obj_t *card;
    lv_obj_t *label;
    lv_obj_t *button;
    const char *code;

    if (runtime == NULL || runtime->identity_ready) {
        return;
    }
    code = runtime->binding_waiting && runtime->binding_code != NULL &&
                   runtime->binding_code[0] != '\0' ?
               runtime->binding_code : "申请中";
    card = tirtc_demo_create_card(content, 170);
    label = tirtc_demo_create_label(card, "设备自动绑定", &jz_ui_font_16,
                                    TIRTC_DEMO_TEXT_COLOR, width - 48,
                                    LV_TEXT_ALIGN_CENTER);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 4);
    label = tirtc_demo_create_label(card, code, &jz_ui_font_20,
                                    TIRTC_DEMO_PURPLE_COLOR, width - 48,
                                    LV_TEXT_ALIGN_CENTER);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 42);
    label = tirtc_demo_create_label(
        card, "在管理端输入验证码完成绑定；设备会自动上线。",
        &jz_ui_font_12, TIRTC_DEMO_MUTED_COLOR, width - 48,
        LV_TEXT_ALIGN_CENTER);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 78);
    button = tirtc_demo_create_button(card, "重新申请", width - 48, 40,
                                      TIRTC_DEMO_PURPLE_COLOR,
                                      tirtc_demo_binding_callback, NULL);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -10);
}

static void tirtc_demo_create_home(lv_obj_t *content)
{
    int width = tirtc_demo_content_width();
    int tile_width = (width - 12) / 2;
    lv_obj_t *grid;
    tirtc_demo_runtime_t runtime;

    tirtc_demo_app_get_runtime(&runtime);
    tirtc_demo_create_status_strip(content);
    tirtc_demo_create_binding_card(content, &runtime);

    grid = lv_obj_create(content);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, width, 386);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    tirtc_demo_create_feature_tile(grid, 0, 0, tile_width,
                                   LV_SYMBOL_EYE_OPEN, "设备查看",
                                   "连接状态与音量", TIRTC_DEMO_GREEN_COLOR,
                                   TIRTC_DEMO_VIEW_DETAIL);
    tirtc_demo_create_feature_tile(grid, tile_width + 12, 0, tile_width,
                                   LV_SYMBOL_CALL, "呼叫",
                                   "设备间发起呼叫", TIRTC_DEMO_PRIMARY_COLOR,
                                   TIRTC_DEMO_VIEW_CALL);
    tirtc_demo_create_feature_tile(grid, 0, 134, tile_width,
                                   LV_SYMBOL_ENVELOPE, "微信通话",
                                   "联系人与通话", TIRTC_DEMO_RED_COLOR,
                                   TIRTC_DEMO_VIEW_WECHAT);
    tirtc_demo_create_feature_tile(grid, tile_width + 12, 134, tile_width,
                                   LV_SYMBOL_LIST, "联系人",
                                   "同步列表与设备呼叫", TIRTC_DEMO_PURPLE_COLOR,
                                   TIRTC_DEMO_VIEW_CONTACTS);
    tirtc_demo_create_feature_tile(grid, 0, 268, tile_width,
                                   LV_SYMBOL_AUDIO, "AI对讲",
                                   "持续聆听与字幕", 0x14A6A0,
                                   TIRTC_DEMO_VIEW_AI_TALK);
    tirtc_demo_create_feature_tile(grid, tile_width + 12, 268, tile_width,
                                   LV_SYMBOL_SETTINGS, "系统",
                                   "配置/网络/设备信息", 0x60758C,
                                   TIRTC_DEMO_VIEW_SYSTEM);
}

static void tirtc_demo_create_volume_card(lv_obj_t *content, const char *title,
                                          tirtc_demo_volume_t volume,
                                          tirtc_demo_volume_action_t down_action,
                                          tirtc_demo_volume_action_t up_action,
                                          tirtc_demo_volume_action_t mute_action)
{
    int width = tirtc_demo_content_width();
    int value = tirtc_demo_app_get_volume(volume);
    int muted = tirtc_demo_app_is_muted(volume) ? 1 : 0;
    lv_obj_t *card = tirtc_demo_create_card(content, 128);
    char value_text[32];

    snprintf(value_text, sizeof(value_text), "%d%s", value, muted ? " / 已禁音" : "");

    lv_obj_t *title_label = tirtc_demo_create_label(card, title, &jz_ui_font_16,
                                                    TIRTC_DEMO_TEXT_COLOR, 0,
                                                    LV_TEXT_ALIGN_LEFT);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 12, 12);

    lv_obj_t *value_label = tirtc_demo_create_label(card, value_text, &jz_ui_font_20,
                                                    muted ? TIRTC_DEMO_RED_COLOR :
                                                            TIRTC_DEMO_GREEN_COLOR,
                                                    width - 40, LV_TEXT_ALIGN_RIGHT);
    lv_obj_align(value_label, LV_ALIGN_TOP_RIGHT, -12, 10);

    lv_obj_t *down = tirtc_demo_create_button(card, "-10", 78, 38, 0x60758C,
                                              tirtc_demo_volume_callback,
                                              (void *)(uintptr_t)down_action);
    lv_obj_align(down, LV_ALIGN_BOTTOM_LEFT, 12, -12);

    lv_obj_t *up = tirtc_demo_create_button(card, "+10", 78, 38,
                                            TIRTC_DEMO_PRIMARY_COLOR,
                                            tirtc_demo_volume_callback,
                                            (void *)(uintptr_t)up_action);
    lv_obj_align(up, LV_ALIGN_BOTTOM_LEFT, 102, -12);

    lv_obj_t *mute = tirtc_demo_create_button(card, muted ? "取消禁音" : "禁音",
                                              width >= 420 ? 104 : 90, 38,
                                              muted ? TIRTC_DEMO_GREEN_COLOR :
                                                      TIRTC_DEMO_RED_COLOR,
                                              tirtc_demo_volume_callback,
                                              (void *)(uintptr_t)mute_action);
    lv_obj_align(mute, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
}

static void tirtc_demo_create_detail(lv_obj_t *content)
{
    tirtc_demo_status_t status;
    tirtc_demo_runtime_t runtime;
    tirtc_demo_settings_t settings;
    lv_obj_t *status_card;
    lv_obj_t *device_card;
    char media_text[48];
    char device_id[TIRTC_DEMO_DEVICE_ID_MAX];

    tirtc_demo_app_get_status(&status);
    tirtc_demo_app_get_runtime(&runtime);
    tirtc_demo_app_get_settings(&settings);
    tirtc_demo_app_copy_device_id(device_id, sizeof(device_id));

    status_card = tirtc_demo_create_card(content, 176);
    tirtc_demo_create_info_row(status_card, 10, "连接状态",
                               tirtc_demo_app_state_name(status.state),
                               status.state == TIRTC_DEMO_STATE_CONNECTED ||
                                       status.state == TIRTC_DEMO_STATE_IN_CALL ?
                                   TIRTC_DEMO_GREEN_COLOR : TIRTC_DEMO_WARNING_COLOR);
    tirtc_demo_create_info_row(status_card, 46, "开放指示",
                               status.state == TIRTC_DEMO_STATE_IDLE ? "未开放" : "已开放",
                               status.state == TIRTC_DEMO_STATE_IDLE ?
                                   TIRTC_DEMO_RED_COLOR : TIRTC_DEMO_GREEN_COLOR);
    tirtc_demo_create_info_row(status_card, 82, "当前阶段",
                               status.stage, TIRTC_DEMO_TEXT_COLOR);
    tirtc_demo_create_info_row(status_card, 118, "通话阶段",
                               runtime.call_phase, TIRTC_DEMO_TEXT_COLOR);

    tirtc_demo_create_volume_card(content, "接收音量", TIRTC_DEMO_VOLUME_RX,
                                  TIRTC_DEMO_VOL_RX_DOWN, TIRTC_DEMO_VOL_RX_UP,
                                  TIRTC_DEMO_VOL_RX_MUTE);
    tirtc_demo_create_volume_card(content, "发送音量", TIRTC_DEMO_VOLUME_TX,
                                  TIRTC_DEMO_VOL_TX_DOWN, TIRTC_DEMO_VOL_TX_UP,
                                  TIRTC_DEMO_VOL_TX_MUTE);

    device_card = tirtc_demo_create_card(content, 176);
    snprintf(media_text, sizeof(media_text), "%s %dk/%dch/%dms",
             settings.codec, settings.sample_rate / 1000, settings.channels,
             settings.packet_ms);
    tirtc_demo_create_info_row(device_card, 10, "设备 ID", device_id,
                               TIRTC_DEMO_TEXT_COLOR);
    tirtc_demo_create_info_row(device_card, 46, "设备绑定",
                               runtime.identity_ready ? "已绑定" : "未绑定",
                               tirtc_demo_ready_color(runtime.identity_ready));
    tirtc_demo_create_info_row(device_card, 82, "音频格式", media_text,
                               TIRTC_DEMO_TEXT_COLOR);
    tirtc_demo_create_info_row(device_card, 118, "SDK", tirtc_demo_app_sdk_version(),
                               runtime.sdk_ready ? TIRTC_DEMO_GREEN_COLOR :
                                                   TIRTC_DEMO_WARNING_COLOR);
    tirtc_demo_create_device_qr_card(content, false);
}

static void tirtc_demo_create_call(lv_obj_t *content)
{
    int width = tirtc_demo_content_width();
    int button_width = (width - 36) / 2;
    tirtc_demo_runtime_t runtime;
    lv_obj_t *summary = tirtc_demo_create_card(content, 176);
    lv_obj_t *actions = tirtc_demo_create_card(content, 132);
    lv_obj_t *button;

    tirtc_demo_app_get_runtime(&runtime);
    tirtc_demo_create_info_row(summary, 10, "设备绑定",
                               runtime.identity_ready ? "已绑定" : "未绑定",
                               tirtc_demo_ready_color(runtime.identity_ready));
    tirtc_demo_create_info_row(summary, 46, "系统网络",
                               runtime.network_ready ? "已连接" : "未连接",
                               tirtc_demo_ready_color(runtime.network_ready));
    tirtc_demo_create_info_row(summary, 82, "对端连接",
                               runtime.peer_connected ? "已连接" : "等待连接",
                               tirtc_demo_ready_color(runtime.peer_connected));
    tirtc_demo_create_info_row(summary, 118, "当前对端", runtime.peer_id,
                               TIRTC_DEMO_TEXT_COLOR);

    tirtc_demo_create_device_qr_card(content, false);

    button = tirtc_demo_create_button(actions, "添加联系人", button_width, 42,
                                      TIRTC_DEMO_PRIMARY_COLOR,
                                      tirtc_demo_view_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_VIEW_ADD_CONTACT);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, 12, 12);

    button = tirtc_demo_create_button(actions, "联系人列表", button_width, 42,
                                      TIRTC_DEMO_PURPLE_COLOR,
                                      tirtc_demo_view_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_VIEW_CONTACTS);
    lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -12, 12);

    button = tirtc_demo_create_button(actions, "当前通话", button_width, 42,
                                      TIRTC_DEMO_GREEN_COLOR,
                                      tirtc_demo_view_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_VIEW_ACTIVE_CALL);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, 12, 68);

    button = tirtc_demo_create_button(actions, "设备配置", button_width, 42,
                                      0x60758C,
                                      tirtc_demo_view_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_VIEW_CONFIG);
    lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -12, 68);

    tirtc_demo_create_status_strip(content);
}

static void tirtc_demo_create_active_call(lv_obj_t *content)
{
    int width = tirtc_demo_content_width();
    int button_width = (width - 36) / 2;
    tirtc_demo_runtime_t runtime;
    lv_obj_t *session = tirtc_demo_create_card(content, 256);
    lv_obj_t *actions = tirtc_demo_create_card(content, 132);
    lv_obj_t *button;
    char duration[16];

    tirtc_demo_app_get_runtime(&runtime);
    snprintf(duration, sizeof(duration), "%lu:%02lu",
             (unsigned long)(runtime.call_duration_s / 60U),
             (unsigned long)(runtime.call_duration_s % 60U));

    tirtc_demo_create_info_row(session, 10, "对端设备", runtime.peer_id,
                               TIRTC_DEMO_TEXT_COLOR);
    tirtc_demo_create_info_row(session, 46, "通话阶段", runtime.call_phase,
                               TIRTC_DEMO_PRIMARY_COLOR);
    tirtc_demo_create_info_row(session, 82, "来电", runtime.incoming_call ? "等待处理" : "无",
                               runtime.incoming_call ? TIRTC_DEMO_WARNING_COLOR :
                                                       TIRTC_DEMO_MUTED_COLOR);
    tirtc_demo_create_info_row(session, 118, "音频发送",
                               runtime.audio_tx_active ? "开启" : "关闭",
                               runtime.audio_tx_active ? TIRTC_DEMO_GREEN_COLOR :
                                                         TIRTC_DEMO_WARNING_COLOR);
    tirtc_demo_create_info_row(session, 154, "远端画面",
                               runtime.video_rx_active ? "已接收" : "等待 MJPEG",
                               runtime.video_rx_active ? TIRTC_DEMO_GREEN_COLOR :
                                                         TIRTC_DEMO_MUTED_COLOR);
    g_call_duration_label = tirtc_demo_create_info_row(
        session, 190, "通话时长", duration, TIRTC_DEMO_GREEN_COLOR);

    if (runtime.incoming_call) {
        button = tirtc_demo_create_button(actions, "接听", button_width, 42,
                                          TIRTC_DEMO_GREEN_COLOR,
                                          tirtc_demo_call_action_callback,
                                          (void *)(uintptr_t)TIRTC_DEMO_CALL_ACCEPT);
        lv_obj_align(button, LV_ALIGN_TOP_LEFT, 12, 12);

        button = tirtc_demo_create_button(actions, "拒绝", button_width, 42,
                                          0x60758C, tirtc_demo_call_action_callback,
                                          (void *)(uintptr_t)TIRTC_DEMO_CALL_REJECT);
        lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -12, 12);
    } else {
        lv_obj_t *label = tirtc_demo_create_label(actions, "当前没有待接来电",
                                                  &jz_ui_font_16,
                                                  TIRTC_DEMO_MUTED_COLOR,
                                                  width - 48,
                                                  LV_TEXT_ALIGN_CENTER);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 20);
    }

    button = tirtc_demo_create_button(actions, "挂断", width - 24, 42,
                                      TIRTC_DEMO_RED_COLOR,
                                      tirtc_demo_call_action_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_CALL_HANGUP);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -10);
    if (!runtime.peer_connected && !runtime.incoming_call &&
        strcmp(runtime.call_phase, "outgoing") != 0 &&
        strcmp(runtime.call_phase, "connecting") != 0) {
        lv_obj_add_state(button, LV_STATE_DISABLED);
    }

    {
        lv_obj_t *video_card = tirtc_demo_create_card(content, 300);
        lv_obj_t *title = tirtc_demo_create_label(
            video_card, "远端画面", &jz_ui_font_16, TIRTC_DEMO_TEXT_COLOR,
            width - 48, LV_TEXT_ALIGN_LEFT);

        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 4);
        lv_obj_set_style_clip_corner(video_card, true, 0);
        g_remote_video_image = lv_img_create(video_card);
        lv_obj_align(g_remote_video_image, LV_ALIGN_CENTER, 0, 14);
        g_remote_video_placeholder = tirtc_demo_create_label(
            video_card,
            runtime.peer_connected ? "等待对端 MJPEG 视频" : "通话建立后显示远端画面",
            &jz_ui_font_16, TIRTC_DEMO_MUTED_COLOR, width - 48,
            LV_TEXT_ALIGN_CENTER);
        lv_obj_align(g_remote_video_placeholder, LV_ALIGN_CENTER, 0, 14);
        tirtc_demo_update_remote_video();
    }

    tirtc_demo_create_volume_card(content, "接收音量", TIRTC_DEMO_VOLUME_RX,
                                  TIRTC_DEMO_VOL_RX_DOWN, TIRTC_DEMO_VOL_RX_UP,
                                  TIRTC_DEMO_VOL_RX_MUTE);
    tirtc_demo_create_volume_card(content, "发送音量", TIRTC_DEMO_VOLUME_TX,
                                  TIRTC_DEMO_VOL_TX_DOWN, TIRTC_DEMO_VOL_TX_UP,
                                  TIRTC_DEMO_VOL_TX_MUTE);
    tirtc_demo_create_status_strip(content);
}

static void tirtc_demo_create_add_contact(lv_obj_t *content)
{
    int width = tirtc_demo_content_width();
    tirtc_demo_runtime_t runtime;
    lv_obj_t *summary = tirtc_demo_create_card(content, 140);
    lv_obj_t *actions = tirtc_demo_create_card(content, 132);
    lv_obj_t *button;

    tirtc_demo_app_get_runtime(&runtime);
    tirtc_demo_create_info_row(summary, 10, "联系人来源", "设备业务服务",
                               TIRTC_DEMO_TEXT_COLOR);
    tirtc_demo_create_info_row(summary, 46, "业务消息",
                               runtime.cloud_online ? "已上线" : "未上线",
                               tirtc_demo_ready_color(runtime.cloud_online));
    tirtc_demo_create_info_row(summary, 82, "同步状态",
                               runtime.contacts_ready ? "已同步" : "待同步",
                               tirtc_demo_ready_color(runtime.contacts_ready));

    g_contact_device_id_ta = tirtc_demo_create_config_field(
        content, "联系人设备 ID", "", TIRTC_DEMO_DEVICE_ID_MAX - 1U, false);
    lv_textarea_set_placeholder_text(g_contact_device_id_ta, "TIRZ00000000");

    button = tirtc_demo_create_button(actions, "发送申请", width - 48, 44,
                                       TIRTC_DEMO_GREEN_COLOR,
                                       tirtc_demo_add_contact_callback,
                                       (void *)(uintptr_t)TIRTC_DEMO_ADD_REQUEST);
    lv_obj_align(button, LV_ALIGN_TOP_MID, 0, 8);

    button = tirtc_demo_create_button(actions, "联系人列表", width - 48, 44,
                                       TIRTC_DEMO_PURPLE_COLOR,
                                       tirtc_demo_view_callback,
                                       (void *)(uintptr_t)TIRTC_DEMO_VIEW_CONTACTS);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -8);

    tirtc_demo_create_status_strip(content);
}

static void tirtc_demo_create_contact_row(lv_obj_t *content, size_t index,
                                          const tirtc_demo_contact_t *contact)
{
    int width = tirtc_demo_content_width();
    lv_obj_t *row = tirtc_demo_create_card(content, 104);
    lv_obj_t *label;
    lv_obj_t *button;

    label = tirtc_demo_create_label(row, contact->name, &jz_ui_font_16,
                                    TIRTC_DEMO_TEXT_COLOR, width - 178,
                                    LV_TEXT_ALIGN_LEFT);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 12, 10);

    label = tirtc_demo_create_label(row, contact->device_id, &jz_ui_font_12,
                                    TIRTC_DEMO_MUTED_COLOR, width - 178,
                                    LV_TEXT_ALIGN_LEFT);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 12, 38);

    label = tirtc_demo_create_label(row, contact->last_call, &jz_ui_font_12,
                                    contact->online ? TIRTC_DEMO_GREEN_COLOR :
                                                      TIRTC_DEMO_MUTED_COLOR,
                                    width - 178, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 12, 66);

    button = tirtc_demo_create_button(row, "呼叫", 88, 44, TIRTC_DEMO_GREEN_COLOR,
                                      tirtc_demo_call_contact_callback,
                                      (void *)(uintptr_t)index);
    lv_obj_align(button, LV_ALIGN_RIGHT_MID, -12, 0);
}

static void tirtc_demo_create_contacts(lv_obj_t *content)
{
    const tirtc_demo_contact_t *contacts = NULL;
    size_t count = tirtc_demo_app_get_contacts(&contacts);
    int width = tirtc_demo_content_width();
    int button_width = (width - 36) / 2;
    lv_obj_t *actions = tirtc_demo_create_card(content, 68);
    lv_obj_t *button;

    button = tirtc_demo_create_button(actions, "刷新", button_width, 42,
                                      TIRTC_DEMO_PRIMARY_COLOR,
                                      tirtc_demo_contacts_refresh_callback, NULL);
    lv_obj_align(button, LV_ALIGN_LEFT_MID, 12, 0);
    button = tirtc_demo_create_button(actions, "添加", button_width, 42,
                                      TIRTC_DEMO_GREEN_COLOR,
                                      tirtc_demo_view_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_VIEW_ADD_CONTACT);
    lv_obj_align(button, LV_ALIGN_RIGHT_MID, -12, 0);

    tirtc_demo_create_status_strip(content);

    if (count == 0) {
        lv_obj_t *empty = tirtc_demo_create_card(content, 120);
        lv_obj_t *label = tirtc_demo_create_label(empty,
                                                  "暂无联系人，请输入设备 ID 申请或从服务端同步",
                                                  &jz_ui_font_16, TIRTC_DEMO_MUTED_COLOR,
                                                  tirtc_demo_content_width() - 48,
                                                  LV_TEXT_ALIGN_CENTER);
        lv_obj_center(label);
    }

    for (size_t i = 0; i < count; i++) {
        tirtc_demo_create_contact_row(content, i, &contacts[i]);
    }
}

static void tirtc_demo_create_ai_message_card(
    lv_obj_t *content, const tirtc_demo_ai_message_t *message)
{
    int width = tirtc_demo_content_width();
    int height = strlen(message->text) > 72U ? 132 : 96;
    bool asr = message->caption_type == 0U;
    lv_obj_t *card = tirtc_demo_create_card(content, height);
    lv_obj_t *role;
    lv_obj_t *text;

    lv_obj_set_style_bg_color(card,
                              lv_color_hex(asr ? 0xEAF9F2 : 0xEAF4FE), 0);
    role = tirtc_demo_create_label(card, asr ? "我" : "AI",
                                   &jz_ui_font_12,
                                   asr ? 0x0D8A59 : 0x1768B7,
                                   width - 48, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(role, LV_ALIGN_TOP_LEFT, 12, 4);
    text = tirtc_demo_create_label(card, message->text, &jz_ui_font_16,
                                   TIRTC_DEMO_TEXT_COLOR, width - 48,
                                   LV_TEXT_ALIGN_LEFT);
    lv_obj_align(text, LV_ALIGN_TOP_LEFT, 12, 30);
}

static void tirtc_demo_create_ai_talk(lv_obj_t *content)
{
    int width = tirtc_demo_content_width();
    int button_width = (width - 36) / 2;
    tirtc_demo_runtime_t runtime;
    tirtc_demo_ai_message_t messages[TIRTC_DEMO_AI_MESSAGE_MAX];
    size_t message_count;
    lv_obj_t *state = tirtc_demo_create_card(content, 176);
    lv_obj_t *actions = tirtc_demo_create_card(content, 244);
    lv_obj_t *button;
    bool ai_session;
    bool ai_pending;

    memset(messages, 0, sizeof(messages));
    tirtc_demo_app_get_runtime(&runtime);
    message_count = tirtc_demo_app_copy_ai_messages(
        messages, TIRTC_DEMO_AI_MESSAGE_MAX);
    ai_session = runtime.session_type != NULL &&
                 strcmp(runtime.session_type, "ai") == 0;
    ai_pending = ai_session && strcmp(runtime.call_phase, "idle") != 0;

    tirtc_demo_create_info_row(state, 10, "AI连接",
                               runtime.ai_ready ? "已连接" : "待连接",
                               runtime.ai_ready ? TIRTC_DEMO_GREEN_COLOR :
                                                  TIRTC_DEMO_WARNING_COLOR);
    tirtc_demo_create_info_row(state, 46, "麦克风",
                               runtime.ai_talking ? "持续聆听" : "已暂停",
                               runtime.ai_talking ? TIRTC_DEMO_GREEN_COLOR :
                                                    TIRTC_DEMO_MUTED_COLOR);
    tirtc_demo_create_info_row(state, 82, "对话记录",
                               message_count > 0U ? "已更新" : "暂无字幕",
                               message_count > 0U ? TIRTC_DEMO_PRIMARY_COLOR :
                                                    TIRTC_DEMO_MUTED_COLOR);
    tirtc_demo_create_info_row(state, 118, "AI形象",
                               runtime.ai_avatar == 0U ? "伙伴" : "新芽",
                               TIRTC_DEMO_TEXT_COLOR);

    if (message_count == 0U) {
        lv_obj_t *empty = tirtc_demo_create_card(content, 104);
        lv_obj_t *label = tirtc_demo_create_label(
            empty, runtime.ai_ready ? "正在聆听，等待 ASR / TTS 字幕" :
                                      "连接 AI 后自动开启持续聆听",
            &jz_ui_font_16, TIRTC_DEMO_MUTED_COLOR, width - 48,
            LV_TEXT_ALIGN_CENTER);
        lv_obj_center(label);
    } else {
        for (size_t i = 0U; i < message_count; ++i) {
            tirtc_demo_create_ai_message_card(content, &messages[i]);
        }
    }

    button = tirtc_demo_create_button(actions, "连接 AI", button_width, 44,
                                      TIRTC_DEMO_PRIMARY_COLOR,
                                      tirtc_demo_ai_action_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_AI_CONNECT);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, 12, 12);
    if (strcmp(runtime.call_phase, "idle") != 0) {
        lv_obj_add_state(button, LV_STATE_DISABLED);
    }
    button = tirtc_demo_create_button(actions, "打断回复", button_width, 44,
                                      TIRTC_DEMO_RED_COLOR,
                                      tirtc_demo_ai_action_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_AI_INTERRUPT);
    lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -12, 12);
    if (!runtime.ai_ready) {
        lv_obj_add_state(button, LV_STATE_DISABLED);
    }

    button = tirtc_demo_create_button(
        actions, runtime.ai_talking ? "暂停聆听" : "继续聆听",
        button_width, 44,
        runtime.ai_talking ? TIRTC_DEMO_WARNING_COLOR :
                             TIRTC_DEMO_GREEN_COLOR,
        tirtc_demo_ai_action_callback,
        (void *)(uintptr_t)(runtime.ai_talking ? TIRTC_DEMO_AI_STOP_TX :
                                                 TIRTC_DEMO_AI_START_TX));
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, 12, 68);
    if (!runtime.ai_ready) {
        lv_obj_add_state(button, LV_STATE_DISABLED);
    }
    button = tirtc_demo_create_button(actions, "开始新对话", button_width, 44,
                                      TIRTC_DEMO_GREEN_COLOR,
                                      tirtc_demo_ai_action_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_AI_NEW_CHAT);
    lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -12, 68);

    button = tirtc_demo_create_button(
        actions, runtime.ai_avatar == 0U ? "形象：伙伴" : "形象：新芽",
        button_width, 44, TIRTC_DEMO_PURPLE_COLOR,
        tirtc_demo_ai_avatar_callback, NULL);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, 12, 124);
    button = tirtc_demo_create_button(
        actions, ai_pending ? "结束 AI 会话" : "暂无活动会话",
        button_width, 44, ai_pending ? TIRTC_DEMO_RED_COLOR : 0x60758C,
        tirtc_demo_call_action_callback,
        (void *)(uintptr_t)TIRTC_DEMO_CALL_HANGUP);
    lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -12, 124);
    if (!ai_pending) {
        lv_obj_add_state(button, LV_STATE_DISABLED);
    }

    tirtc_demo_create_volume_card(content, "麦克风音量", TIRTC_DEMO_VOLUME_TX,
                                  TIRTC_DEMO_VOL_TX_DOWN, TIRTC_DEMO_VOL_TX_UP,
                                  TIRTC_DEMO_VOL_TX_MUTE);
    tirtc_demo_create_volume_card(content, "扬声器音量", TIRTC_DEMO_VOLUME_RX,
                                  TIRTC_DEMO_VOL_RX_DOWN, TIRTC_DEMO_VOL_RX_UP,
                                  TIRTC_DEMO_VOL_RX_MUTE);
    tirtc_demo_create_status_strip(content);
}

static void tirtc_demo_create_system(lv_obj_t *content)
{
    int width = tirtc_demo_content_width();
    int button_width = (width - 36) / 2;
    tirtc_demo_runtime_t runtime;
    lv_obj_t *summary = tirtc_demo_create_card(content, 176);
    lv_obj_t *actions = tirtc_demo_create_card(content, 188);
    lv_obj_t *button;

    tirtc_demo_app_get_runtime(&runtime);
    tirtc_demo_create_info_row(summary, 10, "系统网络",
                               runtime.network_ready ? "已连接" : "未连接",
                               tirtc_demo_ready_color(runtime.network_ready));
    tirtc_demo_create_info_row(summary, 46, "设备身份",
                               runtime.identity_ready ? "已绑定" : "未绑定",
                               tirtc_demo_ready_color(runtime.identity_ready));
    tirtc_demo_create_info_row(summary, 82, "TiRTC SDK",
                               runtime.sdk_ready ? "已加载" : "未加载",
                               tirtc_demo_ready_color(runtime.sdk_ready));
    tirtc_demo_create_info_row(summary, 118, "TiRTC 服务",
                               runtime.sdk_ready ? "在线" :
                                   (runtime.service_starting ? "启动中" : "等待启动"),
                               tirtc_demo_ready_color(runtime.sdk_ready));

    button = tirtc_demo_create_button(actions, "TiRTC 配置", button_width, 42,
                                      TIRTC_DEMO_PRIMARY_COLOR,
                                      tirtc_demo_view_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_VIEW_CONFIG);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, 12, 12);
    button = tirtc_demo_create_button(actions, "设备信息", button_width, 42,
                                      TIRTC_DEMO_GREEN_COLOR,
                                      tirtc_demo_view_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_VIEW_DEVICE_INFO);
    lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -12, 12);
    button = tirtc_demo_create_button(actions, "链路自检", button_width, 42,
                                      TIRTC_DEMO_WARNING_COLOR,
                                      tirtc_demo_view_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_VIEW_DIAGNOSTICS);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, 12, 68);
    button = tirtc_demo_create_button(actions, "WiFi", button_width, 42,
                                      0x60758C,
                                      tirtc_demo_vendor_page_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_VENDOR_SETTINGS);
    lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -12, 68);

    button = tirtc_demo_create_button(actions, "音频设置", button_width, 42,
                                      0x14A6A0, tirtc_demo_vendor_page_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_VENDOR_AUDIO);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, 12, 124);
    button = tirtc_demo_create_button(actions, "相机检查", button_width, 42,
                                      TIRTC_DEMO_GREEN_COLOR,
                                      tirtc_demo_vendor_page_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_VENDOR_CAMERA);
    lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -12, 124);

    tirtc_demo_create_status_strip(content);
}

static void tirtc_demo_create_diagnostics(lv_obj_t *content)
{
    int width = tirtc_demo_content_width();
    int button_width = (width - 36) / 2;
    tirtc_demo_runtime_t runtime;
    lv_obj_t *checks = tirtc_demo_create_card(content, 248);
    lv_obj_t *summary = tirtc_demo_create_card(content, 140);
    lv_obj_t *actions = tirtc_demo_create_card(content, 76);
    lv_obj_t *label;
    lv_obj_t *button;
    char video_text[56];

    tirtc_demo_app_get_runtime(&runtime);
    snprintf(video_text, sizeof(video_text), "%lu 收到 / %lu 丢弃",
             (unsigned long)runtime.rx_video_frames,
             (unsigned long)runtime.rx_video_dropped);
    tirtc_demo_create_info_row(checks, 10, "WiFi",
                               runtime.network_ready ? "已连接" : "未连接",
                               tirtc_demo_ready_color(runtime.network_ready));
    tirtc_demo_create_info_row(checks, 46, "设备身份",
                               runtime.identity_ready ? "已绑定" : "待绑定",
                               tirtc_demo_ready_color(runtime.identity_ready));
    tirtc_demo_create_info_row(checks, 82, "业务云",
                               runtime.cloud_online ? "已上线" : "未上线",
                               tirtc_demo_ready_color(runtime.cloud_online));
    tirtc_demo_create_info_row(checks, 118, "TiRTC",
                               runtime.sdk_ready ? "监听中" : "未启动",
                               tirtc_demo_ready_color(runtime.sdk_ready));
    tirtc_demo_create_info_row(checks, 154, "远端视频", video_text,
                               runtime.video_rx_active ? TIRTC_DEMO_GREEN_COLOR :
                                                         TIRTC_DEMO_MUTED_COLOR);
    tirtc_demo_create_info_row(checks, 190, "门禁逻辑",
                               runtime.door_open ? "打开" : "关闭",
                               runtime.door_open ? TIRTC_DEMO_WARNING_COLOR :
                                                   TIRTC_DEMO_GREEN_COLOR);

    label = tirtc_demo_create_label(summary, "最近一次结果", &jz_ui_font_16,
                                    TIRTC_DEMO_TEXT_COLOR, width - 48,
                                    LV_TEXT_ALIGN_LEFT);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 12, 4);
    label = tirtc_demo_create_label(
        summary,
        runtime.diagnostic_running ? "正在检查业务链路..." :
                                     runtime.diagnostic_summary,
        &jz_ui_font_16, TIRTC_DEMO_MUTED_COLOR, width - 48,
        LV_TEXT_ALIGN_LEFT);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 12, 36);

    button = tirtc_demo_create_button(actions, "立即自检", button_width, 42,
                                      TIRTC_DEMO_PRIMARY_COLOR,
                                      tirtc_demo_diagnostics_callback, NULL);
    lv_obj_align(button, LV_ALIGN_LEFT_MID, 12, 0);
    button = tirtc_demo_create_button(actions, "WiFi 设置", button_width, 42,
                                      0x60758C, tirtc_demo_vendor_page_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_VENDOR_SETTINGS);
    lv_obj_align(button, LV_ALIGN_RIGHT_MID, -12, 0);

    tirtc_demo_create_status_strip(content);
}

static lv_obj_t *tirtc_demo_create_config_field(lv_obj_t *content,
                                                 const char *label_text,
                                                 const char *value,
                                                 uint32_t max_length,
                                                 bool password)
{
    int width = tirtc_demo_content_width();
    lv_obj_t *card = tirtc_demo_create_card(content, 92);
    lv_obj_t *label = tirtc_demo_create_label(card, label_text, &jz_ui_font_12,
                                              TIRTC_DEMO_MUTED_COLOR,
                                              width - 48, LV_TEXT_ALIGN_LEFT);
    lv_obj_t *textarea = lv_textarea_create(card);

    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 12, 4);
    lv_obj_set_size(textarea, width - 48, 44);
    lv_obj_align(textarea, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_textarea_set_one_line(textarea, true);
    lv_textarea_set_max_length(textarea, max_length);
    lv_textarea_set_text(textarea, value != NULL ? value : "");
    lv_textarea_set_password_mode(textarea, password);
    lv_obj_set_style_text_font(textarea, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(textarea, lv_color_hex(TIRTC_DEMO_TEXT_COLOR), 0);
    lv_obj_set_style_bg_color(textarea, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(textarea, lv_color_hex(TIRTC_DEMO_BORDER_COLOR), 0);
    lv_obj_set_style_border_width(textarea, 1, 0);
    lv_obj_set_style_radius(textarea, 6, 0);
    lv_obj_add_event_cb(textarea, tirtc_demo_textarea_callback, LV_EVENT_ALL, NULL);

    return textarea;
}

static void tirtc_demo_create_config_edit(lv_obj_t *content)
{
    int width = tirtc_demo_content_width();
    tirtc_demo_config_t config;
    lv_obj_t *actions;
    lv_obj_t *button;

    tirtc_demo_app_get_config(&config);
    g_config_endpoint_ta = tirtc_demo_create_config_field(
        content, "服务地址", config.endpoint, TIRTC_DEMO_ENDPOINT_MAX - 1U, false);
    g_config_device_id_ta = tirtc_demo_create_config_field(
        content, "设备 ID", config.device_id, TIRTC_DEMO_DEVICE_ID_MAX - 1U, false);
    g_config_secret_ta = tirtc_demo_create_config_field(
        content, "设备密钥", config.device_secret,
        TIRTC_DEMO_DEVICE_SECRET_MAX - 1U, true);
    memset(config.device_secret, 0, sizeof(config.device_secret));

    actions = tirtc_demo_create_card(content, 112);
    g_config_error_label = tirtc_demo_create_label(actions, "",
                                                    &jz_ui_font_12,
                                                    TIRTC_DEMO_MUTED_COLOR,
                                                    width - 48,
                                                    LV_TEXT_ALIGN_CENTER);
    lv_obj_align(g_config_error_label, LV_ALIGN_TOP_MID, 0, 4);
    button = tirtc_demo_create_button(actions, "保存并启用", width - 48, 44,
                                      TIRTC_DEMO_PRIMARY_COLOR,
                                      tirtc_demo_save_config_callback, NULL);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void tirtc_demo_create_config(lv_obj_t *content)
{
    int width = tirtc_demo_content_width();
    int button_width = (width - 36) / 2;
    tirtc_demo_runtime_t runtime;
    tirtc_demo_settings_t settings;
    lv_obj_t *base = tirtc_demo_create_card(content, 320);
    lv_obj_t *actions = tirtc_demo_create_card(content, 244);
    lv_obj_t *button;

    tirtc_demo_app_get_runtime(&runtime);
    tirtc_demo_app_get_settings(&settings);
    tirtc_demo_create_info_row(base, 10, "服务地址", settings.endpoint,
                               TIRTC_DEMO_TEXT_COLOR);
    tirtc_demo_create_info_row(base, 46, "设备 ID", settings.peer_id,
                               TIRTC_DEMO_TEXT_COLOR);
    tirtc_demo_create_info_row(base, 82, "设备凭据",
                               runtime.identity_ready ? "已配置" : "未配置",
                               tirtc_demo_ready_color(runtime.identity_ready));
    tirtc_demo_create_info_row(base, 118, "TiRTC 服务",
                               runtime.sdk_ready ? "在线" :
                                   (runtime.service_starting ? "启动中" : "未启动"),
                               tirtc_demo_ready_color(runtime.sdk_ready));
    tirtc_demo_create_info_row(base, 154, "SDK", tirtc_demo_app_sdk_version(),
                               TIRTC_DEMO_TEXT_COLOR);
    tirtc_demo_create_info_row(base, 190, "系统网络",
                               runtime.network_ready ? "已连接" : "未连接",
                               tirtc_demo_ready_color(runtime.network_ready));
    tirtc_demo_create_info_row(base, 226, "业务消息",
                               runtime.cloud_online ? "已上线" : "未上线",
                               tirtc_demo_ready_color(runtime.cloud_online));
    tirtc_demo_create_info_row(base, 262, "绑定验证码",
                               runtime.binding_waiting &&
                                       runtime.binding_code != NULL &&
                                       runtime.binding_code[0] != '\0' ?
                                   runtime.binding_code : "-",
                               runtime.binding_waiting ? TIRTC_DEMO_PURPLE_COLOR :
                                                         TIRTC_DEMO_MUTED_COLOR);

    button = tirtc_demo_create_button(actions, "编辑配置", button_width, 42,
                                      TIRTC_DEMO_PRIMARY_COLOR,
                                      tirtc_demo_view_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_VIEW_CONFIG_EDIT);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, 12, 12);
    button = tirtc_demo_create_button(actions, "重新连接", button_width, 42,
                                      TIRTC_DEMO_GREEN_COLOR,
                                      tirtc_demo_restart_callback, NULL);
    lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -12, 12);
    button = tirtc_demo_create_button(actions, "设备绑定", button_width, 42,
                                      TIRTC_DEMO_PURPLE_COLOR,
                                      tirtc_demo_binding_callback, NULL);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, 12, 68);
    if (runtime.identity_ready) {
        lv_obj_add_state(button, LV_STATE_DISABLED);
    }
    button = tirtc_demo_create_button(actions, "重置绑定", button_width, 42,
                                      TIRTC_DEMO_RED_COLOR,
                                      tirtc_demo_reset_binding_callback, NULL);
    lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -12, 68);
    if (!runtime.identity_ready) {
        lv_obj_add_state(button, LV_STATE_DISABLED);
    }
    button = tirtc_demo_create_button(actions, "系统扫码", button_width, 42,
                                      0x14A6A0, tirtc_demo_vendor_page_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_VENDOR_SCANCODE);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, 12, 124);
    button = tirtc_demo_create_button(actions, "刷新网络", button_width, 42,
                                      TIRTC_DEMO_WARNING_COLOR,
                                      tirtc_demo_network_refresh_callback, NULL);
    lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -12, 124);
    button = tirtc_demo_create_button(actions, "WiFi 设置", width - 48, 42,
                                      0x60758C, tirtc_demo_vendor_page_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_VENDOR_SETTINGS);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -12);

    tirtc_demo_create_status_strip(content);
}

static void tirtc_demo_create_device_info(lv_obj_t *content)
{
    int width = tirtc_demo_content_width();
    int button_width = (width - 36) / 2;
    tirtc_demo_runtime_t runtime;
    lv_obj_t *base = tirtc_demo_create_card(content, 284);
    lv_obj_t *actions = tirtc_demo_create_card(content, 76);
    lv_obj_t *button;
    char rssi_text[24];
    char device_id[TIRTC_DEMO_DEVICE_ID_MAX];

    tirtc_demo_app_get_runtime(&runtime);
    tirtc_demo_app_copy_device_id(device_id, sizeof(device_id));
    if (runtime.network_ready) {
        snprintf(rssi_text, sizeof(rssi_text), "%d dBm", runtime.network_rssi);
    } else {
        snprintf(rssi_text, sizeof(rssi_text), "%s", "未连接");
    }
    tirtc_demo_create_info_row(base, 10, "芯片", "Ingenic G32S10X",
                               TIRTC_DEMO_TEXT_COLOR);
    tirtc_demo_create_info_row(base, 46, "屏幕", "480 x 854",
                               TIRTC_DEMO_TEXT_COLOR);
    tirtc_demo_create_info_row(base, 82, "WiFi", rssi_text,
                               tirtc_demo_ready_color(runtime.network_ready));
    tirtc_demo_create_info_row(base, 118, "设备 ID", device_id,
                               TIRTC_DEMO_TEXT_COLOR);
    tirtc_demo_create_info_row(base, 154, "TiRTC SDK", tirtc_demo_app_sdk_version(),
                               tirtc_demo_ready_color(runtime.sdk_ready));
    tirtc_demo_create_info_row(base, 190, "构建时间", __DATE__ " " __TIME__,
                               TIRTC_DEMO_TEXT_COLOR);
    tirtc_demo_create_info_row(base, 226, "应用层", "TiRTC Device",
                               TIRTC_DEMO_TEXT_COLOR);

    button = tirtc_demo_create_button(actions, "系统设置", button_width, 42,
                                      0x60758C, tirtc_demo_vendor_page_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_VENDOR_SETTINGS);
    lv_obj_align(button, LV_ALIGN_LEFT_MID, 12, 0);
    button = tirtc_demo_create_button(actions, "相机", button_width, 42,
                                      TIRTC_DEMO_GREEN_COLOR,
                                      tirtc_demo_vendor_page_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_VENDOR_CAMERA);
    lv_obj_align(button, LV_ALIGN_RIGHT_MID, -12, 0);
}

static void tirtc_demo_create_device_qr_view(lv_obj_t *content)
{
    int width = tirtc_demo_content_width();
    lv_obj_t *notice;
    lv_obj_t *label;

    tirtc_demo_create_device_qr_card(content, true);
    notice = tirtc_demo_create_card(content, 96);
    label = tirtc_demo_create_label(
        notice,
        "二维码只包含公开设备 ID，不包含设备密钥、token 或配对密钥。",
        &jz_ui_font_16, TIRTC_DEMO_MUTED_COLOR, width - 48,
        LV_TEXT_ALIGN_CENTER);
    lv_obj_center(label);
}

static void tirtc_demo_create_wechat(lv_obj_t *content)
{
    int width = tirtc_demo_content_width();
    int button_width = (width - 36) / 2;
    tirtc_demo_runtime_t runtime;
    lv_obj_t *summary = tirtc_demo_create_card(content, 176);
    lv_obj_t *actions = tirtc_demo_create_card(content, 132);
    lv_obj_t *button;

    tirtc_demo_app_get_runtime(&runtime);
    tirtc_demo_create_info_row(summary, 10, "设备绑定",
                               runtime.identity_ready ? "已绑定" : "未绑定",
                               tirtc_demo_ready_color(runtime.identity_ready));
    tirtc_demo_create_info_row(summary, 46, "微信服务",
                               runtime.wechat_ready ? "已就绪" : "待同步",
                               tirtc_demo_ready_color(runtime.wechat_ready));
    tirtc_demo_create_info_row(summary, 82, "联系人",
                               runtime.wechat_contacts_ready ? "已同步" : "待同步",
                               tirtc_demo_ready_color(
                                   runtime.wechat_contacts_ready));
    tirtc_demo_create_info_row(summary, 118, "通话状态", runtime.call_phase,
                               TIRTC_DEMO_TEXT_COLOR);

    button = tirtc_demo_create_button(actions, "添加联系人", button_width, 42,
                                      TIRTC_DEMO_PRIMARY_COLOR,
                                      tirtc_demo_view_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_VIEW_WECHAT_ADD);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, 12, 12);
    button = tirtc_demo_create_button(actions, "联系人列表", button_width, 42,
                                      TIRTC_DEMO_PURPLE_COLOR,
                                      tirtc_demo_view_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_VIEW_WECHAT_CONTACTS);
    lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -12, 12);
    button = tirtc_demo_create_button(actions, "当前通话", button_width, 42,
                                      TIRTC_DEMO_GREEN_COLOR,
                                      tirtc_demo_view_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_VIEW_WECHAT_ACTIVE);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, 12, 68);
    button = tirtc_demo_create_button(actions, "刷新微信", button_width, 42,
                                      0x60758C,
                                      tirtc_demo_wechat_refresh_callback, NULL);
    lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -12, 68);

    tirtc_demo_create_status_strip(content);
}

static void tirtc_demo_create_wechat_add(lv_obj_t *content)
{
    int width = tirtc_demo_content_width();
    tirtc_demo_runtime_t runtime;
    lv_obj_t *summary = tirtc_demo_create_card(content, 104);
    lv_obj_t *actions = tirtc_demo_create_card(content, 76);
    lv_obj_t *button;

    tirtc_demo_app_get_runtime(&runtime);
    tirtc_demo_create_info_row(summary, 10, "联系人类型", "微信 OpenID",
                               TIRTC_DEMO_TEXT_COLOR);
    tirtc_demo_create_info_row(summary, 46, "业务状态",
                               runtime.wechat_ready ? "已就绪" : "待同步",
                               tirtc_demo_ready_color(runtime.wechat_ready));

    g_wechat_open_id_ta = tirtc_demo_create_config_field(
        content, "微信 OpenID", "", TIRTC_DEMO_WECHAT_OPENID_MAX - 1U, false);
    lv_textarea_set_placeholder_text(g_wechat_open_id_ta, "请输入微信 OpenID");

    button = tirtc_demo_create_button(actions, "提交添加", width - 48, 42,
                                      TIRTC_DEMO_GREEN_COLOR,
                                      tirtc_demo_wechat_add_callback, NULL);
    lv_obj_align(button, LV_ALIGN_CENTER, 0, 0);

    tirtc_demo_create_status_strip(content);
}

static void tirtc_demo_create_wechat_contact_row(
    lv_obj_t *content, size_t index,
    const tirtc_demo_wechat_contact_t *contact)
{
    int width = tirtc_demo_content_width();
    lv_obj_t *row = tirtc_demo_create_card(content, 122);
    lv_obj_t *label;
    lv_obj_t *button;

    label = tirtc_demo_create_label(row, contact->name, &jz_ui_font_16,
                                    TIRTC_DEMO_TEXT_COLOR, width - 182,
                                    LV_TEXT_ALIGN_LEFT);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 12, 8);
    label = tirtc_demo_create_label(row, contact->open_id, &jz_ui_font_12,
                                    TIRTC_DEMO_MUTED_COLOR, width - 182,
                                    LV_TEXT_ALIGN_LEFT);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 12, 40);
    label = tirtc_demo_create_label(
        row, contact->model_id != NULL && contact->model_id[0] != '\0' ?
                 contact->model_id : "未返回 model_id",
        &jz_ui_font_12, TIRTC_DEMO_MUTED_COLOR, width - 182,
        LV_TEXT_ALIGN_LEFT);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 12, 72);

    button = tirtc_demo_create_button(row, "呼叫", 82, 40,
                                      TIRTC_DEMO_GREEN_COLOR,
                                      tirtc_demo_wechat_call_callback,
                                      (void *)(uintptr_t)index);
    lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -12, 8);
    button = tirtc_demo_create_button(row, "删除", 82, 40,
                                      TIRTC_DEMO_RED_COLOR,
                                      tirtc_demo_wechat_delete_callback,
                                      (void *)(uintptr_t)index);
    lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, -12, -8);
}

static void tirtc_demo_create_wechat_contacts(lv_obj_t *content)
{
    const tirtc_demo_wechat_contact_t *contacts = NULL;
    size_t count = tirtc_demo_app_get_wechat_contacts(&contacts);
    int width = tirtc_demo_content_width();
    int button_width = (width - 36) / 2;
    lv_obj_t *actions = tirtc_demo_create_card(content, 76);
    lv_obj_t *button;

    button = tirtc_demo_create_button(actions, "刷新", button_width, 42,
                                      TIRTC_DEMO_PRIMARY_COLOR,
                                      tirtc_demo_wechat_refresh_callback, NULL);
    lv_obj_align(button, LV_ALIGN_LEFT_MID, 12, 0);
    button = tirtc_demo_create_button(actions, "添加联系人", button_width, 42,
                                      TIRTC_DEMO_PRIMARY_COLOR,
                                      tirtc_demo_view_callback,
                                      (void *)(uintptr_t)TIRTC_DEMO_VIEW_WECHAT_ADD);
    lv_obj_align(button, LV_ALIGN_RIGHT_MID, -12, 0);

    tirtc_demo_create_status_strip(content);
    if (count == 0U) {
        lv_obj_t *empty = tirtc_demo_create_card(content, 120);
        lv_obj_t *label = tirtc_demo_create_label(
            empty, "暂无微信联系人，请刷新或添加 OpenID",
            &jz_ui_font_16, TIRTC_DEMO_MUTED_COLOR, width - 48,
            LV_TEXT_ALIGN_CENTER);
        lv_obj_center(label);
    }
    for (size_t i = 0U; i < count; ++i) {
        tirtc_demo_create_wechat_contact_row(content, i, &contacts[i]);
    }
}

static void tirtc_demo_create_wechat_active(lv_obj_t *content)
{
    int width = tirtc_demo_content_width();
    tirtc_demo_runtime_t runtime;
    lv_obj_t *summary = tirtc_demo_create_card(content, 212);
    int button_width = (width - 36) / 2;
    lv_obj_t *actions = tirtc_demo_create_card(content, 132);
    lv_obj_t *button;
    char duration[16];

    tirtc_demo_app_get_runtime(&runtime);
    snprintf(duration, sizeof(duration), "%lu:%02lu",
             (unsigned long)(runtime.call_duration_s / 60U),
             (unsigned long)(runtime.call_duration_s % 60U));
    tirtc_demo_create_info_row(summary, 10, "微信服务",
                               runtime.wechat_ready ? "已就绪" : "待同步",
                               tirtc_demo_ready_color(runtime.wechat_ready));
    tirtc_demo_create_info_row(summary, 46, "对端", runtime.peer_id,
                               TIRTC_DEMO_TEXT_COLOR);
    tirtc_demo_create_info_row(summary, 82, "通话状态", runtime.call_phase,
                               TIRTC_DEMO_TEXT_COLOR);
    tirtc_demo_create_info_row(summary, 118, "音频",
                               runtime.audio_tx_active ? "发送中" : "未建立",
                               tirtc_demo_ready_color(runtime.audio_tx_active));
    g_call_duration_label = tirtc_demo_create_info_row(
        summary, 154, "通话时长", duration, TIRTC_DEMO_GREEN_COLOR);
    if (runtime.wechat_incoming) {
        button = tirtc_demo_create_button(actions, "接听", button_width, 42,
                                          TIRTC_DEMO_GREEN_COLOR,
                                          tirtc_demo_wechat_answer_callback,
                                          (void *)(uintptr_t)true);
        lv_obj_align(button, LV_ALIGN_TOP_LEFT, 12, 12);
        button = tirtc_demo_create_button(actions, "拒绝", button_width, 42,
                                          TIRTC_DEMO_RED_COLOR,
                                          tirtc_demo_wechat_answer_callback,
                                          (void *)(uintptr_t)false);
        lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -12, 12);
    } else {
        lv_obj_t *label = tirtc_demo_create_label(
            actions,
            runtime.peer_connected ? "微信媒体会话已建立" : "当前没有待接微信来电",
            &jz_ui_font_16, TIRTC_DEMO_MUTED_COLOR, width - 48,
            LV_TEXT_ALIGN_CENTER);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 20);
    }
    button = tirtc_demo_create_button(actions, "挂断", width - 48, 42,
                                      TIRTC_DEMO_RED_COLOR,
                                      tirtc_demo_wechat_hangup_callback, NULL);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -10);
    if (!runtime.peer_connected && !runtime.wechat_incoming &&
        strcmp(runtime.call_phase, "outgoing") != 0 &&
        strcmp(runtime.call_phase, "connecting") != 0) {
        lv_obj_add_state(button, LV_STATE_DISABLED);
    }

    tirtc_demo_create_volume_card(content, "接收音量", TIRTC_DEMO_VOLUME_RX,
                                  TIRTC_DEMO_VOL_RX_DOWN, TIRTC_DEMO_VOL_RX_UP,
                                  TIRTC_DEMO_VOL_RX_MUTE);
    tirtc_demo_create_volume_card(content, "发送音量", TIRTC_DEMO_VOLUME_TX,
                                  TIRTC_DEMO_VOL_TX_DOWN, TIRTC_DEMO_VOL_TX_UP,
                                  TIRTC_DEMO_VOL_TX_MUTE);
    tirtc_demo_create_status_strip(content);
}

#if TIRTC_DEMO_USE_S3_PIXEL_UI
#include "ui_tirtc_demo_pixel.h"
#endif

static void tirtc_demo_render_view(tirtc_demo_view_t view)
{
    lv_obj_t *content;
    const char *title = "TiRTC 设备端";
    int home = view == TIRTC_DEMO_VIEW_HOME ? 1 : 0;

    if (g_page == NULL) {
        return;
    }

    tirtc_demo_release_remote_video();
    tirtc_demo_delete_keyboard();
    g_current_view = view;
    g_state_label = NULL;
    g_stage_label = NULL;
    g_message_label = NULL;
    g_network_pill = NULL;
    g_identity_pill = NULL;
    g_peer_pill = NULL;
    g_call_duration_label = NULL;
    g_config_endpoint_ta = NULL;
    g_config_device_id_ta = NULL;
    g_config_secret_ta = NULL;
    g_config_error_label = NULL;
    g_contact_device_id_ta = NULL;
    g_wechat_open_id_ta = NULL;

    lv_obj_clean(g_page);
#if TIRTC_DEMO_USE_S3_PIXEL_UI
    tirtc_demo_pixel_prepare_canvas();
    tirtc_demo_pixel_render_view(view);
    return;
#endif
    lv_obj_remove_style_all(g_page);
    lv_obj_set_size(g_page, get_screen_width(), get_screen_height());
    lv_obj_set_style_bg_opa(g_page, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g_page, lv_color_hex(TIRTC_DEMO_BG_COLOR), 0);

    create_status_bar(g_page);

    switch (view) {
    case TIRTC_DEMO_VIEW_DETAIL:
        title = "查看详情";
        break;
    case TIRTC_DEMO_VIEW_CALL:
        title = "呼叫";
        break;
    case TIRTC_DEMO_VIEW_ACTIVE_CALL:
        title = "通话控制";
        break;
    case TIRTC_DEMO_VIEW_ADD_CONTACT:
        title = "添加联系人";
        break;
    case TIRTC_DEMO_VIEW_CONTACTS:
        title = "联系人列表";
        break;
    case TIRTC_DEMO_VIEW_WECHAT:
        title = "微信通话";
        break;
    case TIRTC_DEMO_VIEW_WECHAT_ADD:
        title = "添加微信联系人";
        break;
    case TIRTC_DEMO_VIEW_WECHAT_CONTACTS:
        title = "微信联系人";
        break;
    case TIRTC_DEMO_VIEW_WECHAT_ACTIVE:
        title = "微信通话中";
        break;
    case TIRTC_DEMO_VIEW_AI_TALK:
        title = "AI 对讲";
        break;
    case TIRTC_DEMO_VIEW_SYSTEM:
        title = "系统";
        break;
    case TIRTC_DEMO_VIEW_CONFIG:
        title = "TiRTC 配置";
        break;
    case TIRTC_DEMO_VIEW_CONFIG_EDIT:
        title = "编辑 TiRTC 配置";
        break;
    case TIRTC_DEMO_VIEW_DEVICE_INFO:
        title = "设备信息";
        break;
    case TIRTC_DEMO_VIEW_DEVICE_QR:
        title = "本机二维码";
        break;
    case TIRTC_DEMO_VIEW_DIAGNOSTICS:
        title = "链路自检";
        break;
    case TIRTC_DEMO_VIEW_HOME:
    default:
        title = "TiRTC 设备端";
        break;
    }

    tirtc_demo_create_top_bar(title, home);
    content = tirtc_demo_create_content();

    switch (view) {
    case TIRTC_DEMO_VIEW_DETAIL:
        tirtc_demo_create_detail(content);
        break;
    case TIRTC_DEMO_VIEW_CALL:
        tirtc_demo_create_call(content);
        break;
    case TIRTC_DEMO_VIEW_ACTIVE_CALL:
        tirtc_demo_create_active_call(content);
        break;
    case TIRTC_DEMO_VIEW_ADD_CONTACT:
        tirtc_demo_create_add_contact(content);
        break;
    case TIRTC_DEMO_VIEW_CONTACTS:
        tirtc_demo_create_contacts(content);
        break;
    case TIRTC_DEMO_VIEW_WECHAT:
        tirtc_demo_create_wechat(content);
        break;
    case TIRTC_DEMO_VIEW_WECHAT_ADD:
        tirtc_demo_create_wechat_add(content);
        break;
    case TIRTC_DEMO_VIEW_WECHAT_CONTACTS:
        tirtc_demo_create_wechat_contacts(content);
        break;
    case TIRTC_DEMO_VIEW_WECHAT_ACTIVE:
        tirtc_demo_create_wechat_active(content);
        break;
    case TIRTC_DEMO_VIEW_AI_TALK:
        tirtc_demo_create_ai_talk(content);
        break;
    case TIRTC_DEMO_VIEW_SYSTEM:
        tirtc_demo_create_system(content);
        break;
    case TIRTC_DEMO_VIEW_CONFIG:
        tirtc_demo_create_config(content);
        break;
    case TIRTC_DEMO_VIEW_CONFIG_EDIT:
        tirtc_demo_create_config_edit(content);
        break;
    case TIRTC_DEMO_VIEW_DEVICE_INFO:
        tirtc_demo_create_device_info(content);
        break;
    case TIRTC_DEMO_VIEW_DEVICE_QR:
        tirtc_demo_create_device_qr_view(content);
        break;
    case TIRTC_DEMO_VIEW_DIAGNOSTICS:
        tirtc_demo_create_diagnostics(content);
        break;
    case TIRTC_DEMO_VIEW_HOME:
    default:
        tirtc_demo_create_home(content);
        break;
    }

    if (ui_get_page(PAGE_MAIN) == NULL) {
        update_status_bar();
    }
}

int create_tirtc_demo_page(int page_id)
{
    tirtc_demo_status_t status;
    tirtc_demo_runtime_t runtime;

    tirtc_demo_release_remote_video();
    tirtc_demo_stop_refresh_timer();
    g_page = lv_scr_act();
    lv_obj_clean(g_page);

    ui_set_page(NULL, PAGE_MAIN);
    ui_set_page(g_page, page_id);

    g_current_view = TIRTC_DEMO_VIEW_HOME;
    g_visible_incoming_call = false;
    g_visible_contact_count = 0U;
    g_visible_wechat_contact_count = 0U;
    g_visible_cloud_generation = 0U;
    g_visible_ai_generation = 0U;
    g_visible_diagnostic_generation = 0U;
    g_visible_call_phase[0] = '\0';
    g_visible_ai_caption[0] = '\0';
    tirtc_demo_app_enter();
#if TIRTC_DEMO_USE_S3_PIXEL_UI
    tirtc_demo_pixel_resources_init();
#endif
    tirtc_demo_app_get_runtime(&runtime);
    tirtc_demo_app_get_status(&status);
    g_visible_status_state = status.state;
    g_visible_network_ready = runtime.network_ready;
    g_visible_peer_connected = runtime.peer_connected;
    if (runtime.incoming_call) {
        g_current_view = runtime.wechat_incoming ?
                             TIRTC_DEMO_VIEW_WECHAT_INCOMING :
                             TIRTC_DEMO_VIEW_CALL_INCOMING;
    }
    tirtc_demo_render_view(g_current_view);
    g_refresh_timer = lv_timer_create(tirtc_demo_refresh_timer_callback, 1000, NULL);

    return 0;
}
