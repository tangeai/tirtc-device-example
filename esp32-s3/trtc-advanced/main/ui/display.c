#include "display.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "device.h"
#include "display_driver.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include "lvgl.h"

#include "app_task_affinity.h"
#include "app_config.h"
#include "ai_chat_font.h"
#include "ai_chat_assets.h"
#include "ai_chat_avatar_assets.h"
#include "home_assets.h"
#include "platform_task_reaper.h"
#include "text_assets.h"

static const char *TAG = "display";

#define DISPLAY_DEVICE_QR_ESCAPED_VALUE_MAX (DISPLAY_TIRTC_CONFIG_TEXT_MAX * 2U)
#define DISPLAY_DEVICE_QR_PAYLOAD_MAX       (64U + DISPLAY_DEVICE_QR_ESCAPED_VALUE_MAX * 2U)
#define DISPLAY_CONTACT_QR_PAYLOAD_MAX      (128U + DISPLAY_DEVICE_QR_ESCAPED_VALUE_MAX * 2U)
#define DISPLAY_BINDING_PLATFORM_URL        APP_CONFIG_DEVICE_BINDING_API_BASE
#define DISPLAY_BINDING_CODE_PLACEHOLDER    "------"

static lv_disp_t *s_display;
static lv_indev_t *s_touch_indev;
static bool s_display_initialized;

static display_actions_t s_actions;
static display_snapshot_cb_t s_snapshot_provider;
static void *s_snapshot_ctx;
static display_status_t *s_last_status_ptr;
static display_status_t *s_refresh_status_ptr;
static display_status_t *s_refresh_previous_status_ptr;
#define s_last_status (*s_last_status_ptr)
#define s_refresh_status (*s_refresh_status_ptr)
#define s_refresh_previous_status (*s_refresh_previous_status_ptr)

static esp_err_t display_allocate_status_buffers(void)
{
    if (s_last_status_ptr != NULL &&
        s_refresh_status_ptr != NULL &&
        s_refresh_previous_status_ptr != NULL) {
        return ESP_OK;
    }

    s_last_status_ptr = (display_status_t *)heap_caps_calloc(1,
                                                             sizeof(*s_last_status_ptr),
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_refresh_status_ptr = (display_status_t *)heap_caps_calloc(1,
                                                                sizeof(*s_refresh_status_ptr),
                                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_refresh_previous_status_ptr =
        (display_status_t *)heap_caps_calloc(1,
                                             sizeof(*s_refresh_previous_status_ptr),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_last_status_ptr == NULL ||
        s_refresh_status_ptr == NULL ||
        s_refresh_previous_status_ptr == NULL) {
        heap_caps_free(s_last_status_ptr);
        heap_caps_free(s_refresh_status_ptr);
        heap_caps_free(s_refresh_previous_status_ptr);
        s_last_status_ptr = NULL;
        s_refresh_status_ptr = NULL;
        s_refresh_previous_status_ptr = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
static TaskHandle_t s_wechat_hangup_task;
static int64_t s_last_wifi_scan_request_us;
static int64_t s_wifi_connect_request_us;
static char s_selected_ssid[33];
static char s_wifi_connect_target_ssid[33];
static char s_main_hint_text[96] = "Ready";
static bool s_wifi_connect_pending;
static lv_obj_t *s_wifi_alert_box;
static lv_obj_t *s_binding_prompt_overlay;
static lv_obj_t *s_binding_nowifi_dialog;
static lv_obj_t *s_binding_code_dialog;
static lv_obj_t *s_binding_code_label;
static lv_obj_t *s_binding_platform_qrcode;
static bool s_binding_prompt_visible;
static bool s_binding_prompt_code_dialog_visible;
static char s_binding_prompt_code_text[16];
static lv_obj_t *s_call_alert_box;
static lv_obj_t *s_call_delete_confirm_box;
static lv_obj_t *s_wechat_delete_confirm_box;
static bool s_call_alert_wechat;
static uint8_t s_call_delete_pending_index = UINT8_MAX;
static uint8_t s_wechat_delete_pending_index = UINT8_MAX;
static char s_wechat_delete_pending_open_id[DISPLAY_WECHAT_OPEN_ID_MAX];
static bool s_ai_dialog_external_font_applied;
static int64_t s_refresh_slow_last_log_us;

#define DISPLAY_TEXT_IMAGE_MAGIC 0x54455854U
#define DISPLAY_DEVICE_QR_SIZE   150
#define DISPLAY_DEVICE_QR_FULLSCREEN_SIZE 224
#define DISPLAY_HOME_ICON_ZOOM   192
#define DISPLAY_MAIN_DEVICE_ID_LABEL_X 4
#define DISPLAY_MAIN_DEVICE_ID_LABEL_Y 158
#define DISPLAY_MAIN_DEVICE_ID_LABEL_WIDTH 146
#define DISPLAY_MAIN_DEVICE_ID_LABEL_FONT_SIZE 12
#define DISPLAY_REMOTE_VIDEO_BUFFER_COUNT 2U
#define DISPLAY_AI_CHAT_CAPTION_BOLD_LAYER_COUNT 3U
#define DISPLAY_REFRESH_SLOW_LOG_US (40LL * 1000LL)
#define DISPLAY_REFRESH_SLOW_LOG_INTERVAL_US (5LL * 1000LL * 1000LL)
#define DISPLAY_AI_HEADER_SETTINGS_X 239
#define DISPLAY_AI_HEADER_SETTINGS_Y 1
#define DISPLAY_AI_HEADER_SETTINGS_TEXT_Y 7
#define DISPLAY_AI_HEADER_SETTINGS_TEXT_WIDTH 48
#define DISPLAY_AI_HEADER_SETTINGS_HIT_X 226
#define DISPLAY_AI_HEADER_SETTINGS_HIT_Y 0
#define DISPLAY_AI_HEADER_SETTINGS_HIT_WIDTH 66
#define DISPLAY_AI_HEADER_SETTINGS_HIT_HEIGHT 28
#define DISPLAY_AI_HEADER_SETTINGS_ZOOM 307U
#define DISPLAY_AI_CHAT_CARD_WIDTH 304
#define DISPLAY_AI_CHAT_BUBBLE_LEFT_X 11
#define DISPLAY_AI_CHAT_BUBBLE_TEXT_X 9
#define DISPLAY_AI_CHAT_BUBBLE_TEXT_Y 7
#define DISPLAY_AI_CHAT_BUBBLE_PAD_RIGHT 9
#define DISPLAY_AI_CHAT_BUBBLE_PAD_BOTTOM 5
#define DISPLAY_AI_CHAT_BUBBLE_TOP_Y 12
#define DISPLAY_AI_CHAT_BUBBLE_GAP_Y 10
#define DISPLAY_AI_CHAT_BUBBLE_RADIUS 8
#define DISPLAY_AI_CHAT_TEXT_LINE_SPACE 4
#define DISPLAY_AI_CHAT_CJK_CHAR_WIDTH 18
#define DISPLAY_AI_CHAT_MIN_TEXT_WIDTH 36
#define DISPLAY_AI_CHAT_TEXT_SAFE_WIDTH 3
#define DISPLAY_AI_CHAT_TEXT_SAFE_HEIGHT 1
#define DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX 2U
#define DISPLAY_AI_CHAT_SCROLLBAR_GUTTER 8
#define DISPLAY_AI_CHAT_CONTENT_WIDTH (DISPLAY_AI_CHAT_CARD_WIDTH - DISPLAY_AI_CHAT_SCROLLBAR_GUTTER)
#define DISPLAY_AI_CHAT_NEW_BUTTON_WIDTH 178
#define DISPLAY_AI_CHAT_NEW_BUTTON_HEIGHT 40
#define DISPLAY_AI_CHAT_VIEWPORT_HEIGHT 196
#define DISPLAY_AI_CHAT_PAIR_TOP_Y 18
#define DISPLAY_AI_CHAT_PAIR_GAP_Y 14
#define DISPLAY_AI_CHAT_PAIR_NEW_BUTTON_Y 78
#define DISPLAY_AI_CHAT_PAIR_BUBBLE_MAX_HEIGHT 82
#define DISPLAY_AI_CHAT_VIRTUAL_OVERSCAN 52
#define DISPLAY_AI_CHAT_BOTTOM_FOLLOW_THRESHOLD 18
#define DISPLAY_AI_CHAT_BOTTOM_SNAP_THRESHOLD 26
#define DISPLAY_AI_CHAT_SCROLL_RENDER_MIN_INTERVAL_US (33LL * 1000LL)
#define DISPLAY_AI_CHAT_SCROLL_RENDER_SLOW_US (20LL * 1000LL)
#define DISPLAY_AI_CHAT_SCROLL_RENDER_LOG_INTERVAL_US (2LL * 1000LL * 1000LL)
#define DISPLAY_AI_CHAT_PAGE_STEP (DISPLAY_AI_CHAT_VIEWPORT_HEIGHT - 36)
#define DISPLAY_AI_CHAT_SCROLL_BUTTON_SIZE 26
#define DISPLAY_AI_CHAT_SCROLL_BUTTON_X 282
#define DISPLAY_AI_CHAT_SCROLL_BUTTON_UP_Y 48
#define DISPLAY_AI_CHAT_SCROLL_BUTTON_BOTTOM_Y 122
#define DISPLAY_AI_CHAT_SCROLL_BUTTON_DOWN_Y 196
#define DISPLAY_AI_SINGLE_CAPTION_BAR_X 8
#define DISPLAY_AI_SINGLE_CAPTION_BAR_BOTTOM_Y 228
#define DISPLAY_AI_SINGLE_CAPTION_BAR_WIDTH 304
#define DISPLAY_AI_SINGLE_CAPTION_BAR_MIN_HEIGHT 34
#define DISPLAY_AI_SINGLE_CAPTION_TEXT_X 12
#define DISPLAY_AI_SINGLE_CAPTION_TEXT_Y 7
#define DISPLAY_AI_SINGLE_CAPTION_TEXT_WIDTH 280
#define DISPLAY_AI_SINGLE_CAPTION_TEXT_PAD_BOTTOM 7
#define DISPLAY_AI_SINGLE_CAPTION_MAX_LINES 3
#define DISPLAY_AI_AVATAR_COUNT 2U
#define DISPLAY_AI_AVATAR_BUDDY 0U
#define DISPLAY_AI_AVATAR_SPROUT 1U
#define DISPLAY_AI_AVATAR_IMG_X 112
#define DISPLAY_AI_AVATAR_IMG_Y 40
#define DISPLAY_AI_CHAT_TEXT_MAX_WIDTH \
    ((DISPLAY_AI_CHAT_CONTENT_WIDTH - 2 * (DISPLAY_AI_CHAT_BUBBLE_LEFT_X + DISPLAY_AI_CHAT_BUBBLE_TEXT_X)) - \
     (4 * DISPLAY_AI_CHAT_CJK_CHAR_WIDTH))
#define DISPLAY_WECHAT_HANGUP_TASK_STACK (64U * 1024U)
#define DISPLAY_WECHAT_HANGUP_TASK_PRIORITY 4
#define DISPLAY_WIFI_INDICATOR_MAX 32U
#define DISPLAY_WIFI_INDICATOR_BAR_COUNT 3U

typedef struct {
    uint32_t magic;
    lv_coord_t x;
    lv_coord_t y;
    lv_coord_t width;
    const ui_text_asset_t *current_asset;
    uint8_t font_size;
    lv_text_align_t align;
    lv_color_t color;
    bool layout_dirty;
} display_text_image_ctx_t;

typedef struct {
    lv_obj_t *bars[DISPLAY_WIFI_INDICATOR_BAR_COUNT];
    lv_obj_t *x_lines[2];
    lv_color_t active_color;
    lv_color_t inactive_color;
    bool active;
    bool status_valid;
    bool connected;
    uint8_t level;
} display_wifi_indicator_t;

typedef struct {
    uint32_t text_hash;
    int64_t utterance_id;
    lv_coord_t x;
    lv_coord_t y;
    lv_coord_t text_width;
    lv_coord_t text_height;
    lv_coord_t bubble_width;
    lv_coord_t bubble_height;
    uint8_t caption_type;
    uint8_t message_index;
    bool align_right;
} display_ai_message_layout_t;

static display_wifi_indicator_t s_wifi_indicators[DISPLAY_WIFI_INDICATOR_MAX];
static size_t s_wifi_indicator_count;

typedef enum {
    DISPLAY_WIFI_CONNECT_STATE_IDLE = 0,
    DISPLAY_WIFI_CONNECT_STATE_SELECT_FIRST,
    DISPLAY_WIFI_CONNECT_STATE_UNAVAILABLE,
    DISPLAY_WIFI_CONNECT_STATE_SHORT_PASSWORD,
    DISPLAY_WIFI_CONNECT_STATE_CONNECTING,
    DISPLAY_WIFI_CONNECT_STATE_FAILED,
    DISPLAY_WIFI_CONNECT_STATE_TIMEOUT,
    DISPLAY_WIFI_CONNECT_STATE_CONNECTED,
} display_wifi_connect_state_t;

typedef enum {
    DISPLAY_PAGE_HOME = 0,
    DISPLAY_PAGE_DEVICE,
    DISPLAY_PAGE_UUID_EDIT,
    DISPLAY_PAGE_SYSTEM,
    DISPLAY_PAGE_WIFI,
    DISPLAY_PAGE_WIFI_CONNECT,
    DISPLAY_PAGE_CALL,
    DISPLAY_PAGE_CALL_ADD,
    DISPLAY_PAGE_CALL_SCAN,
    DISPLAY_PAGE_CALL_LIST,
    DISPLAY_PAGE_CALL_ACTIVE,
    DISPLAY_PAGE_WECHAT,
    DISPLAY_PAGE_WECHAT_ADD,
    DISPLAY_PAGE_WECHAT_ADD_EDIT,
    DISPLAY_PAGE_WECHAT_LIST,
    DISPLAY_PAGE_WECHAT_ACTIVE,
    DISPLAY_PAGE_AI_CHAT,
    DISPLAY_PAGE_AI_CHAT_SETTINGS,
    DISPLAY_PAGE_NETWORK_TEST,
    DISPLAY_PAGE_TIRTC_CONFIG,
    DISPLAY_PAGE_TIRTC_SCAN,
    DISPLAY_PAGE_DRIVER_STATUS,
    DISPLAY_PAGE_DEVICE_INFO,
} display_page_id_t;

typedef enum {
    DISPLAY_SCAN_OWNER_CALL = 0,
    DISPLAY_SCAN_OWNER_TIRTC_CONFIG,
    DISPLAY_SCAN_OWNER_WECHAT_CONTACT,
} display_scan_owner_t;

typedef enum {
    DISPLAY_CALL_ADD_FIELD_DEVICE_ID = 0,
    DISPLAY_CALL_ADD_FIELD_PAIR_KEY,
    DISPLAY_CALL_ADD_FIELD_COUNT,
} display_call_add_field_t;

typedef enum {
    DISPLAY_DEVICE_VOLUME_RECEIVE_DOWN = 0,
    DISPLAY_DEVICE_VOLUME_RECEIVE_UP,
    DISPLAY_DEVICE_VOLUME_RECEIVE_MUTE,
    DISPLAY_DEVICE_VOLUME_SEND_DOWN,
    DISPLAY_DEVICE_VOLUME_SEND_UP,
    DISPLAY_DEVICE_VOLUME_SEND_MUTE,
} display_device_volume_action_t;

typedef enum {
    DISPLAY_CALL_VOLUME_MIC_DOWN = 0,
    DISPLAY_CALL_VOLUME_MIC_UP,
    DISPLAY_CALL_VOLUME_SPEAKER_DOWN,
    DISPLAY_CALL_VOLUME_SPEAKER_UP,
} display_call_volume_action_t;

typedef enum {
    DISPLAY_AI_SETTING_MIC_DOWN = 0,
    DISPLAY_AI_SETTING_MIC_UP,
    DISPLAY_AI_SETTING_SPEAKER_DOWN,
    DISPLAY_AI_SETTING_SPEAKER_UP,
    DISPLAY_AI_SETTING_AVATAR_BUDDY,
    DISPLAY_AI_SETTING_AVATAR_SPROUT,
} display_ai_setting_action_t;

typedef enum {
    DISPLAY_AI_SCROLL_PAGE_UP = 0,
    DISPLAY_AI_SCROLL_PAGE_DOWN,
    DISPLAY_AI_SCROLL_BOTTOM,
} display_ai_scroll_action_t;

typedef struct {
    uint16_t x;
    uint16_t y;
} display_debug_tap_request_t;

typedef struct {
    uint16_t x;
    uint16_t y;
    int16_t dx;
    int16_t dy;
} display_debug_scroll_request_t;

static display_wifi_connect_state_t s_wifi_connect_state;
static display_page_id_t s_wifi_parent_page = DISPLAY_PAGE_DEVICE;
static display_page_id_t s_uuid_parent_page = DISPLAY_PAGE_DEVICE;

static lv_obj_t *s_home_page;
static lv_obj_t *s_home_carousel;
static lv_obj_t *s_home_content_pages[2];
static lv_obj_t *s_home_indicator_dots[2];
static lv_obj_t *s_home_nav_prev_btn;
static lv_obj_t *s_home_nav_next_btn;
static lv_obj_t *s_home_time_label;
static lv_obj_t *s_home_wifi_bars[3];
static lv_obj_t *s_home_wifi_x_lines[2];
static time_t s_home_clock_last_second = (time_t)-1;
static bool s_home_wifi_status_valid;
static bool s_home_wifi_connected;
static uint8_t s_home_wifi_level;
static bool s_home_indicator_valid;
static bool s_home_indicator_second_page;
static lv_obj_t *s_main_page;
static lv_obj_t *s_call_page;
static lv_obj_t *s_call_add_page;
static lv_obj_t *s_call_add_edit_page;
static lv_obj_t *s_call_scan_page;
static lv_obj_t *s_call_list_page;
static lv_obj_t *s_call_active_page;
static lv_obj_t *s_wechat_page;
static lv_obj_t *s_wechat_add_page;
static lv_obj_t *s_wechat_add_edit_page;
static lv_obj_t *s_wechat_list_page;
static lv_obj_t *s_wechat_active_page;
static lv_obj_t *s_uuid_edit_page;
static lv_obj_t *s_system_page;
static lv_obj_t *s_system_ram_label;
static lv_obj_t *s_wifi_page;
static lv_obj_t *s_wifi_connect_page;
static lv_obj_t *s_network_test_page;
static lv_obj_t *s_tirtc_config_page;
static lv_obj_t *s_tirtc_config_edit_page;
static lv_obj_t *s_test_page;
static lv_obj_t *s_ota_page;
static lv_obj_t *s_ai_chat_page;
static lv_obj_t *s_ai_chat_settings_page;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_uuid_keyboard;
static lv_obj_t *s_uuid_label;
static lv_obj_t *s_device_qrcode;
static lv_obj_t *s_device_qr_overlay;
static lv_obj_t *s_device_qr_overlay_qrcode;
static lv_obj_t *s_main_device_id_label;
static lv_obj_t *s_device_connection_dot;
static lv_obj_t *s_device_door_dot;
static lv_obj_t *s_device_connection_value_label;
static lv_obj_t *s_device_door_value_label;
static lv_obj_t *s_device_receive_volume_label;
static lv_obj_t *s_device_send_volume_label;
static lv_obj_t *s_device_receive_mute_label;
static lv_obj_t *s_device_send_mute_label;
static uint8_t s_device_receive_restore_volume = 50U;
static uint8_t s_device_send_restore_volume = 50U;
static bool s_device_receive_restore_valid;
static bool s_device_send_restore_valid;
static lv_obj_t *s_uuid_ta;
static lv_obj_t *s_uuid_edit_hint_label;
static lv_obj_t *s_uuid_edit_length_label;
static lv_obj_t *s_uuid_edit_status_label;
static lv_obj_t *s_main_hint_label;
static lv_obj_t *s_remote_video_img;
static lv_obj_t *s_remote_video_placeholder_label;
static lv_obj_t *s_wifi_connection_state_label;
static lv_obj_t *s_wifi_scan_state_label;
static lv_obj_t *s_wifi_scan_count_label;
static lv_obj_t *s_wifi_list;
static lv_obj_t *s_wifi_list_buttons[DISPLAY_WIFI_SCAN_MAX];
static lv_obj_t *s_wifi_list_ssid_labels[DISPLAY_WIFI_SCAN_MAX];
static lv_obj_t *s_wifi_list_rssi_labels[DISPLAY_WIFI_SCAN_MAX];
static lv_obj_t *s_wifi_connect_hint_label;
static lv_obj_t *s_wifi_connect_rssi_label;
static lv_obj_t *s_wifi_connect_details_label;
static lv_obj_t *s_network_summary_wifi_label;
static lv_obj_t *s_network_summary_ip_label;
static lv_obj_t *s_network_gateway_value_label;
static lv_obj_t *s_network_dns_value_label;
static lv_obj_t *s_network_wan_value_label;
static lv_obj_t *s_network_service_row;
static lv_obj_t *s_network_service_value_label;
static lv_obj_t *s_network_loss_value_label;
static lv_obj_t *s_network_result_box;
static lv_obj_t *s_network_result_label;
static lv_obj_t *s_network_result_detail_label;
static lv_obj_t *s_tirtc_config_value_labels[DISPLAY_TIRTC_CONFIG_FIELD_COUNT];
static lv_obj_t *s_tirtc_edit_ta;
static lv_obj_t *s_tirtc_edit_keyboard;
static lv_obj_t *s_tirtc_edit_hint_label;
static lv_obj_t *s_tirtc_edit_length_label;
static lv_obj_t *s_tirtc_edit_status_label;
static display_tirtc_config_field_t s_tirtc_edit_field;
static lv_obj_t *s_tirtc_test_session_label;
static lv_obj_t *s_tirtc_test_link_label;
static lv_obj_t *s_tirtc_test_token_value_label;
static lv_obj_t *s_tirtc_test_room_value_label;
static lv_obj_t *s_tirtc_test_tx_audio_value_label;
static lv_obj_t *s_tirtc_test_rx_audio_value_label;
static lv_obj_t *s_tirtc_test_disconnect_value_label;
static lv_obj_t *s_tirtc_test_result_label;
static lv_obj_t *s_tirtc_test_result_detail_label;
static lv_obj_t *s_call_duration_label;
static lv_obj_t *s_call_mic_value_label;
static lv_obj_t *s_call_speaker_value_label;
static lv_obj_t *s_call_qrcode;
static lv_obj_t *s_call_add_value_labels[DISPLAY_CALL_ADD_FIELD_COUNT];
static lv_obj_t *s_call_add_edit_ta;
static lv_obj_t *s_call_add_edit_keyboard;
static lv_obj_t *s_call_add_edit_hint_label;
static lv_obj_t *s_call_add_edit_length_label;
static lv_obj_t *s_call_add_edit_status_label;
static lv_obj_t *s_call_scan_img;
static lv_obj_t *s_call_scan_info_overlay;
static lv_obj_t *s_wechat_qrcode;
static lv_obj_t *s_wechat_duration_label;
static lv_obj_t *s_wechat_mic_value_label;
static lv_obj_t *s_wechat_speaker_value_label;
static lv_obj_t *s_wechat_scan_info_overlay;
static lv_obj_t *s_wechat_add_open_id_label;
static lv_obj_t *s_wechat_add_edit_ta;
static lv_obj_t *s_wechat_add_edit_keyboard;
static lv_obj_t *s_wechat_add_edit_hint_label;
static lv_obj_t *s_wechat_add_edit_length_label;
static lv_obj_t *s_wechat_add_edit_status_label;
static lv_obj_t *s_wechat_empty_label;
static lv_obj_t *s_wechat_contact_rows[DISPLAY_WECHAT_CONTACT_MAX];
static lv_obj_t *s_wechat_contact_open_id_labels[DISPLAY_WECHAT_CONTACT_MAX];
static lv_obj_t *s_test_status_label;
static lv_obj_t *s_test_video_btn;
static lv_obj_t *s_test_video_btn_label;
static lv_obj_t *s_test_audio_btn;
static lv_obj_t *s_test_audio_btn_label;
static lv_obj_t *s_ota_status_label;
static lv_obj_t *s_ota_version_label;
static lv_obj_t *s_ota_second_label;
static lv_obj_t *s_ota_second_value_label;
static lv_obj_t *s_ota_url_label;
static lv_obj_t *s_ota_size_label;
static lv_obj_t *s_ota_progress_bar;
static lv_obj_t *s_ota_start_btn;
static lv_obj_t *s_ota_start_btn_label;
static lv_obj_t *s_ota_reboot_btn;
static lv_obj_t *s_ota_reboot_btn_label;
static lv_obj_t *s_ota_progress_title_label;
static lv_obj_t *s_ota_progress_percent_label;
static lv_obj_t *s_ota_progress_hint_label;
static lv_obj_t *s_ota_action_panel;
static lv_obj_t *s_ai_status_label;
static lv_obj_t *s_ai_avatar_img;
static uint8_t s_ai_avatar_last_variant = UINT8_MAX;
static ai_chat_avatar_state_t s_ai_avatar_last_state = AI_CHAT_AVATAR_STATE_COUNT;
static lv_obj_t *s_ai_caption_bar;
static lv_obj_t *s_ai_single_caption_label;
static lv_obj_t *s_ai_message_list;
static lv_obj_t *s_ai_message_boxes[DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX];
static lv_obj_t *s_ai_message_labels[DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX];
static lv_obj_t *s_ai_message_bold_labels[DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX]
                                               [DISPLAY_AI_CHAT_CAPTION_BOLD_LAYER_COUNT];
static lv_obj_t *s_ai_message_scroll_spacer;
static EXT_RAM_BSS_ATTR display_ai_message_layout_t s_ai_message_layouts[DISPLAY_AI_CHAT_MESSAGE_MAX];
static uint8_t s_ai_visible_message_indices[DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX];
static uint32_t s_ai_visible_message_generations[DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX];
static uint32_t s_ai_visible_message_hashes[DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX];
static lv_coord_t s_ai_visible_message_y[DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX];
static lv_coord_t s_ai_visible_message_bottom_y[DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX];
static uint8_t s_ai_message_layout_count;
static uint32_t s_ai_message_layout_generation;
static lv_coord_t s_ai_message_content_height;
static lv_coord_t s_ai_new_chat_button_y;
static bool s_ai_message_layout_font_ready;
static bool s_ai_message_layout_new_button_visible;
static lv_obj_t *s_ai_new_chat_btn;
static lv_obj_t *s_ai_new_chat_btn_label;
static lv_obj_t *s_ai_scroll_up_btn;
static lv_obj_t *s_ai_scroll_down_btn;
static lv_obj_t *s_ai_scroll_bottom_btn;
static bool s_ai_message_touching;
static int64_t s_ai_message_scroll_render_last_us;
static int64_t s_ai_message_scroll_render_last_log_us;
static lv_obj_t *s_ai_settings_mic_value_label;
static lv_obj_t *s_ai_settings_speaker_value_label;
static lv_obj_t *s_ai_settings_avatar_buttons[DISPLAY_AI_AVATAR_COUNT];
static lv_obj_t *s_ai_settings_avatar_labels[DISPLAY_AI_AVATAR_COUNT];
static lv_obj_t *s_password_ta;
static lv_img_dsc_t s_remote_video_dsc[2];
static uint16_t *s_remote_video_buffers[DISPLAY_REMOTE_VIDEO_BUFFER_COUNT];
static size_t s_remote_video_pixels;
static size_t s_remote_video_dsc_index;
static lv_img_dsc_t s_call_scan_preview_dsc[2];
static uint16_t *s_call_scan_preview_buffers[2];
static size_t s_call_scan_preview_pixels;
static size_t s_call_scan_preview_index;
static bool s_call_scan_active;
static display_scan_owner_t s_scan_owner;
static display_call_add_field_t s_call_add_edit_field;
static EXT_RAM_BSS_ATTR char s_device_qr_payload[DISPLAY_DEVICE_QR_PAYLOAD_MAX];
static EXT_RAM_BSS_ATTR char s_call_qr_payload[DISPLAY_CONTACT_QR_PAYLOAD_MAX];
static EXT_RAM_BSS_ATTR char s_wechat_qr_payload[DISPLAY_CONTACT_QR_PAYLOAD_MAX];
static int64_t s_call_active_started_us;
static int64_t s_wechat_active_started_us;

static void display_wifi_ap_select_cb(lv_event_t *event);
static void display_show_home_page(void);
static void display_show_main_page(void);
static void display_show_call_page(void);
static void display_show_call_add_page(void);
static void display_show_call_add_edit_page(display_call_add_field_t field);
static void display_show_call_scan_page(void);
static void display_show_tirtc_config_scan_page(void);
static void display_show_call_list_page(void);
static void display_show_call_active_page(void);
static void display_show_wechat_page(void);
static void display_show_wechat_add_page(void);
static void display_show_wechat_add_edit_page(void);
static void display_show_wechat_list_page(void);
static void display_show_wechat_active_page(void);
static void display_show_system_page(void);
static void display_show_wifi_page(void);
static void display_open_wifi_page(display_page_id_t parent_page);
static void display_show_uuid_edit_page(void) __attribute__((unused));
static void display_show_ai_chat_page(void);
static void display_show_network_test_page(void);
static void display_show_tirtc_config_page(void);
static void display_show_tirtc_config_edit_page(display_tirtc_config_field_t field);
static void display_show_tirtc_test_page(void);
static void display_show_ota_page(void);
static void display_show_ai_chat_settings_page(void);
static void display_home_view_btn_cb(lv_event_t *event);
static void display_home_call_btn_cb(lv_event_t *event);
static void display_home_wechat_btn_cb(lv_event_t *event);
static void display_home_ai_btn_cb(lv_event_t *event);
static void display_home_settings_btn_cb(lv_event_t *event);
static void display_home_prev_btn_cb(lv_event_t *event);
static void display_home_next_btn_cb(lv_event_t *event);
static void display_binding_wifi_btn_cb(lv_event_t *event);
static void display_binding_refresh_btn_cb(lv_event_t *event);
static void display_ai_back_btn_cb(lv_event_t *event);
static void display_ai_settings_btn_cb(lv_event_t *event);
static void display_ai_settings_back_btn_cb(lv_event_t *event);
static void display_ai_settings_action_btn_cb(lv_event_t *event);
static void display_call_back_btn_cb(lv_event_t *event);
static void display_call_child_back_btn_cb(lv_event_t *event);
static void display_call_add_btn_cb(lv_event_t *event);
static void display_call_list_btn_cb(lv_event_t *event);
static void display_call_scan_btn_cb(lv_event_t *event);
static void display_call_scan_tap_cb(lv_event_t *event);
static void display_call_scan_info_btn_cb(lv_event_t *event);
static void display_call_scan_info_close_btn_cb(lv_event_t *event);
static void display_call_add_field_btn_cb(lv_event_t *event);
static void display_call_add_edit_back_btn_cb(lv_event_t *event);
static void display_call_add_edit_save_btn_cb(lv_event_t *event);
static void display_call_confirm_add_btn_cb(lv_event_t *event);
static void display_call_contact_call_btn_cb(lv_event_t *event);
static void display_call_contact_delete_btn_cb(lv_event_t *event);
static void display_call_delete_cancel_btn_cb(lv_event_t *event);
static void display_call_delete_confirm_btn_cb(lv_event_t *event);
static void display_call_hangup_btn_cb(lv_event_t *event);
static void display_call_volume_btn_cb(lv_event_t *event);
static void display_wechat_child_back_btn_cb(lv_event_t *event);
static void display_wechat_add_btn_cb(lv_event_t *event);
static void display_wechat_list_btn_cb(lv_event_t *event);
static void display_wechat_scan_btn_cb(lv_event_t *event);
static void display_wechat_scan_info_btn_cb(lv_event_t *event);
static void display_wechat_scan_info_close_btn_cb(lv_event_t *event);
static void display_wechat_confirm_add_btn_cb(lv_event_t *event);
static void display_wechat_add_field_btn_cb(lv_event_t *event);
static void display_wechat_add_edit_back_btn_cb(lv_event_t *event);
static void display_wechat_add_edit_save_btn_cb(lv_event_t *event);
static void display_wechat_contact_call_btn_cb(lv_event_t *event);
static void display_wechat_contact_delete_cb(lv_event_t *event);
static void display_wechat_delete_cancel_btn_cb(lv_event_t *event);
static void display_wechat_delete_confirm_btn_cb(lv_event_t *event);
static void display_wechat_hangup_btn_cb(lv_event_t *event);
static void display_wechat_volume_btn_cb(lv_event_t *event);
static void display_system_ota_btn_cb(lv_event_t *event);
static void display_system_tirtc_config_btn_cb(lv_event_t *event);
static void display_system_tirtc_test_btn_cb(lv_event_t *event);
static void display_network_test_start_btn_cb(lv_event_t *event);
static void __attribute__((unused)) display_tirtc_config_field_btn_cb(lv_event_t *event);
static void __attribute__((unused)) display_tirtc_config_scan_btn_cb(lv_event_t *event);
static void display_tirtc_config_edit_back_btn_cb(lv_event_t *event);
static void display_tirtc_config_edit_save_btn_cb(lv_event_t *event);
static void display_call_add_edit_textarea_event_cb(lv_event_t *event);
static void display_wechat_add_edit_textarea_event_cb(lv_event_t *event);
static void display_tirtc_edit_textarea_event_cb(lv_event_t *event);
static void display_update_call_add_field_labels(void);
static void display_update_call_add_edit_feedback(const char *status_text, lv_color_t status_color);
static void display_update_wechat_add_field_label(void);
static void display_update_wechat_add_edit_feedback(const char *status_text, lv_color_t status_color);
static void display_update_tirtc_edit_feedback(const char *status_text, lv_color_t status_color);
static const char *display_tirtc_config_field_title(display_tirtc_config_field_t field);
static const char *display_tirtc_config_field_value(const display_status_t *status,
                                                    display_tirtc_config_field_t field);
static size_t display_tirtc_config_field_max_len(display_tirtc_config_field_t field);
static void display_tirtc_test_start_btn_cb(lv_event_t *event);
static void display_ota_start_btn_cb(lv_event_t *event);
static void display_ota_reboot_btn_cb(lv_event_t *event);
static void display_build_system_page(lv_obj_t *screen);
static void display_build_call_page(lv_obj_t *screen);
static void display_build_call_add_page(lv_obj_t *screen);
static void display_build_call_add_edit_page(lv_obj_t *screen);
static void display_build_call_scan_page(lv_obj_t *screen);
static void display_build_call_list_page(lv_obj_t *screen);
static void display_build_call_active_page(lv_obj_t *screen);
static void display_build_wechat_page(lv_obj_t *screen);
static void display_build_wechat_add_page(lv_obj_t *screen);
static void display_build_wechat_add_edit_page(lv_obj_t *screen);
static void display_build_wechat_list_page(lv_obj_t *screen);
static void display_build_wechat_active_page(lv_obj_t *screen);
static void display_build_network_test_page(lv_obj_t *screen);
static void display_build_tirtc_config_page(lv_obj_t *screen);
static void display_build_tirtc_config_edit_page(lv_obj_t *screen);
static void display_build_test_page(lv_obj_t *screen);
static void display_build_ai_chat_page(lv_obj_t *screen);
static void display_build_ai_chat_settings_page(lv_obj_t *screen);
static void display_build_ota_page(lv_obj_t *screen);
static void display_build_uuid_edit_page(lv_obj_t *screen);
static void display_build_wifi_page(lv_obj_t *screen);
static void display_build_wifi_connect_page(lv_obj_t *screen);
static void display_build_binding_prompt_overlay(lv_obj_t *parent);
static void display_refresh_wifi_list(const display_status_t *status);
static void display_update_wifi_scan_state(const display_status_t *status);
static void display_update_network_test_page(const display_status_t *status);
static void display_update_tirtc_config_page(const display_status_t *status);
static void display_update_call_page(const display_status_t *status);
static void display_update_call_active_page(const display_status_t *status);
static void display_update_wechat_page(const display_status_t *status);
static void display_update_wechat_contact_list(const display_status_t *status);
static void display_update_wechat_active_page(const display_status_t *status);
static void display_update_test_page(const display_status_t *status);
static void display_update_ota_page(const display_status_t *status);
static void display_update_main_page(const display_status_t *status);
static void display_update_ai_chat_page(const display_status_t *status);
static void display_update_ai_chat_settings_page(const display_status_t *status);
static void display_update_ai_chat_scroll_spacer(void);
static void display_render_ai_chat_visible_messages(const display_status_t *status);
static void display_update_ai_chat_scroll_controls(void);
static void display_ai_chat_render_scroll_limited(const display_status_t *status, bool force);
static void display_update_binding_prompt(const display_status_t *status);
static void display_show_wifi_alert(const char *title, const char *message);
static const lv_font_t *display_ascii_font(uint8_t size);
static const lv_font_t *display_ai_chat_font(void);
static void display_apply_ai_dialog_font_if_ready(void);
static void display_text_set(lv_obj_t *obj, const char *text);
static void display_text_set_color(lv_obj_t *obj, lv_color_t color, lv_style_selector_t selector);
static void display_text_set_layout(lv_obj_t *obj,
                                    lv_coord_t x,
                                    lv_coord_t y,
                                    lv_coord_t width,
                                    lv_text_align_t align);
static lv_obj_t *display_create_figma_text(lv_obj_t *parent,
                                           const char *text,
                                           lv_coord_t x,
                                           lv_coord_t y,
                                           lv_coord_t width,
                                           lv_color_t color,
                                           uint8_t font_size,
                                           lv_text_align_t align);
static lv_obj_t *display_create_ai_text(lv_obj_t *parent,
                                        const char *text,
                                        lv_coord_t x,
                                        lv_coord_t y,
                                        lv_coord_t width,
                                        lv_color_t color,
                                        lv_text_align_t align);
static lv_obj_t *display_create_ai_dialog_text(lv_obj_t *parent,
                                               const char *text,
                                               lv_coord_t x,
                                               lv_coord_t y,
                                               lv_coord_t width,
                                               lv_color_t color,
                                               lv_text_align_t align);
static lv_obj_t *display_create_ai_static_text(lv_obj_t *parent,
                                               const char *text,
                                               lv_coord_t x,
                                               lv_coord_t y,
                                               lv_coord_t width,
                                               lv_color_t color,
                                               uint8_t font_size,
                                               lv_text_align_t align);
static lv_obj_t *display_create_figma_box(lv_obj_t *parent,
                                          lv_coord_t x,
                                          lv_coord_t y,
                                          lv_coord_t width,
                                          lv_coord_t height,
                                          lv_color_t fill,
                                          lv_color_t stroke,
                                          lv_coord_t radius);
static lv_obj_t *display_create_figma_button(lv_obj_t *parent,
                                             lv_coord_t x,
                                             lv_coord_t y,
                                             lv_coord_t width,
                                             lv_coord_t height,
                                             lv_color_t fill,
                                             lv_color_t stroke,
                                             const char *text,
                                             lv_color_t text_color,
                                             uint8_t font_size,
                                             lv_event_cb_t cb);
static void display_device_volume_btn_cb(lv_event_t *event);
static void display_layout_wifi_keyboard(void);
static void display_set_password_placeholder(const char *text, lv_color_t border_color);
static bool display_page_is_visible(lv_obj_t *page);
static uint8_t display_adjust_volume(uint8_t current, int delta);
static void display_update_home_status_bar(const display_status_t *status);
static void display_update_home_indicators(void);
static void display_home_scroll_to_page(bool second_page);
static esp_err_t display_enter_app(display_app_id_t app_id);
static void display_return_home(void);
static void display_hide_call_alert(void);
static void display_hide_call_delete_confirm(void);
static void display_show_call_delete_confirm(uint8_t contact_index);
static void display_hide_wechat_delete_confirm(void);
static void display_show_wechat_delete_confirm(uint8_t contact_index);

#define DISPLAY_WIFI_KEYBOARD_HEIGHT         128
#define DISPLAY_WIFI_CONNECT_INPUT_WIDTH     304
#define DISPLAY_WIFI_CONNECT_INPUT_HEIGHT    31
#define DISPLAY_WIFI_CONNECT_INPUT_TOP       58
#define DISPLAY_WIFI_CONNECT_HINT_TOP        36
#define DISPLAY_WIFI_CONNECT_HINT_LEFT       8
#define DISPLAY_WIFI_CONNECT_HINT_WIDTH      210
#define DISPLAY_WIFI_CONNECT_RSSI_WIDTH      70
#define DISPLAY_WIFI_CONNECT_DETAILS_TOP     100
#define DISPLAY_WIFI_CONNECT_DETAILS_WIDTH   280
#define DISPLAY_WIFI_CONNECT_TIMEOUT_US      (25LL * 1000000LL)
#define DISPLAY_WIFI_PASSWORD_MIN_LEN        8
#define DISPLAY_WIFI_KEYBOARD_LEFT           4
#define DISPLAY_WIFI_KEYBOARD_TOP            104
#define DISPLAY_WIFI_KEYBOARD_WIDTH          (DISPLAY_DRIVER_WIDTH - (DISPLAY_WIFI_KEYBOARD_LEFT * 2))
#define DISPLAY_WIFI_LIST_SSID_WIDTH         188
#define DISPLAY_WIFI_LIST_RSSI_WIDTH         68
#define DISPLAY_WIFI_LIST_BUILD_BATCH        1
#define DISPLAY_WIFI_STATUS_LEFT_WIDTH       196
#define DISPLAY_WIFI_STATUS_RIGHT_WIDTH      96
#define DISPLAY_WIFI_SCAN_REFRESH_GRACE_US   (500LL * 1000LL)
#define DISPLAY_WIFI_SCAN_REFRESH_TIMEOUT_US (8LL * 1000000LL)
#define DISPLAY_DEVICE_VOLUME_RESTORE_DEFAULT 50U
#define DISPLAY_REMOTE_VIDEO_WIDTH           110
#define DISPLAY_REMOTE_VIDEO_HEIGHT          82
#define DISPLAY_MIN_VALID_UNIX_TIME          1672531200LL
#define DISPLAY_UUID_INPUT_WIDTH             304
#define DISPLAY_UUID_INPUT_HEIGHT            31
#define DISPLAY_UUID_INPUT_TOP               55
#define DISPLAY_UUID_HINT_TOP                35
#define DISPLAY_UUID_HINT_LEFT               8
#define DISPLAY_UUID_HINT_WIDTH              196
#define DISPLAY_UUID_LENGTH_WIDTH            72
#define DISPLAY_UUID_STATUS_TOP              91
#define DISPLAY_UUID_STATUS_WIDTH            304
#define DISPLAY_UUID_KEYBOARD_TOP            112
#define DISPLAY_UUID_KEYBOARD_LEFT           4
#define DISPLAY_UUID_KEYBOARD_WIDTH          (DISPLAY_DRIVER_WIDTH - (DISPLAY_UUID_KEYBOARD_LEFT * 2))
#define DISPLAY_UUID_KEYBOARD_HEIGHT         112
#define DISPLAY_CALL_CONTACT_COUNT           DISPLAY_CALL_CONTACT_MAX
#define DISPLAY_WECHAT_CONTACT_COUNT         DISPLAY_WECHAT_CONTACT_MAX
#define DISPLAY_WECHAT_OPEN_ID_LENGTH        28U
#define DISPLAY_WECHAT_OPEN_ID_ACCEPTED_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-"
#define DISPLAY_CALL_VOLUME_STEP             8
#define DISPLAY_SCAN_PREVIEW_BUFFER_COUNT    1
#define DISPLAY_KB_BTN_KEYBOARD_ID           35
#define DISPLAY_KB_BTN_CURSOR_LEFT_ID        36
#define DISPLAY_KB_BTN_SPACE_ID              37
#define DISPLAY_KB_BTN_CURSOR_RIGHT_ID       38
#define DISPLAY_KB_BTN_JOIN_ID               39
#define DISPLAY_TIRTC_VERSION_TEXT           "TiRTC 2.2.0"
#define DISPLAY_AI_CHAT_STATE_IDLE           0U
#define DISPLAY_AI_CHAT_STATE_STARTING       1U
#define DISPLAY_AI_CHAT_STATE_TOKEN          2U
#define DISPLAY_AI_CHAT_STATE_CONNECTING     3U
#define DISPLAY_AI_CHAT_STATE_CONNECTED      4U
#define DISPLAY_AI_CHAT_STATE_STARTING_SESSION 5U
#define DISPLAY_AI_CHAT_STATE_IN_SESSION     6U
#define DISPLAY_AI_CHAT_STATE_STOPPING       7U
#define DISPLAY_AI_CHAT_STATE_ERROR          8U
#define DISPLAY_AI_CHAT_CAPTION_TYPE_ASR     0U
#define DISPLAY_AI_CHAT_CAPTION_TYPE_TTS     1U
#define DISPLAY_SCAN_RESULT_DISPATCH_TASK_STACK 8192
#define DISPLAY_SCAN_RESULT_DISPATCH_TASK_PRIORITY 2
#define DISPLAY_SCAN_RESULT_DISPATCH_TASK_ALLOC_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define DISPLAY_SCAN_RESULT_DISPATCH_RETRY_COUNT 20U
#define DISPLAY_SCAN_RESULT_DISPATCH_RETRY_MS 20U
#define DISPLAY_KB_BTN(width) (LV_BTNMATRIX_CTRL_POPOVER | (width))

typedef struct {
    esp_err_t result;
    char device_id[DISPLAY_CALL_CONTACT_DEVICE_ID_MAX];
    char pair_key[DISPLAY_CALL_CONTACT_PAIR_KEY_MAX];
    char open_id[DISPLAY_WECHAT_OPEN_ID_MAX];
    char raw_payload[DISPLAY_CONTACT_QR_PAYLOAD_MAX];
} display_contact_scan_result_event_t;

static const char * const s_wifi_keyboard_map_lc[] = {
    "1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "z", "x", "c", "v", "b", "n", "m", ".", ",", ":", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, "Space", LV_SYMBOL_RIGHT, "Join", ""
};

static const lv_btnmatrix_ctrl_t s_wifi_keyboard_ctrl_lc_map[] = {
    LV_KEYBOARD_CTRL_BTN_FLAGS | 5, DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4),
    DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4),
    DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4), LV_BTNMATRIX_CTRL_CHECKED | 7,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 6, DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3),
    DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3),
    DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3), LV_BTNMATRIX_CTRL_CHECKED | 7,
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1),
    LV_KEYBOARD_CTRL_BTN_FLAGS | 3, 2, 4, 2,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 5,
};

static const char * const s_wifi_keyboard_map_uc[] = {
    "1#", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "Z", "X", "C", "V", "B", "N", "M", ".", ",", ":", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, "Space", LV_SYMBOL_RIGHT, "Join", ""
};

static const lv_btnmatrix_ctrl_t s_wifi_keyboard_ctrl_uc_map[] = {
    LV_KEYBOARD_CTRL_BTN_FLAGS | 5, DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4),
    DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4),
    DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4), LV_BTNMATRIX_CTRL_CHECKED | 7,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 6, DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3),
    DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3),
    DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3), LV_BTNMATRIX_CTRL_CHECKED | 7,
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1),
    LV_KEYBOARD_CTRL_BTN_FLAGS | 3, 2, 4, 2,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 5,
};

static const char * const s_wifi_keyboard_map_spec[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "+", "&", "/", "*", "=", "%", "!", "?", "#", "<", ">", "\n",
    "\\", "@", "$", "(", ")", "{", "}", "[", "]", ";", "\"", "'", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, "Space", LV_SYMBOL_RIGHT, "Join", ""
};

static const lv_btnmatrix_ctrl_t s_wifi_keyboard_ctrl_spec_map[] = {
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | 2,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2, DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    LV_KEYBOARD_CTRL_BTN_FLAGS | 3, 2, 4, 2,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 5,
};

static const char * const s_uuid_keyboard_map[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    "Clear", "Z", "X", "C", "V", "B", "N", "M", ""
};

static EXT_RAM_BSS_ATTR display_call_contact_t s_call_contacts[DISPLAY_CALL_CONTACT_COUNT];
static uint8_t s_call_contact_count;
static EXT_RAM_BSS_ATTR char s_call_add_device_id[DISPLAY_CALL_CONTACT_DEVICE_ID_MAX];
static EXT_RAM_BSS_ATTR char s_call_add_pair_key[DISPLAY_CALL_CONTACT_PAIR_KEY_MAX];
static EXT_RAM_BSS_ATTR char s_wechat_add_open_id[DISPLAY_WECHAT_OPEN_ID_MAX];

static void display_set_wifi_keyboard_mode(lv_keyboard_mode_t mode)
{
    if (s_keyboard == NULL) {
        return;
    }

    switch (mode) {
    case LV_KEYBOARD_MODE_USER_2:
        lv_btnmatrix_set_map(s_keyboard, (const char **)s_wifi_keyboard_map_uc);
        lv_btnmatrix_set_ctrl_map(s_keyboard, s_wifi_keyboard_ctrl_uc_map);
        break;
    case LV_KEYBOARD_MODE_USER_3:
        lv_btnmatrix_set_map(s_keyboard, (const char **)s_wifi_keyboard_map_spec);
        lv_btnmatrix_set_ctrl_map(s_keyboard, s_wifi_keyboard_ctrl_spec_map);
        break;
    case LV_KEYBOARD_MODE_USER_1:
    default:
        lv_btnmatrix_set_map(s_keyboard, (const char **)s_wifi_keyboard_map_lc);
        lv_btnmatrix_set_ctrl_map(s_keyboard, s_wifi_keyboard_ctrl_lc_map);
        break;
    }
}

static void display_write_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void display_write_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
    dst[2] = (uint8_t)((value >> 16) & 0xFFU);
    dst[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static void display_json_escape(char *dst, size_t dst_len, const char *src)
{
    size_t out = 0;

    if (dst == NULL || dst_len == 0) {
        return;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return;
    }

    for (size_t index = 0; src[index] != '\0' && out + 1 < dst_len; ++index) {
        char ch = src[index];
        if ((ch == '"' || ch == '\\') && out + 2 < dst_len) {
            dst[out++] = '\\';
            dst[out++] = ch;
        } else if (ch != '"' && ch != '\\') {
            dst[out++] = ch;
        }
    }
    dst[out] = '\0';
}

static bool display_build_wechat_qr_payload(char *payload,
                                            size_t payload_size,
                                            const display_status_t *status)
{
    const char *device_id = NULL;

    if (payload == NULL || payload_size == 0) {
        return false;
    }
    if (status != NULL && status->tirtc_device_id[0] != '\0') {
        device_id = status->tirtc_device_id;
    }
    if (device_id == NULL || device_id[0] == '\0' || strlen(device_id) >= payload_size) {
        payload[0] = '\0';
        return false;
    }

    strlcpy(payload, device_id, payload_size);
    return true;
}

static bool display_build_contact_qr_payload(char *payload,
                                             size_t payload_size,
                                             const display_status_t *status)
{
    const char *device_id = "";
    const char *pair_key = "";
    char escaped_device_id[DISPLAY_DEVICE_QR_ESCAPED_VALUE_MAX] = {0};
    char escaped_pair_key[DISPLAY_DEVICE_QR_ESCAPED_VALUE_MAX] = {0};
    int written = 0;

    if (payload == NULL || payload_size == 0) {
        return false;
    }
    if (status != NULL && status->tirtc_device_id[0] != '\0') {
        device_id = status->tirtc_device_id;
    }
    if (status != NULL && status->tirtc_device_secret[0] != '\0') {
        pair_key = status->tirtc_device_secret;
    }
    display_json_escape(escaped_device_id, sizeof(escaped_device_id), device_id);
    display_json_escape(escaped_pair_key, sizeof(escaped_pair_key), pair_key);

    written = snprintf(payload,
                       payload_size,
                       "{\n"
                       "  \"device_id\": \"%s\",\n"
                       "  \"device_secret_key\": \"%s\"\n"
                       "}",
                       escaped_device_id,
                       escaped_pair_key);
    return written > 0 && written < (int)payload_size;
}

static uint16_t display_remote_video_zoom(uint16_t width, uint16_t height)
{
    if (width == 0 || height == 0) {
        return LV_IMG_ZOOM_NONE;
    }

    uint32_t zoom_x = (DISPLAY_REMOTE_VIDEO_WIDTH * LV_IMG_ZOOM_NONE) / width;
    uint32_t zoom_y = (DISPLAY_REMOTE_VIDEO_HEIGHT * LV_IMG_ZOOM_NONE) / height;
    uint32_t zoom = zoom_x < zoom_y ? zoom_x : zoom_y;

    if (zoom == 0) {
        zoom = 1;
    }
    if (zoom > LV_IMG_ZOOM_NONE) {
        zoom = LV_IMG_ZOOM_NONE;
    }
    return (uint16_t)zoom;
}

static void display_clear_remote_video_locked(const char *text)
{
    if (s_remote_video_img != NULL) {
        lv_obj_add_flag(s_remote_video_img, LV_OBJ_FLAG_HIDDEN);
        lv_img_set_src(s_remote_video_img, NULL);
    }
    if (s_remote_video_placeholder_label != NULL) {
        display_text_set(s_remote_video_placeholder_label,
                          text != NULL ? text : "Remote video\nwaiting");
        lv_obj_clear_flag(s_remote_video_placeholder_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_center(s_remote_video_placeholder_label);
    }
}

static esp_err_t display_ensure_remote_video_buffers(size_t pixel_count)
{
    bool buffers_ready = true;

    if (pixel_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t index = 0; index < DISPLAY_REMOTE_VIDEO_BUFFER_COUNT; ++index) {
        if (s_remote_video_buffers[index] == NULL) {
            buffers_ready = false;
            break;
        }
    }
    if (s_remote_video_pixels == pixel_count && buffers_ready) {
        return ESP_OK;
    }

    if (s_remote_video_img != NULL) {
        lv_obj_add_flag(s_remote_video_img, LV_OBJ_FLAG_HIDDEN);
        lv_img_set_src(s_remote_video_img, NULL);
    }
    for (size_t index = 0; index < DISPLAY_REMOTE_VIDEO_BUFFER_COUNT; ++index) {
        free(s_remote_video_buffers[index]);
        s_remote_video_buffers[index] = NULL;
        memset(&s_remote_video_dsc[index], 0, sizeof(s_remote_video_dsc[index]));
    }
    s_remote_video_pixels = 0;
    s_remote_video_dsc_index = 0;

    size_t bytes = pixel_count * sizeof(uint16_t);
    for (size_t index = 0; index < DISPLAY_REMOTE_VIDEO_BUFFER_COUNT; ++index) {
        s_remote_video_buffers[index] =
            heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_remote_video_buffers[index] == NULL) {
            s_remote_video_buffers[index] = malloc(bytes);
        }
        if (s_remote_video_buffers[index] == NULL) {
            for (size_t cleanup = 0; cleanup < DISPLAY_REMOTE_VIDEO_BUFFER_COUNT; ++cleanup) {
                free(s_remote_video_buffers[cleanup]);
                s_remote_video_buffers[cleanup] = NULL;
            }
            return ESP_ERR_NO_MEM;
        }
    }

    s_remote_video_pixels = pixel_count;
    return ESP_OK;
}

static void display_show_remote_video_locked(const uint16_t *pixels, uint16_t width, uint16_t height)
{
    if (s_remote_video_img == NULL || s_remote_video_placeholder_label == NULL || pixels == NULL || width == 0 || height == 0) {
        display_clear_remote_video_locked(NULL);
        return;
    }

    size_t pixel_count = (size_t)width * height;
    if (height != 0 && pixel_count / height != width) {
        display_clear_remote_video_locked("Remote video\ninvalid");
        return;
    }
    if (display_ensure_remote_video_buffers(pixel_count) != ESP_OK) {
        ESP_LOGW(TAG, "remote video buffer allocation failed: %ux%u", (unsigned)width, (unsigned)height);
        display_clear_remote_video_locked("Remote video\nno memory");
        return;
    }

    s_remote_video_dsc_index = (s_remote_video_dsc_index + 1U) % DISPLAY_REMOTE_VIDEO_BUFFER_COUNT;
    uint16_t *buffer = s_remote_video_buffers[s_remote_video_dsc_index];
    lv_img_dsc_t *frame_dsc = &s_remote_video_dsc[s_remote_video_dsc_index];

    memcpy(buffer, pixels, pixel_count * sizeof(uint16_t));
    memset(frame_dsc, 0, sizeof(*frame_dsc));
    frame_dsc->header.always_zero = 0;
    frame_dsc->header.w = width;
    frame_dsc->header.h = height;
    frame_dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
    frame_dsc->data_size = (uint32_t)(pixel_count * sizeof(uint16_t));
    frame_dsc->data = (const uint8_t *)buffer;

    lv_img_set_src(s_remote_video_img, frame_dsc);
    lv_img_set_zoom(s_remote_video_img, display_remote_video_zoom(width, height));
    lv_obj_center(s_remote_video_img);
    lv_obj_clear_flag(s_remote_video_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_remote_video_placeholder_label, LV_OBJ_FLAG_HIDDEN);
}

static uint16_t display_scan_preview_zoom(uint16_t width, uint16_t height)
{
    if (width == 0 || height == 0) {
        return LV_IMG_ZOOM_NONE;
    }

    uint32_t zoom_x = (DISPLAY_DRIVER_WIDTH * LV_IMG_ZOOM_NONE) / width;
    uint32_t zoom_y = (DISPLAY_DRIVER_HEIGHT * LV_IMG_ZOOM_NONE) / height;
    uint32_t zoom = zoom_x > zoom_y ? zoom_x : zoom_y;

    if (zoom == 0) {
        zoom = 1;
    }
    if (zoom > UINT16_MAX) {
        zoom = UINT16_MAX;
    }
    return (uint16_t)zoom;
}

static esp_err_t display_ensure_call_scan_preview_buffers(size_t pixel_count)
{
    bool buffers_ready = true;

    if (pixel_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t index = 0; index < DISPLAY_SCAN_PREVIEW_BUFFER_COUNT; ++index) {
        if (s_call_scan_preview_buffers[index] == NULL) {
            buffers_ready = false;
            break;
        }
    }
    if (s_call_scan_preview_pixels == pixel_count && buffers_ready) {
        return ESP_OK;
    }

    for (size_t index = 0; index < DISPLAY_SCAN_PREVIEW_BUFFER_COUNT; ++index) {
        free(s_call_scan_preview_buffers[index]);
        s_call_scan_preview_buffers[index] = NULL;
        memset(&s_call_scan_preview_dsc[index], 0, sizeof(s_call_scan_preview_dsc[index]));
    }
    s_call_scan_preview_pixels = 0;
    s_call_scan_preview_index = 0;

    size_t bytes = pixel_count * sizeof(uint16_t);
    for (size_t index = 0; index < DISPLAY_SCAN_PREVIEW_BUFFER_COUNT; ++index) {
        s_call_scan_preview_buffers[index] =
            heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_call_scan_preview_buffers[index] == NULL) {
            s_call_scan_preview_buffers[index] = malloc(bytes);
        }
        if (s_call_scan_preview_buffers[index] == NULL) {
            for (size_t cleanup = 0; cleanup < DISPLAY_SCAN_PREVIEW_BUFFER_COUNT; ++cleanup) {
                free(s_call_scan_preview_buffers[cleanup]);
                s_call_scan_preview_buffers[cleanup] = NULL;
            }
            return ESP_ERR_NO_MEM;
        }
    }

    s_call_scan_preview_pixels = pixel_count;
    return ESP_OK;
}

static void display_show_call_scan_preview_locked(const uint16_t *pixels, uint16_t width, uint16_t height)
{
    if (s_call_scan_img == NULL || pixels == NULL || width == 0 || height == 0) {
        return;
    }

    size_t pixel_count = (size_t)width * height;
    if (display_ensure_call_scan_preview_buffers(pixel_count) != ESP_OK) {
        ESP_LOGW(TAG, "call scan preview buffer allocation failed");
        return;
    }

    s_call_scan_preview_index =
        (s_call_scan_preview_index + 1U) % DISPLAY_SCAN_PREVIEW_BUFFER_COUNT;
    uint16_t *buffer = s_call_scan_preview_buffers[s_call_scan_preview_index];
    lv_img_dsc_t *frame_dsc = &s_call_scan_preview_dsc[s_call_scan_preview_index];

    memcpy(buffer, pixels, pixel_count * sizeof(uint16_t));
    memset(frame_dsc, 0, sizeof(*frame_dsc));
    frame_dsc->header.always_zero = 0;
    frame_dsc->header.w = width;
    frame_dsc->header.h = height;
    frame_dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
    frame_dsc->data_size = (uint32_t)(pixel_count * sizeof(uint16_t));
    frame_dsc->data = (const uint8_t *)buffer;

    lv_img_set_src(s_call_scan_img, frame_dsc);
    lv_img_set_zoom(s_call_scan_img, display_scan_preview_zoom(width, height));
    lv_obj_center(s_call_scan_img);
    lv_obj_clear_flag(s_call_scan_img, LV_OBJ_FLAG_HIDDEN);
}

static void display_call_scan_preview_cb(const uint16_t *rgb565_pixels,
                                         uint16_t width,
                                         uint16_t height,
                                         void *ctx)
{
    (void)ctx;

    if (rgb565_pixels == NULL || width == 0 || height == 0 || !s_display_initialized) {
        return;
    }
    if (!lvgl_port_lock(20)) {
        return;
    }
    if (s_call_scan_active) {
        display_show_call_scan_preview_locked(rgb565_pixels, width, height);
    }
    lvgl_port_unlock();
}

static bool display_text_has_visible_char(const char *text)
{
    if (text == NULL) {
        return false;
    }
    while (*text != '\0') {
        if ((uint8_t)*text > (uint8_t)' ') {
            return true;
        }
        ++text;
    }
    return false;
}

static void display_copy_trimmed_text(char *dst, size_t dst_len, const char *src)
{
    const char *begin = src;
    const char *end = NULL;
    size_t copy_len = 0;

    if (dst == NULL || dst_len == 0) {
        return;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return;
    }

    while (*begin != '\0' && (uint8_t)*begin <= (uint8_t)' ') {
        ++begin;
    }

    end = begin + strlen(begin);
    while (end > begin && (uint8_t)*(end - 1) <= (uint8_t)' ') {
        --end;
    }

    copy_len = (size_t)(end - begin);
    if (copy_len >= dst_len) {
        copy_len = dst_len - 1U;
    }
    memcpy(dst, begin, copy_len);
    dst[copy_len] = '\0';
}

static void display_reset_call_add_inputs(void)
{
    s_call_add_device_id[0] = '\0';
    s_call_add_pair_key[0] = '\0';
    display_update_call_add_field_labels();
}

static bool display_wechat_open_id_char_valid(char ch)
{
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           ch == '_' ||
           ch == '-';
}

static bool display_wechat_open_id_valid(const char *open_id)
{
    if (open_id == NULL || strlen(open_id) != DISPLAY_WECHAT_OPEN_ID_LENGTH) {
        return false;
    }

    for (size_t index = 0; index < DISPLAY_WECHAT_OPEN_ID_LENGTH; ++index) {
        if (!display_wechat_open_id_char_valid(open_id[index])) {
            return false;
        }
    }
    return true;
}

static void display_reset_wechat_add_input(void)
{
    s_wechat_add_open_id[0] = '\0';
    display_update_wechat_add_field_label();
}

static void display_invalidate_call_list_page(void)
{
    if (s_call_list_page != NULL) {
        lv_obj_add_flag(s_call_list_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_del_async(s_call_list_page);
        s_call_list_page = NULL;
    }
}

static bool display_call_contacts_match_status(const display_status_t *status)
{
    if (status == NULL) {
        return s_call_contact_count == 0;
    }

    uint8_t count = status->call_contact_count > DISPLAY_CALL_CONTACT_COUNT ?
        DISPLAY_CALL_CONTACT_COUNT : status->call_contact_count;
    if (s_call_contact_count != count) {
        return false;
    }
    for (uint8_t index = 0; index < count; ++index) {
        if (strcmp(s_call_contacts[index].device_id, status->call_contacts[index].device_id) != 0 ||
            strcmp(s_call_contacts[index].pair_key, status->call_contacts[index].pair_key) != 0 ||
            strcmp(s_call_contacts[index].last_time, status->call_contacts[index].last_time) != 0) {
            return false;
        }
    }
    return true;
}

static bool display_sync_call_contacts_from_status(const display_status_t *status)
{
    if (display_call_contacts_match_status(status)) {
        return false;
    }

    memset(s_call_contacts, 0, sizeof(s_call_contacts));
    s_call_contact_count = 0;
    if (status != NULL) {
        uint8_t count = status->call_contact_count > DISPLAY_CALL_CONTACT_COUNT ?
            DISPLAY_CALL_CONTACT_COUNT : status->call_contact_count;
        s_call_contact_count = count;
        for (uint8_t index = 0; index < count; ++index) {
            s_call_contacts[index] = status->call_contacts[index];
        }
    }
    return true;
}

static bool display_remove_call_contact(uint8_t contact_index)
{
    if (contact_index >= s_call_contact_count || contact_index >= DISPLAY_CALL_CONTACT_COUNT) {
        return false;
    }

    for (uint8_t index = contact_index; index + 1U < s_call_contact_count; ++index) {
        s_call_contacts[index] = s_call_contacts[index + 1U];
    }
    if (s_call_contact_count > 0) {
        --s_call_contact_count;
        memset(&s_call_contacts[s_call_contact_count], 0, sizeof(s_call_contacts[s_call_contact_count]));
    }
    return true;
}

static const char *display_call_add_field_title(display_call_add_field_t field)
{
    switch (field) {
    case DISPLAY_CALL_ADD_FIELD_PAIR_KEY:
        return "配对 Key";
    case DISPLAY_CALL_ADD_FIELD_DEVICE_ID:
    default:
        return "Device ID";
    }
}

static size_t display_call_add_field_max_len(display_call_add_field_t field)
{
    return field == DISPLAY_CALL_ADD_FIELD_PAIR_KEY ?
        DISPLAY_CALL_CONTACT_PAIR_KEY_MAX - 1U :
        DISPLAY_CALL_CONTACT_DEVICE_ID_MAX - 1U;
}

static char *display_call_add_field_buffer(display_call_add_field_t field)
{
    switch (field) {
    case DISPLAY_CALL_ADD_FIELD_PAIR_KEY:
        return s_call_add_pair_key;
    case DISPLAY_CALL_ADD_FIELD_DEVICE_ID:
    default:
        return s_call_add_device_id;
    }
}

static const char *display_call_add_field_placeholder(display_call_add_field_t field)
{
    return field == DISPLAY_CALL_ADD_FIELD_PAIR_KEY ? "A1B2C3D4" : "240617000001";
}

static void display_update_call_add_field_labels(void)
{
    for (uint8_t index = 0; index < DISPLAY_CALL_ADD_FIELD_COUNT; ++index) {
        display_call_add_field_t field = (display_call_add_field_t)index;
        lv_obj_t *label = s_call_add_value_labels[index];
        const char *value = display_call_add_field_buffer(field);

        if (label == NULL) {
            continue;
        }

        if (value[0] != '\0') {
            display_text_set_color(label, lv_color_hex(0x10233B), 0);
            display_text_set(label, value);
        } else {
            display_text_set_color(label, lv_color_hex(0x8AA0B5), 0);
            display_text_set(label, display_call_add_field_placeholder(field));
        }
    }
}

static void display_update_wechat_add_field_label(void)
{
    if (s_wechat_add_open_id_label == NULL) {
        return;
    }

    if (s_wechat_add_open_id[0] != '\0') {
        display_text_set_color(s_wechat_add_open_id_label, lv_color_hex(0x10233B), 0);
        display_text_set(s_wechat_add_open_id_label, s_wechat_add_open_id);
    } else {
        display_text_set_color(s_wechat_add_open_id_label, lv_color_hex(0x8AA0B5), 0);
        display_text_set(s_wechat_add_open_id_label, "28位微信Open ID");
    }
}

static void display_store_scanned_call_contact(const char *device_id, const char *pair_key)
{
    display_call_contact_t contact = {0};
    display_call_contact_t previous_contacts[DISPLAY_CALL_CONTACT_COUNT] = {0};
    uint8_t existing_index = DISPLAY_CALL_CONTACT_COUNT;
    uint8_t previous_count = s_call_contact_count;

    memcpy(previous_contacts, s_call_contacts, sizeof(previous_contacts));

    if (device_id == NULL || device_id[0] == '\0' ||
        pair_key == NULL || pair_key[0] == '\0') {
        return;
    }

    strlcpy(contact.device_id, device_id, sizeof(contact.device_id));
    strlcpy(contact.pair_key, pair_key, sizeof(contact.pair_key));
    strlcpy(contact.last_time, "Just now", sizeof(contact.last_time));

    time_t now = time(NULL);
    if (now >= DISPLAY_MIN_VALID_UNIX_TIME) {
        struct tm time_info = {0};
        localtime_r(&now, &time_info);
        strftime(contact.last_time,
                 sizeof(contact.last_time),
                 "%Y-%m-%d %H:%M",
                 &time_info);
    } else {
        strlcpy(contact.last_time, "Just now", sizeof(contact.last_time));
    }
    memcpy(s_call_contacts, previous_contacts, sizeof(s_call_contacts));
    s_call_contact_count = previous_count;

    for (uint8_t index = 0; index < s_call_contact_count; ++index) {
        if (strcmp(s_call_contacts[index].device_id, device_id) == 0) {
            existing_index = index;
            break;
        }
    }

    if (existing_index < s_call_contact_count) {
        for (uint8_t index = existing_index; index > 0; --index) {
            s_call_contacts[index] = s_call_contacts[index - 1];
        }
    } else if (s_call_contact_count < DISPLAY_CALL_CONTACT_COUNT) {
        s_call_contact_count++;
        for (uint8_t index = s_call_contact_count - 1U; index > 0; --index) {
            s_call_contacts[index] = s_call_contacts[index - 1];
        }
    } else {
        for (uint8_t index = DISPLAY_CALL_CONTACT_COUNT - 1U; index > 0; --index) {
            s_call_contacts[index] = s_call_contacts[index - 1];
        }
    }

    s_call_contacts[0] = contact;
}

static void display_store_scanned_wechat_contact(const char *open_id)
{
    display_wechat_contact_t contact = {0};
    uint8_t count = s_last_status.wechat_contact_count > DISPLAY_WECHAT_CONTACT_COUNT ?
        DISPLAY_WECHAT_CONTACT_COUNT : s_last_status.wechat_contact_count;
    uint8_t existing_index = DISPLAY_WECHAT_CONTACT_COUNT;

    if (open_id == NULL || open_id[0] == '\0') {
        return;
    }

    for (uint8_t index = 0; index < count; ++index) {
        if (strcmp(s_last_status.wechat_contacts[index].open_id, open_id) == 0) {
            existing_index = index;
            break;
        }
    }

    if (existing_index < count) {
        for (uint8_t index = existing_index; index > 0; --index) {
            s_last_status.wechat_contacts[index] = s_last_status.wechat_contacts[index - 1U];
        }
    } else if (count < DISPLAY_WECHAT_CONTACT_COUNT) {
        ++count;
        for (uint8_t index = count - 1U; index > 0; --index) {
            s_last_status.wechat_contacts[index] = s_last_status.wechat_contacts[index - 1U];
        }
    } else {
        for (uint8_t index = DISPLAY_WECHAT_CONTACT_COUNT - 1U; index > 0; --index) {
            s_last_status.wechat_contacts[index] = s_last_status.wechat_contacts[index - 1U];
        }
        count = DISPLAY_WECHAT_CONTACT_COUNT;
    }

    strlcpy(contact.open_id, open_id, sizeof(contact.open_id));
    s_last_status.wechat_contacts[0] = contact;
    s_last_status.wechat_contact_count = count;
}

static const char *display_contact_scan_error_text(esp_err_t result)
{
    switch (result) {
    case ESP_ERR_NOT_SUPPORTED:
        return "摄像头不可用";
    case ESP_ERR_INVALID_STATE:
        return "扫码服务忙";
    case ESP_ERR_NOT_FOUND:
        return "未识别二维码";
    case ESP_ERR_INVALID_RESPONSE:
        return "二维码格式错误";
    case ESP_ERR_TIMEOUT:
        return "摄像头超时";
    case ESP_ERR_NO_MEM:
        return "内存不足";
    default:
        return "扫码失败";
    }
}

static void display_call_scan_result_async_cb(void *arg)
{
    display_contact_scan_result_event_t *event = (display_contact_scan_result_event_t *)arg;

    if (event == NULL) {
        return;
    }

    s_call_scan_active = false;
    if (event->result == ESP_OK) {
        esp_err_t ret = s_actions.on_add_call_contact != NULL ?
            s_actions.on_add_call_contact(event->device_id, event->pair_key, s_actions.ctx) :
            ESP_ERR_INVALID_STATE;
        if (ret != ESP_OK) {
            display_show_call_add_page();
            display_show_wifi_alert("Add Contact", "Contact save failed.");
            free(event);
            return;
        }
        display_store_scanned_call_contact(event->device_id, event->pair_key);
        display_invalidate_call_list_page();
        display_show_call_list_page();
        display_show_wifi_alert("扫码添加", "扫描成功");
    } else {
        display_show_call_add_page();
        if (event->result == ESP_ERR_INVALID_RESPONSE && event->raw_payload[0] != '\0') {
            display_show_wifi_alert("QR Payload", event->raw_payload);
        } else {
            display_show_wifi_alert("扫码添加", display_contact_scan_error_text(event->result));
        }
    }

    free(event);
}

static void display_call_scan_result_cb(esp_err_t result,
                                        const char *device_id,
                                        const char *pair_key,
                                        const char *raw_payload,
                                        void *ctx)
{
    display_contact_scan_result_event_t *event = NULL;

    (void)ctx;

    event = calloc(1, sizeof(*event));
    if (event == NULL) {
        return;
    }

    event->result = result;
    if (device_id != NULL) {
        strlcpy(event->device_id, device_id, sizeof(event->device_id));
    }
    if (pair_key != NULL) {
        strlcpy(event->pair_key, pair_key, sizeof(event->pair_key));
    }
    if (raw_payload != NULL) {
        strlcpy(event->raw_payload, raw_payload, sizeof(event->raw_payload));
    }

    if (!lvgl_port_lock(100)) {
        free(event);
        return;
    }
    lv_res_t async_ret = lv_async_call(display_call_scan_result_async_cb, event);
    lvgl_port_unlock();
    if (async_ret != LV_RES_OK) {
        free(event);
    }
}

static void display_tirtc_config_scan_result_async_cb(void *arg)
{
    display_contact_scan_result_event_t *event = (display_contact_scan_result_event_t *)arg;

    if (event == NULL) {
        return;
    }

    s_call_scan_active = false;
    if (event->result == ESP_OK) {
        strlcpy(s_last_status.tirtc_device_id, event->device_id, sizeof(s_last_status.tirtc_device_id));
        strlcpy(s_last_status.tirtc_device_secret, event->pair_key, sizeof(s_last_status.tirtc_device_secret));
        display_show_tirtc_config_page();
        display_show_wifi_alert("TiRTC 配置", "扫描成功");
    } else {
        display_show_tirtc_config_page();
        if (event->result == ESP_ERR_INVALID_RESPONSE && event->raw_payload[0] != '\0') {
            display_show_wifi_alert("QR Payload", event->raw_payload);
        } else {
            display_show_wifi_alert("TiRTC 配置", display_contact_scan_error_text(event->result));
        }
    }

    free(event);
}

static bool display_queue_tirtc_config_scan_result(display_contact_scan_result_event_t *event,
                                                   bool log_failure)
{
    if (event == NULL) {
        return true;
    }

    if (!lvgl_port_lock(1)) {
        if (log_failure) {
            ESP_LOGW(TAG,
                     "tirtc config scan result dispatch waits for lvgl lock: result=%s",
                     esp_err_to_name(event->result));
        }
        return false;
    }

    lv_res_t async_ret = lv_async_call(display_tirtc_config_scan_result_async_cb, event);
    lvgl_port_unlock();
    if (async_ret != LV_RES_OK) {
        if (log_failure) {
            ESP_LOGW(TAG,
                     "tirtc config scan result dispatch failed: async result=%d",
                     (int)async_ret);
        }
        return false;
    }
    return true;
}

static void display_tirtc_config_scan_result_dispatch_task(void *arg)
{
    display_contact_scan_result_event_t *event = (display_contact_scan_result_event_t *)arg;

    for (uint8_t attempt = 0; attempt < DISPLAY_SCAN_RESULT_DISPATCH_RETRY_COUNT; ++attempt) {
        if (display_queue_tirtc_config_scan_result(event, attempt == 0)) {
            platform_task_reaper_delete_current_with_caps(TAG);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_SCAN_RESULT_DISPATCH_RETRY_MS));
    }

    ESP_LOGW(TAG, "tirtc config scan result dispatch abandoned after retries");
    free(event);
    platform_task_reaper_delete_current_with_caps(TAG);
}

static void __attribute__((unused)) display_tirtc_config_scan_result_cb(esp_err_t result,
                                                const char *device_id,
                                                const char *device_secret,
                                                const char *raw_payload,
                                                void *ctx)
{
    display_contact_scan_result_event_t *event = NULL;

    (void)ctx;

    event = calloc(1, sizeof(*event));
    if (event == NULL) {
        return;
    }

    event->result = result;
    if (device_id != NULL) {
        strlcpy(event->device_id, device_id, sizeof(event->device_id));
    }
    if (device_secret != NULL) {
        strlcpy(event->pair_key, device_secret, sizeof(event->pair_key));
    }
    if (raw_payload != NULL) {
        strlcpy(event->raw_payload, raw_payload, sizeof(event->raw_payload));
    }

    if (!display_queue_tirtc_config_scan_result(event, false)) {
        BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(display_tirtc_config_scan_result_dispatch_task,
                                                              "tirtc_ui_evt",
                                                              DISPLAY_SCAN_RESULT_DISPATCH_TASK_STACK,
                                                              event,
                                                              DISPLAY_SCAN_RESULT_DISPATCH_TASK_PRIORITY,
                                                              NULL,
                                                              APP_TASK_CORE_BACKGROUND,
                                                              DISPLAY_SCAN_RESULT_DISPATCH_TASK_ALLOC_CAPS);
        if (task_ret != pdPASS) {
            ESP_LOGW(TAG, "tirtc config scan result dispatch task create failed");
            free(event);
        }
    }
}

static void display_wechat_scan_result_async_cb(void *arg)
{
    display_contact_scan_result_event_t *event = (display_contact_scan_result_event_t *)arg;

    if (event == NULL) {
        return;
    }

    s_call_scan_active = false;
    if (event->result == ESP_OK) {
        display_store_scanned_wechat_contact(event->open_id);
        display_reset_wechat_add_input();
        display_show_wechat_list_page();
        display_show_wifi_alert("扫码添加", "扫描成功");
    } else {
        display_show_wechat_add_page();
        if (event->result == ESP_ERR_INVALID_RESPONSE && event->raw_payload[0] != '\0') {
            display_show_wifi_alert("QR Payload", event->raw_payload);
        } else {
            display_show_wifi_alert("扫码添加", display_contact_scan_error_text(event->result));
        }
    }

    free(event);
}

static void display_wechat_scan_result_cb(esp_err_t result,
                                          const char *open_id,
                                          const char *raw_payload,
                                          void *ctx)
{
    display_contact_scan_result_event_t *event = NULL;

    (void)ctx;

    event = calloc(1, sizeof(*event));
    if (event == NULL) {
        return;
    }

    event->result = result;
    if (open_id != NULL) {
        strlcpy(event->open_id, open_id, sizeof(event->open_id));
    }
    if (raw_payload != NULL) {
        strlcpy(event->raw_payload, raw_payload, sizeof(event->raw_payload));
    }

    if (!lvgl_port_lock(100)) {
        free(event);
        return;
    }
    lv_res_t async_ret = lv_async_call(display_wechat_scan_result_async_cb, event);
    lvgl_port_unlock();
    if (async_ret != LV_RES_OK) {
        free(event);
    }
}

static void display_set_main_hint(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_main_hint_text, sizeof(s_main_hint_text), fmt, args);
    va_end(args);

    if (s_main_hint_label != NULL) {
        display_text_set(s_main_hint_label, s_main_hint_text);
        if (s_main_hint_text[0] != '\0' && strcmp(s_main_hint_text, "Ready") != 0) {
            lv_obj_clear_flag(s_main_hint_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_main_hint_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void display_hide_keyboard(void)
{
    if (s_keyboard != NULL) {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_tirtc_edit_keyboard != NULL) {
        lv_keyboard_set_textarea(s_tirtc_edit_keyboard, NULL);
        lv_obj_add_flag(s_tirtc_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_call_add_edit_keyboard != NULL) {
        lv_keyboard_set_textarea(s_call_add_edit_keyboard, NULL);
        lv_obj_add_flag(s_call_add_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_wechat_add_edit_keyboard != NULL) {
        lv_keyboard_set_textarea(s_wechat_add_edit_keyboard, NULL);
        lv_obj_add_flag(s_wechat_add_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_password_ta != NULL) {
        lv_obj_clear_state(s_password_ta, LV_STATE_FOCUSED);
    }
    if (s_tirtc_edit_ta != NULL) {
        lv_obj_clear_state(s_tirtc_edit_ta, LV_STATE_FOCUSED);
    }
    if (s_call_add_edit_ta != NULL) {
        lv_obj_clear_state(s_call_add_edit_ta, LV_STATE_FOCUSED);
    }
    if (s_wechat_add_edit_ta != NULL) {
        lv_obj_clear_state(s_wechat_add_edit_ta, LV_STATE_FOCUSED);
    }
}

static void display_layout_wifi_keyboard(void)
{
    if (s_keyboard == NULL) {
        return;
    }

    lv_obj_set_pos(s_keyboard, DISPLAY_WIFI_KEYBOARD_LEFT, DISPLAY_WIFI_KEYBOARD_TOP);
    lv_obj_set_size(s_keyboard, DISPLAY_WIFI_KEYBOARD_WIDTH, DISPLAY_WIFI_KEYBOARD_HEIGHT);
    lv_obj_set_style_pad_all(s_keyboard, 0, 0);
    lv_obj_set_style_pad_row(s_keyboard, 3, 0);
    lv_obj_set_style_pad_column(s_keyboard, 2, 0);
    lv_obj_set_style_pad_all(s_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_keyboard, 6, LV_PART_ITEMS);
}

static void display_keyboard_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        display_hide_keyboard();
    }
}

static void display_wifi_alert_event_cb(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_target(event);
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_DELETE && target == s_wifi_alert_box) {
        s_wifi_alert_box = NULL;
    }
}

static void display_wifi_alert_ok_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_wifi_alert_box == NULL) {
        return;
    }

    lv_obj_del_async(s_wifi_alert_box);
    s_wifi_alert_box = NULL;
}

static void display_call_alert_event_cb(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_target(event);
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_DELETE && target == s_call_alert_box) {
        s_call_alert_box = NULL;
        s_call_alert_wechat = false;
    }
}

static void display_call_alert_accept_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    bool wechat = s_call_alert_wechat;
    if (wechat) {
        if (s_actions.on_wechat_accept_call != NULL) {
            esp_err_t ret = s_actions.on_wechat_accept_call(s_actions.ctx);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "accept wechat call failed: %s", esp_err_to_name(ret));
            } else {
                s_wechat_active_started_us = 0;
                display_show_wechat_active_page();
            }
        }
        display_hide_call_alert();
        return;
    }

    if (s_actions.on_accept_call != NULL) {
        esp_err_t ret = s_actions.on_accept_call(s_actions.ctx);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "accept call failed: %s", esp_err_to_name(ret));
        } else {
            s_call_active_started_us = esp_timer_get_time();
            display_show_call_active_page();
        }
    }
    display_hide_call_alert();
}

static void display_call_alert_reject_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    bool wechat = s_call_alert_wechat;
    if (wechat) {
        if (s_actions.on_wechat_reject_call != NULL) {
            esp_err_t ret = s_actions.on_wechat_reject_call(s_actions.ctx);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "reject wechat call failed: %s", esp_err_to_name(ret));
            }
        }
        display_hide_call_alert();
        return;
    }

    if (s_actions.on_reject_call != NULL) {
        esp_err_t ret = s_actions.on_reject_call(s_actions.ctx);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "reject call failed: %s", esp_err_to_name(ret));
        }
    }
    display_hide_call_alert();
}

static void display_hide_call_alert(void)
{
    if (s_call_alert_box != NULL) {
        lv_obj_del(s_call_alert_box);
        s_call_alert_box = NULL;
    }
    s_call_alert_wechat = false;
}

static void display_show_call_alert(bool wechat)
{
    lv_obj_t *card = NULL;
    lv_obj_t *reject_btn = NULL;
    lv_obj_t *accept_btn = NULL;

    if (s_call_alert_box != NULL) {
        if (s_call_alert_wechat == wechat) {
            return;
        }
        display_hide_call_alert();
    }

    s_call_alert_wechat = wechat;
    if (wechat) {
        /* Keep incoming WeChat alerts lightweight: do not build or switch pages from
         * the refresh timer. The alert itself is enough for answer/reject. */
    } else {
        esp_err_t enter_ret = display_enter_app(DISPLAY_APP_CALL);
        if (enter_ret != ESP_OK) {
            ESP_LOGW(TAG, "enter call for incoming alert failed: %s", esp_err_to_name(enter_ret));
        }
        display_show_call_page();
    }

    s_call_alert_box = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_call_alert_box);
    lv_obj_set_pos(s_call_alert_box, 0, 0);
    lv_obj_set_size(s_call_alert_box, 320, 240);
    lv_obj_set_style_bg_color(s_call_alert_box, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_bg_opa(s_call_alert_box, LV_OPA_20, 0);
    lv_obj_set_style_border_width(s_call_alert_box, 0, 0);
    lv_obj_set_style_pad_all(s_call_alert_box, 0, 0);
    lv_obj_clear_flag(s_call_alert_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_call_alert_box);
    lv_obj_add_event_cb(s_call_alert_box, display_call_alert_event_cb, LV_EVENT_ALL, NULL);

    card = display_create_figma_box(s_call_alert_box,
                                    64,
                                    68,
                                    192,
                                    104,
                                    lv_color_hex(0xFFFFFF),
                                    lv_color_hex(0xD6E4EF),
                                    9);
    lv_obj_set_style_shadow_width(card, 14, 0);
    lv_obj_set_style_shadow_ofs_y(card, 5, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);

    (void)display_create_figma_text(card,
                                    "来电",
                                    14,
                                    16,
                                    164,
                                    lv_color_hex(0x10233B),
                                    16,
                                    LV_TEXT_ALIGN_CENTER);
    reject_btn = display_create_figma_button(card,
                                             14,
                                             52,
                                             76,
                                             34,
                                             lv_color_hex(0xFFE7E7),
                                             lv_color_hex(0xF15A5A),
                                             "挂断",
                                             lv_color_hex(0xE44747),
                                             12,
                                             display_call_alert_reject_btn_cb);
    accept_btn = display_create_figma_button(card,
                                             102,
                                             52,
                                             76,
                                             34,
                                             lv_color_hex(0x21C783),
                                             lv_color_hex(0x21C783),
                                             "接听",
                                             lv_color_hex(0xFFFFFF),
                                             12,
                                             display_call_alert_accept_btn_cb);
    lv_obj_set_style_radius(reject_btn, 7, 0);
    lv_obj_set_style_radius(accept_btn, 7, 0);
}

static void display_hide_call_delete_confirm(void)
{
    if (s_call_delete_confirm_box != NULL) {
        lv_obj_del(s_call_delete_confirm_box);
        s_call_delete_confirm_box = NULL;
    }
    s_call_delete_pending_index = UINT8_MAX;
}

static void display_show_call_delete_confirm(uint8_t contact_index)
{
    lv_obj_t *card = NULL;
    lv_obj_t *cancel_btn = NULL;
    lv_obj_t *delete_btn = NULL;

    if (contact_index >= s_call_contact_count ||
        contact_index >= DISPLAY_CALL_CONTACT_COUNT ||
        s_call_contacts[contact_index].device_id[0] == '\0') {
        display_show_wifi_alert("删除联系人", "联系人不存在");
        return;
    }

    display_hide_call_delete_confirm();
    s_call_delete_pending_index = contact_index;

    s_call_delete_confirm_box = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_call_delete_confirm_box);
    lv_obj_set_pos(s_call_delete_confirm_box, 0, 0);
    lv_obj_set_size(s_call_delete_confirm_box, 320, 240);
    lv_obj_set_style_bg_color(s_call_delete_confirm_box, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_bg_opa(s_call_delete_confirm_box, LV_OPA_20, 0);
    lv_obj_set_style_border_width(s_call_delete_confirm_box, 0, 0);
    lv_obj_set_style_pad_all(s_call_delete_confirm_box, 0, 0);
    lv_obj_clear_flag(s_call_delete_confirm_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_call_delete_confirm_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_call_delete_confirm_box);

    card = display_create_figma_box(s_call_delete_confirm_box,
                                    44,
                                    62,
                                    232,
                                    116,
                                    lv_color_hex(0xFFFFFF),
                                    lv_color_hex(0xD6E4EF),
                                    9);
    lv_obj_set_style_shadow_width(card, 14, 0);
    lv_obj_set_style_shadow_ofs_y(card, 5, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);

    (void)display_create_figma_text(card,
                                    "删除联系人",
                                    12,
                                    13,
                                    208,
                                    lv_color_hex(0x10233B),
                                    16,
                                    LV_TEXT_ALIGN_CENTER);
    (void)display_create_figma_text(card,
                                    s_call_contacts[contact_index].device_id,
                                    16,
                                    42,
                                    200,
                                    lv_color_hex(0x64758A),
                                    12,
                                    LV_TEXT_ALIGN_CENTER);

    cancel_btn = display_create_figma_button(card,
                                             18,
                                             76,
                                             88,
                                             30,
                                             lv_color_hex(0xE9F5FF),
                                             lv_color_hex(0x2F82D7),
                                             "取消",
                                             lv_color_hex(0x2F82D7),
                                             12,
                                             display_call_delete_cancel_btn_cb);
    delete_btn = display_create_figma_button(card,
                                             126,
                                             76,
                                             88,
                                             30,
                                             lv_color_hex(0xFFE7E7),
                                             lv_color_hex(0xF15A5A),
                                             "删除",
                                             lv_color_hex(0xE44747),
                                             12,
                                             display_call_delete_confirm_btn_cb);
    lv_obj_set_style_radius(cancel_btn, 7, 0);
    lv_obj_set_style_radius(delete_btn, 7, 0);
}

static void display_hide_wechat_delete_confirm(void)
{
    if (s_wechat_delete_confirm_box != NULL) {
        lv_obj_del(s_wechat_delete_confirm_box);
        s_wechat_delete_confirm_box = NULL;
    }
    s_wechat_delete_pending_index = UINT8_MAX;
    s_wechat_delete_pending_open_id[0] = '\0';
}

static void display_show_wechat_delete_confirm(uint8_t contact_index)
{
    lv_obj_t *card = NULL;
    lv_obj_t *cancel_btn = NULL;
    lv_obj_t *delete_btn = NULL;

    if (contact_index >= DISPLAY_WECHAT_CONTACT_COUNT ||
        contact_index >= s_last_status.wechat_contact_count ||
        s_last_status.wechat_contacts[contact_index].open_id[0] == '\0') {
        display_show_wifi_alert("微信联系人", "联系人不存在");
        return;
    }

    display_hide_wechat_delete_confirm();
    s_wechat_delete_pending_index = contact_index;
    strlcpy(s_wechat_delete_pending_open_id,
            s_last_status.wechat_contacts[contact_index].open_id,
            sizeof(s_wechat_delete_pending_open_id));

    s_wechat_delete_confirm_box = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_wechat_delete_confirm_box);
    lv_obj_set_pos(s_wechat_delete_confirm_box, 0, 0);
    lv_obj_set_size(s_wechat_delete_confirm_box, 320, 240);
    lv_obj_set_style_bg_color(s_wechat_delete_confirm_box, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_bg_opa(s_wechat_delete_confirm_box, LV_OPA_20, 0);
    lv_obj_set_style_border_width(s_wechat_delete_confirm_box, 0, 0);
    lv_obj_set_style_pad_all(s_wechat_delete_confirm_box, 0, 0);
    lv_obj_clear_flag(s_wechat_delete_confirm_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wechat_delete_confirm_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_wechat_delete_confirm_box);

    card = display_create_figma_box(s_wechat_delete_confirm_box,
                                    44,
                                    62,
                                    232,
                                    116,
                                    lv_color_hex(0xFFFFFF),
                                    lv_color_hex(0xD6E4EF),
                                    9);
    lv_obj_set_style_shadow_width(card, 14, 0);
    lv_obj_set_style_shadow_ofs_y(card, 5, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);

    (void)display_create_figma_text(card,
                                    "删除联系人",
                                    12,
                                    13,
                                    208,
                                    lv_color_hex(0x10233B),
                                    16,
                                    LV_TEXT_ALIGN_CENTER);
    (void)display_create_figma_text(card,
                                    s_wechat_delete_pending_open_id,
                                    16,
                                    42,
                                    200,
                                    lv_color_hex(0x64758A),
                                    12,
                                    LV_TEXT_ALIGN_CENTER);

    cancel_btn = display_create_figma_button(card,
                                             18,
                                             76,
                                             88,
                                             30,
                                             lv_color_hex(0xE9F5FF),
                                             lv_color_hex(0x2F82D7),
                                             "Cancel",
                                             lv_color_hex(0x2F82D7),
                                             12,
                                             display_wechat_delete_cancel_btn_cb);
    delete_btn = display_create_figma_button(card,
                                             126,
                                             76,
                                             88,
                                             30,
                                             lv_color_hex(0xFFE7E7),
                                             lv_color_hex(0xF15A5A),
                                             "Delete",
                                             lv_color_hex(0xE44747),
                                             12,
                                             display_wechat_delete_confirm_btn_cb);
    lv_obj_set_style_radius(cancel_btn, 7, 0);
    lv_obj_set_style_radius(delete_btn, 7, 0);
}

static void display_show_wifi_alert(const char *title, const char *message)
{
    if (s_wifi_alert_box != NULL) {
        lv_obj_del(s_wifi_alert_box);
        s_wifi_alert_box = NULL;
    }

    s_wifi_alert_box = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_wifi_alert_box);
    lv_obj_set_size(s_wifi_alert_box, 232, 118);
    lv_obj_set_style_radius(s_wifi_alert_box, 8, 0);
    lv_obj_set_style_bg_color(s_wifi_alert_box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_wifi_alert_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_wifi_alert_box, 1, 0);
    lv_obj_set_style_border_color(s_wifi_alert_box, lv_color_hex(0xD5E0EB), 0);
    lv_obj_set_style_pad_all(s_wifi_alert_box, 0, 0);
    lv_obj_clear_flag(s_wifi_alert_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(s_wifi_alert_box);
    lv_obj_move_foreground(s_wifi_alert_box);
    lv_obj_add_event_cb(s_wifi_alert_box, display_wifi_alert_event_cb, LV_EVENT_ALL, NULL);

    (void)display_create_figma_text(s_wifi_alert_box,
                                    title != NULL ? title : "",
                                    12,
                                    14,
                                    208,
                                    lv_color_hex(0x10243E),
                                    16,
                                    LV_TEXT_ALIGN_CENTER);
    (void)display_create_figma_text(s_wifi_alert_box,
                                    message != NULL ? message : "",
                                    16,
                                    46,
                                    200,
                                    lv_color_hex(0x64758A),
                                    12,
                                    LV_TEXT_ALIGN_CENTER);
    (void)display_create_figma_button(s_wifi_alert_box,
                                      68,
                                      80,
                                      96,
                                      28,
                                      lv_color_hex(0x1768B7),
                                      lv_color_hex(0x1768B7),
                                      "OK",
                                      lv_color_hex(0xFFFFFF),
                                      12,
                                      display_wifi_alert_ok_btn_cb);
}

static void display_update_uuid_edit_feedback(const char *override_text, lv_color_t override_color)
{
    const char *uuid = "";
    size_t uuid_len = 0;
    lv_color_t length_color = lv_color_hex(0x48656F);
    const char *status_text = override_text;
    lv_color_t status_color = override_color;

    if (s_uuid_ta != NULL) {
        uuid = lv_textarea_get_text(s_uuid_ta);
    }
    uuid_len = strlen(uuid);

    if (s_uuid_edit_length_label != NULL) {
        if (uuid_len > 0 && uuid_len < DEVICE_UUID_MIN_LEN) {
            length_color = lv_color_hex(0xC8513C);
        } else if (uuid_len >= DEVICE_UUID_MIN_LEN) {
            length_color = lv_color_hex(0x2E8F6B);
        }
        display_text_set_color(s_uuid_edit_length_label, length_color, 0);
        lv_label_set_text_fmt(s_uuid_edit_length_label,
                              "%u/%u",
                              (unsigned)uuid_len,
                              (unsigned)DEVICE_UUID_EDIT_MAX_LEN);
    }

    if (s_uuid_edit_status_label == NULL) {
        return;
    }

    if (status_text == NULL) {
        if (uuid_len == 0) {
            status_text = "请输入 4-12 位";
            status_color = lv_color_hex(0x64758A);
        } else if (uuid_len < DEVICE_UUID_MIN_LEN) {
            status_text = "Device ID 过短";
            status_color = lv_color_hex(0xE45656);
        } else {
            status_text = "点击保存生效";
            status_color = lv_color_hex(0x0D8A59);
        }
    }

    display_text_set_color(s_uuid_edit_status_label, status_color, 0);
    display_text_set(s_uuid_edit_status_label, status_text);
}

static void display_submit_uuid(void)
{
    const char *uuid = NULL;
    size_t uuid_len = 0;
    esp_err_t ret = ESP_OK;

    if (s_uuid_ta == NULL) {
        return;
    }

    uuid = lv_textarea_get_text(s_uuid_ta);
    uuid_len = strlen(uuid);
    if (uuid_len < DEVICE_UUID_MIN_LEN) {
        display_update_uuid_edit_feedback("UUID too short", lv_color_hex(0xC8513C));
        display_show_wifi_alert("Device UUID", "UUID must be 4-12 uppercase letters or digits.");
        return;
    }
    if (s_actions.on_set_device_uuid == NULL) {
        display_update_uuid_edit_feedback("UUID update unavailable", lv_color_hex(0xC8513C));
        display_show_wifi_alert("Device UUID", "UUID update is unavailable right now.");
        return;
    }
    if (strcmp(uuid, s_last_status.device_uuid) == 0) {
        display_set_main_hint("Ready");
        if (s_uuid_parent_page == DISPLAY_PAGE_TIRTC_CONFIG) {
            display_show_tirtc_config_page();
        } else {
            display_show_main_page();
        }
        return;
    }

    ret = s_actions.on_set_device_uuid(uuid, s_actions.ctx);
    if (ret == ESP_OK) {
        if (s_uuid_label != NULL) {
            display_text_set(s_uuid_label, uuid);
        }
        display_set_main_hint("Ready");
        if (s_uuid_parent_page == DISPLAY_PAGE_TIRTC_CONFIG) {
            display_show_tirtc_config_page();
        } else {
            display_show_main_page();
        }
        return;
    }

    display_update_uuid_edit_feedback("Save failed", lv_color_hex(0xC8513C));
    if (ret == ESP_ERR_INVALID_ARG || ret == ESP_ERR_INVALID_SIZE) {
        display_show_wifi_alert("Device UUID", "Use only A-Z and 0-9, with 4-12 characters.");
    } else {
        display_show_wifi_alert("Device UUID", "Saving UUID failed. Please try again.");
    }
}

static void display_uuid_keyboard_value_event_cb(lv_event_t *event)
{
    lv_obj_t *keyboard = lv_event_get_target(event);
    uint16_t button_id = lv_btnmatrix_get_selected_btn(keyboard);
    const char *text = NULL;

    if (button_id == LV_BTNMATRIX_BTN_NONE || s_uuid_ta == NULL) {
        return;
    }

    text = lv_btnmatrix_get_btn_text(keyboard, button_id);
    if (text == NULL) {
        return;
    }

    if (strcmp(text, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_del_char(s_uuid_ta);
    } else if (strcmp(text, "Clear") == 0) {
        lv_textarea_set_text(s_uuid_ta, "");
    } else {
        lv_textarea_add_text(s_uuid_ta, text);
    }

    display_update_uuid_edit_feedback(NULL, lv_color_hex(0x48656F));
}

static void display_uuid_keyboard_draw_part_event_cb(lv_event_t *event)
{
    lv_obj_t *keyboard = lv_event_get_target(event);
    lv_obj_draw_part_dsc_t *draw_part = lv_event_get_draw_part_dsc(event);
    const char *text = NULL;

    if (lv_event_get_code(event) != LV_EVENT_DRAW_PART_BEGIN || draw_part == NULL || draw_part->part != LV_PART_ITEMS) {
        return;
    }

    text = lv_btnmatrix_get_btn_text(keyboard, draw_part->id);
    if (text == NULL) {
        return;
    }

    if (strcmp(text, LV_SYMBOL_BACKSPACE) == 0 || strcmp(text, "Clear") == 0) {
        draw_part->rect_dsc->bg_color = lv_color_hex(0x183642);
        draw_part->rect_dsc->bg_opa = LV_OPA_COVER;
        draw_part->label_dsc->color = lv_color_hex(0xF7F5F2);
    }
}

static void display_set_password_border_color(lv_color_t border_color)
{
    if (s_password_ta == NULL) {
        return;
    }

    lv_obj_set_style_border_color(s_password_ta, border_color, 0);
}

static const display_wifi_scan_result_t *display_find_selected_wifi_result(
    const display_status_t *status)
{
    if (status == NULL || s_selected_ssid[0] == '\0') {
        return NULL;
    }

    for (uint16_t index = 0; index < status->wifi_scan_count && index < DISPLAY_WIFI_SCAN_MAX; ++index) {
        if (strcmp(status->wifi_scan_results[index].ssid, s_selected_ssid) == 0) {
            return &status->wifi_scan_results[index];
        }
    }

    return NULL;
}

static bool display_selected_wifi_has_saved_config(const display_status_t *status)
{
    return status != NULL &&
           s_selected_ssid[0] != '\0' &&
           status->saved_network_ssid[0] != '\0' &&
           strcmp(status->saved_network_ssid, s_selected_ssid) == 0;
}

static bool display_selected_wifi_requires_password(const display_status_t *status)
{
    const display_wifi_scan_result_t *selected_result = display_find_selected_wifi_result(status);

    if (selected_result != NULL) {
        return selected_result->secure;
    }

    return display_selected_wifi_has_saved_config(status);
}

static void display_prepare_password_entry(const display_status_t *status)
{
    if (s_password_ta == NULL) {
        return;
    }

    if (display_selected_wifi_has_saved_config(status) &&
        status != NULL &&
        status->saved_network_password[0] != '\0') {
        lv_textarea_set_text(s_password_ta, status->saved_network_password);
        display_set_password_placeholder("Password", lv_color_hex(0xD1D7DB));
    } else if (!display_selected_wifi_requires_password(status)) {
        lv_textarea_set_text(s_password_ta, "");
        display_set_password_placeholder("Open network", lv_color_hex(0xD1D7DB));
    } else {
        lv_textarea_set_text(s_password_ta, "");
        display_set_password_placeholder("Password", lv_color_hex(0xD1D7DB));
    }
}

static void display_update_wifi_connect_details_line(const display_status_t *status)
{
    char detail_text[192] = {0};
    const display_wifi_scan_result_t *selected_result = NULL;
    bool has_saved_config = false;

    if (s_wifi_connect_details_label == NULL || status == NULL) {
        return;
    }

    selected_result = display_find_selected_wifi_result(status);
    has_saved_config = display_selected_wifi_has_saved_config(status);
    if (s_selected_ssid[0] == '\0') {
        strlcpy(detail_text, "SSID: --\nType: Select WiFi first", sizeof(detail_text));
    } else if (selected_result != NULL) {
        snprintf(detail_text,
                 sizeof(detail_text),
                 has_saved_config
                     ? "SSID: %s\nType: %s | CH %u | RSSI %d dBm\nSaved credential available"
                     : "SSID: %s\nType: %s | CH %u | RSSI %d dBm",
                 s_selected_ssid,
                 selected_result->secure ? "Secured" : "Open",
                 (unsigned)selected_result->channel,
                 selected_result->rssi);
    } else if (has_saved_config) {
        snprintf(detail_text,
                 sizeof(detail_text),
                 "SSID: %s\nType: Saved network | CH -- | RSSI --\nSaved credential available",
                 s_selected_ssid);
    } else if (status->network_connected &&
               strcmp(status->network_ssid, s_selected_ssid) == 0 &&
               status->network_rssi > -120) {
        snprintf(detail_text,
                 sizeof(detail_text),
                 "SSID: %s\nType: Connected | CH -- | RSSI %d dBm",
                 s_selected_ssid,
                 status->network_rssi);
    } else {
        snprintf(detail_text,
                 sizeof(detail_text),
                 "SSID: %s\nType: Unavailable | CH -- | RSSI --",
                 s_selected_ssid);
    }

    display_text_set(s_wifi_connect_details_label, detail_text);
}

static void display_update_wifi_connect_status_line(const display_status_t *status)
{
    char hint_text[64] = {0};
    char rssi_text[24] = {0};
    lv_color_t hint_color = lv_color_hex(0x48656F);
    lv_color_t rssi_color = lv_color_hex(0x48656F);
    const display_wifi_scan_result_t *selected_result = NULL;
    bool signal_known = false;
    int signal_rssi = 0;

    if (s_wifi_connect_hint_label == NULL || s_wifi_connect_rssi_label == NULL || status == NULL) {
        return;
    }

    selected_result = display_find_selected_wifi_result(status);
    if (selected_result != NULL) {
        signal_known = true;
        signal_rssi = selected_result->rssi;
    } else if (status->network_connected &&
               s_selected_ssid[0] != '\0' &&
               strcmp(status->network_ssid, s_selected_ssid) == 0 &&
               status->network_rssi > -120) {
        signal_known = true;
        signal_rssi = status->network_rssi;
    }

    if (signal_known) {
        snprintf(rssi_text, sizeof(rssi_text), "%d dBm", signal_rssi);
        if (signal_rssi >= -60) {
            rssi_color = lv_color_hex(0x0D8A59);
        } else if (signal_rssi >= -75) {
            rssi_color = lv_color_hex(0xF59E0B);
        }
    } else if (s_selected_ssid[0] != '\0') {
        strlcpy(rssi_text, "无信号", sizeof(rssi_text));
        rssi_color = lv_color_hex(0xE45656);
    }

    if (s_selected_ssid[0] == '\0') {
        strlcpy(hint_text, "请选择 Wi-Fi", sizeof(hint_text));
        hint_color = lv_color_hex(0xE45656);
    } else {
        switch (s_wifi_connect_state) {
        case DISPLAY_WIFI_CONNECT_STATE_SELECT_FIRST:
            strlcpy(hint_text, "请选择 Wi-Fi", sizeof(hint_text));
            hint_color = lv_color_hex(0xE45656);
            break;
        case DISPLAY_WIFI_CONNECT_STATE_UNAVAILABLE:
            strlcpy(hint_text, "Wi-Fi 不可用", sizeof(hint_text));
            hint_color = lv_color_hex(0xE45656);
            break;
        case DISPLAY_WIFI_CONNECT_STATE_SHORT_PASSWORD:
            strlcpy(hint_text, "密码过短", sizeof(hint_text));
            hint_color = lv_color_hex(0xE45656);
            break;
        case DISPLAY_WIFI_CONNECT_STATE_CONNECTING: {
            static const char *dots = ".........";
            int dot_count = (int)((esp_timer_get_time() / 250000ULL) % 9ULL) + 1;
            snprintf(hint_text, sizeof(hint_text), "连接中%.*s", dot_count, dots);
            hint_color = lv_color_hex(0xF59E0B);
            break;
        }
        case DISPLAY_WIFI_CONNECT_STATE_FAILED:
            strlcpy(hint_text, "密码错误", sizeof(hint_text));
            hint_color = lv_color_hex(0xE45656);
            break;
        case DISPLAY_WIFI_CONNECT_STATE_TIMEOUT:
            strlcpy(hint_text, "连接超时", sizeof(hint_text));
            hint_color = lv_color_hex(0xE45656);
            break;
        case DISPLAY_WIFI_CONNECT_STATE_CONNECTED:
            strlcpy(hint_text, "已连接并保存", sizeof(hint_text));
            hint_color = lv_color_hex(0x0D8A59);
            break;
        case DISPLAY_WIFI_CONNECT_STATE_IDLE:
        default:
            if (display_selected_wifi_has_saved_config(status)) {
                strlcpy(hint_text, "凭据已保存", sizeof(hint_text));
                hint_color = lv_color_hex(0x0D8A59);
            } else if (!display_selected_wifi_requires_password(status)) {
                strlcpy(hint_text, "开放网络", sizeof(hint_text));
            } else {
                strlcpy(hint_text, "输入密码加入", sizeof(hint_text));
            }
            break;
        }
    }

    display_text_set_color(s_wifi_connect_hint_label, hint_color, 0);
    display_text_set(s_wifi_connect_hint_label, hint_text);
    display_text_set_color(s_wifi_connect_rssi_label, rssi_color, 0);
    display_text_set(s_wifi_connect_rssi_label, rssi_text);
}

static void display_keyboard_draw_part_event_cb(lv_event_t *event)
{
    lv_obj_t *keyboard = lv_event_get_target(event);
    lv_obj_draw_part_dsc_t *draw_part = lv_event_get_draw_part_dsc(event);
    const char *text = NULL;

    if (lv_event_get_code(event) != LV_EVENT_DRAW_PART_BEGIN || draw_part == NULL || draw_part->part != LV_PART_ITEMS) {
        return;
    }

    text = lv_btnmatrix_get_btn_text(keyboard, draw_part->id);
    if (text == NULL) {
        return;
    }

    if (draw_part->id == DISPLAY_KB_BTN_CURSOR_LEFT_ID ||
        draw_part->id == DISPLAY_KB_BTN_CURSOR_RIGHT_ID) {
        draw_part->rect_dsc->bg_color = lv_color_hex(0x183642);
        draw_part->rect_dsc->bg_opa = LV_OPA_COVER;
        draw_part->label_dsc->color = lv_color_hex(0xF7F5F2);
    } else if (draw_part->id == DISPLAY_KB_BTN_JOIN_ID) {
        draw_part->rect_dsc->bg_color = lv_color_hex(0x2E8F6B);
        draw_part->rect_dsc->bg_opa = LV_OPA_COVER;
        draw_part->label_dsc->color = lv_color_hex(0xF7F5F2);
    } else if (draw_part->id == DISPLAY_KB_BTN_SPACE_ID ||
               strcmp(text, "'") == 0) {
        draw_part->rect_dsc->bg_color = lv_color_hex(0xF7F5F2);
        draw_part->rect_dsc->bg_opa = LV_OPA_COVER;
        draw_part->label_dsc->color = lv_color_hex(0x183642);
    }

    if (draw_part->id == DISPLAY_KB_BTN_SPACE_ID ||
        draw_part->id == DISPLAY_KB_BTN_JOIN_ID) {
#if LV_FONT_MONTSERRAT_12
        draw_part->label_dsc->font = &lv_font_montserrat_12;
#endif
        draw_part->label_dsc->letter_space = 0;
    }
}

static void display_set_password_placeholder(const char *text, lv_color_t border_color)
{
    if (s_password_ta == NULL) {
        return;
    }

    lv_textarea_set_placeholder_text(s_password_ta, text);
    display_set_password_border_color(border_color);
}

static void display_submit_wifi_connect(void)
{
    size_t password_len = 0;
    bool has_saved_config = false;
    bool requires_password = false;
    bool using_saved_password = false;

    if (s_password_ta == NULL) {
        return;
    }

    const char *password = lv_textarea_get_text(s_password_ta);
    display_set_password_border_color(lv_color_hex(0xD1D7DB));
    password_len = strlen(password);
    has_saved_config = display_selected_wifi_has_saved_config(&s_last_status);
    requires_password = display_selected_wifi_requires_password(&s_last_status);
    using_saved_password = has_saved_config && password_len == 0;

    if (s_selected_ssid[0] == '\0') {
        s_wifi_connect_state = DISPLAY_WIFI_CONNECT_STATE_SELECT_FIRST;
        return;
    }
    if (s_last_status.network_connected && strcmp(s_last_status.network_ssid, s_selected_ssid) == 0) {
        s_wifi_connect_pending = false;
        s_wifi_connect_target_ssid[0] = '\0';
        display_show_wifi_page();
        return;
    }
    if (s_actions.on_wifi_connect == NULL) {
        s_wifi_connect_state = DISPLAY_WIFI_CONNECT_STATE_UNAVAILABLE;
        return;
    }
    if (requires_password && !using_saved_password && password_len < DISPLAY_WIFI_PASSWORD_MIN_LEN) {
        s_wifi_connect_state = DISPLAY_WIFI_CONNECT_STATE_SHORT_PASSWORD;
        display_set_password_border_color(lv_color_hex(0xC8513C));
        display_show_wifi_alert("WiFi Password", "Password must be at least 8 characters.");
        return;
    }

    esp_err_t ret = s_actions.on_wifi_connect(s_selected_ssid, password, s_actions.ctx);
    if (ret == ESP_OK) {
        s_wifi_connect_pending = true;
        s_wifi_connect_request_us = esp_timer_get_time();
        strlcpy(s_wifi_connect_target_ssid, s_selected_ssid, sizeof(s_wifi_connect_target_ssid));
        s_wifi_connect_state = DISPLAY_WIFI_CONNECT_STATE_CONNECTING;
        display_set_password_border_color(lv_color_hex(0xC89F4A));
        display_set_main_hint("Ready");
    } else {
        s_wifi_connect_pending = false;
        s_wifi_connect_target_ssid[0] = '\0';
        s_wifi_connect_state = DISPLAY_WIFI_CONNECT_STATE_FAILED;
        display_set_password_border_color(lv_color_hex(0xC8513C));
    }
}

static void display_keyboard_value_event_cb(lv_event_t *event)
{
    lv_obj_t *keyboard = lv_event_get_target(event);
    const bool wifi_keyboard = keyboard == s_keyboard;
    uint16_t button_id = wifi_keyboard ? lv_btnmatrix_get_selected_btn(keyboard)
                                       : lv_keyboard_get_selected_btn(keyboard);
    lv_obj_t *textarea = wifi_keyboard ? s_password_ta : lv_keyboard_get_textarea(keyboard);
    const char *text = NULL;

    if (button_id == LV_BTNMATRIX_BTN_NONE) {
        return;
    }

    text = wifi_keyboard ? lv_btnmatrix_get_btn_text(keyboard, button_id)
                         : lv_keyboard_get_btn_text(keyboard, button_id);
    if (text == NULL) {
        return;
    }

    if (strcmp(text, "abc") == 0) {
        if (wifi_keyboard) {
            display_set_wifi_keyboard_mode(LV_KEYBOARD_MODE_USER_1);
        } else {
            lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_USER_1);
        }
        return;
    }
    if (strcmp(text, "ABC") == 0) {
        if (wifi_keyboard) {
            display_set_wifi_keyboard_mode(LV_KEYBOARD_MODE_USER_2);
        } else {
            lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_USER_2);
        }
        return;
    }
    if (strcmp(text, "1#") == 0) {
        if (wifi_keyboard) {
            display_set_wifi_keyboard_mode(LV_KEYBOARD_MODE_USER_3);
        } else {
            lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_USER_3);
        }
        return;
    }
    if (strcmp(text, LV_SYMBOL_CLOSE) == 0 || strcmp(text, LV_SYMBOL_KEYBOARD) == 0) {
        display_hide_keyboard();
        return;
    }
    if (strcmp(text, "Connect") == 0 || strcmp(text, "Join") == 0) {
        if (textarea == s_tirtc_edit_ta) {
            display_tirtc_config_edit_save_btn_cb(NULL);
        } else if (textarea == s_call_add_edit_ta) {
            display_call_add_edit_save_btn_cb(NULL);
        } else if (textarea == s_wechat_add_edit_ta) {
            display_wechat_add_edit_save_btn_cb(NULL);
        } else {
            display_submit_wifi_connect();
        }
        return;
    }
    if (textarea == NULL) {
        return;
    }
    if (button_id == DISPLAY_KB_BTN_CURSOR_LEFT_ID) {
        lv_textarea_cursor_left(textarea);
        return;
    }
    if (button_id == DISPLAY_KB_BTN_CURSOR_RIGHT_ID) {
        lv_textarea_cursor_right(textarea);
        return;
    }
    if (strcmp(text, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_del_char(textarea);
        return;
    }
    if (button_id == DISPLAY_KB_BTN_SPACE_ID || strcmp(text, "Space") == 0) {
        lv_textarea_add_text(textarea, " ");
        return;
    }

    lv_textarea_add_text(textarea, text);
}

static void display_textarea_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *target = lv_event_get_target(event);

    if (code == LV_EVENT_FOCUSED && s_keyboard != NULL) {
        if (target == s_password_ta) {
            display_set_wifi_keyboard_mode(LV_KEYBOARD_MODE_USER_1);
        }
        display_layout_wifi_keyboard();
        lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_keyboard);
    }
}

static void display_call_add_edit_textarea_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *target = lv_event_get_target(event);

    if (code == LV_EVENT_FOCUSED && s_call_add_edit_keyboard != NULL) {
        lv_keyboard_set_textarea(s_call_add_edit_keyboard, target);
        lv_keyboard_set_mode(s_call_add_edit_keyboard, LV_KEYBOARD_MODE_USER_1);
        lv_obj_clear_flag(s_call_add_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_call_add_edit_keyboard);
    }
    if (code == LV_EVENT_VALUE_CHANGED) {
        display_update_call_add_edit_feedback(NULL, lv_color_hex(0x0D8A59));
    }
}

static void display_wechat_add_edit_textarea_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *target = lv_event_get_target(event);

    if (code == LV_EVENT_FOCUSED && s_wechat_add_edit_keyboard != NULL) {
        lv_keyboard_set_textarea(s_wechat_add_edit_keyboard, target);
        lv_keyboard_set_mode(s_wechat_add_edit_keyboard, LV_KEYBOARD_MODE_USER_1);
        lv_obj_clear_flag(s_wechat_add_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_wechat_add_edit_keyboard);
    }
    if (code == LV_EVENT_VALUE_CHANGED) {
        display_update_wechat_add_edit_feedback(NULL, lv_color_hex(0x0D8A59));
    }
}

static void display_tirtc_edit_textarea_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *target = lv_event_get_target(event);

    if (code == LV_EVENT_FOCUSED && s_tirtc_edit_keyboard != NULL) {
        lv_keyboard_set_textarea(s_tirtc_edit_keyboard, target);
        lv_keyboard_set_mode(s_tirtc_edit_keyboard, LV_KEYBOARD_MODE_USER_1);
        lv_obj_clear_flag(s_tirtc_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_tirtc_edit_keyboard);
    }
    if (code == LV_EVENT_VALUE_CHANGED) {
        display_update_tirtc_edit_feedback(NULL, lv_color_hex(0x0D8A59));
    }
}

static const lv_font_t *display_ascii_font(uint8_t size)
{
    if (size <= 12U) {
#if LV_FONT_MONTSERRAT_12
        return &lv_font_montserrat_12;
#endif
    }
    if (size >= 18U) {
#if LV_FONT_MONTSERRAT_20
        return &lv_font_montserrat_20;
#endif
    }
#if LV_FONT_MONTSERRAT_14
    return &lv_font_montserrat_14;
#else
    return LV_FONT_DEFAULT;
#endif
}

static const lv_font_t *display_ai_chat_font(void)
{
    return ai_chat_font_get_current();
}

static void display_apply_ai_dialog_font_one(lv_obj_t *label)
{
    if (label != NULL && lv_obj_check_type(label, &lv_label_class)) {
        lv_obj_set_style_text_font(label, display_ai_chat_font(), 0);
        lv_obj_invalidate(label);
    }
}

static void display_apply_ai_dialog_font_group(lv_obj_t *label, lv_obj_t **bold_labels)
{
    display_apply_ai_dialog_font_one(label);
    if (bold_labels != NULL) {
        for (size_t index = 0; index < DISPLAY_AI_CHAT_CAPTION_BOLD_LAYER_COUNT; ++index) {
            display_apply_ai_dialog_font_one(bold_labels[index]);
        }
    }
}

static void display_apply_ai_dialog_font_if_ready(void)
{
    bool external_ready = ai_chat_font_is_ready();
    if (external_ready == s_ai_dialog_external_font_applied) {
        return;
    }

    for (size_t index = 0; index < DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX; ++index) {
        display_apply_ai_dialog_font_group(s_ai_message_labels[index],
                                           s_ai_message_bold_labels[index]);
    }
    display_apply_ai_dialog_font_one(s_ai_new_chat_btn_label);
    display_apply_ai_dialog_font_one(s_ai_single_caption_label);
    s_ai_dialog_external_font_applied = external_ready;
}

static display_text_image_ctx_t *display_text_image_ctx(lv_obj_t *obj)
{
    if (obj == NULL || !lv_obj_check_type(obj, &lv_img_class)) {
        return NULL;
    }

    display_text_image_ctx_t *ctx = (display_text_image_ctx_t *)lv_obj_get_user_data(obj);
    return ctx != NULL && ctx->magic == DISPLAY_TEXT_IMAGE_MAGIC ? ctx : NULL;
}

static void display_text_image_delete_cb(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_target(event);
    display_text_image_ctx_t *ctx = display_text_image_ctx(target);

    if (ctx != NULL) {
        lv_obj_set_user_data(target, NULL);
        free(ctx);
    }
}

static lv_obj_t *display_create_text_asset_obj(lv_obj_t *parent,
                                               lv_coord_t x,
                                               lv_coord_t y,
                                               lv_coord_t width,
                                               lv_color_t color,
                                               uint8_t font_size,
                                               lv_text_align_t align)
{
    display_text_image_ctx_t *ctx = (display_text_image_ctx_t *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return NULL;
    }

    lv_obj_t *img = lv_img_create(parent);

    ctx->magic = DISPLAY_TEXT_IMAGE_MAGIC;
    ctx->x = x;
    ctx->y = y;
    ctx->width = width;
    ctx->font_size = font_size;
    ctx->align = align;
    ctx->color = color;
    lv_obj_set_user_data(img, ctx);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(img, display_text_image_delete_cb, LV_EVENT_DELETE, NULL);
    return img;
}

static void display_apply_text_asset(lv_obj_t *obj,
                                     const ui_text_asset_t *asset,
                                     display_text_image_ctx_t *ctx)
{
    if (obj == NULL || asset == NULL || asset->image == NULL || ctx == NULL) {
        return;
    }

    lv_coord_t x = ctx->x;
    lv_coord_t y = ctx->y + asset->y_offset;
    lv_coord_t image_width = (lv_coord_t)asset->image->header.w;
    lv_coord_t image_height = (lv_coord_t)asset->image->header.h;

    if (ctx->align == LV_TEXT_ALIGN_CENTER) {
        x += (ctx->width - image_width) / 2;
    } else if (ctx->align == LV_TEXT_ALIGN_RIGHT) {
        x += ctx->width - image_width;
    } else {
        x += asset->x_offset;
    }

    lv_img_set_src(obj, asset->image);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, image_width, image_height);
    lv_obj_set_style_img_recolor(obj, ctx->color, 0);
    lv_obj_set_style_img_recolor_opa(obj, LV_OPA_COVER, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    ctx->current_asset = asset;
    ctx->layout_dirty = false;
}

static void display_text_set(lv_obj_t *obj, const char *text)
{
    if (obj == NULL) {
        return;
    }

    display_text_image_ctx_t *ctx = display_text_image_ctx(obj);
    if (ctx != NULL) {
        const ui_text_asset_t *asset = ui_text_asset_find(text, ctx->font_size);
        if (asset != NULL) {
            if (ctx->current_asset == asset &&
                !ctx->layout_dirty &&
                !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
                return;
            }
            display_apply_text_asset(obj, asset, ctx);
        } else {
            if (ctx->current_asset == NULL &&
                !ctx->layout_dirty &&
                lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
                return;
            }
            ctx->current_asset = NULL;
            ctx->layout_dirty = false;
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *next_text = text != NULL ? text : "";
        const char *current_text = lv_label_get_text(obj);
        if (current_text != NULL && strcmp(current_text, next_text) == 0) {
            return;
        }
        lv_label_set_text(obj, next_text);
    }
}

static void display_text_set_color(lv_obj_t *obj, lv_color_t color, lv_style_selector_t selector)
{
    if (obj == NULL) {
        return;
    }

    display_text_image_ctx_t *ctx = display_text_image_ctx(obj);
    if (ctx != NULL) {
        if (selector == 0) {
            ctx->color = color;
        }
        lv_obj_set_style_img_recolor(obj, color, selector);
        lv_obj_set_style_img_recolor_opa(obj, LV_OPA_COVER, selector);
        return;
    }

    lv_obj_set_style_text_color(obj, color, selector);
}

static void display_text_set_layout(lv_obj_t *obj,
                                    lv_coord_t x,
                                    lv_coord_t y,
                                    lv_coord_t width,
                                    lv_text_align_t align)
{
    display_text_image_ctx_t *ctx = display_text_image_ctx(obj);
    if (ctx != NULL) {
        ctx->x = x;
        ctx->y = y;
        ctx->width = width;
        ctx->align = align;
        ctx->layout_dirty = true;
        return;
    }

    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, width);
    lv_obj_set_style_text_align(obj, align, 0);
}

static const char *display_tirtc_config_field_title(display_tirtc_config_field_t field)
{
    switch (field) {
    case DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_SECRET:
        return "Binding";
    case DISPLAY_TIRTC_CONFIG_FIELD_TOKEN_SUBJECT:
        return "Token Subject";
    case DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_ID:
        return "Token API";
    case DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_SECRET:
        return "Credential";
    case DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_ID:
    default:
        return "Device ID";
    }
}

static const char *display_tirtc_binding_text(const display_status_t *status)
{
    static char text[64];

    if (status == NULL) {
        return "Unknown";
    }

    switch (status->binding_state) {
    case DISPLAY_DEVICE_BINDING_STATE_REPORTING:
        return "Reporting";
    case DISPLAY_DEVICE_BINDING_STATE_WAITING_USER:
        if (status->binding_code[0] != '\0') {
            snprintf(text, sizeof(text), "Code %s", status->binding_code);
            return text;
        }
        return "Waiting user";
    case DISPLAY_DEVICE_BINDING_STATE_BOUND:
        return "Bound";
    case DISPLAY_DEVICE_BINDING_STATE_ERROR:
        return status->binding_message[0] != '\0' ? status->binding_message : "Binding failed";
    case DISPLAY_DEVICE_BINDING_STATE_IDLE:
        return "Idle";
    case DISPLAY_DEVICE_BINDING_STATE_DISABLED:
    default:
        return "Disabled";
    }
}

static const char *display_tirtc_config_field_value(const display_status_t *status,
                                                    display_tirtc_config_field_t field)
{
    switch (field) {
    case DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_SECRET:
        return display_tirtc_binding_text(status);
    case DISPLAY_TIRTC_CONFIG_FIELD_TOKEN_SUBJECT:
        return status != NULL && status->tirtc_token_subject[0] != '\0' ?
            status->tirtc_token_subject : "Not set";
    case DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_ID:
        return status != NULL && status->tirtc_server_api[0] != '\0' ?
            status->tirtc_server_api : "Service issued";
    case DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_SECRET:
        return "Managed by binding";
    case DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_ID:
    default:
        return status != NULL && status->tirtc_device_id[0] != '\0' ?
            status->tirtc_device_id : "Unbound";
    }
}

static size_t display_tirtc_config_field_max_len(display_tirtc_config_field_t field)
{
    return field == DISPLAY_TIRTC_CONFIG_FIELD_TOKEN_SUBJECT ?
        DISPLAY_TIRTC_CONFIG_TOKEN_SUBJECT_MAX - 1U :
        DISPLAY_TIRTC_CONFIG_TEXT_MAX - 1U;
}

static void display_update_call_add_edit_feedback(const char *status_text, lv_color_t status_color)
{
    const char *value = s_call_add_edit_ta != NULL ? lv_textarea_get_text(s_call_add_edit_ta) : "";
    size_t value_len = strlen(value);
    size_t max_len = display_call_add_field_max_len(s_call_add_edit_field);

    if (s_call_add_edit_length_label != NULL) {
        lv_label_set_text_fmt(s_call_add_edit_length_label,
                              "%u/%u",
                              (unsigned)value_len,
                              (unsigned)max_len);
    }

    if (s_call_add_edit_status_label == NULL) {
        return;
    }

    if (status_text == NULL) {
        if (value_len > max_len) {
            status_text = "内容不合法";
            status_color = lv_color_hex(0xE45656);
        } else {
            status_text = "点击保存生效";
            status_color = lv_color_hex(0x0D8A59);
        }
    }

    display_text_set_color(s_call_add_edit_status_label, status_color, 0);
    display_text_set(s_call_add_edit_status_label, status_text);
}

static void display_update_wechat_add_edit_feedback(const char *status_text, lv_color_t status_color)
{
    const char *value = s_wechat_add_edit_ta != NULL ? lv_textarea_get_text(s_wechat_add_edit_ta) : "";
    size_t value_len = strlen(value);

    if (s_wechat_add_edit_length_label != NULL) {
        lv_label_set_text_fmt(s_wechat_add_edit_length_label,
                              "%u/%u",
                              (unsigned)value_len,
                              (unsigned)DISPLAY_WECHAT_OPEN_ID_LENGTH);
    }

    if (s_wechat_add_edit_status_label == NULL) {
        return;
    }

    if (status_text == NULL) {
        if (value_len == 0) {
            status_text = "请输入微信Open ID";
            status_color = lv_color_hex(0x64758A);
        } else if (value_len != DISPLAY_WECHAT_OPEN_ID_LENGTH) {
            status_text = "必须是28位微信Open ID";
            status_color = lv_color_hex(0xE45656);
        } else {
            status_text = "点击保存生效";
            status_color = lv_color_hex(0x0D8A59);
        }
    }

    display_text_set_color(s_wechat_add_edit_status_label, status_color, 0);
    display_text_set(s_wechat_add_edit_status_label, status_text);
}

static void display_update_tirtc_edit_feedback(const char *status_text, lv_color_t status_color)
{
    const char *value = s_tirtc_edit_ta != NULL ? lv_textarea_get_text(s_tirtc_edit_ta) : "";
    size_t value_len = strlen(value);
    size_t max_len = display_tirtc_config_field_max_len(s_tirtc_edit_field);

    if (s_tirtc_edit_length_label != NULL) {
        lv_label_set_text_fmt(s_tirtc_edit_length_label,
                              "%u/%u",
                              (unsigned)value_len,
                              (unsigned)max_len);
    }

    if (s_tirtc_edit_status_label == NULL) {
        return;
    }

    if (status_text == NULL) {
        if (value_len == 0) {
            status_text = "不能为空";
            status_color = lv_color_hex(0xE45656);
        } else {
            status_text = "点击保存生效";
            status_color = lv_color_hex(0x0D8A59);
        }
    }

    display_text_set_color(s_tirtc_edit_status_label, status_color, 0);
    display_text_set(s_tirtc_edit_status_label, status_text);
}

static void display_prepare_figma_page(lv_obj_t *page)
{
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_color(page, lv_color_hex(0xE8F3FA), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *display_create_figma_text(lv_obj_t *parent,
                                                    const char *text,
                                                    lv_coord_t x,
                                                    lv_coord_t y,
                                                    lv_coord_t width,
                                                    lv_color_t color,
                                                    uint8_t font_size,
                                                    lv_text_align_t align)
{
    const ui_text_asset_t *asset = ui_text_asset_find(text, font_size);
    if (asset != NULL) {
        lv_obj_t *img = display_create_text_asset_obj(parent, x, y, width, color, font_size, align);
        if (img != NULL) {
            display_text_image_ctx_t *ctx = display_text_image_ctx(img);
            display_apply_text_asset(img, asset, ctx);
            return img;
        }
    }

    if (ui_text_asset_has_cjk(text)) {
        lv_obj_t *img = display_create_text_asset_obj(parent, x, y, width, color, font_size, align);
        if (img != NULL) {
            lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGW(TAG, "missing figma text asset: text=\"%s\" size=%u", text != NULL ? text : "", (unsigned)font_size);
            return img;
        }
    }

    lv_obj_t *label = lv_label_create(parent);

    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    display_text_set_color(label, color, 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_style_text_font(label, display_ascii_font(font_size), 0);
    display_text_set(label, text != NULL ? text : "");
    return label;
}

static lv_obj_t *display_create_ai_text(lv_obj_t *parent,
                                        const char *text,
                                        lv_coord_t x,
                                        lv_coord_t y,
                                        lv_coord_t width,
                                        lv_color_t color,
                                        lv_text_align_t align)
{
    return display_create_figma_text(parent, text, x, y, width, color, 12, align);
}

static lv_obj_t *display_create_ai_dialog_text(lv_obj_t *parent,
                                               const char *text,
                                               lv_coord_t x,
                                               lv_coord_t y,
                                               lv_coord_t width,
                                               lv_color_t color,
                                               lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    display_text_set_color(label, color, 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_style_text_font(label, display_ai_chat_font(), 0);
    lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(label, LV_OPA_COVER, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    display_text_set(label, text != NULL ? text : "");
    return label;
}

static lv_obj_t *display_create_ai_chat_caption_text(lv_obj_t *parent,
                                                     const char *text,
                                                     lv_coord_t x,
                                                     lv_coord_t y,
                                                     lv_coord_t width,
                                                     lv_color_t color,
                                                     lv_text_align_t align,
                                                     lv_obj_t **bold_labels)
{
    if (bold_labels != NULL) {
        for (size_t index = 0; index < DISPLAY_AI_CHAT_CAPTION_BOLD_LAYER_COUNT; ++index) {
            bold_labels[index] = NULL;
        }
    }

    lv_obj_t *label = display_create_ai_dialog_text(parent, text, x, y, width, color, align);
    if (label != NULL) {
        lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(label, LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(label);
    }
    return label;
}

static void display_set_ai_chat_caption_label_text(lv_obj_t *label, const char *text)
{
    if (label == NULL) {
        return;
    }
    lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(label, LV_OPA_COVER, 0);
    display_text_set(label, text);
    lv_obj_invalidate(label);
}

static void display_set_ai_chat_caption_text(lv_obj_t *label,
                                             lv_obj_t **bold_labels,
                                             const char *text)
{
    if (bold_labels != NULL) {
        for (size_t index = 0; index < DISPLAY_AI_CHAT_CAPTION_BOLD_LAYER_COUNT; ++index) {
            if (bold_labels[index] != NULL) {
                display_set_ai_chat_caption_label_text(bold_labels[index], text);
            }
        }
    }
    display_set_ai_chat_caption_label_text(label, text);
}

static void display_set_ai_chat_caption_long_mode(lv_obj_t *label, lv_obj_t **bold_labels)
{
    if (label != NULL) {
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_line_space(label, DISPLAY_AI_CHAT_TEXT_LINE_SPACE, 0);
    }
    if (bold_labels != NULL) {
        for (size_t index = 0; index < DISPLAY_AI_CHAT_CAPTION_BOLD_LAYER_COUNT; ++index) {
            if (bold_labels[index] != NULL) {
                lv_label_set_long_mode(bold_labels[index], LV_LABEL_LONG_WRAP);
                lv_obj_set_style_text_line_space(bold_labels[index], DISPLAY_AI_CHAT_TEXT_LINE_SPACE, 0);
            }
        }
    }
}

static void display_layout_ai_single_caption(const char *text)
{
    const lv_font_t *font = display_ai_chat_font();
    lv_point_t wrapped = {0};
    lv_coord_t line_height = font != NULL ? font->line_height : 16;
    lv_coord_t line_space = DISPLAY_AI_CHAT_TEXT_LINE_SPACE;
    lv_coord_t line_step = line_height + line_space;
    lv_coord_t text_height = line_height;
    lv_coord_t bar_height = DISPLAY_AI_SINGLE_CAPTION_BAR_MIN_HEIGHT;
    lv_coord_t bar_y = DISPLAY_AI_SINGLE_CAPTION_BAR_BOTTOM_Y - DISPLAY_AI_SINGLE_CAPTION_BAR_MIN_HEIGHT;
    uint8_t lines = 1;

    if (line_height <= 0) {
        line_height = 16;
    }
    if (line_step <= 0) {
        line_step = line_height;
    }

    if (text != NULL && text[0] != '\0') {
        lv_txt_get_size(&wrapped,
                        text,
                        font,
                        0,
                        line_space,
                        DISPLAY_AI_SINGLE_CAPTION_TEXT_WIDTH,
                        LV_TEXT_FLAG_NONE);
        if (wrapped.y > 0) {
            lines = (uint8_t)((wrapped.y + line_space + line_step - 1) / line_step);
        }
    }
    if (lines == 0U) {
        lines = 1U;
    }
    if (lines > DISPLAY_AI_SINGLE_CAPTION_MAX_LINES) {
        lines = DISPLAY_AI_SINGLE_CAPTION_MAX_LINES;
    }

    text_height = (lv_coord_t)lines * line_height + (lv_coord_t)(lines - 1U) * line_space;
    bar_height = text_height + DISPLAY_AI_SINGLE_CAPTION_TEXT_Y + DISPLAY_AI_SINGLE_CAPTION_TEXT_PAD_BOTTOM;
    if (bar_height < DISPLAY_AI_SINGLE_CAPTION_BAR_MIN_HEIGHT) {
        bar_height = DISPLAY_AI_SINGLE_CAPTION_BAR_MIN_HEIGHT;
    }
    bar_y = DISPLAY_AI_SINGLE_CAPTION_BAR_BOTTOM_Y - bar_height;

    if (s_ai_caption_bar != NULL) {
        lv_obj_set_pos(s_ai_caption_bar, DISPLAY_AI_SINGLE_CAPTION_BAR_X, bar_y);
        lv_obj_set_size(s_ai_caption_bar,
                        DISPLAY_AI_SINGLE_CAPTION_BAR_WIDTH,
                        bar_height);
    }
    if (s_ai_single_caption_label != NULL) {
        lv_obj_set_pos(s_ai_single_caption_label,
                       DISPLAY_AI_SINGLE_CAPTION_TEXT_X,
                       DISPLAY_AI_SINGLE_CAPTION_TEXT_Y);
        lv_obj_set_size(s_ai_single_caption_label,
                        DISPLAY_AI_SINGLE_CAPTION_TEXT_WIDTH,
                        text_height);
        lv_label_set_long_mode(s_ai_single_caption_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_line_space(s_ai_single_caption_label, line_space, 0);
        lv_obj_set_style_text_align(s_ai_single_caption_label, LV_TEXT_ALIGN_CENTER, 0);
    }
}

static void display_measure_ai_chat_bubble_text(const char *text,
                                                lv_coord_t *text_width,
                                                lv_coord_t *text_height)
{
    const lv_font_t *font = display_ai_chat_font();
    lv_point_t natural = {0};
    lv_point_t wrapped = {0};
    lv_coord_t width = DISPLAY_AI_CHAT_MIN_TEXT_WIDTH;
    lv_coord_t height = font != NULL ? font->line_height : 16;

    if (text_width == NULL || text_height == NULL) {
        return;
    }
    if (text == NULL || text[0] == '\0') {
        *text_width = width;
        *text_height = height;
        return;
    }

    lv_txt_get_size(&natural,
                    text,
                    font,
                    0,
                    DISPLAY_AI_CHAT_TEXT_LINE_SPACE,
                    LV_COORD_MAX,
                    LV_TEXT_FLAG_NONE);
    if (natural.x + DISPLAY_AI_CHAT_TEXT_SAFE_WIDTH <= DISPLAY_AI_CHAT_TEXT_MAX_WIDTH) {
        width = natural.x < DISPLAY_AI_CHAT_MIN_TEXT_WIDTH ? DISPLAY_AI_CHAT_MIN_TEXT_WIDTH : natural.x;
        height = natural.y;
    } else {
        lv_txt_get_size(&wrapped,
                        text,
                        font,
                        0,
                        DISPLAY_AI_CHAT_TEXT_LINE_SPACE,
                        DISPLAY_AI_CHAT_TEXT_MAX_WIDTH,
                        LV_TEXT_FLAG_NONE);
        width = DISPLAY_AI_CHAT_TEXT_MAX_WIDTH;
        height = wrapped.y;
    }

    if (width + DISPLAY_AI_CHAT_TEXT_SAFE_WIDTH <= DISPLAY_AI_CHAT_TEXT_MAX_WIDTH) {
        width += DISPLAY_AI_CHAT_TEXT_SAFE_WIDTH;
    }
    if (font != NULL && height < font->line_height) {
        height = font->line_height;
    }
    height += DISPLAY_AI_CHAT_TEXT_SAFE_HEIGHT;

    *text_width = width;
    *text_height = height;
}

static void display_layout_ai_chat_caption_label(lv_obj_t *label,
                                                 lv_coord_t text_width,
                                                 lv_coord_t text_height,
                                                 lv_color_t text_color)
{
    if (label == NULL) {
        return;
    }

    lv_obj_set_pos(label, DISPLAY_AI_CHAT_BUBBLE_TEXT_X, DISPLAY_AI_CHAT_BUBBLE_TEXT_Y);
    lv_obj_set_size(label, text_width, text_height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(label, DISPLAY_AI_CHAT_TEXT_LINE_SPACE, 0);
    display_text_set_color(label, text_color, 0);
}

static uint32_t display_ai_chat_hash_text(const char *text)
{
    uint32_t hash = 2166136261UL;

    if (text == NULL) {
        return hash;
    }

    for (size_t index = 0; text[index] != '\0'; ++index) {
        hash ^= (uint8_t)text[index];
        hash *= 16777619UL;
    }
    return hash;
}

static void display_hide_ai_chat_message_slot(uint8_t slot)
{
    if (slot >= DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX) {
        return;
    }

    if (s_ai_message_boxes[slot] != NULL) {
        lv_obj_add_flag(s_ai_message_boxes[slot], LV_OBJ_FLAG_HIDDEN);
    }
    s_ai_visible_message_indices[slot] = UINT8_MAX;
    s_ai_visible_message_generations[slot] = 0;
    s_ai_visible_message_hashes[slot] = 0;
    s_ai_visible_message_y[slot] = 0;
    s_ai_visible_message_bottom_y[slot] = 0;
}

static void display_reset_ai_chat_visible_slots(void)
{
    for (uint8_t slot = 0; slot < DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX; ++slot) {
        display_hide_ai_chat_message_slot(slot);
    }
}

static void display_apply_ai_chat_caption_bubble_layout(uint8_t slot,
                                                        const display_ai_message_layout_t *layout,
                                                        const char *text)
{
    lv_obj_t *box = NULL;
    lv_obj_t *label = NULL;
    lv_obj_t **bold_labels = NULL;
    lv_color_t fill = lv_color_hex(0x2F82D7);
    lv_color_t text_color = lv_color_hex(0xFFFFFF);

    if (slot >= DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX || layout == NULL) {
        return;
    }

    box = s_ai_message_boxes[slot];
    label = s_ai_message_labels[slot];
    bold_labels = s_ai_message_bold_labels[slot];
    if (box == NULL || label == NULL || text == NULL || text[0] == '\0') {
        display_hide_ai_chat_message_slot(slot);
        return;
    }

    fill = layout->align_right ? lv_color_hex(0x21C783) : lv_color_hex(0x2F82D7);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(box, layout->x, layout->y);
    lv_obj_set_size(box, layout->bubble_width, layout->bubble_height);
    lv_obj_set_style_radius(box, DISPLAY_AI_CHAT_BUBBLE_RADIUS, 0);
    lv_obj_set_style_bg_color(box, fill, 0);
    lv_obj_set_style_border_color(box, fill, 0);

    display_layout_ai_chat_caption_label(label,
                                         layout->text_width,
                                         layout->text_height,
                                         text_color);
    for (size_t index = 0; index < DISPLAY_AI_CHAT_CAPTION_BOLD_LAYER_COUNT; ++index) {
        display_layout_ai_chat_caption_label(bold_labels[index],
                                             layout->text_width,
                                             layout->text_height,
                                             text_color);
    }
    display_set_ai_chat_caption_text(label, bold_labels, text);
    s_ai_visible_message_indices[slot] = layout->message_index;
    s_ai_visible_message_generations[slot] = s_ai_message_layout_generation;
    s_ai_visible_message_hashes[slot] = layout->text_hash;
    s_ai_visible_message_y[slot] = layout->y;
    s_ai_visible_message_bottom_y[slot] = layout->y + layout->bubble_height;
}

static void display_update_ai_chat_scroll_spacer(void)
{
    if (s_ai_message_scroll_spacer == NULL) {
        return;
    }

    lv_obj_set_pos(s_ai_message_scroll_spacer, 0, 0);
    lv_obj_set_size(s_ai_message_scroll_spacer, 1, s_ai_message_content_height);
}

static uint8_t display_find_ai_chat_slot_for_message(uint8_t message_index, const bool *slot_used)
{
    if (slot_used == NULL) {
        return UINT8_MAX;
    }

    for (uint8_t slot = 0; slot < DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX; ++slot) {
        if (!slot_used[slot] && s_ai_visible_message_indices[slot] == message_index) {
            return slot;
        }
    }
    return UINT8_MAX;
}

static uint8_t display_find_ai_chat_free_slot(const bool *slot_used)
{
    if (slot_used == NULL) {
        return UINT8_MAX;
    }

    for (uint8_t slot = 0; slot < DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX; ++slot) {
        if (!slot_used[slot] && s_ai_visible_message_indices[slot] == UINT8_MAX) {
            return slot;
        }
    }
    for (uint8_t slot = 0; slot < DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX; ++slot) {
        if (!slot_used[slot]) {
            return slot;
        }
    }
    return UINT8_MAX;
}

static void display_clear_ai_chat_message_view(void)
{
    if (s_actions.on_clear_ai_chat_messages != NULL) {
        (void)s_actions.on_clear_ai_chat_messages(s_actions.ctx);
    }

    if (s_last_status_ptr != NULL) {
        s_last_status.ai_chat_asr_caption[0] = '\0';
        s_last_status.ai_chat_tts_caption[0] = '\0';
        s_last_status.ai_chat_message_count = 0;
        memset(s_last_status.ai_chat_messages, 0, sizeof(s_last_status.ai_chat_messages));
    }

    s_ai_message_layout_count = 0;
    s_ai_message_content_height = DISPLAY_AI_CHAT_VIEWPORT_HEIGHT;
    s_ai_new_chat_button_y = DISPLAY_AI_CHAT_BUBBLE_TOP_Y;
    s_ai_message_layout_new_button_visible = false;
    s_ai_message_layout_font_ready = ai_chat_font_is_ready();
    s_ai_message_touching = false;
    s_ai_message_scroll_render_last_us = 0;
    s_ai_message_layout_generation++;
    if (s_ai_message_layout_generation == 0U) {
        s_ai_message_layout_generation = 1U;
    }

    display_reset_ai_chat_visible_slots();
    display_update_ai_chat_scroll_spacer();
    if (s_ai_new_chat_btn != NULL) {
        lv_obj_add_flag(s_ai_new_chat_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ai_message_list != NULL) {
        lv_obj_scroll_to_y(s_ai_message_list, 0, LV_ANIM_OFF);
    }
    display_update_ai_chat_scroll_controls();
}

static void display_render_ai_chat_visible_messages(const display_status_t *status)
{
    int64_t start_us = esp_timer_get_time();
    lv_coord_t viewport_top = 0;
    lv_coord_t viewport_bottom = DISPLAY_AI_CHAT_VIEWPORT_HEIGHT;
    uint8_t visible_layouts[DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX];
    bool slot_used[DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX] = {0};
    uint8_t visible_count = 0;
    uint8_t changed_count = 0;
    uint8_t hidden_count = 0;
    int64_t elapsed_us = 0;

    if (status == NULL || s_ai_message_list == NULL) {
        return;
    }

    viewport_top = lv_obj_get_scroll_y(s_ai_message_list) - DISPLAY_AI_CHAT_VIRTUAL_OVERSCAN;
    viewport_bottom = lv_obj_get_scroll_y(s_ai_message_list) +
                      DISPLAY_AI_CHAT_VIEWPORT_HEIGHT +
                      DISPLAY_AI_CHAT_VIRTUAL_OVERSCAN;

    for (uint8_t index = 0;
         index < s_ai_message_layout_count && visible_count < DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX;
         ++index) {
        const display_ai_message_layout_t *layout = &s_ai_message_layouts[index];
        lv_coord_t bubble_bottom = layout->y + layout->bubble_height;
        if (bubble_bottom < viewport_top || layout->y > viewport_bottom) {
            continue;
        }
        visible_layouts[visible_count++] = index;
    }

    for (uint8_t index = 0; index < visible_count; ++index) {
        const display_ai_message_layout_t *layout = &s_ai_message_layouts[visible_layouts[index]];
        const display_ai_chat_message_t *message = &status->ai_chat_messages[layout->message_index];
        uint8_t slot = display_find_ai_chat_slot_for_message(layout->message_index, slot_used);
        if (slot == UINT8_MAX) {
            slot = display_find_ai_chat_free_slot(slot_used);
        }
        if (slot == UINT8_MAX) {
            break;
        }
        slot_used[slot] = true;

        if (s_ai_visible_message_indices[slot] != layout->message_index ||
            s_ai_visible_message_generations[slot] != s_ai_message_layout_generation) {
            display_apply_ai_chat_caption_bubble_layout(slot, layout, message->text);
            changed_count++;
        }
    }

    for (uint8_t slot = 0; slot < DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX; ++slot) {
        if (!slot_used[slot]) {
            if (s_ai_visible_message_indices[slot] != UINT8_MAX) {
                hidden_count++;
            }
            display_hide_ai_chat_message_slot(slot);
        }
    }

    elapsed_us = esp_timer_get_time() - start_us;
    if (elapsed_us >= DISPLAY_AI_CHAT_SCROLL_RENDER_SLOW_US &&
        start_us - s_ai_message_scroll_render_last_log_us >= DISPLAY_AI_CHAT_SCROLL_RENDER_LOG_INTERVAL_US) {
        s_ai_message_scroll_render_last_log_us = start_us;
        ESP_LOGW(TAG,
                 "AI Chat scroll render slow: elapsed=%lldus y=%d bottom=%d visible=%u changed=%u hidden=%u content=%d",
                 (long long)elapsed_us,
                 (int)lv_obj_get_scroll_y(s_ai_message_list),
                 (int)lv_obj_get_scroll_bottom(s_ai_message_list),
                 (unsigned)visible_count,
                 (unsigned)changed_count,
                 (unsigned)hidden_count,
                 (int)s_ai_message_content_height);
    }
}

static bool display_ai_chat_should_show_new_chat_button(const display_status_t *status)
{
    if (status == NULL || status->ai_chat_active || s_actions.on_start_ai_chat == NULL) {
        return false;
    }

    return status->ai_chat_state == DISPLAY_AI_CHAT_STATE_IDLE ||
           status->ai_chat_state == DISPLAY_AI_CHAT_STATE_ERROR;
}

static lv_coord_t display_update_ai_chat_new_chat_button(lv_coord_t y, bool visible)
{
    if (s_ai_new_chat_btn == NULL) {
        return y;
    }
    if (!visible) {
        lv_obj_add_flag(s_ai_new_chat_btn, LV_OBJ_FLAG_HIDDEN);
        return y;
    }

    lv_coord_t x = (DISPLAY_AI_CHAT_CONTENT_WIDTH - DISPLAY_AI_CHAT_NEW_BUTTON_WIDTH) / 2;
    lv_obj_clear_flag(s_ai_new_chat_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_ai_new_chat_btn, x, y);
    lv_obj_set_size(s_ai_new_chat_btn,
                    DISPLAY_AI_CHAT_NEW_BUTTON_WIDTH,
                    DISPLAY_AI_CHAT_NEW_BUTTON_HEIGHT);
    return y + DISPLAY_AI_CHAT_NEW_BUTTON_HEIGHT + DISPLAY_AI_CHAT_BUBBLE_GAP_Y;
}

static void display_ai_chat_message_list_scroll_to_bottom(bool should_follow)
{
    if (!should_follow || s_ai_message_list == NULL) {
        return;
    }

    lv_obj_update_layout(s_ai_message_list);
    lv_obj_scroll_to_y(s_ai_message_list, LV_COORD_MAX, LV_ANIM_OFF);
}

static void display_set_ai_chat_scroll_button_visible(lv_obj_t *button, bool visible)
{
    if (button == NULL) {
        return;
    }

    if (visible) {
        lv_obj_clear_flag(button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(button);
    } else {
        lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
    }
}

static void display_update_ai_chat_scroll_controls(void)
{
    lv_coord_t scroll_y = 0;
    lv_coord_t scroll_bottom = 0;
    bool scrollable = false;

    if (s_ai_message_list == NULL) {
        return;
    }

    scroll_y = lv_obj_get_scroll_y(s_ai_message_list);
    scroll_bottom = lv_obj_get_scroll_bottom(s_ai_message_list);
    scrollable = s_ai_message_content_height > DISPLAY_AI_CHAT_VIEWPORT_HEIGHT + DISPLAY_AI_CHAT_BUBBLE_GAP_Y;

    display_set_ai_chat_scroll_button_visible(s_ai_scroll_up_btn, scrollable && scroll_y > 0);
    display_set_ai_chat_scroll_button_visible(s_ai_scroll_down_btn,
                                              scrollable &&
                                                  scroll_bottom > DISPLAY_AI_CHAT_BOTTOM_FOLLOW_THRESHOLD);
    display_set_ai_chat_scroll_button_visible(s_ai_scroll_bottom_btn,
                                              scrollable &&
                                              scroll_bottom > DISPLAY_AI_CHAT_BOTTOM_SNAP_THRESHOLD);
}

static const display_ai_chat_message_t *display_find_latest_ai_chat_message(const display_status_t *status,
                                                                            uint8_t caption_type,
                                                                            uint8_t message_count,
                                                                            uint8_t *message_index)
{
    if (message_index != NULL) {
        *message_index = UINT8_MAX;
    }
    if (status == NULL || message_count == 0) {
        return NULL;
    }

    for (int index = (int)message_count - 1; index >= 0; --index) {
        const display_ai_chat_message_t *message = &status->ai_chat_messages[index];
        if (message->caption_type == caption_type && message->text[0] != '\0') {
            if (message_index != NULL) {
                *message_index = (uint8_t)index;
            }
            return message;
        }
    }
    return NULL;
}

static lv_coord_t display_apply_ai_chat_pair_message(uint8_t slot,
                                                     const display_ai_chat_message_t *message,
                                                     uint8_t message_index,
                                                     lv_coord_t y)
{
    display_ai_message_layout_t layout = {0};
    lv_coord_t text_width = 0;
    lv_coord_t text_height = 0;
    lv_coord_t bubble_width = 0;
    lv_coord_t bubble_height = 0;
    lv_coord_t max_text_height = 0;
    bool align_right = false;
    uint32_t text_hash = 0;
    lv_obj_t *box = NULL;

    if (slot >= DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX || message == NULL || message->text[0] == '\0') {
        return y;
    }

    box = s_ai_message_boxes[slot];
    text_hash = display_ai_chat_hash_text(message->text);
    if (box != NULL &&
        !lv_obj_has_flag(box, LV_OBJ_FLAG_HIDDEN) &&
        s_ai_visible_message_indices[slot] == message_index &&
        s_ai_visible_message_hashes[slot] == text_hash &&
        s_ai_visible_message_y[slot] == y &&
        s_ai_visible_message_bottom_y[slot] > y) {
        return s_ai_visible_message_bottom_y[slot] + DISPLAY_AI_CHAT_PAIR_GAP_Y;
    }

    display_measure_ai_chat_bubble_text(message->text, &text_width, &text_height);
    bubble_width = text_width + DISPLAY_AI_CHAT_BUBBLE_TEXT_X + DISPLAY_AI_CHAT_BUBBLE_PAD_RIGHT;
    bubble_height = text_height + DISPLAY_AI_CHAT_BUBBLE_TEXT_Y + DISPLAY_AI_CHAT_BUBBLE_PAD_BOTTOM;
    if (bubble_height > DISPLAY_AI_CHAT_PAIR_BUBBLE_MAX_HEIGHT) {
        bubble_height = DISPLAY_AI_CHAT_PAIR_BUBBLE_MAX_HEIGHT;
        max_text_height = bubble_height - DISPLAY_AI_CHAT_BUBBLE_TEXT_Y - DISPLAY_AI_CHAT_BUBBLE_PAD_BOTTOM;
        if (text_height > max_text_height) {
            text_height = max_text_height;
        }
    }

    align_right = message->caption_type == DISPLAY_AI_CHAT_CAPTION_TYPE_ASR;
    layout.message_index = message_index;
    layout.caption_type = message->caption_type;
    layout.utterance_id = message->utterance_id;
    layout.text_hash = text_hash;
    layout.align_right = align_right;
    layout.text_width = text_width;
    layout.text_height = text_height;
    layout.bubble_width = bubble_width;
    layout.bubble_height = bubble_height;
    layout.x = align_right
                   ? DISPLAY_AI_CHAT_CONTENT_WIDTH - DISPLAY_AI_CHAT_BUBBLE_LEFT_X - bubble_width
                   : DISPLAY_AI_CHAT_BUBBLE_LEFT_X;
    layout.y = y;

    display_apply_ai_chat_caption_bubble_layout(slot, &layout, message->text);
    return y + bubble_height + DISPLAY_AI_CHAT_PAIR_GAP_Y;
}

static void __attribute__((unused)) display_update_ai_chat_pair_page(const display_status_t *status,
                                                                     uint8_t message_count,
                                                                     bool show_new_chat_button,
                                                                     bool font_ready)
{
    uint8_t asr_index = UINT8_MAX;
    uint8_t tts_index = UINT8_MAX;
    const display_ai_chat_message_t *asr =
        display_find_latest_ai_chat_message(status,
                                            DISPLAY_AI_CHAT_CAPTION_TYPE_ASR,
                                            message_count,
                                            &asr_index);
    const display_ai_chat_message_t *tts =
        display_find_latest_ai_chat_message(status,
                                            DISPLAY_AI_CHAT_CAPTION_TYPE_TTS,
                                            message_count,
                                            &tts_index);
    const display_ai_chat_message_t *first = NULL;
    const display_ai_chat_message_t *second = NULL;
    uint8_t first_index = UINT8_MAX;
    uint8_t second_index = UINT8_MAX;
    lv_coord_t next_y = DISPLAY_AI_CHAT_PAIR_TOP_Y;
    lv_coord_t new_button_y = DISPLAY_AI_CHAT_PAIR_NEW_BUTTON_Y;
    bool has_message = false;

    s_ai_message_layout_count = 0;
    s_ai_message_content_height = DISPLAY_AI_CHAT_VIEWPORT_HEIGHT;
    s_ai_message_layout_new_button_visible = show_new_chat_button;
    s_ai_message_layout_font_ready = font_ready;
    display_update_ai_chat_scroll_spacer();

    if (asr != NULL && tts != NULL) {
        if (asr_index <= tts_index) {
            first = asr;
            first_index = asr_index;
            second = tts;
            second_index = tts_index;
        } else {
            first = tts;
            first_index = tts_index;
            second = asr;
            second_index = asr_index;
        }
    } else if (asr != NULL) {
        first = asr;
        first_index = asr_index;
    } else if (tts != NULL) {
        first = tts;
        first_index = tts_index;
    }

    if (first != NULL) {
        has_message = true;
        next_y = display_apply_ai_chat_pair_message(0, first, first_index, next_y);
    } else {
        display_hide_ai_chat_message_slot(0);
    }

    if (second != NULL) {
        next_y = display_apply_ai_chat_pair_message(1, second, second_index, next_y);
    } else {
        display_hide_ai_chat_message_slot(1);
    }

    if (show_new_chat_button) {
        if (has_message) {
            new_button_y = next_y;
            if (new_button_y + DISPLAY_AI_CHAT_NEW_BUTTON_HEIGHT >
                DISPLAY_AI_CHAT_VIEWPORT_HEIGHT - DISPLAY_AI_CHAT_BUBBLE_GAP_Y) {
                new_button_y = DISPLAY_AI_CHAT_VIEWPORT_HEIGHT -
                               DISPLAY_AI_CHAT_NEW_BUTTON_HEIGHT -
                               DISPLAY_AI_CHAT_BUBBLE_GAP_Y;
            }
        }
        (void)display_update_ai_chat_new_chat_button(new_button_y, true);
    } else if (s_ai_new_chat_btn != NULL) {
        lv_obj_add_flag(s_ai_new_chat_btn, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_ai_message_list != NULL && lv_obj_get_scroll_y(s_ai_message_list) != 0) {
        lv_obj_scroll_to_y(s_ai_message_list, 0, LV_ANIM_OFF);
    }
    display_update_ai_chat_scroll_controls();
}

static void display_ai_chat_render_scroll_limited(const display_status_t *status, bool force)
{
    int64_t now_us = esp_timer_get_time();

    if (status == NULL) {
        return;
    }

    if (!force &&
        s_ai_message_scroll_render_last_us > 0 &&
        now_us - s_ai_message_scroll_render_last_us < DISPLAY_AI_CHAT_SCROLL_RENDER_MIN_INTERVAL_US) {
        return;
    }

    s_ai_message_scroll_render_last_us = now_us;
    display_render_ai_chat_visible_messages(status);
    display_update_ai_chat_scroll_controls();
}

static void display_ai_chat_scroll_to(lv_coord_t target_y)
{
    if (s_ai_message_list == NULL) {
        return;
    }

    lv_obj_scroll_to_y(s_ai_message_list, target_y, LV_ANIM_OFF);
    display_ai_chat_render_scroll_limited(&s_last_status, true);
}

static void display_ai_chat_scroll_action_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    display_ai_scroll_action_t action =
        (display_ai_scroll_action_t)(intptr_t)lv_event_get_user_data(event);
    lv_coord_t current_y = 0;

    if (code != LV_EVENT_CLICKED || s_ai_message_list == NULL) {
        return;
    }

    current_y = lv_obj_get_scroll_y(s_ai_message_list);
    switch (action) {
    case DISPLAY_AI_SCROLL_PAGE_UP:
        display_ai_chat_scroll_to(current_y - DISPLAY_AI_CHAT_PAGE_STEP);
        break;
    case DISPLAY_AI_SCROLL_PAGE_DOWN:
        display_ai_chat_scroll_to(current_y + DISPLAY_AI_CHAT_PAGE_STEP);
        break;
    case DISPLAY_AI_SCROLL_BOTTOM:
        display_ai_chat_message_list_scroll_to_bottom(true);
        display_ai_chat_render_scroll_limited(&s_last_status, true);
        break;
    default:
        break;
    }
}

static void display_ai_chat_message_list_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_SCROLL_BEGIN) {
        s_ai_message_touching = true;
    } else if (code == LV_EVENT_SCROLL) {
        display_ai_chat_render_scroll_limited(&s_last_status, false);
    } else if (code == LV_EVENT_RELEASED ||
               code == LV_EVENT_PRESS_LOST ||
               code == LV_EVENT_SCROLL_END) {
        s_ai_message_touching = false;
        if (code == LV_EVENT_SCROLL_END &&
            s_ai_message_list != NULL &&
            lv_obj_get_scroll_bottom(s_ai_message_list) <= DISPLAY_AI_CHAT_BOTTOM_SNAP_THRESHOLD) {
            display_ai_chat_message_list_scroll_to_bottom(true);
        }
        display_ai_chat_render_scroll_limited(&s_last_status, true);
    }
}

static lv_obj_t *display_create_ai_chat_scroll_button(lv_obj_t *parent,
                                                      lv_coord_t y,
                                                      const char *symbol,
                                                      display_ai_scroll_action_t action)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_t *label = NULL;

    if (button == NULL) {
        return NULL;
    }

    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button, DISPLAY_AI_CHAT_SCROLL_BUTTON_X, y);
    lv_obj_set_size(button,
                    DISPLAY_AI_CHAT_SCROLL_BUTTON_SIZE,
                    DISPLAY_AI_CHAT_SCROLL_BUTTON_SIZE);
    lv_obj_set_style_radius(button, DISPLAY_AI_CHAT_SCROLL_BUTTON_SIZE / 2, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xD9ECFA), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_90, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0xB7D4E8), 0);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(button,
                        display_ai_chat_scroll_action_cb,
                        LV_EVENT_CLICKED,
                        (void *)(intptr_t)action);

    label = lv_label_create(button);
    if (label != NULL) {
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        display_text_set_color(label, lv_color_hex(0x2F82D7), 0);
        display_text_set(label, symbol);
        lv_obj_center(label);
    }

    return button;
}

static lv_obj_t *display_create_ai_static_text(lv_obj_t *parent,
                                               const char *text,
                                               lv_coord_t x,
                                               lv_coord_t y,
                                               lv_coord_t width,
                                               lv_color_t color,
                                               uint8_t font_size,
                                               lv_text_align_t align)
{
    return display_create_figma_text(parent, text, x, y, width, color, font_size, align);
}

static lv_obj_t *display_create_figma_box(lv_obj_t *parent,
                                                   lv_coord_t x,
                                                   lv_coord_t y,
                                                   lv_coord_t width,
                                                   lv_coord_t height,
                                                   lv_color_t fill,
                                                   lv_color_t stroke,
                                                   lv_coord_t radius)
{
    lv_obj_t *box = lv_obj_create(parent);

    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, width, height);
    lv_obj_set_style_radius(box, radius, 0);
    lv_obj_set_style_bg_color(box, fill, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, stroke, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

static lv_obj_t *display_create_figma_button(lv_obj_t *parent,
                                                      lv_coord_t x,
                                                      lv_coord_t y,
                                                      lv_coord_t width,
                                                      lv_coord_t height,
                                                      lv_color_t fill,
                                                      lv_color_t stroke,
                                                      const char *text,
                                                      lv_color_t text_color,
                                                      uint8_t font_size,
                                                      lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_t *label = NULL;

    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, width, height);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, fill, 0);
    lv_obj_set_style_bg_color(btn, lv_color_darken(fill, 18), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, stroke, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }

    label = display_create_figma_text(btn,
                                      text,
                                      0,
                                      (height - 16) / 2,
                                      width,
                                      text_color,
                                      font_size,
                                      LV_TEXT_ALIGN_CENTER);
    if (label != NULL) {
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    }
    return btn;
}

static lv_obj_t *display_create_wifi_signal_bar(lv_obj_t *parent,
                                                lv_coord_t x,
                                                lv_coord_t y,
                                                lv_coord_t height,
                                                lv_color_t color)
{
    lv_obj_t *bar = lv_obj_create(parent);

    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, 4, height);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_bg_color(bar, color, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return bar;
}

static lv_obj_t *display_create_wifi_x_line(lv_obj_t *parent,
                                            lv_coord_t x,
                                            lv_coord_t y,
                                            const lv_point_t *points)
{
    lv_obj_t *line = lv_line_create(parent);

    lv_line_set_points(line, points, 2);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_style_line_width(line, 2, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(0xF6494C), 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return line;
}

static uint8_t display_wifi_status_level(const display_status_t *status)
{
    if (status == NULL || !status->network_connected) {
        return 0;
    }
    if (status->network_rssi >= -60) {
        return 3;
    }
    if (status->network_rssi >= -75) {
        return 2;
    }
    return 1;
}

static display_wifi_indicator_t *display_wifi_indicator_alloc(lv_color_t active_color)
{
    display_wifi_indicator_t *indicator = NULL;

    for (size_t index = 0; index < s_wifi_indicator_count; ++index) {
        if (!s_wifi_indicators[index].active) {
            indicator = &s_wifi_indicators[index];
            break;
        }
    }

    if (indicator == NULL) {
        if (s_wifi_indicator_count >= DISPLAY_WIFI_INDICATOR_MAX) {
            ESP_LOGW(TAG, "wifi indicator capacity exhausted");
            return NULL;
        }

        indicator = &s_wifi_indicators[s_wifi_indicator_count++];
    }

    memset(indicator, 0, sizeof(*indicator));
    indicator->active = true;
    indicator->active_color = active_color;
    indicator->inactive_color = lv_color_hex(0xBCCAD8);
    return indicator;
}

static void display_wifi_indicator_owner_delete_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DELETE) {
        return;
    }

    display_wifi_indicator_t *indicator = lv_event_get_user_data(event);
    if (indicator == NULL) {
        return;
    }

    memset(indicator, 0, sizeof(*indicator));
}

static bool display_wifi_indicator_prune_invalid_objects(display_wifi_indicator_t *indicator)
{
    bool has_valid_object = false;

    for (uint8_t index = 0; index < DISPLAY_WIFI_INDICATOR_BAR_COUNT; ++index) {
        if (indicator->bars[index] == NULL) {
            continue;
        }
        if (!lv_obj_is_valid(indicator->bars[index])) {
            indicator->bars[index] = NULL;
            continue;
        }
        has_valid_object = true;
    }

    for (uint8_t index = 0; index < 2; ++index) {
        if (indicator->x_lines[index] == NULL) {
            continue;
        }
        if (!lv_obj_is_valid(indicator->x_lines[index])) {
            indicator->x_lines[index] = NULL;
            continue;
        }
        has_valid_object = true;
    }

    if (!has_valid_object) {
        memset(indicator, 0, sizeof(*indicator));
    }

    return has_valid_object;
}

static void display_update_wifi_indicator(display_wifi_indicator_t *indicator,
                                          const display_status_t *status)
{
    uint8_t level = display_wifi_status_level(status);
    bool connected = level > 0;

    if (indicator == NULL || !indicator->active) {
        return;
    }
    if (!display_wifi_indicator_prune_invalid_objects(indicator)) {
        return;
    }

    if (indicator->status_valid &&
        indicator->connected == connected &&
        indicator->level == level) {
        return;
    }
    indicator->status_valid = true;
    indicator->connected = connected;
    indicator->level = level;

    for (uint8_t index = 0; index < DISPLAY_WIFI_INDICATOR_BAR_COUNT; ++index) {
        if (indicator->bars[index] == NULL) {
            continue;
        }

        lv_obj_set_style_bg_color(indicator->bars[index],
                                  (connected && index < level)
                                      ? indicator->active_color
                                      : indicator->inactive_color,
                                  0);
    }

    for (uint8_t index = 0; index < 2; ++index) {
        if (indicator->x_lines[index] == NULL) {
            continue;
        }
        if (connected) {
            lv_obj_add_flag(indicator->x_lines[index], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(indicator->x_lines[index], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void display_update_wifi_indicators(const display_status_t *status)
{
    for (size_t index = 0; index < s_wifi_indicator_count; ++index) {
        if (s_wifi_indicators[index].active) {
            display_update_wifi_indicator(&s_wifi_indicators[index], status);
        }
    }
}

static display_wifi_indicator_t *display_create_wifi_indicator(lv_obj_t *parent,
                                                              lv_coord_t x,
                                                              lv_coord_t y,
                                                              lv_color_t active_color)
{
    static const lv_point_t wifi_x_line_a[] = {
        {0, 0},
        {6, 6},
    };
    static const lv_point_t wifi_x_line_b[] = {
        {6, 0},
        {0, 6},
    };
    display_wifi_indicator_t *indicator = display_wifi_indicator_alloc(active_color);

    if (indicator == NULL) {
        return NULL;
    }

    indicator->bars[0] = display_create_wifi_signal_bar(parent, x, y + 8, 6, indicator->inactive_color);
    indicator->bars[1] = display_create_wifi_signal_bar(parent, x + 7, y + 4, 10, indicator->inactive_color);
    indicator->bars[2] = display_create_wifi_signal_bar(parent, x + 14, y, 14, indicator->inactive_color);
    indicator->x_lines[0] = display_create_wifi_x_line(parent, x + 19, y - 2, wifi_x_line_a);
    indicator->x_lines[1] = display_create_wifi_x_line(parent, x + 19, y - 2, wifi_x_line_b);
    lv_obj_add_event_cb(parent, display_wifi_indicator_owner_delete_cb, LV_EVENT_DELETE, indicator);
    display_update_wifi_indicator(indicator, &s_last_status);
    return indicator;
}

static void display_create_figma_signal(lv_obj_t *header)
{
    (void)display_create_wifi_indicator(header, 294, 8, lv_color_hex(0x20BF7A));
}

static lv_obj_t *display_create_figma_header(lv_obj_t *page,
                                                      const char *title,
                                                      lv_event_cb_t back_cb,
                                                      const char *action_text,
                                                      lv_color_t action_color,
                                                      lv_event_cb_t action_cb)
{
    lv_obj_t *header = display_create_figma_box(page,
                                                0,
                                                0,
                                                320,
                                                28,
                                                lv_color_hex(0xF7FBFF),
                                                lv_color_hex(0xD5E0EB),
                                                0);

    if (back_cb != NULL) {
        lv_obj_t *back_btn = lv_btn_create(header);
        lv_obj_remove_style_all(back_btn);
        lv_obj_set_pos(back_btn, 0, 0);
        lv_obj_set_size(back_btn, 66, 28);
        lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(back_btn, back_cb, LV_EVENT_CLICKED, NULL);
        display_create_figma_text(back_btn,
                                  "<",
                                  8,
                                  3,
                                  34,
                                  lv_color_hex(0x10243E),
                                  16,
                                  LV_TEXT_ALIGN_CENTER);
    }

    display_create_figma_text(header,
                              title,
                              66,
                              5,
                              188,
                              lv_color_hex(0x10243E),
                              16,
                              LV_TEXT_ALIGN_CENTER);

    if (action_text != NULL) {
        display_create_figma_button(header,
                                    262,
                                    3,
                                    50,
                                    22,
                                    action_color,
                                    action_color,
                                    action_text,
                                    lv_color_hex(0xFFFFFF),
                                    12,
                                    action_cb);
    } else {
        display_create_figma_signal(header);
    }

    return header;
}

static void display_create_ai_signal(lv_obj_t *header)
{
    (void)display_create_wifi_indicator(header, 294, 8, lv_color_hex(0x23C17D));
}

static lv_obj_t *display_create_ai_mask_image(lv_obj_t *parent,
                                              const lv_img_dsc_t *src,
                                              lv_coord_t x,
                                              lv_coord_t y,
                                              lv_color_t color,
                                              uint16_t zoom)
{
    lv_obj_t *img = lv_img_create(parent);

    lv_img_set_src(img, src);
    lv_img_set_zoom(img, zoom);
    lv_obj_set_pos(img, x, y);
    lv_obj_set_style_img_recolor(img, color, 0);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    return img;
}

static lv_obj_t *display_create_ai_header(lv_obj_t *page,
                                          const char *title,
                                          lv_event_cb_t back_cb,
                                          bool show_status,
                                          bool show_settings)
{
    lv_obj_t *header = display_create_figma_box(page,
                                                0,
                                                0,
                                                320,
                                                28,
                                                lv_color_hex(0xF7FBFE),
                                                lv_color_hex(0xD2E1EC),
                                                0);

    if (back_cb != NULL) {
        lv_obj_t *back_btn = lv_btn_create(header);
        lv_obj_remove_style_all(back_btn);
        lv_obj_set_pos(back_btn, 0, 0);
        lv_obj_set_size(back_btn, 64, 28);
        lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(back_btn, back_cb, LV_EVENT_CLICKED, NULL);
        display_create_figma_text(back_btn,
                                  "<",
                                  11,
                                  2,
                                  30,
                                  lv_color_hex(0x11233C),
                                  18,
                                  LV_TEXT_ALIGN_CENTER);
    }

    if (show_status) {
        s_ai_status_label = display_create_ai_static_text(header,
                                                          "待命",
                                                          41,
                                                          5,
                                                          48,
                                                          lv_color_hex(0x23C17D),
                                                          12,
                                                          LV_TEXT_ALIGN_LEFT);
    }

    display_create_ai_static_text(header,
                                  title,
                                  show_settings ? 100 : 76,
                                  4,
                                  show_settings ? 120 : 168,
                                  lv_color_hex(0x11233C),
                                  12,
                                  LV_TEXT_ALIGN_CENTER);

    if (show_settings) {
        display_create_ai_mask_image(header,
                                     &ai_chat_settings_button_fill_img,
                                     DISPLAY_AI_HEADER_SETTINGS_X,
                                     DISPLAY_AI_HEADER_SETTINGS_Y,
                                     lv_color_hex(0xE9F5FF),
                                     DISPLAY_AI_HEADER_SETTINGS_ZOOM);
        display_create_ai_mask_image(header,
                                     &ai_chat_settings_button_stroke_img,
                                     DISPLAY_AI_HEADER_SETTINGS_X,
                                     DISPLAY_AI_HEADER_SETTINGS_Y,
                                     lv_color_hex(0x2F82D7),
                                     DISPLAY_AI_HEADER_SETTINGS_ZOOM);
        display_create_ai_static_text(header,
                                      "设置",
                                      DISPLAY_AI_HEADER_SETTINGS_X,
                                      DISPLAY_AI_HEADER_SETTINGS_TEXT_Y,
                                      DISPLAY_AI_HEADER_SETTINGS_TEXT_WIDTH,
                                      lv_color_hex(0x2F82D7),
                                      11,
                                      LV_TEXT_ALIGN_CENTER);
        lv_obj_t *settings_btn = lv_btn_create(header);
        lv_obj_remove_style_all(settings_btn);
        lv_obj_set_pos(settings_btn,
                       DISPLAY_AI_HEADER_SETTINGS_HIT_X,
                       DISPLAY_AI_HEADER_SETTINGS_HIT_Y);
        lv_obj_set_size(settings_btn,
                        DISPLAY_AI_HEADER_SETTINGS_HIT_WIDTH,
                        DISPLAY_AI_HEADER_SETTINGS_HIT_HEIGHT);
        lv_obj_set_style_bg_opa(settings_btn, LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(settings_btn, display_ai_settings_btn_cb, LV_EVENT_CLICKED, NULL);
    }

    display_create_ai_signal(header);
    return header;
}

static lv_obj_t *display_create_ai_setting_button(lv_obj_t *parent,
                                                  lv_coord_t x,
                                                  lv_coord_t y,
                                                  lv_coord_t width,
                                                  lv_coord_t height,
                                                  const char *text,
                                                  display_ai_setting_action_t action)
{
    lv_obj_t *btn = display_create_figma_button(parent,
                                                x,
                                                y,
                                                width,
                                                height,
                                                lv_color_hex(0xEAF4FB),
                                                lv_color_hex(0xD2E1EC),
                                                text,
                                                lv_color_hex(0x11233C),
                                                16,
                                                NULL);
    lv_obj_set_style_radius(btn, 7, 0);
    lv_obj_add_event_cb(btn,
                        display_ai_settings_action_btn_cb,
                        LV_EVENT_CLICKED,
                        (void *)(uintptr_t)action);
    return btn;
}

static lv_obj_t *display_create_settings_row(lv_obj_t *parent,
                                                      lv_coord_t y,
                                                      const char *text,
                                                      lv_event_cb_t cb)
{
    return display_create_figma_button(parent,
                                       12,
                                       y,
                                       296,
                                       34,
                                       lv_color_hex(0xFFFFFF),
                                       lv_color_hex(0xD5E0EB),
                                       text,
                                       lv_color_hex(0x10243E),
                                       16,
                                       cb);
}

static lv_obj_t *display_create_device_dot(lv_obj_t *parent,
                                                   lv_coord_t x,
                                                   lv_coord_t y,
                                                   lv_color_t color)
{
    lv_obj_t *dot = lv_obj_create(parent);

    lv_obj_remove_style_all(dot);
    lv_obj_set_pos(dot, x, y);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, 4, 0);
    lv_obj_set_style_bg_color(dot, color, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return dot;
}

static uint8_t display_ai_avatar_normalize(uint8_t avatar)
{
    return avatar < DISPLAY_AI_AVATAR_COUNT ? avatar : DISPLAY_AI_AVATAR_BUDDY;
}

static const char *display_ai_avatar_name(uint8_t avatar)
{
    return display_ai_avatar_normalize(avatar) == DISPLAY_AI_AVATAR_SPROUT ? "小芽" : "小云";
}

static ai_chat_avatar_state_t display_ai_avatar_visual_state(const display_status_t *status,
                                                             const display_ai_chat_message_t *latest_message)
{
    if (status == NULL || display_ai_chat_should_show_new_chat_button(status)) {
        return AI_CHAT_AVATAR_STATE_RESTING;
    }
    if (status->ai_chat_state == DISPLAY_AI_CHAT_STATE_ERROR) {
        return AI_CHAT_AVATAR_STATE_ERROR;
    }
    if (status->ai_chat_cloud_speaking) {
        return AI_CHAT_AVATAR_STATE_SPEAKING;
    }
    if (latest_message != NULL && latest_message->text[0] != '\0') {
        if (latest_message->caption_type == DISPLAY_AI_CHAT_CAPTION_TYPE_TTS) {
            return AI_CHAT_AVATAR_STATE_IDLE;
        }
        if (latest_message->caption_type == DISPLAY_AI_CHAT_CAPTION_TYPE_ASR) {
            return AI_CHAT_AVATAR_STATE_LISTENING;
        }
    }
    if (status->ai_chat_listening) {
        return AI_CHAT_AVATAR_STATE_LISTENING;
    }
    if (status->ai_chat_state == DISPLAY_AI_CHAT_STATE_STARTING ||
        status->ai_chat_state == DISPLAY_AI_CHAT_STATE_TOKEN ||
        status->ai_chat_state == DISPLAY_AI_CHAT_STATE_CONNECTING ||
        status->ai_chat_state == DISPLAY_AI_CHAT_STATE_CONNECTED ||
        status->ai_chat_state == DISPLAY_AI_CHAT_STATE_STARTING_SESSION) {
        return AI_CHAT_AVATAR_STATE_THINKING;
    }
    if (status->ai_chat_active) {
        return AI_CHAT_AVATAR_STATE_IDLE;
    }
    return AI_CHAT_AVATAR_STATE_RESTING;
}

static void display_update_ai_avatar(const display_status_t *status,
                                     const display_ai_chat_message_t *latest_message)
{
    uint8_t avatar = status != NULL ? display_ai_avatar_normalize(status->ai_chat_avatar) : DISPLAY_AI_AVATAR_BUDDY;
    ai_chat_avatar_state_t state = display_ai_avatar_visual_state(status, latest_message);

    if (s_ai_avatar_img != NULL &&
        (s_ai_avatar_last_variant != avatar || s_ai_avatar_last_state != state)) {
        lv_img_set_src(s_ai_avatar_img, ai_chat_avatar_asset_get(avatar, state));
        s_ai_avatar_last_variant = avatar;
        s_ai_avatar_last_state = state;
    }
}

static lv_obj_t *display_create_ai_avatar_choice_button(lv_obj_t *parent,
                                                        lv_coord_t x,
                                                        lv_coord_t y,
                                                        uint8_t avatar,
                                                        display_ai_setting_action_t action)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, 76, 32);
    lv_obj_set_style_radius(btn, 7, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xF7FBFE), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xD2E1EC), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xDDF5E9), LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, display_ai_settings_action_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)action);

    uint8_t normalized = display_ai_avatar_normalize(avatar);
    s_ai_settings_avatar_buttons[normalized] = btn;

    lv_obj_t *label = display_create_ai_static_text(btn,
                                                    display_ai_avatar_name(normalized),
                                                    0,
                                                    7,
                                                    76,
                                                    lv_color_hex(0x11233C),
                                                    12,
                                                    LV_TEXT_ALIGN_CENTER);
    if (label != NULL) {
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        s_ai_settings_avatar_labels[normalized] = label;
    }

    return btn;
}

static void display_update_ai_avatar_choice_buttons(uint8_t avatar)
{
    uint8_t active_avatar = display_ai_avatar_normalize(avatar);

    for (uint8_t index = 0; index < DISPLAY_AI_AVATAR_COUNT; ++index) {
        bool selected = index == active_avatar;
        if (s_ai_settings_avatar_buttons[index] != NULL) {
            lv_obj_set_style_bg_color(s_ai_settings_avatar_buttons[index],
                                      selected ? lv_color_hex(0xE5FAF0) : lv_color_hex(0xF7FBFE),
                                      0);
            lv_obj_set_style_border_color(s_ai_settings_avatar_buttons[index],
                                          selected ? lv_color_hex(0x23C17D) : lv_color_hex(0xD2E1EC),
                                          0);
            lv_obj_set_style_border_width(s_ai_settings_avatar_buttons[index], selected ? 2 : 1, 0);
        }
        if (s_ai_settings_avatar_labels[index] != NULL) {
            display_text_set_color(s_ai_settings_avatar_labels[index],
                                   selected ? lv_color_hex(0x0D8A59) : lv_color_hex(0x11233C),
                                   0);
        }
    }
}

static void display_create_device_status_row(lv_obj_t *parent,
                                                     lv_coord_t y,
                                                     const char *label,
                                                     const char *value,
                                                     lv_color_t dot_color,
                                                     lv_obj_t **dot,
                                                     lv_obj_t **value_label)
{
    if (dot != NULL) {
        *dot = display_create_device_dot(parent, 8, y + 4, dot_color);
    }
    display_create_figma_text(parent,
                              label,
                              21,
                              y + 1,
                              64,
                              lv_color_hex(0x64758A),
                              12,
                              LV_TEXT_ALIGN_LEFT);
    if (value_label != NULL) {
        *value_label = display_create_figma_text(parent,
                                                 value,
                                                 88,
                                                 y + 1,
                                                 46,
                                                 lv_color_hex(0x10243E),
                                                 12,
                                                 LV_TEXT_ALIGN_RIGHT);
    }
}

static lv_obj_t *display_create_device_volume_button(lv_obj_t *parent,
                                                              lv_coord_t x,
                                                              const char *text,
                                                              bool mute,
                                                              display_device_volume_action_t action,
                                                              lv_obj_t **text_label)
{
    lv_obj_t *btn = lv_btn_create(parent);

    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, 27);
    lv_obj_set_size(btn, 38, 24);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, mute ? lv_color_hex(0xFFF2D8) : lv_color_hex(0xF7FBFF), 0);
    lv_obj_set_style_bg_color(btn, mute ? lv_color_hex(0xFFE3B3) : lv_color_hex(0xE7F1FB), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, mute ? lv_color_hex(0xFFD59D) : lv_color_hex(0xD5E0EB), 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn,
                        display_device_volume_btn_cb,
                        LV_EVENT_CLICKED,
                        (void *)(uintptr_t)action);

    if (text_label != NULL) {
        lv_obj_t *label = display_create_figma_text(btn,
                                                    text,
                                                    0,
                                                    mute ? 3 : 5,
                                                    38,
                                                    mute ? lv_color_hex(0x9A5A00) : lv_color_hex(0x10243E),
                                                    12,
                                                    LV_TEXT_ALIGN_CENTER);
        *text_label = label;
    } else {
        display_create_figma_text(btn,
                                  text,
                                  0,
                                  mute ? 3 : 5,
                                  38,
                                  mute ? lv_color_hex(0x9A5A00) : lv_color_hex(0x10243E),
                                  12,
                                  LV_TEXT_ALIGN_CENTER);
    }
    return btn;
}

static void display_create_device_volume_card(lv_obj_t *parent,
                                                      lv_coord_t y,
                                                      const char *title,
                                                      const char *value,
                                                      display_device_volume_action_t down_action,
                                                      display_device_volume_action_t up_action,
                                                      display_device_volume_action_t mute_action,
                                                       lv_obj_t **value_label,
                                                       lv_obj_t **mute_label)
{
    lv_obj_t *card = display_create_figma_box(parent,
                                              8,
                                              y,
                                              142,
                                              67,
                                              lv_color_hex(0xFFFFFF),
                                              lv_color_hex(0xD5E0EB),
                                              8);
    lv_obj_t *pill = NULL;

    display_create_figma_text(card,
                              title,
                              8,
                              6,
                              90,
                              lv_color_hex(0x10243E),
                              14,
                              LV_TEXT_ALIGN_LEFT);
    pill = display_create_figma_box(card,
                                    102,
                                    5,
                                    32,
                                    18,
                                    lv_color_hex(0xE7F1FB),
                                    lv_color_hex(0xE7F1FB),
                                    6);
    if (value_label != NULL) {
        *value_label = display_create_figma_text(pill,
                                                 value,
                                                 0,
                                                 3,
                                                 32,
                                                 lv_color_hex(0x1768B7),
                                                 12,
                                                 LV_TEXT_ALIGN_CENTER);
    }

    (void)display_create_device_volume_button(card, 11, "-10", false, down_action, NULL);
    (void)display_create_device_volume_button(card, 52, "+10", false, up_action, NULL);
    (void)display_create_device_volume_button(card, 93, "禁音", true, mute_action, mute_label);
}

static bool display_update_device_qr_object(lv_obj_t *qrcode, const char *payload)
{
#if LV_USE_QRCODE
    if (qrcode == NULL || payload == NULL) {
        return false;
    }

    return lv_qrcode_update(qrcode, payload, strlen(payload)) == LV_RES_OK;
#else
    (void)qrcode;
    (void)payload;
    return false;
#endif
}

static void display_create_device_qr_overlay(lv_obj_t *parent);

static void display_hide_device_qr_overlay(void)
{
    if (s_device_qr_overlay != NULL) {
        lv_obj_add_flag(s_device_qr_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void display_show_device_qr_overlay(void)
{
    if (s_device_qr_overlay == NULL) {
        display_create_device_qr_overlay(s_main_page);
    }
    if (s_device_qr_overlay == NULL) {
        return;
    }

#if LV_USE_QRCODE
    if (s_device_qr_overlay_qrcode != NULL && s_device_qr_payload[0] != '\0') {
        if (!display_update_device_qr_object(s_device_qr_overlay_qrcode, s_device_qr_payload)) {
            ESP_LOGW(TAG, "device qr fullscreen update failed");
        }
    }
#endif

    lv_obj_clear_flag(s_device_qr_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_device_qr_overlay);
}

static void display_device_qr_tap_cb(lv_event_t *event)
{
    (void)event;

    if (s_device_qr_overlay == NULL || lv_obj_has_flag(s_device_qr_overlay, LV_OBJ_FLAG_HIDDEN)) {
        display_show_device_qr_overlay();
    } else {
        display_hide_device_qr_overlay();
    }
}

static void display_create_device_qr(lv_obj_t *parent)
{
    if (parent == NULL) {
        return;
    }

#if LV_USE_QRCODE
    s_device_qrcode = lv_qrcode_create(parent,
                                       DISPLAY_DEVICE_QR_SIZE,
                                       lv_color_hex(0x111111),
                                       lv_color_hex(0xFFFFFF));
    lv_obj_center(s_device_qrcode);
    lv_obj_clear_flag(s_device_qrcode, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
#else
    display_create_figma_text(parent,
                              "QR",
                              0,
                              44,
                              112,
                              lv_color_hex(0x10243E),
                              18,
                              LV_TEXT_ALIGN_CENTER);
#endif
}

static void display_create_device_qr_overlay(lv_obj_t *parent)
{
    if (parent == NULL) {
        return;
    }

    s_device_qr_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_device_qr_overlay);
    lv_obj_set_pos(s_device_qr_overlay, 0, 0);
    lv_obj_set_size(s_device_qr_overlay, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_color(s_device_qr_overlay, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_device_qr_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_device_qr_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_device_qr_overlay, 0, 0);
    lv_obj_clear_flag(s_device_qr_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_device_qr_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_device_qr_overlay, display_device_qr_tap_cb, LV_EVENT_CLICKED, NULL);

#if LV_USE_QRCODE
    s_device_qr_overlay_qrcode = lv_qrcode_create(s_device_qr_overlay,
                                                  DISPLAY_DEVICE_QR_FULLSCREEN_SIZE,
                                                  lv_color_hex(0x111111),
                                                  lv_color_hex(0xFFFFFF));
    lv_obj_center(s_device_qr_overlay_qrcode);
    lv_obj_clear_flag(s_device_qr_overlay_qrcode, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
#else
    display_create_figma_text(s_device_qr_overlay,
                              "QR",
                              0,
                              102,
                              DISPLAY_DRIVER_WIDTH,
                              lv_color_hex(0x10243E),
                              18,
                              LV_TEXT_ALIGN_CENTER);
#endif
}

static lv_obj_t *display_create_info_row(lv_obj_t *parent,
                                                 lv_coord_t y,
                                                 const char *label,
                                                 const char *value,
                                                 lv_color_t value_color,
                                                 lv_obj_t **value_label)
{
    lv_obj_t *row = display_create_figma_box(parent,
                                             8,
                                             y,
                                             304,
                                             34,
                                             lv_color_hex(0xFFFFFF),
                                             lv_color_hex(0xD5E0EB),
                                             6);

    display_create_figma_text(row,
                              label,
                              9,
                              9,
                              86,
                              lv_color_hex(0x64758A),
                              12,
                              LV_TEXT_ALIGN_LEFT);
    lv_obj_t *value_obj = display_create_figma_text(row,
                                                    value,
                                                    103,
                                                    9,
                                                    190,
                                                    value_color,
                                                    12,
                                                    LV_TEXT_ALIGN_LEFT);
    if (value_label != NULL) {
        *value_label = value_obj;
    }
    return row;
}

static lv_obj_t *display_create_check_row(lv_obj_t *parent,
                                             lv_coord_t y,
                                             const char *label,
                                             const char *value,
                                             lv_color_t fill,
                                             lv_color_t value_color,
                                             lv_obj_t **value_label)
{
    lv_obj_t *row = display_create_figma_box(parent,
                                             8,
                                             y,
                                             304,
                                             24,
                                             fill,
                                             lv_color_hex(0xD5E0EB),
                                             5);

    display_create_figma_text(row,
                              label,
                              9,
                              4,
                              126,
                              lv_color_hex(0x10243E),
                              12,
                              LV_TEXT_ALIGN_LEFT);
    lv_obj_t *value_obj = display_create_figma_text(row,
                                                    value,
                                                    143,
                                                    4,
                                                    152,
                                                    value_color,
                                                    12,
                                                    LV_TEXT_ALIGN_RIGHT);
    if (value_label != NULL) {
        *value_label = value_obj;
    }
    return row;
}

static void display_create_call_qr(lv_obj_t *parent,
                                   lv_coord_t x,
                                   lv_coord_t y,
                                   lv_coord_t size)
{
    lv_obj_t *qr_box = display_create_figma_box(parent,
                                                x,
                                                y,
                                                size,
                                                size,
                                                lv_color_hex(0xFFFFFF),
                                                lv_color_hex(0xD6E4EF),
                                                4);
#if LV_USE_QRCODE
    s_call_qrcode = lv_qrcode_create(qr_box,
                                     size - 12,
                                     lv_color_hex(0x111111),
                                     lv_color_hex(0xFFFFFF));
    lv_obj_center(s_call_qrcode);
    lv_obj_clear_flag(s_call_qrcode, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
#else
    display_create_figma_text(qr_box,
                              "QR",
                              0,
                              (size - 24) / 2,
                              size,
                              lv_color_hex(0x10243E),
                              18,
                              LV_TEXT_ALIGN_CENTER);
#endif
}

static void display_create_wechat_qr(lv_obj_t *parent,
                                     lv_coord_t x,
                                     lv_coord_t y,
                                     lv_coord_t size)
{
    lv_obj_t *qr_box = display_create_figma_box(parent,
                                                x,
                                                y,
                                                size,
                                                size,
                                                lv_color_hex(0xFFFFFF),
                                                lv_color_hex(0xD6E4EF),
                                                4);
#if LV_USE_QRCODE
    s_wechat_qrcode = lv_qrcode_create(qr_box,
                                       size - 12,
                                       lv_color_hex(0x111111),
                                       lv_color_hex(0xFFFFFF));
    lv_obj_center(s_wechat_qrcode);
    lv_obj_clear_flag(s_wechat_qrcode, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
#else
    display_create_figma_text(qr_box,
                              "QR",
                              0,
                              (size - 24) / 2,
                              size,
                              lv_color_hex(0x10243E),
                              18,
                              LV_TEXT_ALIGN_CENTER);
#endif
}

static lv_obj_t *display_create_call_menu_button(lv_obj_t *parent,
                                                 lv_coord_t x,
                                                 lv_coord_t y,
                                                 lv_coord_t width,
                                                 lv_coord_t height,
                                                 const char *line1,
                                                 const char *line2,
                                                 bool primary,
                                                 lv_event_cb_t cb)
{
    lv_color_t fill = primary ? lv_color_hex(0x21C783) : lv_color_hex(0xFFFFFF);
    lv_color_t stroke = primary ? lv_color_hex(0x21C783) : lv_color_hex(0xD6E4EF);
    lv_color_t text_color = primary ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x10233B);
    lv_obj_t *btn = lv_btn_create(parent);

    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, width, height);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, fill, 0);
    lv_obj_set_style_bg_color(btn, lv_color_darken(fill, 18), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, stroke, 0);
    lv_obj_set_style_shadow_width(btn, 8, 0);
    lv_obj_set_style_shadow_ofs_y(btn, 2, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_10, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }

    if (line2 != NULL && line2[0] != '\0') {
        display_create_figma_text(btn, line1, 0, 23, width, text_color, 16, LV_TEXT_ALIGN_CENTER);
        display_create_figma_text(btn, line2, 0, 48, width, text_color, 16, LV_TEXT_ALIGN_CENTER);
    } else {
        display_create_figma_text(btn, line1, 0, (height - 20) / 2, width, text_color, 16, LV_TEXT_ALIGN_CENTER);
    }

    return btn;
}

static lv_obj_t *display_create_call_add_field_row(lv_obj_t *parent,
                                                   lv_coord_t y,
                                                   display_call_add_field_t field)
{
    const char *label = display_call_add_field_title(field);
    const char *value = display_call_add_field_buffer(field);
    lv_obj_t *row = display_create_figma_box(parent,
                                             8,
                                             y,
                                             304,
                                             42,
                                             lv_color_hex(0xFFFFFF),
                                             lv_color_hex(0xD6E4EF),
                                             7);

    display_create_figma_text(row,
                              label,
                              11,
                              13,
                              70,
                              lv_color_hex(0x65768A),
                              12,
                              LV_TEXT_ALIGN_LEFT);

    lv_obj_t *value_label = display_create_figma_text(row,
                                                      value[0] != '\0' ?
                                                          value : display_call_add_field_placeholder(field),
                                                      85,
                                                      13,
                                                      176,
                                                      value[0] != '\0' ?
                                                          lv_color_hex(0x10233B) : lv_color_hex(0x8AA0B5),
                                                      12,
                                                      LV_TEXT_ALIGN_LEFT);
    display_create_figma_text(row,
                              ">",
                              270,
                              13,
                              20,
                              lv_color_hex(0x64758A),
                              14,
                              LV_TEXT_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row,
                        display_call_add_field_btn_cb,
                        LV_EVENT_CLICKED,
                        (void *)(uintptr_t)field);

    return value_label;
}

static lv_obj_t *display_create_wechat_add_field_row(lv_obj_t *parent, lv_coord_t y)
{
    lv_obj_t *row = display_create_figma_box(parent,
                                             8,
                                             y,
                                             304,
                                             42,
                                             lv_color_hex(0xFFFFFF),
                                             lv_color_hex(0xD6E4EF),
                                             7);

    display_create_figma_text(row,
                              "OpenID",
                              11,
                              13,
                              70,
                              lv_color_hex(0x65768A),
                              12,
                              LV_TEXT_ALIGN_LEFT);

    lv_obj_t *value_label = display_create_figma_text(row,
                                                      s_wechat_add_open_id[0] != '\0' ?
                                                          s_wechat_add_open_id : "28位微信Open ID",
                                                      85,
                                                      13,
                                                      176,
                                                      s_wechat_add_open_id[0] != '\0' ?
                                                          lv_color_hex(0x10233B) : lv_color_hex(0x8AA0B5),
                                                      12,
                                                      LV_TEXT_ALIGN_LEFT);
    display_create_figma_text(row,
                              ">",
                              270,
                              13,
                              20,
                              lv_color_hex(0x64758A),
                              14,
                              LV_TEXT_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, display_wechat_add_field_btn_cb, LV_EVENT_CLICKED, NULL);

    return value_label;
}

static void display_create_call_contact_row(lv_obj_t *parent, uint8_t index, lv_coord_t y)
{
    lv_color_t button_fill = lv_color_hex(0xDDF8EA);
    lv_color_t button_text = lv_color_hex(0x1FC985);
    lv_obj_t *call_btn = NULL;
    lv_obj_t *row = display_create_figma_box(parent,
                                             8,
                                             y,
                                             304,
                                             44,
                                             lv_color_hex(0xFFFFFF),
                                             lv_color_hex(0xD6E4EF),
                                             7);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row,
                        display_call_contact_delete_btn_cb,
                        LV_EVENT_LONG_PRESSED,
                        (void *)(uintptr_t)index);

    display_create_figma_text(row,
                              s_call_contacts[index].device_id,
                              11,
                              6,
                              220,
                              lv_color_hex(0x10233B),
                              12,
                              LV_TEXT_ALIGN_LEFT);
    display_create_figma_text(row,
                              s_call_contacts[index].last_time,
                              11,
                              25,
                              190,
                              lv_color_hex(0x65768A),
                              10,
                              LV_TEXT_ALIGN_LEFT);

    call_btn = display_create_figma_button(row,
                                           245,
                                           9,
                                           46,
                                           26,
                                           button_fill,
                                           button_fill,
                                           "呼叫",
                                           button_text,
                                           10,
                                           NULL);
    lv_obj_set_style_radius(call_btn, 7, 0);
    lv_obj_add_event_cb(call_btn,
                        display_call_contact_call_btn_cb,
                        LV_EVENT_CLICKED,
                        (void *)(uintptr_t)index);
}

static void display_create_wechat_contact_row(lv_obj_t *parent, uint8_t index, lv_coord_t y)
{
    lv_color_t button_fill = lv_color_hex(0xDDF8EA);
    lv_color_t button_text = lv_color_hex(0x1FC985);
    lv_obj_t *open_id_label = NULL;
    lv_obj_t *row = display_create_figma_box(parent,
                                             8,
                                             y,
                                             304,
                                             44,
                                             lv_color_hex(0xFFFFFF),
                                             lv_color_hex(0xD6E4EF),
                                             7);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row,
                        display_wechat_contact_delete_cb,
                        LV_EVENT_LONG_PRESSED,
                        (void *)(uintptr_t)index);

    open_id_label = display_create_figma_text(row,
                                              "",
                                              11,
                                              13,
                                              206,
                                              lv_color_hex(0x10233B),
                                              12,
                                              LV_TEXT_ALIGN_LEFT);
    lv_obj_t *btn = display_create_figma_button(row,
                                                226,
                                                5,
                                                66,
                                                34,
                                                button_fill,
                                                button_fill,
                                                "呼叫",
                                                button_text,
                                                12,
                                                NULL);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_event_cb(btn,
                        display_wechat_contact_call_btn_cb,
                        LV_EVENT_CLICKED,
                        (void *)(uintptr_t)index);
    lv_obj_add_event_cb(btn,
                        display_wechat_contact_delete_cb,
                        LV_EVENT_LONG_PRESSED,
                        (void *)(uintptr_t)index);
    s_wechat_contact_rows[index] = row;
    s_wechat_contact_open_id_labels[index] = open_id_label;
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *display_create_call_duration_row(lv_obj_t *parent,
                                                  lv_coord_t y,
                                                  lv_obj_t **value_label)
{
    lv_obj_t *row = display_create_figma_box(parent,
                                             8,
                                             y,
                                             304,
                                             32,
                                             lv_color_hex(0xFFFFFF),
                                             lv_color_hex(0xD6E4EF),
                                             7);

    display_create_figma_text(row,
                              "通话时长",
                              16,
                              8,
                              180,
                              lv_color_hex(0x10233B),
                              12,
                              LV_TEXT_ALIGN_LEFT);
    lv_obj_t *value_obj = lv_label_create(row);
    lv_obj_set_pos(value_obj, 184, 7);
    lv_obj_set_width(value_obj, 104);
    lv_label_set_long_mode(value_obj, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(value_obj, lv_color_hex(0x20C982), 0);
    lv_obj_set_style_text_align(value_obj, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(value_obj, display_ai_chat_font(), 0);
    lv_label_set_text(value_obj, "00:00");
    if (value_label != NULL) {
        *value_label = value_obj;
    }
    return row;
}

static void display_create_call_scan_info_overlay(lv_obj_t *parent)
{
    lv_obj_t *card = NULL;
    lv_obj_t *field = NULL;
    lv_obj_t *format_label = NULL;
    lv_obj_t *close_btn = NULL;

    s_call_scan_info_overlay = display_create_figma_box(parent,
                                                        0,
                                                        0,
                                                        320,
                                                        240,
                                                        lv_color_hex(0x10233B),
                                                        lv_color_hex(0x10233B),
                                                        0);
    lv_obj_set_style_bg_opa(s_call_scan_info_overlay, LV_OPA_20, 0);
    lv_obj_set_style_border_width(s_call_scan_info_overlay, 0, 0);
    lv_obj_add_flag(s_call_scan_info_overlay, LV_OBJ_FLAG_HIDDEN);

    card = display_create_figma_box(s_call_scan_info_overlay,
                                    30,
                                    52,
                                    260,
                                    140,
                                    lv_color_hex(0xFFFFFF),
                                    lv_color_hex(0xD6E4EF),
                                    9);
    lv_obj_set_style_shadow_width(card, 14, 0);
    lv_obj_set_style_shadow_ofs_y(card, 5, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);

    display_create_figma_text(card,
                              "QR Format",
                              12,
                              12,
                              236,
                              lv_color_hex(0x10233B),
                              16,
                              LV_TEXT_ALIGN_CENTER);

    field = display_create_figma_box(card,
                                     12,
                                     42,
                                     236,
                                     66,
                                     lv_color_hex(0xF4F9FD),
                                     lv_color_hex(0xD6E4EF),
                                     6);
    format_label = display_create_figma_text(field,
                                             "{\n"
                                             "  \"device_id\": \"\",\n"
                                             "  \"device_secret_key\": \"\"\n"
                                             "}",
                                             9,
                                             8,
                                             216,
                                             lv_color_hex(0x10233B),
                                             12,
                                             LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(format_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(format_label, 56);

    close_btn = display_create_figma_button(card,
                                            92,
                                            110,
                                            76,
                                            24,
                                            lv_color_hex(0xE9F5FF),
                                            lv_color_hex(0x2F82D7),
                                            "Close",
                                            lv_color_hex(0x2F82D7),
                                            12,
                                            display_call_scan_info_close_btn_cb);
    lv_obj_set_style_radius(close_btn, 7, 0);
}

static void display_create_wechat_scan_info_overlay(lv_obj_t *parent)
{
    lv_obj_t *card = NULL;
    lv_obj_t *field = NULL;
    lv_obj_t *format_label = NULL;
    lv_obj_t *close_btn = NULL;

    s_wechat_scan_info_overlay = display_create_figma_box(parent,
                                                          0,
                                                          0,
                                                          320,
                                                          240,
                                                          lv_color_hex(0x10233B),
                                                          lv_color_hex(0x10233B),
                                                          0);
    lv_obj_set_style_bg_opa(s_wechat_scan_info_overlay, LV_OPA_20, 0);
    lv_obj_set_style_border_width(s_wechat_scan_info_overlay, 0, 0);
    lv_obj_add_flag(s_wechat_scan_info_overlay, LV_OBJ_FLAG_HIDDEN);

    card = display_create_figma_box(s_wechat_scan_info_overlay,
                                    30,
                                    52,
                                    260,
                                    140,
                                    lv_color_hex(0xFFFFFF),
                                    lv_color_hex(0xD6E4EF),
                                    9);
    lv_obj_set_style_shadow_width(card, 14, 0);
    lv_obj_set_style_shadow_ofs_y(card, 5, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);

    display_create_figma_text(card,
                              "扫码格式",
                              12,
                              12,
                              236,
                              lv_color_hex(0x10233B),
                              16,
                              LV_TEXT_ALIGN_CENTER);

    field = display_create_figma_box(card,
                                     12,
                                     42,
                                     236,
                                     66,
                                     lv_color_hex(0xF4F9FD),
                                     lv_color_hex(0xD6E4EF),
                                     6);
    format_label = display_create_ai_text(field,
                                          "28-character WeChat Open ID",
                                          9,
                                          24,
                                          216,
                                          lv_color_hex(0x10233B),
                                          LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(format_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(format_label, 44);

    close_btn = display_create_figma_button(card,
                                            92,
                                            110,
                                            76,
                                            24,
                                            lv_color_hex(0xE9F5FF),
                                            lv_color_hex(0x2F82D7),
                                            "关闭",
                                            lv_color_hex(0x2F82D7),
                                            12,
                                            display_wechat_scan_info_close_btn_cb);
    lv_obj_set_style_radius(close_btn, 7, 0);
}

static lv_obj_t *display_create_call_volume_row(lv_obj_t *parent,
                                                lv_coord_t y,
                                                const char *label,
                                                const char *value,
                                                display_call_volume_action_t down_action,
                                                display_call_volume_action_t up_action,
                                                lv_obj_t **value_label,
                                                lv_event_cb_t cb)
{
    lv_obj_t *row = display_create_figma_box(parent,
                                             8,
                                             y,
                                             304,
                                             34,
                                             lv_color_hex(0xFFFFFF),
                                             lv_color_hex(0xD6E4EF),
                                             7);
    lv_obj_t *down_btn = NULL;
    lv_obj_t *up_btn = NULL;

    display_create_figma_text(row,
                              label,
                              12,
                              9,
                              132,
                              lv_color_hex(0x10233B),
                              12,
                              LV_TEXT_ALIGN_LEFT);
    down_btn = display_create_figma_button(row,
                                           152,
                                           4,
                                           32,
                                           26,
                                           lv_color_hex(0xEDF5FB),
                                           lv_color_hex(0xD6E4EF),
                                           "-",
                                           lv_color_hex(0x10233B),
                                           16,
                                           NULL);
    lv_obj_set_style_radius(down_btn, 7, 0);
    lv_obj_add_event_cb(down_btn,
                        cb,
                        LV_EVENT_CLICKED,
                        (void *)(uintptr_t)down_action);
    lv_obj_t *pill = display_create_figma_box(row,
                                              192,
                                              4,
                                              34,
                                              26,
                                              lv_color_hex(0xEDF8F2),
                                              lv_color_hex(0xEDF8F2),
                                              6);
    if (value_label != NULL) {
        *value_label = display_create_figma_text(pill,
                                                 value,
                                                 0,
                                                 6,
                                                 34,
                                                 lv_color_hex(0x20C982),
                                                 12,
                                                 LV_TEXT_ALIGN_CENTER);
    }
    up_btn = display_create_figma_button(row,
                                         234,
                                         4,
                                         32,
                                         26,
                                         lv_color_hex(0xEDF5FB),
                                         lv_color_hex(0xD6E4EF),
                                         "+",
                                         lv_color_hex(0x10233B),
                                         16,
                                         NULL);
    lv_obj_set_style_radius(up_btn, 7, 0);
    lv_obj_add_event_cb(up_btn,
                        cb,
                        LV_EVENT_CLICKED,
                        (void *)(uintptr_t)up_action);
    return row;
}

static lv_obj_t *display_create_card(lv_obj_t *parent,
                                              lv_coord_t x,
                                              lv_coord_t y,
                                              lv_coord_t width,
                                              lv_coord_t height)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, width, height);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2A4B58), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x112630), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static void display_style_wifi_list_button(lv_obj_t *btn)
{
    lv_obj_set_height(btn, 46);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xD5E0EB), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xE7F1FB), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_left(btn, 14, 0);
    lv_obj_set_style_pad_right(btn, 12, 0);
    lv_obj_set_style_pad_top(btn, 0, 0);
    lv_obj_set_style_pad_bottom(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
}

static bool display_page_is_visible(lv_obj_t *page)
{
    return page != NULL && !lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN);
}

static bool display_wifi_scan_result_equals(const display_wifi_scan_result_t *lhs,
                                                     const display_wifi_scan_result_t *rhs)
{
    return lhs->rssi == rhs->rssi &&
           lhs->secure == rhs->secure &&
           lhs->channel == rhs->channel &&
           strcmp(lhs->ssid, rhs->ssid) == 0;
}

static bool display_wifi_scan_equals(const display_status_t *lhs,
                                               const display_status_t *rhs)
{
    if (lhs->wifi_scan_in_progress != rhs->wifi_scan_in_progress) {
        return false;
    }
    if (lhs->wifi_scan_count != rhs->wifi_scan_count) {
        return false;
    }

    for (uint16_t index = 0; index < lhs->wifi_scan_count && index < DISPLAY_WIFI_SCAN_MAX; ++index) {
        if (!display_wifi_scan_result_equals(&lhs->wifi_scan_results[index], &rhs->wifi_scan_results[index])) {
            return false;
        }
    }
    return true;
}

static lv_color_t display_wifi_signal_color(int rssi)
{
    (void)rssi;
    return lv_color_hex(0xF59E0B);
}

static void display_add_wifi_list_item(const display_status_t *status, uint16_t index)
{
    lv_obj_t *btn = NULL;
    lv_obj_t *ssid_label = NULL;
    lv_obj_t *rssi_label = NULL;
    char rssi_text[24] = {0};
    const display_wifi_scan_result_t *result = &status->wifi_scan_results[index];
    bool connected = status->network_connected && strcmp(status->network_ssid, result->ssid) == 0;

    if (s_wifi_list == NULL || index >= DISPLAY_WIFI_SCAN_MAX) {
        return;
    }

    btn = s_wifi_list_buttons[index];
    ssid_label = s_wifi_list_ssid_labels[index];
    rssi_label = s_wifi_list_rssi_labels[index];

    if (btn == NULL || ssid_label == NULL || rssi_label == NULL) {
        return;
    }

    if (result->rssi > -120) {
        snprintf(rssi_text, sizeof(rssi_text), "%d dBm", result->rssi);
    } else {
        strlcpy(rssi_text, "--", sizeof(rssi_text));
    }

    lv_obj_set_style_bg_color(btn, connected ? lv_color_hex(0xDDF7EC) : lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(btn, connected ? lv_color_hex(0x20BF7A) : lv_color_hex(0xD5E0EB), 0);
    lv_obj_set_style_bg_color(btn, connected ? lv_color_hex(0xC9F0DF) : lv_color_hex(0xE7F1FB), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, connected ? lv_color_hex(0x0D8A59) : lv_color_hex(0x1768B7), LV_STATE_PRESSED);
    display_text_set_color(ssid_label, connected ? lv_color_hex(0x0D8A59) : lv_color_hex(0x10243E), 0);
    display_text_set_color(rssi_label, display_wifi_signal_color(result->rssi), 0);
    display_text_set(ssid_label, result->ssid);
    display_text_set(rssi_label, rssi_text);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_HIDDEN);
}

static void display_request_wifi_scan(void)
{
    if (s_actions.on_wifi_scan == NULL) {
        return;
    }

    esp_err_t ret = s_actions.on_wifi_scan(s_actions.ctx);
    if (ret == ESP_OK) {
        s_last_wifi_scan_request_us = esp_timer_get_time();
        if (s_wifi_scan_state_label != NULL) {
            display_text_set_color(s_wifi_scan_state_label, lv_color_hex(0x1768B7), 0);
            display_text_set(s_wifi_scan_state_label, "扫描中");
        }
        if (s_wifi_scan_count_label != NULL) {
            lv_obj_add_flag(s_wifi_scan_count_label, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        if (s_wifi_scan_state_label != NULL) {
            display_text_set_color(s_wifi_scan_state_label, lv_color_hex(0xE45656), 0);
            display_text_set(s_wifi_scan_state_label, "扫描失败");
        }
        if (s_wifi_scan_count_label != NULL) {
            lv_obj_add_flag(s_wifi_scan_count_label, LV_OBJ_FLAG_HIDDEN);
        }
        s_last_wifi_scan_request_us = 0;
    }
}

static esp_err_t display_enter_app(display_app_id_t app_id)
{
    if (s_actions.on_enter_app == NULL) {
        return ESP_OK;
    }

    return s_actions.on_enter_app(app_id, s_actions.ctx);
}

static void display_return_home(void)
{
    if (display_page_is_visible(s_ai_chat_page) ||
        display_page_is_visible(s_ai_chat_settings_page)) {
        display_clear_ai_chat_message_view();
    }

    if (s_actions.on_return_home != NULL) {
        (void)s_actions.on_return_home(s_actions.ctx);
    }
    display_show_home_page();
}

static void display_hide_all_pages(void)
{
    display_hide_device_qr_overlay();

    lv_obj_t *pages[] = {
        s_home_page,
        s_main_page,
        s_call_page,
        s_call_add_page,
        s_call_add_edit_page,
        s_call_scan_page,
        s_call_list_page,
        s_call_active_page,
        s_wechat_page,
        s_wechat_add_page,
        s_wechat_add_edit_page,
        s_wechat_list_page,
        s_wechat_active_page,
        s_uuid_edit_page,
        s_system_page,
        s_wifi_page,
        s_wifi_connect_page,
        s_network_test_page,
        s_tirtc_config_page,
        s_tirtc_config_edit_page,
        s_test_page,
        s_ota_page,
        s_ai_chat_page,
        s_ai_chat_settings_page,
    };

    for (size_t index = 0; index < sizeof(pages) / sizeof(pages[0]); ++index) {
        if (pages[index] != NULL) {
            lv_obj_add_flag(pages[index], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void display_stop_call_scan_if_active(void)
{
    if (!s_call_scan_active) {
        return;
    }

    s_call_scan_active = false;
    if (s_scan_owner == DISPLAY_SCAN_OWNER_TIRTC_CONFIG &&
        s_actions.on_stop_tirtc_config_scan != NULL) {
        esp_err_t ret = s_actions.on_stop_tirtc_config_scan(s_actions.ctx);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "stop tirtc config scan failed: %s", esp_err_to_name(ret));
        }
    } else if (s_scan_owner == DISPLAY_SCAN_OWNER_WECHAT_CONTACT &&
               s_actions.on_stop_wechat_contact_scan != NULL) {
        esp_err_t ret = s_actions.on_stop_wechat_contact_scan(s_actions.ctx);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "stop wechat contact scan failed: %s", esp_err_to_name(ret));
        }
    } else if (s_actions.on_stop_contact_scan != NULL) {
        esp_err_t ret = s_actions.on_stop_contact_scan(s_actions.ctx);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "stop contact scan failed: %s", esp_err_to_name(ret));
        }
    }
}

static void display_exit_call_scan_to_previous(void)
{
    display_stop_call_scan_if_active();
    if (s_scan_owner == DISPLAY_SCAN_OWNER_TIRTC_CONFIG) {
        display_show_tirtc_config_page();
    } else if (s_scan_owner == DISPLAY_SCAN_OWNER_WECHAT_CONTACT) {
        display_show_wechat_add_page();
    } else {
        display_show_call_add_page();
    }
}

static void display_show_page(lv_obj_t *page)
{
    if (page != s_call_scan_page) {
        display_stop_call_scan_if_active();
    }
    display_hide_call_delete_confirm();
    display_hide_wechat_delete_confirm();
    display_hide_all_pages();
    if (page != NULL) {
        lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
    }
    s_wifi_connect_state = DISPLAY_WIFI_CONNECT_STATE_IDLE;
    display_hide_keyboard();
}

static void display_show_home_page(void)
{
    display_update_home_status_bar(&s_last_status);
    display_show_page(s_home_page);
    display_update_binding_prompt(&s_last_status);
}

static void display_show_main_page(void)
{
    display_show_page(s_main_page);
}

static void display_show_call_page(void)
{
    if (s_call_page == NULL) {
        display_build_call_page(lv_scr_act());
    }
    display_update_call_page(&s_last_status);
    display_show_page(s_call_page);
}

static void display_show_call_add_page(void)
{
    if (s_call_add_page == NULL) {
        display_build_call_add_page(lv_scr_act());
    }
    if (s_call_scan_info_overlay != NULL) {
        lv_obj_add_flag(s_call_scan_info_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    display_hide_keyboard();
    display_update_call_add_field_labels();
    display_show_page(s_call_add_page);
}

static void display_show_call_add_edit_page(display_call_add_field_t field)
{
    const char *current_value = NULL;
    size_t max_len = display_call_add_field_max_len(field);

    if (field >= DISPLAY_CALL_ADD_FIELD_COUNT) {
        return;
    }
    if (s_call_add_edit_page == NULL) {
        display_build_call_add_edit_page(lv_scr_act());
    }

    s_call_add_edit_field = field;
    current_value = display_call_add_field_buffer(field);
    display_show_page(s_call_add_edit_page);
    if (s_call_add_edit_hint_label != NULL) {
        display_text_set(s_call_add_edit_hint_label, display_call_add_field_title(field));
    }
    if (s_call_add_edit_ta != NULL) {
        lv_textarea_set_max_length(s_call_add_edit_ta, max_len);
        lv_textarea_set_placeholder_text(s_call_add_edit_ta, display_call_add_field_title(field));
        lv_textarea_set_text(s_call_add_edit_ta, current_value != NULL ? current_value : "");
        lv_textarea_set_cursor_pos(s_call_add_edit_ta,
                                   (uint32_t)strlen(current_value != NULL ? current_value : ""));
    }
    display_update_call_add_edit_feedback(NULL, lv_color_hex(0x0D8A59));
    if (s_call_add_edit_keyboard != NULL && s_call_add_edit_ta != NULL) {
        lv_keyboard_set_textarea(s_call_add_edit_keyboard, s_call_add_edit_ta);
        lv_keyboard_set_mode(s_call_add_edit_keyboard, LV_KEYBOARD_MODE_USER_1);
        lv_obj_clear_flag(s_call_add_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_call_add_edit_keyboard);
        lv_obj_add_state(s_call_add_edit_ta, LV_STATE_FOCUSED);
        lv_event_send(s_call_add_edit_ta, LV_EVENT_FOCUSED, NULL);
    }
}

static void display_show_call_scan_page(void)
{
    if (s_call_scan_page == NULL) {
        display_build_call_scan_page(lv_scr_act());
    }
    if (s_call_scan_img != NULL) {
        lv_obj_add_flag(s_call_scan_img, LV_OBJ_FLAG_HIDDEN);
    }
    s_scan_owner = DISPLAY_SCAN_OWNER_CALL;
    s_call_scan_active = true;
    display_show_page(s_call_scan_page);
}

static void __attribute__((unused)) display_show_tirtc_config_scan_page(void)
{
    if (s_call_scan_page == NULL) {
        display_build_call_scan_page(lv_scr_act());
    }
    if (s_call_scan_img != NULL) {
        lv_obj_add_flag(s_call_scan_img, LV_OBJ_FLAG_HIDDEN);
    }
    s_scan_owner = DISPLAY_SCAN_OWNER_TIRTC_CONFIG;
    s_call_scan_active = true;
    display_show_page(s_call_scan_page);
}

static void display_show_wechat_scan_page(void)
{
    if (s_call_scan_page == NULL) {
        display_build_call_scan_page(lv_scr_act());
    }
    if (s_call_scan_img != NULL) {
        lv_obj_add_flag(s_call_scan_img, LV_OBJ_FLAG_HIDDEN);
    }
    s_scan_owner = DISPLAY_SCAN_OWNER_WECHAT_CONTACT;
    s_call_scan_active = true;
    display_show_page(s_call_scan_page);
}

static void display_show_call_list_page(void)
{
    if (s_call_list_page == NULL) {
        display_build_call_list_page(lv_scr_act());
    }
    display_show_page(s_call_list_page);
}

static void display_show_call_active_page(void)
{
    if (s_call_active_page == NULL) {
        display_build_call_active_page(lv_scr_act());
    }
    display_update_call_active_page(&s_last_status);
    display_show_page(s_call_active_page);
}

static void display_show_wechat_page(void)
{
    if (s_wechat_page == NULL) {
        display_build_wechat_page(lv_scr_act());
    }
    display_show_page(s_wechat_page);
    display_update_wechat_page(&s_last_status);
}

static void display_show_wechat_add_page(void)
{
    if (s_wechat_add_page == NULL) {
        display_build_wechat_add_page(lv_scr_act());
    }
    if (s_wechat_scan_info_overlay != NULL) {
        lv_obj_add_flag(s_wechat_scan_info_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    display_update_wechat_add_field_label();
    display_show_page(s_wechat_add_page);
}

static void display_show_wechat_add_edit_page(void)
{
    if (s_wechat_add_edit_page == NULL) {
        display_build_wechat_add_edit_page(lv_scr_act());
    }

    display_show_page(s_wechat_add_edit_page);
    if (s_wechat_add_edit_ta != NULL) {
        lv_textarea_set_max_length(s_wechat_add_edit_ta, DISPLAY_WECHAT_OPEN_ID_LENGTH);
        lv_textarea_set_text(s_wechat_add_edit_ta, s_wechat_add_open_id);
        lv_textarea_set_cursor_pos(s_wechat_add_edit_ta, (uint32_t)strlen(s_wechat_add_open_id));
    }
    display_update_wechat_add_edit_feedback(NULL, lv_color_hex(0x0D8A59));
    if (s_wechat_add_edit_keyboard != NULL && s_wechat_add_edit_ta != NULL) {
        lv_keyboard_set_textarea(s_wechat_add_edit_keyboard, s_wechat_add_edit_ta);
        lv_keyboard_set_mode(s_wechat_add_edit_keyboard, LV_KEYBOARD_MODE_USER_1);
        lv_obj_clear_flag(s_wechat_add_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_wechat_add_edit_keyboard);
        lv_obj_add_state(s_wechat_add_edit_ta, LV_STATE_FOCUSED);
        lv_event_send(s_wechat_add_edit_ta, LV_EVENT_FOCUSED, NULL);
    }
}

static void display_show_wechat_list_page(void)
{
    if (s_wechat_list_page == NULL) {
        display_build_wechat_list_page(lv_scr_act());
    }
    display_update_wechat_contact_list(&s_last_status);
    display_show_page(s_wechat_list_page);
}

static void display_show_wechat_active_page(void)
{
    if (s_wechat_active_page == NULL) {
        display_build_wechat_active_page(lv_scr_act());
    }
    display_update_wechat_active_page(&s_last_status);
    display_show_page(s_wechat_active_page);
}

static void display_update_system_ram_label(void)
{
    if (s_system_ram_label == NULL) {
        return;
    }

    char text[48];
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    snprintf(text,
             sizeof(text),
             "RAM %uK / %uK",
             (unsigned)(internal_free / 1024U),
             (unsigned)(internal_largest / 1024U));
    display_text_set(s_system_ram_label, text);
}

static void display_show_system_page(void)
{
    if (s_system_page == NULL) {
        display_build_system_page(lv_scr_act());
    }
    display_update_system_ram_label();
    display_show_page(s_system_page);
}

static void display_open_wifi_page(display_page_id_t parent_page)
{
    s_wifi_parent_page = parent_page;
    display_show_wifi_page();
}

static void display_show_wifi_page(void)
{
    if (s_wifi_page == NULL) {
        display_build_wifi_page(lv_scr_act());
    }
    if (s_wifi_connect_page == NULL) {
        display_build_wifi_connect_page(lv_scr_act());
    }
    display_show_page(s_wifi_page);
    display_update_wifi_scan_state(&s_last_status);
    display_refresh_wifi_list(&s_last_status);
}

static void display_show_wifi_connect_page(void)
{
    if (s_wifi_connect_page == NULL) {
        display_build_wifi_connect_page(lv_scr_act());
    }
    display_hide_all_pages();
    if (s_wifi_connect_page != NULL) {
        lv_obj_clear_flag(s_wifi_connect_page, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_password_ta != NULL) {
    display_prepare_password_entry(&s_last_status);
    }
    s_wifi_connect_state = DISPLAY_WIFI_CONNECT_STATE_IDLE;
    if (s_keyboard != NULL && s_password_ta != NULL) {
        display_set_wifi_keyboard_mode(LV_KEYBOARD_MODE_USER_1);
        display_layout_wifi_keyboard();
        lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_keyboard);
        lv_obj_add_state(s_password_ta, LV_STATE_FOCUSED);
        lv_event_send(s_password_ta, LV_EVENT_FOCUSED, NULL);
    }
}

static void display_show_uuid_edit_page(void)
{
    const char *current_uuid = s_last_status.device_uuid[0] != '\0' ? s_last_status.device_uuid : "";

    if (s_uuid_edit_page == NULL) {
        display_build_uuid_edit_page(lv_scr_act());
    }

    display_show_page(s_uuid_edit_page);
    if (s_uuid_ta != NULL) {
        lv_textarea_set_text(s_uuid_ta, current_uuid);
        lv_textarea_set_cursor_pos(s_uuid_ta, (uint32_t)strlen(current_uuid));
    }
    display_update_uuid_edit_feedback(NULL, lv_color_hex(0x48656F));
}

static void display_show_ai_chat_page(void)
{
    if (s_ai_chat_page == NULL) {
        display_build_ai_chat_page(lv_scr_act());
    }
    display_update_ai_chat_page(&s_last_status);
    display_show_page(s_ai_chat_page);
}

static void display_show_ai_chat_settings_page(void)
{
    if (s_ai_chat_settings_page == NULL) {
        display_build_ai_chat_settings_page(lv_scr_act());
    }
    display_update_ai_chat_settings_page(&s_last_status);
    display_show_page(s_ai_chat_settings_page);
}

static void display_update_home_indicators(void)
{
    if (s_home_indicator_dots[0] == NULL || s_home_indicator_dots[1] == NULL) {
        return;
    }

    bool second_page = s_home_indicator_second_page;

    if (s_home_indicator_valid && s_home_indicator_second_page == second_page) {
        return;
    }
    s_home_indicator_valid = true;
    s_home_indicator_second_page = second_page;

    lv_obj_set_pos(s_home_indicator_dots[0], 0, 0);
    lv_obj_set_size(s_home_indicator_dots[0], second_page ? 8 : 22, 8);
    lv_obj_set_style_bg_color(s_home_indicator_dots[0],
                              second_page ? lv_color_hex(0xBCCAD8) : lv_color_hex(0x1768B7),
                              0);

    lv_obj_set_pos(s_home_indicator_dots[1], second_page ? 15 : 29, 0);
    lv_obj_set_size(s_home_indicator_dots[1], second_page ? 22 : 8, 8);
    lv_obj_set_style_bg_color(s_home_indicator_dots[1],
                              second_page ? lv_color_hex(0x1768B7) : lv_color_hex(0xBCCAD8),
                              0);

    if (s_home_nav_prev_btn != NULL) {
        if (second_page) {
            lv_obj_clear_state(s_home_nav_prev_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_home_nav_prev_btn, LV_STATE_DISABLED);
        }
    }
    if (s_home_nav_next_btn != NULL) {
        if (second_page) {
            lv_obj_add_state(s_home_nav_next_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(s_home_nav_next_btn, LV_STATE_DISABLED);
        }
    }
}

static void display_home_scroll_to_page(bool second_page)
{
    if (s_home_content_pages[0] == NULL || s_home_content_pages[1] == NULL) {
        return;
    }

    if (second_page) {
        lv_obj_add_flag(s_home_content_pages[0], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_home_content_pages[1], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_home_content_pages[0], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_home_content_pages[1], LV_OBJ_FLAG_HIDDEN);
    }
    s_home_indicator_second_page = second_page;
    s_home_indicator_valid = false;
    display_update_home_indicators();
}

static void display_home_prev_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    display_home_scroll_to_page(false);
}

static void display_home_next_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    display_home_scroll_to_page(true);
}

static lv_obj_t *display_create_binding_prompt_button(lv_obj_t *parent,
                                                      lv_coord_t x,
                                                      lv_coord_t y,
                                                      lv_coord_t width,
                                                      lv_coord_t height,
                                                      lv_coord_t radius,
                                                      lv_color_t fill,
                                                      lv_color_t stroke,
                                                      const char *text,
                                                      lv_coord_t text_y,
                                                      lv_color_t text_color,
                                                      uint8_t font_size,
                                                      lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_t *label = NULL;

    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, width, height);
    lv_obj_set_style_radius(btn, radius, 0);
    lv_obj_set_style_bg_color(btn, fill, 0);
    lv_obj_set_style_bg_color(btn, lv_color_darken(fill, 18), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, stroke, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }

    label = display_create_figma_text(btn,
                                      text,
                                      0,
                                      text_y,
                                      width,
                                      text_color,
                                      font_size,
                                      LV_TEXT_ALIGN_CENTER);
    if (label != NULL) {
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    }
    return btn;
}

static void display_build_binding_nowifi_dialog(lv_obj_t *overlay)
{
    lv_obj_t *panel = NULL;
    lv_obj_t *icon = NULL;
    lv_obj_t *icon_label = NULL;

    s_binding_nowifi_dialog = display_create_figma_box(overlay,
                                                       24,
                                                       46,
                                                       272,
                                                       148,
                                                       lv_color_hex(0xFFFFFF),
                                                       lv_color_hex(0xD5E0EB),
                                                       8);

    display_create_figma_text(s_binding_nowifi_dialog,
                              "设备绑定",
                              0,
                              11,
                              272,
                              lv_color_hex(0x10243E),
                              16,
                              LV_TEXT_ALIGN_CENTER);

    panel = display_create_figma_box(s_binding_nowifi_dialog,
                                     15,
                                     41,
                                     240,
                                     48,
                                     lv_color_hex(0xFFF1F1),
                                     lv_color_hex(0xFFAEAE),
                                     6);

    icon = lv_obj_create(panel);
    lv_obj_remove_style_all(icon);
    lv_obj_set_pos(icon, 17, 12);
    lv_obj_set_size(icon, 22, 22);
    lv_obj_set_style_radius(icon, 11, 0);
    lv_obj_set_style_bg_color(icon, lv_color_hex(0xE41C1C), 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    icon_label = lv_label_create(icon);
    lv_obj_set_width(icon_label, 22);
    lv_obj_set_style_text_align(icon_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(icon_label, display_ascii_font(14), 0);
    display_text_set_color(icon_label, lv_color_hex(0xFFFFFF), 0);
    display_text_set(icon_label, "!");
    lv_obj_center(icon_label);

    display_create_figma_text(panel,
                              "请先连接 WiFi",
                              47,
                              7,
                              170,
                              lv_color_hex(0x10243E),
                              14,
                              LV_TEXT_ALIGN_LEFT);
    display_create_figma_text(panel,
                              "联网后自动获取6位绑定码",
                              47,
                              27,
                              180,
                              lv_color_hex(0x64758A),
                              10,
                              LV_TEXT_ALIGN_LEFT);

    display_create_binding_prompt_button(s_binding_nowifi_dialog,
                                         15,
                                         107,
                                         240,
                                         28,
                                         6,
                                         lv_color_hex(0x20BF7A),
                                         lv_color_hex(0x20BF7A),
                                         "设置WiFi",
                                         4,
                                         lv_color_hex(0xFFFFFF),
                                         13,
                                         display_binding_wifi_btn_cb);
}

static void display_build_binding_code_dialog(lv_obj_t *overlay)
{
    lv_obj_t *code_panel = NULL;
    lv_obj_t *qr_card = NULL;

    s_binding_code_dialog = display_create_figma_box(overlay,
                                                     22,
                                                     32,
                                                     276,
                                                     176,
                                                     lv_color_hex(0xFFFFFF),
                                                     lv_color_hex(0xD5E0EB),
                                                     8);

    display_create_figma_text(s_binding_code_dialog,
                              "设备绑定",
                              0,
                              8,
                              276,
                              lv_color_hex(0x10243E),
                              16,
                              LV_TEXT_ALIGN_CENTER);

    code_panel = display_create_figma_box(s_binding_code_dialog,
                                          15,
                                          33,
                                          136,
                                          50,
                                          lv_color_hex(0xF7FBFF),
                                          lv_color_hex(0xD5E0EB),
                                          6);
    display_create_figma_text(code_panel,
                              "绑定码",
                              0,
                              6,
                              136,
                              lv_color_hex(0x64758A),
                              10,
                              LV_TEXT_ALIGN_CENTER);

    s_binding_code_label = lv_label_create(code_panel);
    lv_obj_set_pos(s_binding_code_label, 0, 19);
    lv_obj_set_width(s_binding_code_label, 136);
    lv_label_set_long_mode(s_binding_code_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_binding_code_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_binding_code_label, display_ascii_font(30), 0);
    lv_obj_set_style_text_letter_space(s_binding_code_label, 1, 0);
    display_text_set_color(s_binding_code_label, lv_color_hex(0x10243E), 0);
    display_text_set(s_binding_code_label, DISPLAY_BINDING_CODE_PLACEHOLDER);

    display_create_figma_text(s_binding_code_dialog,
                              "绑定网址",
                              15,
                              89,
                              136,
                              lv_color_hex(0x64758A),
                              10,
                              LV_TEXT_ALIGN_LEFT);
    display_create_figma_text(s_binding_code_dialog,
                              DISPLAY_BINDING_PLATFORM_URL,
                              15,
                              104,
                              138,
                              lv_color_hex(0x10243E),
                              8,
                              LV_TEXT_ALIGN_LEFT);

    qr_card = display_create_figma_box(s_binding_code_dialog,
                                       158,
                                       30,
                                       101,
                                       101,
                                       lv_color_hex(0xFFFFFF),
                                       lv_color_hex(0xD5E0EB),
                                       5);
#if LV_USE_QRCODE
    s_binding_platform_qrcode = lv_qrcode_create(qr_card,
                                                 94,
                                                 lv_color_hex(0x111111),
                                                 lv_color_hex(0xFFFFFF));
    lv_obj_set_pos(s_binding_platform_qrcode, 3, 3);
    lv_obj_clear_flag(s_binding_platform_qrcode, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (lv_qrcode_update(s_binding_platform_qrcode,
                         DISPLAY_BINDING_PLATFORM_URL,
                         strlen(DISPLAY_BINDING_PLATFORM_URL)) != LV_RES_OK) {
        ESP_LOGW(TAG, "binding platform qr update failed");
    }
#else
    display_create_figma_text(qr_card,
                              "QR",
                              0,
                              39,
                              101,
                              lv_color_hex(0x10243E),
                              16,
                              LV_TEXT_ALIGN_CENTER);
#endif

    display_create_binding_prompt_button(s_binding_code_dialog,
                                         15,
                                         147,
                                         244,
                                         22,
                                         5,
                                         lv_color_hex(0xFFFFFF),
                                         lv_color_hex(0xD5E0EB),
                                         "刷新",
                                         3,
                                         lv_color_hex(0x10243E),
                                         12,
                                         display_binding_refresh_btn_cb);
}

static void display_build_binding_prompt_overlay(lv_obj_t *parent)
{
    if (parent == NULL || s_binding_prompt_overlay != NULL) {
        return;
    }

    s_binding_prompt_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_binding_prompt_overlay);
    lv_obj_set_pos(s_binding_prompt_overlay, 0, 0);
    lv_obj_set_size(s_binding_prompt_overlay, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_color(s_binding_prompt_overlay, lv_color_hex(0x10243E), 0);
    lv_obj_set_style_bg_opa(s_binding_prompt_overlay, 82, 0);
    lv_obj_set_style_pad_all(s_binding_prompt_overlay, 0, 0);
    lv_obj_clear_flag(s_binding_prompt_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_binding_prompt_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);

    display_build_binding_nowifi_dialog(s_binding_prompt_overlay);
    display_build_binding_code_dialog(s_binding_prompt_overlay);
    lv_obj_add_flag(s_binding_nowifi_dialog, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_binding_code_dialog, LV_OBJ_FLAG_HIDDEN);
}

static void display_update_binding_prompt(const display_status_t *status)
{
    bool bound = false;
    bool show_code_dialog = false;
    bool should_show = false;
    const char *code_text = DISPLAY_BINDING_CODE_PLACEHOLDER;

    if (s_binding_prompt_overlay == NULL || status == NULL) {
        return;
    }

    bound = status->binding_state == DISPLAY_DEVICE_BINDING_STATE_BOUND ||
            status->tirtc_device_id[0] != '\0';
    should_show = display_page_is_visible(s_home_page) && !bound;
    if (!should_show) {
        if (s_binding_prompt_visible) {
            lv_obj_add_flag(s_binding_prompt_overlay, LV_OBJ_FLAG_HIDDEN);
            s_binding_prompt_visible = false;
            s_binding_prompt_code_dialog_visible = false;
        }
        return;
    }

    show_code_dialog = status->network_connected;
    if (show_code_dialog && status->binding_code[0] != '\0') {
        code_text = status->binding_code;
    }

    if (s_binding_code_label != NULL &&
        strcmp(s_binding_prompt_code_text, code_text) != 0) {
        display_text_set(s_binding_code_label, code_text);
        strlcpy(s_binding_prompt_code_text, code_text, sizeof(s_binding_prompt_code_text));
    }

    if (!s_binding_prompt_visible ||
        s_binding_prompt_code_dialog_visible != show_code_dialog) {
        if (s_binding_nowifi_dialog != NULL) {
            if (show_code_dialog) {
                lv_obj_add_flag(s_binding_nowifi_dialog, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(s_binding_nowifi_dialog, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (s_binding_code_dialog != NULL) {
            if (show_code_dialog) {
                lv_obj_clear_flag(s_binding_code_dialog, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_binding_code_dialog, LV_OBJ_FLAG_HIDDEN);
            }
        }
        s_binding_prompt_code_dialog_visible = show_code_dialog;
    }

    if (!s_binding_prompt_visible) {
        lv_obj_clear_flag(s_binding_prompt_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_binding_prompt_overlay);
        s_binding_prompt_visible = true;
    }
}

static void display_binding_wifi_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_binding_prompt_overlay != NULL) {
        lv_obj_add_flag(s_binding_prompt_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    display_open_wifi_page(DISPLAY_PAGE_HOME);
    display_request_wifi_scan();
}

static void display_binding_refresh_btn_cb(lv_event_t *event)
{
    esp_err_t ret = ESP_OK;

    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_actions.on_start_device_binding == NULL) {
        ESP_LOGW(TAG, "binding refresh unavailable");
        return;
    }

    ret = s_actions.on_start_device_binding(s_actions.ctx);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "binding refresh failed: %s", esp_err_to_name(ret));
    }
    display_update_binding_prompt(&s_last_status);
}

static void display_home_view_btn_cb(lv_event_t *event)
{
    (void)event;
    if (display_enter_app(DISPLAY_APP_DEVICE) != ESP_OK) {
        display_show_wifi_alert("APP", "Open failed.");
        return;
    }
    display_show_main_page();
}

static void display_home_call_btn_cb(lv_event_t *event)
{
    (void)event;
    if (display_enter_app(DISPLAY_APP_CALL) != ESP_OK) {
        display_show_wifi_alert("APP", "Open failed.");
        return;
    }
    display_show_call_page();
}

static void display_home_wechat_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    esp_err_t ret = display_enter_app(DISPLAY_APP_WECHAT);
    if (ret != ESP_OK) {
        display_show_wifi_alert("APP", ret == ESP_ERR_INVALID_STATE ? "Connect WiFi first." : "Open failed.");
        return;
    }
    display_show_wechat_page();
}

static void display_home_ai_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    esp_err_t ret = display_enter_app(DISPLAY_APP_AI_CHAT);
    if (ret != ESP_OK) {
        display_show_wifi_alert("AI Chat", ret == ESP_ERR_INVALID_STATE ? "Connect WiFi first." : "Open failed.");
        return;
    }
    display_show_ai_chat_page();
}

static void display_home_settings_btn_cb(lv_event_t *event)
{
    (void)event;
    if (display_enter_app(DISPLAY_APP_SYSTEM) != ESP_OK) {
        display_show_wifi_alert("APP", "Open failed.");
        return;
    }
    display_show_system_page();
}

static void display_ai_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_return_home();
}

static void display_ai_settings_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_ai_chat_settings_page();
}

static void display_ai_settings_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_ai_chat_page();
}

static void display_ai_start_new_btn_cb(lv_event_t *event)
{
    (void)event;
    if (s_actions.on_start_ai_chat == NULL) {
        return;
    }

    esp_err_t ret = s_actions.on_start_ai_chat(s_actions.ctx);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        display_show_wifi_alert("AI Chat", "Start failed.");
        return;
    }
    if (ret == ESP_ERR_INVALID_STATE && !s_last_status.network_connected) {
        display_show_wifi_alert("AI Chat", "Connect WiFi first.");
    }
}

static void display_show_network_test_page(void)
{
    if (s_network_test_page == NULL) {
        display_build_network_test_page(lv_scr_act());
    }
    display_update_network_test_page(&s_last_status);
    display_show_page(s_network_test_page);
}

static void display_show_tirtc_config_page(void)
{
    if (s_tirtc_config_page == NULL) {
        display_build_tirtc_config_page(lv_scr_act());
    }
    display_update_tirtc_config_page(&s_last_status);
    display_show_page(s_tirtc_config_page);
}

static void display_show_tirtc_config_edit_page(display_tirtc_config_field_t field)
{
    const char *current_value = display_tirtc_config_field_value(&s_last_status, field);
    size_t max_len = display_tirtc_config_field_max_len(field);

    if (s_tirtc_config_edit_page == NULL) {
        display_build_tirtc_config_edit_page(lv_scr_act());
    }

    s_tirtc_edit_field = field;
    display_show_page(s_tirtc_config_edit_page);
    if (s_tirtc_edit_hint_label != NULL) {
        display_text_set(s_tirtc_edit_hint_label, display_tirtc_config_field_title(field));
    }
    if (s_tirtc_edit_ta != NULL) {
        lv_textarea_set_max_length(s_tirtc_edit_ta, max_len);
        lv_textarea_set_placeholder_text(s_tirtc_edit_ta, display_tirtc_config_field_title(field));
        lv_textarea_set_text(s_tirtc_edit_ta, current_value != NULL ? current_value : "");
        lv_textarea_set_cursor_pos(s_tirtc_edit_ta, (uint32_t)strlen(current_value != NULL ? current_value : ""));
    }
    display_update_tirtc_edit_feedback(NULL, lv_color_hex(0x0D8A59));
    if (s_tirtc_edit_keyboard != NULL && s_tirtc_edit_ta != NULL) {
        lv_keyboard_set_textarea(s_tirtc_edit_keyboard, s_tirtc_edit_ta);
        lv_keyboard_set_mode(s_tirtc_edit_keyboard, LV_KEYBOARD_MODE_USER_1);
        lv_obj_clear_flag(s_tirtc_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_tirtc_edit_keyboard);
        lv_obj_add_state(s_tirtc_edit_ta, LV_STATE_FOCUSED);
        lv_event_send(s_tirtc_edit_ta, LV_EVENT_FOCUSED, NULL);
    }
}

static void display_show_tirtc_test_page(void)
{
    if (s_test_page == NULL) {
        display_build_test_page(lv_scr_act());
    }
    display_update_test_page(&s_last_status);
    display_show_page(s_test_page);
}

static void display_show_ota_page(void)
{
    if (s_ota_page == NULL) {
        display_build_ota_page(lv_scr_act());
    }
    display_update_ota_page(&s_last_status);
    display_show_page(s_ota_page);
}

static void display_uuid_back_btn_cb(lv_event_t *event)
{
    (void)event;
    if (s_uuid_parent_page == DISPLAY_PAGE_TIRTC_CONFIG) {
        display_show_tirtc_config_page();
    } else {
        display_show_main_page();
    }
}

static void display_uuid_save_btn_cb(lv_event_t *event)
{
    (void)event;
    display_submit_uuid();
}

static void display_device_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_return_home();
}

static void display_system_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_return_home();
}

static void display_system_child_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_system_page();
}

static void display_system_wifi_btn_cb(lv_event_t *event)
{
    (void)event;
    display_open_wifi_page(DISPLAY_PAGE_SYSTEM);
    display_request_wifi_scan();
}

static void display_system_network_test_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_network_test_page();
}

static void display_system_ota_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_ota_page();
}

static void display_call_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_return_home();
}

static void display_call_child_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_call_page();
}

static void display_call_add_btn_cb(lv_event_t *event)
{
    (void)event;
    display_hide_keyboard();
    display_reset_call_add_inputs();
    display_show_call_add_page();
}

static void display_call_list_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_call_list_page();
}

static void display_call_scan_btn_cb(lv_event_t *event)
{
    esp_err_t ret = ESP_OK;

    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_actions.on_start_contact_scan == NULL) {
        display_show_wifi_alert("扫码添加", "扫码接口不可用");
        return;
    }

    display_show_call_scan_page();
    ret = s_actions.on_start_contact_scan(display_call_scan_preview_cb,
                                          display_call_scan_result_cb,
                                          NULL,
                                          s_actions.ctx);
    if (ret == ESP_OK) {
        return;
    }

    s_call_scan_active = false;
    display_show_call_add_page();
    switch (ret) {
    case ESP_ERR_NOT_SUPPORTED:
        display_show_wifi_alert("扫码添加", "摄像头不可用");
        break;
    case ESP_ERR_INVALID_STATE:
        display_show_wifi_alert("扫码添加", "扫码服务忙");
        break;
    case ESP_ERR_NOT_FOUND:
        display_show_wifi_alert("扫码添加", "未识别二维码");
        break;
    case ESP_ERR_INVALID_RESPONSE:
        display_show_wifi_alert("扫码添加", "二维码格式错误");
        break;
    case ESP_ERR_TIMEOUT:
        display_show_wifi_alert("扫码添加", "摄像头超时");
        break;
    default:
        display_show_wifi_alert("扫码添加", "扫码失败");
        break;
    }
}

static void display_call_scan_tap_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    lv_event_stop_bubbling(event);
    lv_event_stop_processing(event);
    display_exit_call_scan_to_previous();
}

static void display_call_scan_info_btn_cb(lv_event_t *event)
{
    (void)event;
    if (s_call_scan_info_overlay != NULL) {
        lv_obj_clear_flag(s_call_scan_info_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_call_scan_info_overlay);
    }
}

static void display_call_scan_info_close_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_call_scan_info_overlay == NULL) {
        return;
    }
    lv_obj_add_flag(s_call_scan_info_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void display_call_add_field_btn_cb(lv_event_t *event)
{
    display_call_add_field_t field =
        (display_call_add_field_t)(uintptr_t)lv_event_get_user_data(event);

    if (field >= DISPLAY_CALL_ADD_FIELD_COUNT) {
        return;
    }
    display_show_call_add_edit_page(field);
}

static void display_call_add_edit_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_call_add_page();
}

static void display_call_add_edit_save_btn_cb(lv_event_t *event)
{
    const char *value = NULL;
    char trimmed[DISPLAY_CALL_CONTACT_PAIR_KEY_MAX] = {0};
    char *target = NULL;
    size_t max_len = display_call_add_field_max_len(s_call_add_edit_field);

    (void)event;

    if (s_call_add_edit_ta == NULL || s_call_add_edit_field >= DISPLAY_CALL_ADD_FIELD_COUNT) {
        return;
    }

    value = lv_textarea_get_text(s_call_add_edit_ta);
    display_copy_trimmed_text(trimmed, sizeof(trimmed), value);
    if (!display_text_has_visible_char(trimmed) || strlen(trimmed) > max_len) {
        display_update_call_add_edit_feedback("内容不合法", lv_color_hex(0xE45656));
        return;
    }

    target = display_call_add_field_buffer(s_call_add_edit_field);
    strlcpy(target, trimmed, max_len + 1U);
    display_update_call_add_field_labels();
    display_show_call_add_page();
}

static void display_call_confirm_add_btn_cb(lv_event_t *event)
{
    char device_id_trimmed[DISPLAY_CALL_CONTACT_DEVICE_ID_MAX] = {0};
    char pair_key_trimmed[DISPLAY_CALL_CONTACT_PAIR_KEY_MAX] = {0};

    (void)event;

    display_copy_trimmed_text(device_id_trimmed, sizeof(device_id_trimmed), s_call_add_device_id);
    display_copy_trimmed_text(pair_key_trimmed, sizeof(pair_key_trimmed), s_call_add_pair_key);

    if (!display_text_has_visible_char(device_id_trimmed) ||
        !display_text_has_visible_char(pair_key_trimmed)) {
        display_show_wifi_alert("Add Contact", "Device ID and Pair Key are required.");
        return;
    }

    display_hide_keyboard();
    if (s_actions.on_add_call_contact == NULL) {
        display_show_wifi_alert("Add Contact", "Contact storage is unavailable.");
        return;
    }
    esp_err_t ret = s_actions.on_add_call_contact(device_id_trimmed, pair_key_trimmed, s_actions.ctx);
    if (ret != ESP_OK) {
        display_show_wifi_alert("Add Contact", "Contact save failed.");
        return;
    }
    display_store_scanned_call_contact(device_id_trimmed, pair_key_trimmed);
    display_reset_call_add_inputs();
    display_invalidate_call_list_page();
    display_show_call_list_page();
}

static void display_call_contact_call_btn_cb(lv_event_t *event)
{
    uint8_t contact_index = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    esp_err_t ret = ESP_OK;

    if (contact_index >= s_call_contact_count ||
        s_call_contacts[contact_index].device_id[0] == '\0' ||
        s_call_contacts[contact_index].pair_key[0] == '\0') {
        display_show_wifi_alert("呼叫", "联系人不存在");
        return;
    }
    if (s_actions.on_call_contact == NULL) {
        display_show_wifi_alert("呼叫", "呼叫接口不可用");
        return;
    }

    ret = s_actions.on_call_contact(s_call_contacts[contact_index].device_id,
                                    s_call_contacts[contact_index].pair_key,
                                    s_actions.ctx);
    if (ret != ESP_OK) {
        display_show_wifi_alert("呼叫",
                                ret == ESP_ERR_INVALID_STATE ? "请先连接 Wi-Fi" : "呼叫启动失败");
        return;
    }
    s_call_active_started_us = 0;
    display_show_call_active_page();
}

static void display_call_contact_delete_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_LONG_PRESSED) {
        return;
    }

    uint8_t contact_index = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    display_show_call_delete_confirm(contact_index);
}

static void display_call_delete_cancel_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    display_hide_call_delete_confirm();
}

static void display_call_delete_confirm_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    uint8_t contact_index = s_call_delete_pending_index;
    char device_id[DISPLAY_CALL_CONTACT_DEVICE_ID_MAX] = {0};
    if (contact_index < s_call_contact_count && contact_index < DISPLAY_CALL_CONTACT_COUNT) {
        strlcpy(device_id, s_call_contacts[contact_index].device_id, sizeof(device_id));
    }
    if (device_id[0] == '\0') {
        display_hide_call_delete_confirm();
        display_show_wifi_alert("Delete Contact", "Contact not found.");
        return;
    }
    if (s_actions.on_remove_call_contact == NULL) {
        display_hide_call_delete_confirm();
        display_show_wifi_alert("Delete Contact", "Contact storage is unavailable.");
        return;
    }
    esp_err_t ret = s_actions.on_remove_call_contact(device_id, s_actions.ctx);
    if (ret != ESP_OK) {
        display_hide_call_delete_confirm();
        display_show_wifi_alert("Delete Contact", "Contact delete failed.");
        return;
    }
    if (!display_remove_call_contact(contact_index)) {
        display_hide_call_delete_confirm();
        display_show_wifi_alert("删除联系人", "联系人不存在");
        return;
    }

    display_hide_call_delete_confirm();
    display_invalidate_call_list_page();
    display_show_call_list_page();
    display_show_wifi_alert("删除联系人", "已删除");
}

static void display_call_hangup_btn_cb(lv_event_t *event)
{
    (void)event;

    if (s_actions.on_hangup_call != NULL) {
        (void)s_actions.on_hangup_call(s_actions.ctx);
    } else if (s_actions.on_disconnect_rtc != NULL) {
        (void)s_actions.on_disconnect_rtc(s_actions.ctx);
    }
    s_call_active_started_us = 0;
    display_show_call_page();
}

static void display_apply_call_volume_action(display_call_volume_action_t action, bool wechat)
{
    uint8_t current = 0;

    switch (action) {
    case DISPLAY_CALL_VOLUME_MIC_DOWN:
    case DISPLAY_CALL_VOLUME_MIC_UP:
        current = s_last_status.audio_capture_gain_percent;
        if (action == DISPLAY_CALL_VOLUME_MIC_UP) {
            current = current > (100U - DISPLAY_CALL_VOLUME_STEP) ? 100U : current + DISPLAY_CALL_VOLUME_STEP;
        } else {
            current = current < DISPLAY_CALL_VOLUME_STEP ? 0U : current - DISPLAY_CALL_VOLUME_STEP;
        }
        if (s_actions.on_set_capture_gain != NULL) {
            (void)s_actions.on_set_capture_gain(current, s_actions.ctx);
        }
        s_last_status.audio_capture_gain_percent = current;
        break;
    case DISPLAY_CALL_VOLUME_SPEAKER_DOWN:
    case DISPLAY_CALL_VOLUME_SPEAKER_UP:
    default:
        current = s_last_status.audio_speaker_volume_percent;
        if (action == DISPLAY_CALL_VOLUME_SPEAKER_UP) {
            current = current > (100U - DISPLAY_CALL_VOLUME_STEP) ? 100U : current + DISPLAY_CALL_VOLUME_STEP;
        } else {
            current = current < DISPLAY_CALL_VOLUME_STEP ? 0U : current - DISPLAY_CALL_VOLUME_STEP;
        }
        if (s_actions.on_set_speaker_volume != NULL) {
            (void)s_actions.on_set_speaker_volume(current, s_actions.ctx);
        }
        s_last_status.audio_speaker_volume_percent = current;
        break;
    }

    if (wechat) {
        display_update_wechat_active_page(&s_last_status);
    } else {
        display_update_call_active_page(&s_last_status);
    }
}

static void display_call_volume_btn_cb(lv_event_t *event)
{
    display_call_volume_action_t action =
        (display_call_volume_action_t)(uintptr_t)lv_event_get_user_data(event);
    display_apply_call_volume_action(action, false);
}

static void display_wechat_child_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_wechat_page();
}

static void display_wechat_add_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_wechat_add_page();
}

static void display_wechat_list_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    display_show_wechat_list_page();
}

static void display_wechat_scan_btn_cb(lv_event_t *event)
{
    esp_err_t ret = ESP_OK;

    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_actions.on_start_wechat_contact_scan == NULL) {
        display_show_wifi_alert("扫码添加", "扫码接口不可用");
        return;
    }

    display_show_wechat_scan_page();
    ret = s_actions.on_start_wechat_contact_scan(display_call_scan_preview_cb,
                                                 display_wechat_scan_result_cb,
                                                 NULL,
                                                 s_actions.ctx);
    if (ret == ESP_OK) {
        return;
    }

    s_call_scan_active = false;
    display_show_wechat_add_page();
    switch (ret) {
    case ESP_ERR_NOT_SUPPORTED:
        display_show_wifi_alert("扫码添加", "摄像头不可用");
        break;
    case ESP_ERR_INVALID_STATE:
        display_show_wifi_alert("扫码添加", "扫码服务忙");
        break;
    case ESP_ERR_NOT_FOUND:
        display_show_wifi_alert("扫码添加", "未识别二维码");
        break;
    case ESP_ERR_INVALID_RESPONSE:
        display_show_wifi_alert("扫码添加", "二维码格式错误");
        break;
    case ESP_ERR_TIMEOUT:
        display_show_wifi_alert("扫码添加", "摄像头超时");
        break;
    default:
        display_show_wifi_alert("扫码添加", "扫码失败");
        break;
    }
}

static void display_wechat_scan_info_btn_cb(lv_event_t *event)
{
    (void)event;
    if (s_wechat_scan_info_overlay != NULL) {
        lv_obj_clear_flag(s_wechat_scan_info_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_wechat_scan_info_overlay);
    }
}

static void display_wechat_scan_info_close_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_wechat_scan_info_overlay == NULL) {
        return;
    }
    lv_obj_add_flag(s_wechat_scan_info_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void display_wechat_add_field_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    display_show_wechat_add_edit_page();
}

static void display_wechat_add_edit_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_wechat_add_page();
}

static void display_wechat_add_edit_save_btn_cb(lv_event_t *event)
{
    const char *value = NULL;
    char trimmed[DISPLAY_WECHAT_OPEN_ID_MAX] = {0};

    (void)event;

    if (s_wechat_add_edit_ta == NULL) {
        return;
    }

    value = lv_textarea_get_text(s_wechat_add_edit_ta);
    display_copy_trimmed_text(trimmed, sizeof(trimmed), value);
    if (!display_wechat_open_id_valid(trimmed)) {
        display_update_wechat_add_edit_feedback("必须是28位微信Open ID", lv_color_hex(0xE45656));
        return;
    }

    strlcpy(s_wechat_add_open_id, trimmed, sizeof(s_wechat_add_open_id));
    display_update_wechat_add_field_label();
    display_show_wechat_add_page();
}

static void display_wechat_confirm_add_btn_cb(lv_event_t *event)
{
    char open_id_trimmed[DISPLAY_WECHAT_OPEN_ID_MAX] = {0};
    esp_err_t ret = ESP_OK;

    (void)event;

    display_copy_trimmed_text(open_id_trimmed, sizeof(open_id_trimmed), s_wechat_add_open_id);
    if (!display_wechat_open_id_valid(open_id_trimmed)) {
        display_show_wifi_alert("微信联系人", "请输入28位微信Open ID");
        return;
    }
    if (s_actions.on_add_wechat_contact == NULL) {
        display_show_wifi_alert("微信联系人", "添加接口不可用");
        return;
    }

    ret = s_actions.on_add_wechat_contact(open_id_trimmed, s_actions.ctx);
    if (ret != ESP_OK) {
        display_show_wifi_alert("微信联系人",
                                ret == ESP_ERR_INVALID_STATE ? "请先进入微信呼叫" : "添加失败");
        return;
    }

    display_store_scanned_wechat_contact(open_id_trimmed);
    display_reset_wechat_add_input();
    display_show_wechat_list_page();
    display_show_wifi_alert("微信联系人", "添加成功");
}

static void display_wechat_contact_call_btn_cb(lv_event_t *event)
{
    uint8_t contact_index = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    const char *open_id = NULL;
    esp_err_t ret = ESP_OK;

    if (contact_index >= DISPLAY_WECHAT_CONTACT_COUNT ||
        contact_index >= s_last_status.wechat_contact_count ||
        s_last_status.wechat_contacts[contact_index].open_id[0] == '\0') {
        display_show_wifi_alert("微信呼叫", "联系人不存在");
        return;
    }
    if (s_actions.on_wechat_contact == NULL) {
        display_show_wifi_alert("微信呼叫", "呼叫接口不可用");
        return;
    }

    open_id = s_last_status.wechat_contacts[contact_index].open_id;
    ret = s_actions.on_wechat_contact(open_id, s_actions.ctx);
    if (ret != ESP_OK) {
        display_show_wifi_alert("微信呼叫",
                                ret == ESP_ERR_INVALID_STATE ? "请先连接 Wi-Fi" : "呼叫启动失败");
        return;
    }
    s_wechat_active_started_us = 0;
    display_show_wechat_active_page();
}

static void display_wechat_hangup_task(void *arg)
{
    display_actions_t actions = s_actions;

    (void)arg;
    if (actions.on_wechat_hangup_call != NULL) {
        esp_err_t ret = actions.on_wechat_hangup_call(actions.ctx);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "wechat hangup action failed: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGW(TAG, "wechat hangup action missing");
    }
    s_wechat_hangup_task = NULL;
    platform_task_reaper_delete_current_with_caps(TAG);
}

static void display_queue_wechat_hangup(void)
{
    if (s_actions.on_wechat_hangup_call == NULL) {
        ESP_LOGW(TAG, "queue wechat hangup skipped: no action");
        return;
    }
    if (s_wechat_hangup_task != NULL) {
        ESP_LOGW(TAG, "queue wechat hangup skipped: task already running");
        return;
    }

    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(display_wechat_hangup_task,
                                                          "ui_wx_hangup",
                                                          DISPLAY_WECHAT_HANGUP_TASK_STACK,
                                                          NULL,
                                                          DISPLAY_WECHAT_HANGUP_TASK_PRIORITY,
                                                          &s_wechat_hangup_task,
                                                          APP_TASK_CORE_BACKGROUND,
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ret != pdPASS) {
        s_wechat_hangup_task = NULL;
        ESP_LOGW(TAG, "queue wechat hangup task failed");
    }
}

static void display_remove_wechat_contact_from_last_status(uint8_t contact_index)
{
    if (contact_index >= s_last_status.wechat_contact_count ||
        contact_index >= DISPLAY_WECHAT_CONTACT_COUNT) {
        return;
    }

    uint8_t count = s_last_status.wechat_contact_count > DISPLAY_WECHAT_CONTACT_COUNT ?
        DISPLAY_WECHAT_CONTACT_COUNT : s_last_status.wechat_contact_count;
    for (uint8_t index = contact_index; index + 1U < count; ++index) {
        s_last_status.wechat_contacts[index] = s_last_status.wechat_contacts[index + 1U];
    }
    if (count > 0) {
        memset(&s_last_status.wechat_contacts[count - 1U],
               0,
               sizeof(s_last_status.wechat_contacts[count - 1U]));
        --s_last_status.wechat_contact_count;
    }
}

static void display_wechat_contact_delete_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_LONG_PRESSED) {
        return;
    }

    uint8_t contact_index = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    display_show_wechat_delete_confirm(contact_index);
}

static void display_wechat_delete_cancel_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    display_hide_wechat_delete_confirm();
}

static void display_wechat_delete_confirm_btn_cb(lv_event_t *event)
{
    uint8_t contact_index = s_wechat_delete_pending_index;
    char open_id[DISPLAY_WECHAT_OPEN_ID_MAX] = {0};
    esp_err_t ret = ESP_OK;

    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    strlcpy(open_id, s_wechat_delete_pending_open_id, sizeof(open_id));

    if (contact_index >= DISPLAY_WECHAT_CONTACT_COUNT ||
        contact_index >= s_last_status.wechat_contact_count ||
        strcmp(s_last_status.wechat_contacts[contact_index].open_id, open_id) != 0) {
        uint8_t count = s_last_status.wechat_contact_count > DISPLAY_WECHAT_CONTACT_COUNT ?
            DISPLAY_WECHAT_CONTACT_COUNT : s_last_status.wechat_contact_count;
        contact_index = DISPLAY_WECHAT_CONTACT_COUNT;
        for (uint8_t index = 0; index < count; ++index) {
            if (strcmp(s_last_status.wechat_contacts[index].open_id, open_id) == 0) {
                contact_index = index;
                break;
            }
        }
    }

    if (open_id[0] == '\0' ||
        contact_index >= DISPLAY_WECHAT_CONTACT_COUNT ||
        contact_index >= s_last_status.wechat_contact_count ||
        s_last_status.wechat_contacts[contact_index].open_id[0] == '\0') {
        display_hide_wechat_delete_confirm();
        display_show_wifi_alert("微信联系人", "联系人不存在");
        return;
    }
    if (s_actions.on_remove_wechat_contact == NULL) {
        display_hide_wechat_delete_confirm();
        display_show_wifi_alert("微信联系人", "删除接口不可用");
        return;
    }

    ret = s_actions.on_remove_wechat_contact(open_id, s_actions.ctx);
    if (ret != ESP_OK) {
        display_hide_wechat_delete_confirm();
        display_show_wifi_alert("微信联系人",
                                ret == ESP_ERR_NOT_FOUND ? "联系人不存在" : "删除失败");
        return;
    }

    display_hide_wechat_delete_confirm();
    display_remove_wechat_contact_from_last_status(contact_index);
    display_update_wechat_contact_list(&s_last_status);
    display_show_wifi_alert("微信联系人", "已删除");
}

static void display_wechat_hangup_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    s_wechat_active_started_us = 0;
    display_queue_wechat_hangup();
    display_show_wechat_page();
}

static void display_wechat_volume_btn_cb(lv_event_t *event)
{
    display_call_volume_action_t action =
        (display_call_volume_action_t)(uintptr_t)lv_event_get_user_data(event);
    display_apply_call_volume_action(action, true);
}

static void display_ai_settings_action_btn_cb(lv_event_t *event)
{
    display_ai_setting_action_t action =
        (display_ai_setting_action_t)(uintptr_t)lv_event_get_user_data(event);
    uint8_t next = 0;
    esp_err_t ret = ESP_OK;

    switch (action) {
    case DISPLAY_AI_SETTING_MIC_DOWN:
        next = display_adjust_volume(s_last_status.audio_capture_gain_percent, -10);
        if (s_actions.on_set_capture_gain != NULL) {
            ret = s_actions.on_set_capture_gain(next, s_actions.ctx);
        }
        if (ret == ESP_OK) {
            s_last_status.audio_capture_gain_percent = next;
        }
        break;
    case DISPLAY_AI_SETTING_MIC_UP:
        next = display_adjust_volume(s_last_status.audio_capture_gain_percent, 10);
        if (s_actions.on_set_capture_gain != NULL) {
            ret = s_actions.on_set_capture_gain(next, s_actions.ctx);
        }
        if (ret == ESP_OK) {
            s_last_status.audio_capture_gain_percent = next;
        }
        break;
    case DISPLAY_AI_SETTING_SPEAKER_DOWN:
        next = display_adjust_volume(s_last_status.audio_speaker_volume_percent, -10);
        if (s_actions.on_set_speaker_volume != NULL) {
            ret = s_actions.on_set_speaker_volume(next, s_actions.ctx);
        }
        if (ret == ESP_OK) {
            s_last_status.audio_speaker_volume_percent = next;
        }
        break;
    case DISPLAY_AI_SETTING_SPEAKER_UP:
        next = display_adjust_volume(s_last_status.audio_speaker_volume_percent, 10);
        if (s_actions.on_set_speaker_volume != NULL) {
            ret = s_actions.on_set_speaker_volume(next, s_actions.ctx);
        }
        if (ret == ESP_OK) {
            s_last_status.audio_speaker_volume_percent = next;
        }
        break;
    case DISPLAY_AI_SETTING_AVATAR_BUDDY:
    case DISPLAY_AI_SETTING_AVATAR_SPROUT:
        next = action == DISPLAY_AI_SETTING_AVATAR_SPROUT ?
            DISPLAY_AI_AVATAR_SPROUT : DISPLAY_AI_AVATAR_BUDDY;
        if (s_actions.on_set_ai_chat_avatar != NULL) {
            ret = s_actions.on_set_ai_chat_avatar(next, s_actions.ctx);
        }
        if (ret == ESP_OK) {
            s_last_status.ai_chat_avatar = next;
        }
        break;
    default:
        return;
    }

    display_update_ai_chat_settings_page(&s_last_status);
    display_update_ai_avatar(&s_last_status, NULL);
}

static void display_system_tirtc_config_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_tirtc_config_page();
}

static void display_system_tirtc_test_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_tirtc_test_page();
}

static void display_network_test_start_btn_cb(lv_event_t *event)
{
    (void)event;

    if (s_actions.on_ping_test != NULL) {
        (void)s_actions.on_ping_test(s_actions.ctx);
    }
    display_update_network_test_page(&s_last_status);
}

static void __attribute__((unused)) display_tirtc_config_field_btn_cb(lv_event_t *event)
{
    display_tirtc_config_field_t field =
        (display_tirtc_config_field_t)(uintptr_t)lv_event_get_user_data(event);

    if (field >= DISPLAY_TIRTC_CONFIG_FIELD_COUNT) {
        return;
    }
    if (field != DISPLAY_TIRTC_CONFIG_FIELD_TOKEN_SUBJECT) {
        display_show_wifi_alert("TiRTC Config", "Managed by device binding service");
        return;
    }
    display_show_tirtc_config_edit_page(field);
}

static void __attribute__((unused)) display_tirtc_config_scan_btn_cb(lv_event_t *event)
{
    esp_err_t ret = ESP_OK;

    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_actions.on_reset_device_binding == NULL) {
        display_show_wifi_alert("TiRTC Config", "Device binding reset unavailable");
        return;
    }

    ret = s_actions.on_reset_device_binding(s_actions.ctx);
    if (ret == ESP_OK) {
        s_last_status.tirtc_device_id[0] = '\0';
        s_last_status.tirtc_device_secret[0] = '\0';
        s_last_status.binding_code[0] = '\0';
        s_last_status.binding_running = true;
        s_last_status.binding_state = DISPLAY_DEVICE_BINDING_STATE_REPORTING;
        display_update_tirtc_config_page(&s_last_status);
        display_update_binding_prompt(&s_last_status);
        return;
    }

    display_show_wifi_alert("TiRTC Config", display_contact_scan_error_text(ret));
}

static void display_tirtc_config_edit_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_tirtc_config_page();
}

static void display_tirtc_config_edit_save_btn_cb(lv_event_t *event)
{
    const char *value = NULL;
    size_t value_len = 0;
    esp_err_t ret = ESP_OK;

    (void)event;
    if (s_tirtc_edit_ta == NULL) {
        return;
    }

    value = lv_textarea_get_text(s_tirtc_edit_ta);
    value_len = strlen(value);
    if (value_len == 0 || value_len > display_tirtc_config_field_max_len(s_tirtc_edit_field)) {
        display_update_tirtc_edit_feedback("内容不合法", lv_color_hex(0xE45656));
        return;
    }
    if (s_actions.on_set_tirtc_config_field == NULL) {
        display_update_tirtc_edit_feedback("保存接口不可用", lv_color_hex(0xE45656));
        return;
    }

    ret = s_actions.on_set_tirtc_config_field(s_tirtc_edit_field, value, s_actions.ctx);
    if (ret != ESP_OK) {
        display_update_tirtc_edit_feedback("保存失败", lv_color_hex(0xE45656));
        return;
    }

    switch (s_tirtc_edit_field) {
    case DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_SECRET:
        strlcpy(s_last_status.tirtc_device_secret, value, sizeof(s_last_status.tirtc_device_secret));
        break;
    case DISPLAY_TIRTC_CONFIG_FIELD_TOKEN_SUBJECT:
        strlcpy(s_last_status.tirtc_token_subject, value, sizeof(s_last_status.tirtc_token_subject));
        break;
    case DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_ID:
        strlcpy(s_last_status.tirtc_access_key_id, value, sizeof(s_last_status.tirtc_access_key_id));
        break;
    case DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_SECRET:
        strlcpy(s_last_status.tirtc_access_key_secret, value, sizeof(s_last_status.tirtc_access_key_secret));
        break;
    case DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_ID:
    default:
        strlcpy(s_last_status.tirtc_device_id, value, sizeof(s_last_status.tirtc_device_id));
        break;
    }
    display_show_tirtc_config_page();
}

static void display_tirtc_test_start_btn_cb(lv_event_t *event)
{
    (void)event;

    if (s_actions.on_start_rtc != NULL) {
        (void)s_actions.on_start_rtc(s_actions.ctx);
    }
    display_update_test_page(&s_last_status);
}

static void display_ota_start_btn_cb(lv_event_t *event)
{
    (void)event;

    if (!s_last_status.network_connected) {
        display_show_wifi_alert("OTA", "Connect WiFi first.");
        return;
    }
    if (s_actions.on_start_ota == NULL) {
        display_show_wifi_alert("OTA", "OTA is unavailable.");
        return;
    }

    esp_err_t ret = s_actions.on_start_ota(s_actions.ctx);
    if (ret == ESP_ERR_INVALID_STATE) {
        display_show_wifi_alert("OTA", s_last_status.ota_running ? "OTA is already running." : "Connect WiFi first.");
    } else if (ret != ESP_OK) {
        display_show_wifi_alert("OTA", "OTA start failed.");
    }
    display_update_ota_page(&s_last_status);
}

static void display_ota_reboot_btn_cb(lv_event_t *event)
{
    (void)event;

    if (s_last_status.ota_state != DISPLAY_OTA_STATE_READY_TO_REBOOT) {
        display_show_wifi_alert("OTA", "No staged update yet.");
        return;
    }
    if (s_actions.on_restart_for_ota == NULL) {
        display_show_wifi_alert("OTA", "Restart action is unavailable.");
        return;
    }

    (void)s_actions.on_restart_for_ota(s_actions.ctx);
}

static void display_test_video_btn_cb(lv_event_t *event)
{
    (void)event;

    if (s_actions.on_start_sender_video_test == NULL) {
        display_set_main_hint("Video test unavailable");
        return;
    }

    esp_err_t ret = s_actions.on_start_sender_video_test(s_actions.ctx);
    if (ret != ESP_OK) {
        display_set_main_hint(ret == ESP_ERR_INVALID_STATE ? "Connect WiFi first" : "Video test failed");
    } else {
        display_set_main_hint("Video switch requested");
    }
}

static void display_test_audio_btn_cb(lv_event_t *event)
{
    (void)event;

    if (s_actions.on_start_sender_audio_test == NULL) {
        display_set_main_hint("Audio test unavailable");
        return;
    }

    esp_err_t ret = s_actions.on_start_sender_audio_test(s_actions.ctx);
    if (ret != ESP_OK) {
        display_set_main_hint(ret == ESP_ERR_INVALID_STATE ? "Connect WiFi first" : "Audio test failed");
    } else {
        display_set_main_hint("Audio switch requested");
    }
}

static void display_wifi_back_btn_cb(lv_event_t *event)
{
    (void)event;
    if (s_wifi_parent_page == DISPLAY_PAGE_HOME) {
        display_show_home_page();
    } else if (s_wifi_parent_page == DISPLAY_PAGE_SYSTEM) {
        display_show_system_page();
    } else {
        display_show_main_page();
    }
}

static void display_wifi_connect_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_wifi_page();
}

static void display_wifi_refresh_btn_cb(lv_event_t *event)
{
    (void)event;
    display_request_wifi_scan();
}

static void display_wifi_ap_select_cb(lv_event_t *event)
{
    uintptr_t index = (uintptr_t)lv_event_get_user_data(event);
    if (index >= s_last_status.wifi_scan_count || index >= DISPLAY_WIFI_SCAN_MAX) {
        return;
    }

    strlcpy(s_selected_ssid,
            s_last_status.wifi_scan_results[index].ssid,
            sizeof(s_selected_ssid));
    display_show_wifi_connect_page();
}

static uint8_t display_adjust_volume(uint8_t current, int delta)
{
    int next = (int)current + delta;

    if (next < 0) {
        next = 0;
    } else if (next > 100) {
        next = 100;
    }
    return (uint8_t)next;
}

static void display_device_volume_btn_cb(lv_event_t *event)
{
    display_device_volume_action_t action =
        (display_device_volume_action_t)(uintptr_t)lv_event_get_user_data(event);
    bool receive = action == DISPLAY_DEVICE_VOLUME_RECEIVE_DOWN ||
                   action == DISPLAY_DEVICE_VOLUME_RECEIVE_UP ||
                   action == DISPLAY_DEVICE_VOLUME_RECEIVE_MUTE;
    uint8_t current = receive ? s_last_status.audio_speaker_volume_percent
                              : s_last_status.audio_capture_gain_percent;
    uint8_t next = current;
    bool mute_action = action == DISPLAY_DEVICE_VOLUME_RECEIVE_MUTE ||
                       action == DISPLAY_DEVICE_VOLUME_SEND_MUTE;
    esp_err_t ret = ESP_OK;

    switch (action) {
    case DISPLAY_DEVICE_VOLUME_RECEIVE_DOWN:
    case DISPLAY_DEVICE_VOLUME_SEND_DOWN:
        next = display_adjust_volume(current, -10);
        break;
    case DISPLAY_DEVICE_VOLUME_RECEIVE_UP:
    case DISPLAY_DEVICE_VOLUME_SEND_UP:
        next = display_adjust_volume(current, 10);
        break;
    case DISPLAY_DEVICE_VOLUME_RECEIVE_MUTE:
    case DISPLAY_DEVICE_VOLUME_SEND_MUTE:
        if (current > 0U) {
            if (receive) {
                s_device_receive_restore_volume = current;
                s_device_receive_restore_valid = true;
            } else {
                s_device_send_restore_volume = current;
                s_device_send_restore_valid = true;
            }
            next = 0;
        } else if (receive) {
            next = s_device_receive_restore_valid
                       ? s_device_receive_restore_volume
                       : DISPLAY_DEVICE_VOLUME_RESTORE_DEFAULT;
        } else {
            next = s_device_send_restore_valid
                       ? s_device_send_restore_volume
                       : DISPLAY_DEVICE_VOLUME_RESTORE_DEFAULT;
        }
        break;
    default:
        return;
    }

    if (receive) {
        if (s_actions.on_set_speaker_volume == NULL) {
            display_set_main_hint("Speaker control unavailable");
            return;
        }
        ret = s_actions.on_set_speaker_volume(next, s_actions.ctx);
        if (ret == ESP_OK) {
            s_last_status.audio_speaker_volume_percent = next;
            if (!mute_action && next > 0U) {
                s_device_receive_restore_volume = next;
                s_device_receive_restore_valid = true;
            }
        }
    } else {
        if (s_actions.on_set_capture_gain == NULL) {
            display_set_main_hint("Capture control unavailable");
            return;
        }
        ret = s_actions.on_set_capture_gain(next, s_actions.ctx);
        if (ret == ESP_OK) {
            s_last_status.audio_capture_gain_percent = next;
            if (!mute_action && next > 0U) {
                s_device_send_restore_volume = next;
                s_device_send_restore_valid = true;
            }
        }
    }

    if (ret != ESP_OK) {
        display_set_main_hint("Audio control failed: %s", esp_err_to_name(ret));
        return;
    }
    display_update_main_page(&s_last_status);
}

static void display_refresh_wifi_list(const display_status_t *status)
{
    uint16_t visible_count = 0;

    if (s_wifi_list == NULL || status == NULL) {
        return;
    }

    visible_count = status->wifi_scan_count < DISPLAY_WIFI_SCAN_MAX
                        ? status->wifi_scan_count
                        : DISPLAY_WIFI_SCAN_MAX;

    for (uint16_t index = 0; index < visible_count; ++index) {
        display_add_wifi_list_item(status, index);
    }

    for (uint16_t index = visible_count; index < DISPLAY_WIFI_SCAN_MAX; ++index) {
        if (s_wifi_list_buttons[index] != NULL) {
            lv_obj_add_flag(s_wifi_list_buttons[index], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void display_update_wifi_scan_state(const display_status_t *status)
{
    char left_text[96] = {0};
    char right_text[32] = {0};
    lv_color_t left_color = lv_color_hex(0x64758A);
    lv_color_t right_color = lv_color_hex(0x64758A);

    if (status == NULL || s_wifi_connection_state_label == NULL ||
        s_wifi_scan_state_label == NULL || s_wifi_scan_count_label == NULL) {
        return;
    }

    if (status->network_connected) {
        left_color = lv_color_hex(0x0D8A59);
        if (status->network_ssid[0] != '\0') {
            snprintf(left_text, sizeof(left_text), "已连接 %s", status->network_ssid);
        } else {
            strlcpy(left_text, "已连接 Wi-Fi", sizeof(left_text));
        }
    } else {
        strlcpy(left_text, "未连接 Wi-Fi", sizeof(left_text));
    }

    if (status->wifi_scan_in_progress) {
        strlcpy(right_text, "扫描中", sizeof(right_text));
        right_color = lv_color_hex(0x1768B7);
    } else {
        snprintf(right_text, sizeof(right_text), "%u APs", status->wifi_scan_count);
        right_color = status->wifi_scan_count > 0 ? lv_color_hex(0x20BF7A) : lv_color_hex(0x64758A);
    }

    display_text_set_color(s_wifi_connection_state_label, left_color, 0);
    display_text_set(s_wifi_connection_state_label, left_text);
    if (status->wifi_scan_in_progress) {
        lv_obj_add_flag(s_wifi_scan_count_label, LV_OBJ_FLAG_HIDDEN);
        display_text_set_color(s_wifi_scan_state_label, right_color, 0);
        display_text_set(s_wifi_scan_state_label, right_text);
    } else {
        lv_obj_add_flag(s_wifi_scan_state_label, LV_OBJ_FLAG_HIDDEN);
        display_text_set_color(s_wifi_scan_count_label, right_color, 0);
        display_text_set(s_wifi_scan_count_label, right_text);
        lv_obj_clear_flag(s_wifi_scan_count_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void display_update_main_page(const display_status_t *status)
{
    const char *device_id = "--";
    const char *display_device_id = "--";
    const char *pair_key = "";
    bool rtc_ready = false;
    lv_color_t connection_dot_color = lv_color_hex(0xBCCAD8);

    if (status == NULL) {
        return;
    }

    if (status->tirtc_device_id[0] != '\0') {
        device_id = status->tirtc_device_id;
    }
    if (device_id != NULL && device_id[0] != '\0') {
        display_device_id = device_id;
    }
    if (status->tirtc_device_secret[0] != '\0') {
        pair_key = status->tirtc_device_secret;
    }
    rtc_ready = status->rtc_connected || status->rtc_call_active;
    if (rtc_ready) {
        connection_dot_color = lv_color_hex(0x20BF7A);
    } else if (status->network_connected) {
        connection_dot_color = lv_color_hex(0xF59E0B);
    }

    if (s_uuid_label != NULL) {
        display_text_set(s_uuid_label, display_device_id);
    }
    if (s_main_device_id_label != NULL) {
        display_text_set(s_main_device_id_label, display_device_id);
    }
#if LV_USE_QRCODE
    if (s_device_qrcode != NULL || s_device_qr_overlay_qrcode != NULL) {
        char payload[DISPLAY_DEVICE_QR_PAYLOAD_MAX] = {0};
        char escaped_device_id[DISPLAY_DEVICE_QR_ESCAPED_VALUE_MAX] = {0};
        char escaped_pair_key[DISPLAY_DEVICE_QR_ESCAPED_VALUE_MAX] = {0};
        bool updated = false;

        display_json_escape(escaped_device_id, sizeof(escaped_device_id), device_id);
        display_json_escape(escaped_pair_key, sizeof(escaped_pair_key), pair_key);

        (void)snprintf(payload,
                       sizeof(payload),
                       "{\n  \"device_id\": \"%s\",\n  \"device_secret_key\": \"%s\"\n}",
                       escaped_device_id,
                       escaped_pair_key);
        if (strcmp(payload, s_device_qr_payload) != 0) {
            if (s_device_qrcode != NULL && display_update_device_qr_object(s_device_qrcode, payload)) {
                updated = true;
            } else if (s_device_qrcode != NULL) {
                ESP_LOGW(TAG, "device qr update failed");
            }
            if (s_device_qr_overlay_qrcode != NULL &&
                display_update_device_qr_object(s_device_qr_overlay_qrcode, payload)) {
                updated = true;
            } else if (s_device_qr_overlay_qrcode != NULL) {
                ESP_LOGW(TAG, "device qr fullscreen update failed");
            }
            if (updated) {
                strlcpy(s_device_qr_payload, payload, sizeof(s_device_qr_payload));
            }
        }
    }
#endif
    if (s_device_connection_dot != NULL) {
        lv_obj_set_style_bg_color(s_device_connection_dot, connection_dot_color, 0);
    }
    if (s_device_connection_value_label != NULL) {
        display_text_set(s_device_connection_value_label,
                          rtc_ready ? "已连接" : (status->network_connected ? "待连接" : "未连接"));
    }
    if (s_device_door_dot != NULL) {
        lv_obj_set_style_bg_color(s_device_door_dot,
                                  status->device_door_open ? lv_color_hex(0x20BF7A) : lv_color_hex(0xF59E0B),
                                  0);
    }
    if (s_device_door_value_label != NULL) {
        display_text_set(s_device_door_value_label, status->device_door_open ? "已开门" : "未开门");
    }
    if (s_device_receive_volume_label != NULL) {
        lv_label_set_text_fmt(s_device_receive_volume_label,
                              "%u",
                              (unsigned)status->audio_speaker_volume_percent);
    }
    if (s_device_receive_mute_label != NULL) {
        display_text_set(s_device_receive_mute_label,
                         status->audio_speaker_volume_percent == 0U ? "恢复" : "禁音");
    }
    if (s_device_send_volume_label != NULL) {
        lv_label_set_text_fmt(s_device_send_volume_label,
                              "%u",
                              (unsigned)status->audio_capture_gain_percent);
    }
    if (s_device_send_mute_label != NULL) {
        display_text_set(s_device_send_mute_label,
                         status->audio_capture_gain_percent == 0U ? "恢复" : "禁音");
    }
}

static void display_update_ai_chat_page(const display_status_t *status)
{
    const char *state_text = "待命";
    const char *caption_text = "";
    lv_color_t state_color = lv_color_hex(0x23C17D);
    lv_color_t caption_color = lv_color_hex(0x1768B7);
    uint8_t message_count = 0;
    const display_ai_chat_message_t *latest_message = NULL;
    bool ai_dialog_font_ready = false;
    bool show_new_chat_button = false;

    if (status == NULL) {
        return;
    }

    display_apply_ai_dialog_font_if_ready();
    ai_dialog_font_ready = ai_chat_font_is_ready();
    show_new_chat_button = display_ai_chat_should_show_new_chat_button(status);

    if (show_new_chat_button) {
        state_text = "休息";
        state_color = lv_color_hex(0x64758A);
    } else if (status->ai_chat_listening && !status->ai_chat_cloud_speaking) {
        state_text = "聆听";
        state_color = lv_color_hex(0x2F82D7);
    } else if (status->ai_chat_cloud_speaking) {
        state_text = "回复";
        state_color = lv_color_hex(0x23C17D);
    } else if (status->ai_chat_active) {
        state_text = "待命";
    } else if (status->ai_chat_state != 0) {
        state_text = "连接";
        state_color = lv_color_hex(0xF59E0B);
    }
    if (status->ai_chat_state == DISPLAY_AI_CHAT_STATE_ERROR) {
        state_text = "异常";
        state_color = lv_color_hex(0xE45757);
    }

    if (s_ai_status_label != NULL) {
        display_text_set_color(s_ai_status_label, state_color, 0);
        display_text_set(s_ai_status_label, state_text);
    }
    message_count = status->ai_chat_message_count > DISPLAY_AI_CHAT_MESSAGE_MAX ?
        DISPLAY_AI_CHAT_MESSAGE_MAX : status->ai_chat_message_count;

    for (int index = (int)message_count - 1; index >= 0; --index) {
        if (status->ai_chat_messages[index].text[0] != '\0') {
            latest_message = &status->ai_chat_messages[index];
            break;
        }
    }

    display_update_ai_avatar(status, latest_message);

    if (latest_message != NULL) {
        caption_text = latest_message->text;
        caption_color = latest_message->caption_type == DISPLAY_AI_CHAT_CAPTION_TYPE_ASR ?
            lv_color_hex(0x0D8A59) :
            lv_color_hex(0x1768B7);
    } else if (status->ai_chat_cloud_speaking && status->ai_chat_tts_caption[0] != '\0') {
        caption_text = status->ai_chat_tts_caption;
        caption_color = lv_color_hex(0x1768B7);
    } else if (status->ai_chat_listening && status->ai_chat_asr_caption[0] != '\0') {
        caption_text = status->ai_chat_asr_caption;
        caption_color = lv_color_hex(0x0D8A59);
    } else if (status->ai_chat_active) {
        caption_text = status->ai_chat_listening ? "聆听中" : "已连接";
        caption_color = state_color;
    } else if (status->ai_chat_state != DISPLAY_AI_CHAT_STATE_IDLE) {
        caption_text = "正在连接";
        caption_color = lv_color_hex(0xF59E0B);
    }

    display_set_ai_chat_scroll_button_visible(s_ai_scroll_up_btn, false);
    display_set_ai_chat_scroll_button_visible(s_ai_scroll_down_btn, false);
    display_set_ai_chat_scroll_button_visible(s_ai_scroll_bottom_btn, false);
    display_reset_ai_chat_visible_slots();

    if (show_new_chat_button) {
        if (s_ai_caption_bar != NULL) {
            lv_obj_add_flag(s_ai_caption_bar, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_ai_new_chat_btn != NULL) {
            lv_obj_clear_flag(s_ai_new_chat_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_ai_new_chat_btn);
        }
        return;
    }

    if (s_ai_new_chat_btn != NULL) {
        lv_obj_add_flag(s_ai_new_chat_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ai_caption_bar != NULL) {
        lv_obj_clear_flag(s_ai_caption_bar, LV_OBJ_FLAG_HIDDEN);
    }
    if (message_count > 0 && !ai_dialog_font_ready) {
        if (s_ai_status_label != NULL) {
            display_text_set_color(s_ai_status_label, lv_color_hex(0xF59E0B), 0);
            display_text_set(s_ai_status_label, "连接");
        }
        if (s_ai_single_caption_label != NULL) {
            display_text_set_color(s_ai_single_caption_label, lv_color_hex(0xF59E0B), 0);
            display_set_ai_chat_caption_label_text(s_ai_single_caption_label, "字幕加载中");
            display_layout_ai_single_caption("字幕加载中");
        }
        return;
    }

    if (s_ai_single_caption_label != NULL) {
        display_text_set_color(s_ai_single_caption_label, caption_color, 0);
        display_set_ai_chat_caption_label_text(s_ai_single_caption_label, caption_text);
        display_layout_ai_single_caption(caption_text);
    }
    return;
}

static void display_update_ai_chat_settings_page(const display_status_t *status)
{
    if (status == NULL) {
        status = &s_last_status;
    }

    if (s_ai_settings_mic_value_label != NULL) {
        lv_label_set_text_fmt(s_ai_settings_mic_value_label,
                              "%u",
                              (unsigned)status->audio_capture_gain_percent);
    }

    if (s_ai_settings_speaker_value_label != NULL) {
        lv_label_set_text_fmt(s_ai_settings_speaker_value_label,
                              "%u",
                              (unsigned)status->audio_speaker_volume_percent);
    }

    display_update_ai_avatar_choice_buttons(status->ai_chat_avatar);
}

static bool display_read_uint_after(const char *text, const char *needle, uint32_t *value)
{
    const char *cursor = NULL;

    if (text == NULL || needle == NULL || value == NULL) {
        return false;
    }

    cursor = strstr(text, needle);
    if (cursor == NULL) {
        return false;
    }
    cursor += strlen(needle);
    while (*cursor != '\0' && (*cursor < '0' || *cursor > '9')) {
        ++cursor;
    }
    if (*cursor == '\0') {
        return false;
    }

    *value = (uint32_t)strtoul(cursor, NULL, 10);
    return true;
}

static void display_update_network_test_page(const display_status_t *status)
{
    uint32_t latency_ms = 0;
    uint32_t success_percent = 0;
    char wifi_text[48] = {0};
    char latency_text[24] = {0};
    char loss_text[24] = {0};
    bool has_latency = false;
    bool has_success = false;
    bool service_ok = false;
    bool service_warn = false;
    bool network_ok = false;
    const lv_color_t ok_text = lv_color_hex(0x0B6B45);
    const lv_color_t ok_fill = lv_color_hex(0xEAF8F1);
    const lv_color_t ok_stroke = lv_color_hex(0xBFEAD4);
    const lv_color_t warn_text = lv_color_hex(0xF59E0B);
    const lv_color_t warn_fill = lv_color_hex(0xFFF2D8);
    const lv_color_t neutral_fill = lv_color_hex(0xFFFFFF);
    const lv_color_t neutral_stroke = lv_color_hex(0xD5E0EB);

    if (status == NULL ||
        s_network_summary_wifi_label == NULL ||
        s_network_summary_ip_label == NULL ||
        s_network_gateway_value_label == NULL ||
        s_network_dns_value_label == NULL ||
        s_network_wan_value_label == NULL ||
        s_network_service_row == NULL ||
        s_network_service_value_label == NULL ||
        s_network_loss_value_label == NULL ||
        s_network_result_box == NULL ||
        s_network_result_label == NULL ||
        s_network_result_detail_label == NULL) {
        return;
    }

    if (status->network_connected) {
        if (status->network_ssid[0] != '\0') {
            snprintf(wifi_text, sizeof(wifi_text), "Wi-Fi %s", status->network_ssid);
        } else {
            strlcpy(wifi_text, "Wi-Fi 已连接", sizeof(wifi_text));
        }
    } else {
        strlcpy(wifi_text, "Wi-Fi 未连接", sizeof(wifi_text));
    }
    display_text_set(s_network_summary_wifi_label, wifi_text);
    display_text_set(s_network_summary_ip_label,
                      status->network_ip_addr[0] != '\0' ? status->network_ip_addr : "IP --");

    display_text_set(s_network_gateway_value_label, status->network_connected ? "正常" : "等待");
    display_text_set_color(s_network_gateway_value_label,
                                status->network_connected ? lv_color_hex(0x0D8A59) : lv_color_hex(0xF59E0B),
                                0);
    display_text_set(s_network_dns_value_label, status->network_connected ? "正常" : "等待");
    display_text_set_color(s_network_dns_value_label,
                                status->network_connected ? lv_color_hex(0x0D8A59) : lv_color_hex(0xF59E0B),
                                0);

    has_latency = display_read_uint_after(status->ping_summary, "Ping", &latency_ms);
    has_success = display_read_uint_after(status->ping_summary, "Success", &success_percent);
    service_ok = has_latency && latency_ms <= 120U;
    service_warn = has_latency && latency_ms > 120U;
    network_ok = has_success && success_percent == 100U;

    if (status->ping_running) {
        display_text_set(s_network_wan_value_label, "测试中");
        display_text_set(s_network_service_value_label, "测试中");
        display_text_set(s_network_loss_value_label, "--");
        display_text_set(s_network_result_label, "正在测试网络");
        display_text_set(s_network_result_detail_label, "请稍候");
        lv_obj_set_style_bg_color(s_network_service_row, lv_color_hex(0xE7F1FB), 0);
        lv_obj_set_style_border_color(s_network_service_row, neutral_stroke, 0);
        lv_obj_set_style_bg_color(s_network_result_box, lv_color_hex(0xE7F1FB), 0);
        lv_obj_set_style_border_color(s_network_result_box, neutral_stroke, 0);
        display_text_set_color(s_network_result_label, lv_color_hex(0x1768B7), 0);
        display_text_set_color(s_network_result_detail_label, lv_color_hex(0x64758A), 0);
    } else if (!status->network_connected) {
        display_text_set(s_network_wan_value_label, "等待");
        display_text_set(s_network_service_value_label, "等待");
        display_text_set(s_network_loss_value_label, "--");
        display_text_set(s_network_result_label, "网络未连接");
        display_text_set(s_network_result_detail_label, "先连接 Wi-Fi");
        lv_obj_set_style_bg_color(s_network_service_row, neutral_fill, 0);
        lv_obj_set_style_border_color(s_network_service_row, neutral_stroke, 0);
        lv_obj_set_style_bg_color(s_network_result_box, warn_fill, 0);
        lv_obj_set_style_border_color(s_network_result_box, warn_text, 0);
        display_text_set_color(s_network_result_label, warn_text, 0);
        display_text_set_color(s_network_result_detail_label, lv_color_hex(0x64758A), 0);
    } else if (has_latency || has_success) {
        snprintf(latency_text, sizeof(latency_text), has_latency ? "%lu ms" : "--", (unsigned long)latency_ms);
        if (has_success && success_percent <= 100U) {
            snprintf(loss_text, sizeof(loss_text), "%lu%%", (unsigned long)(100U - success_percent));
        } else {
            strlcpy(loss_text, "--", sizeof(loss_text));
        }
        display_text_set(s_network_wan_value_label, has_success && success_percent > 0 ? "正常" : "异常");
        display_text_set(s_network_service_value_label, latency_text);
        display_text_set(s_network_loss_value_label, loss_text);
        display_text_set(s_network_result_label,
                          has_success && success_percent == 100U ? "基础网络正常" : "网络质量波动");
        display_text_set(s_network_result_detail_label,
                          has_latency && latency_ms > 120U ? "服务延迟略高" : "服务响应正常");
        lv_obj_set_style_bg_color(s_network_service_row,
                                  service_ok ? ok_fill : (service_warn ? warn_fill : neutral_fill),
                                  0);
        lv_obj_set_style_border_color(s_network_service_row,
                                      service_ok ? ok_stroke : (service_warn ? warn_text : neutral_stroke),
                                      0);
        lv_obj_set_style_bg_color(s_network_result_box, network_ok ? ok_fill : warn_fill, 0);
        lv_obj_set_style_border_color(s_network_result_box,
                                      network_ok ? ok_stroke : warn_text,
                                      0);
        display_text_set_color(s_network_result_label,
                                    network_ok ? ok_text : warn_text,
                                    0);
        display_text_set_color(s_network_result_detail_label,
                                    service_warn ? warn_text : ok_text,
                                    0);
    } else {
        display_text_set(s_network_wan_value_label, "未测试");
        display_text_set(s_network_service_value_label, "--");
        display_text_set(s_network_loss_value_label, "--");
        display_text_set(s_network_result_label, "基础网络待测");
        display_text_set(s_network_result_detail_label, "点击重测");
        lv_obj_set_style_bg_color(s_network_service_row, neutral_fill, 0);
        lv_obj_set_style_border_color(s_network_service_row, neutral_stroke, 0);
        lv_obj_set_style_bg_color(s_network_result_box, warn_fill, 0);
        lv_obj_set_style_border_color(s_network_result_box, warn_text, 0);
        display_text_set_color(s_network_result_label, warn_text, 0);
        display_text_set_color(s_network_result_detail_label, lv_color_hex(0x64758A), 0);
    }

    display_text_set_color(s_network_wan_value_label,
                                status->network_connected ? lv_color_hex(0x0D8A59) : lv_color_hex(0xF59E0B),
                                0);
    display_text_set_color(s_network_service_value_label,
                                service_warn ? warn_text : lv_color_hex(0x0D8A59),
                                0);
    display_text_set_color(s_network_loss_value_label,
                                has_success && success_percent < 100U ? lv_color_hex(0xF59E0B) : lv_color_hex(0x0D8A59),
                                0);
}

static void display_update_tirtc_config_page(const display_status_t *status)
{
    if (s_tirtc_config_value_labels[DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_ID] == NULL) {
        return;
    }

    for (uint8_t index = 0; index < DISPLAY_TIRTC_CONFIG_FIELD_COUNT; ++index) {
        if (s_tirtc_config_value_labels[index] != NULL) {
            display_text_set(s_tirtc_config_value_labels[index],
                              display_tirtc_config_field_value(status,
                                                               (display_tirtc_config_field_t)index));
        }
    }
}

static void display_update_call_page(const display_status_t *status)
{
#if LV_USE_QRCODE
    char payload[DISPLAY_CONTACT_QR_PAYLOAD_MAX] = {0};

    if (s_call_qrcode == NULL) {
        return;
    }
    if (!display_build_contact_qr_payload(payload, sizeof(payload), status)) {
        ESP_LOGW(TAG, "call qr payload build failed");
        return;
    }
    if (strcmp(payload, s_call_qr_payload) == 0) {
        return;
    }

    lv_res_t qr_ret = lv_qrcode_update(s_call_qrcode, payload, strlen(payload));
    if (qr_ret == LV_RES_OK) {
        strlcpy(s_call_qr_payload, payload, sizeof(s_call_qr_payload));
    } else {
        ESP_LOGW(TAG, "call qr update failed");
    }
#else
    (void)status;
#endif
}

static void display_update_wechat_page(const display_status_t *status)
{
#if LV_USE_QRCODE
    char payload[DISPLAY_CONTACT_QR_PAYLOAD_MAX] = {0};

    if (s_wechat_qrcode == NULL) {
        return;
    }
    if (!display_build_wechat_qr_payload(payload, sizeof(payload), status)) {
        ESP_LOGW(TAG, "wechat qr payload build failed");
        return;
    }
    if (strcmp(payload, s_wechat_qr_payload) == 0) {
        return;
    }

    lv_res_t qr_ret = lv_qrcode_update(s_wechat_qrcode, payload, strlen(payload));
    if (qr_ret == LV_RES_OK) {
        strlcpy(s_wechat_qr_payload, payload, sizeof(s_wechat_qr_payload));
    } else {
        ESP_LOGW(TAG, "wechat qr update failed");
    }
#else
    (void)status;
#endif
}

static void display_update_wechat_contact_list(const display_status_t *status)
{
    uint8_t visible_count = 0;
    uint8_t shown_count = 0;

    if (status == NULL) {
        status = &s_last_status;
    }

    visible_count = status->wechat_contact_count > DISPLAY_WECHAT_CONTACT_COUNT ?
        DISPLAY_WECHAT_CONTACT_COUNT : status->wechat_contact_count;

    for (uint8_t index = 0; index < DISPLAY_WECHAT_CONTACT_COUNT; ++index) {
        const char *open_id = "";
        bool show = index < visible_count &&
                    status->wechat_contacts[index].open_id[0] != '\0';

        if (show) {
            open_id = status->wechat_contacts[index].open_id;
            ++shown_count;
        }
        if (s_wechat_contact_open_id_labels[index] != NULL) {
            display_text_set(s_wechat_contact_open_id_labels[index], open_id);
        }
        if (s_wechat_contact_rows[index] != NULL) {
            if (show) {
                lv_obj_clear_flag(s_wechat_contact_rows[index], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_wechat_contact_rows[index], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (s_wechat_empty_label != NULL) {
        if (shown_count == 0) {
            lv_obj_clear_flag(s_wechat_empty_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_wechat_empty_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void display_update_call_active_page(const display_status_t *status)
{
    uint8_t mic = 0;
    uint8_t speaker = 0;
    int64_t elapsed_seconds = 0;
    char duration[8] = "00:00";

    if (status == NULL) {
        status = &s_last_status;
    }

    if (status->rtc_call_active && s_call_active_started_us == 0) {
        s_call_active_started_us = esp_timer_get_time();
    }

    if (s_call_active_started_us > 0) {
        elapsed_seconds = (esp_timer_get_time() - s_call_active_started_us) / 1000000LL;
        if (elapsed_seconds < 0) {
            elapsed_seconds = 0;
        }
        snprintf(duration,
                 sizeof(duration),
                 "%02lld:%02lld",
                 (long long)((elapsed_seconds / 60LL) % 100LL),
                 (long long)(elapsed_seconds % 60LL));
    }

    mic = status->audio_capture_gain_percent;
    speaker = status->audio_speaker_volume_percent;

    if (s_call_duration_label != NULL) {
        display_text_set(s_call_duration_label, duration);
    }
    if (s_call_mic_value_label != NULL) {
        lv_label_set_text_fmt(s_call_mic_value_label, "%u", (unsigned)mic);
    }
    if (s_call_speaker_value_label != NULL) {
        lv_label_set_text_fmt(s_call_speaker_value_label, "%u", (unsigned)speaker);
    }
}

static bool display_wechat_call_state_keeps_active_page(display_wechat_call_state_t state)
{
    switch (state) {
    case DISPLAY_WECHAT_CALL_STATE_CALLING:
    case DISPLAY_WECHAT_CALL_STATE_CONNECTING:
    case DISPLAY_WECHAT_CALL_STATE_IN_CALL:
    case DISPLAY_WECHAT_CALL_STATE_CLOSING:
        return true;
    case DISPLAY_WECHAT_CALL_STATE_IDLE:
    case DISPLAY_WECHAT_CALL_STATE_INCOMING:
    default:
        return false;
    }
}

static void display_update_wechat_active_page(const display_status_t *status)
{
    uint8_t mic = 0;
    uint8_t speaker = 0;
    int64_t elapsed_seconds = 0;
    char duration[16] = "呼叫中";

    if (status == NULL) {
        status = &s_last_status;
    }

    bool in_call = status->wechat_call_state == DISPLAY_WECHAT_CALL_STATE_IN_CALL ||
                   status->rtc_call_active;

    if (in_call && s_wechat_active_started_us == 0) {
        s_wechat_active_started_us = esp_timer_get_time();
    }
    if (!in_call) {
        s_wechat_active_started_us = 0;
    }

    if (in_call && s_wechat_active_started_us > 0) {
        elapsed_seconds = (esp_timer_get_time() - s_wechat_active_started_us) / 1000000LL;
        if (elapsed_seconds < 0) {
            elapsed_seconds = 0;
        }
        snprintf(duration,
                 sizeof(duration),
                 "%02lld:%02lld",
                 (long long)((elapsed_seconds / 60LL) % 100LL),
                 (long long)(elapsed_seconds % 60LL));
    }

    mic = status->audio_capture_gain_percent;
    speaker = status->audio_speaker_volume_percent;

    if (s_wechat_duration_label != NULL) {
        display_text_set(s_wechat_duration_label, duration);
    }
    if (s_wechat_mic_value_label != NULL) {
        lv_label_set_text_fmt(s_wechat_mic_value_label, "%u", (unsigned)mic);
    }
    if (s_wechat_speaker_value_label != NULL) {
        lv_label_set_text_fmt(s_wechat_speaker_value_label, "%u", (unsigned)speaker);
    }
}

static void display_update_wifi_connect_feedback(const display_status_t *status)
{
    if (!s_wifi_connect_pending) {
        return;
    }

    bool connect_target_matched = status->network_connected &&
                                  (s_wifi_connect_target_ssid[0] == '\0' ||
                                   status->network_ssid[0] == '\0' ||
                                   strcmp(status->network_ssid, s_wifi_connect_target_ssid) == 0);

    if (connect_target_matched) {
        s_wifi_connect_pending = false;
        s_wifi_connect_state = DISPLAY_WIFI_CONNECT_STATE_CONNECTED;
        display_set_password_border_color(lv_color_hex(0x2E8F6B));
        s_wifi_connect_target_ssid[0] = '\0';
        display_show_wifi_page();
        return;
    }

    if (status->network_connect_failed) {
        s_wifi_connect_pending = false;
        s_wifi_connect_state = DISPLAY_WIFI_CONNECT_STATE_FAILED;
        display_set_password_border_color(lv_color_hex(0xC8513C));
        s_wifi_connect_target_ssid[0] = '\0';
        return;
    }

    if ((esp_timer_get_time() - s_wifi_connect_request_us) > DISPLAY_WIFI_CONNECT_TIMEOUT_US) {
        s_wifi_connect_pending = false;
        s_wifi_connect_state = DISPLAY_WIFI_CONNECT_STATE_TIMEOUT;
        display_set_password_border_color(lv_color_hex(0xC8513C));
        s_wifi_connect_target_ssid[0] = '\0';
    }
}

static void display_update_test_page(const display_status_t *status)
{
    bool rtc_ready = false;
    bool connected = false;
    bool sending_audio = false;
    bool receiving_audio = false;
    lv_color_t ok_color = lv_color_hex(0x0D8A59);
    lv_color_t warn_color = lv_color_hex(0xF59E0B);

    if (status == NULL ||
        s_tirtc_test_session_label == NULL ||
        s_tirtc_test_link_label == NULL ||
        s_tirtc_test_token_value_label == NULL ||
        s_tirtc_test_room_value_label == NULL ||
        s_tirtc_test_tx_audio_value_label == NULL ||
        s_tirtc_test_rx_audio_value_label == NULL ||
        s_tirtc_test_disconnect_value_label == NULL ||
        s_tirtc_test_result_label == NULL ||
        s_tirtc_test_result_detail_label == NULL) {
        return;
    }

    rtc_ready = status->network_connected &&
                status->rtc_state != 0U &&
                status->rtc_state != 6U;
    connected = status->rtc_connected || status->rtc_call_active;
    sending_audio = status->rtc_tx_audio_fps > 0U || status->audio_input_level > 0U;
    receiving_audio = status->rtc_rx_audio_fps > 0U || status->audio_output_level > 0U;

    display_text_set(s_tirtc_test_session_label, connected ? "会话 已连接" : "会话 TEST");
    display_text_set(s_tirtc_test_link_label, status->rtc_call_active ? "音频通话中" : "音频链路");

    display_text_set(s_tirtc_test_token_value_label, status->network_connected ? "就绪" : "等待");
    display_text_set_color(s_tirtc_test_token_value_label,
                                status->network_connected ? ok_color : warn_color,
                                0);

    display_text_set(s_tirtc_test_room_value_label, connected || rtc_ready ? "正常" : "等待");
    display_text_set_color(s_tirtc_test_room_value_label,
                                connected || rtc_ready ? ok_color : warn_color,
                                0);

    display_text_set(s_tirtc_test_tx_audio_value_label, sending_audio ? "有输入" : "等待");
    display_text_set_color(s_tirtc_test_tx_audio_value_label,
                                sending_audio ? ok_color : warn_color,
                                0);

    display_text_set(s_tirtc_test_rx_audio_value_label, receiving_audio ? "有输出" : "等待");
    display_text_set_color(s_tirtc_test_rx_audio_value_label,
                                receiving_audio ? ok_color : warn_color,
                                0);

    display_text_set(s_tirtc_test_disconnect_value_label, connected ? "可断开" : "就绪");
    display_text_set_color(s_tirtc_test_disconnect_value_label, ok_color, 0);

    if (!status->network_connected) {
        display_text_set(s_tirtc_test_result_label, "网络未连接");
        display_text_set(s_tirtc_test_result_detail_label, "先连接 Wi-Fi");
    } else if (connected || status->test_running) {
        display_text_set(s_tirtc_test_result_label, "测试进行中");
        display_text_set(s_tirtc_test_result_detail_label,
                          receiving_audio ? "音频收发正常" : "等待 App 发送音频");
    } else {
        display_text_set(s_tirtc_test_result_label, "测试就绪");
        display_text_set(s_tirtc_test_result_detail_label, "点击开始");
    }
}

static void display_update_ota_page(const display_status_t *status)
{
    char progress_text[16] = {0};
    const char *current_version = "--";
    const char *target_version = "--";
    bool ready_to_reboot = false;
    bool checking = false;
    bool upgrading = false;
    bool failed = false;
    bool up_to_date = false;

    if (status == NULL || s_ota_status_label == NULL ||
        s_ota_version_label == NULL || s_ota_second_label == NULL ||
        s_ota_second_value_label == NULL || s_ota_progress_bar == NULL ||
        s_ota_start_btn == NULL || s_ota_start_btn_label == NULL ||
        s_ota_reboot_btn == NULL || s_ota_reboot_btn_label == NULL ||
        s_ota_action_panel == NULL || s_ota_progress_title_label == NULL ||
        s_ota_progress_percent_label == NULL || s_ota_progress_hint_label == NULL) {
        return;
    }

    ready_to_reboot = status->ota_state == DISPLAY_OTA_STATE_READY_TO_REBOOT;
    checking = status->ota_state == DISPLAY_OTA_STATE_CHECKING;
    upgrading = status->ota_state == DISPLAY_OTA_STATE_DOWNLOADING ||
                status->ota_state == DISPLAY_OTA_STATE_VERIFYING;
    failed = status->ota_state == DISPLAY_OTA_STATE_FAILED;
    up_to_date = status->ota_state == DISPLAY_OTA_STATE_IDLE &&
                 strstr(status->ota_message, "No update") != NULL;
    current_version = status->ota_current_version[0] != '\0' ? status->ota_current_version : "--";
    target_version = status->ota_target_version[0] != '\0' ? status->ota_target_version : "--";

    display_text_set(s_ota_version_label, current_version);
    display_text_set(s_ota_second_label,
                      status->ota_target_version[0] != '\0' || ready_to_reboot || failed ? "目标版本" : "协议版本");
    display_text_set(s_ota_second_value_label,
                      status->ota_target_version[0] != '\0' || ready_to_reboot || failed ? target_version : DISPLAY_TIRTC_VERSION_TEXT);

    lv_obj_add_flag(s_ota_action_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ota_progress_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ota_progress_title_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ota_progress_percent_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ota_progress_hint_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ota_start_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ota_reboot_btn, LV_OBJ_FLAG_HIDDEN);

    if (checking) {
        display_text_set(s_ota_status_label, "检查中");
        display_text_set_color(s_ota_status_label, lv_color_hex(0x1768B7), 0);
        lv_obj_clear_flag(s_ota_action_panel, LV_OBJ_FLAG_HIDDEN);
        display_text_set_layout(s_ota_progress_title_label, 114, 20, 150, LV_TEXT_ALIGN_LEFT);
        display_text_set(s_ota_progress_title_label, "正在检查更新");
        lv_obj_clear_flag(s_ota_progress_title_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (upgrading) {
        uint8_t percent = status->ota_progress_percent > 100U ? 100U : status->ota_progress_percent;
        snprintf(progress_text, sizeof(progress_text), "%u%%", (unsigned)percent);
        display_text_set(s_ota_status_label, "升级中");
        display_text_set_color(s_ota_status_label, lv_color_hex(0x0D8A59), 0);
        lv_obj_clear_flag(s_ota_action_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ota_progress_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ota_progress_title_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ota_progress_percent_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ota_progress_hint_label, LV_OBJ_FLAG_HIDDEN);
        display_text_set_layout(s_ota_progress_title_label, 10, 7, 180, LV_TEXT_ALIGN_LEFT);
        display_text_set(s_ota_progress_title_label,
                          status->ota_state == DISPLAY_OTA_STATE_VERIFYING ? "正在校验固件" : "正在升级固件");
        display_text_set(s_ota_progress_percent_label, progress_text);
        display_text_set(s_ota_progress_hint_label, "升级中请保持供电");
        lv_bar_set_value(s_ota_progress_bar, percent, LV_ANIM_OFF);
        return;
    }

    if (ready_to_reboot) {
        display_text_set(s_ota_status_label, "待重启");
        display_text_set_color(s_ota_status_label, lv_color_hex(0x0D8A59), 0);
        lv_obj_clear_flag(s_ota_reboot_btn, LV_OBJ_FLAG_HIDDEN);
        display_text_set(s_ota_reboot_btn_label, "重启生效");
        return;
    }

    lv_obj_clear_flag(s_ota_start_btn, LV_OBJ_FLAG_HIDDEN);
    if (failed) {
        display_text_set(s_ota_status_label, "升级失败");
        display_text_set_color(s_ota_status_label, lv_color_hex(0xE45656), 0);
        lv_obj_set_style_bg_color(s_ota_start_btn, lv_color_hex(0xFFF1F1), 0);
        lv_obj_set_style_bg_color(s_ota_start_btn, lv_color_hex(0xFFE0E0), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(s_ota_start_btn, lv_color_hex(0xE45656), 0);
        display_text_set_color(s_ota_start_btn, lv_color_hex(0xE45656), 0);
        display_text_set_color(s_ota_start_btn_label, lv_color_hex(0xE45656), 0);
        display_text_set(s_ota_start_btn_label, "重新检查");
    } else if (up_to_date) {
        display_text_set(s_ota_status_label, "已是最新");
        display_text_set_color(s_ota_status_label, lv_color_hex(0x0D8A59), 0);
        lv_obj_set_style_bg_color(s_ota_start_btn, lv_color_hex(0xE7F1FB), 0);
        lv_obj_set_style_bg_color(s_ota_start_btn, lv_color_hex(0xD7EAFB), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(s_ota_start_btn, lv_color_hex(0x1768B7), 0);
        display_text_set_color(s_ota_start_btn, lv_color_hex(0x10243E), 0);
        display_text_set_color(s_ota_start_btn_label, lv_color_hex(0x10243E), 0);
        display_text_set(s_ota_start_btn_label, "重新检查");
    } else {
        display_text_set(s_ota_status_label,
                          status->ota_target_version[0] != '\0' ? "可升级" : "可检查");
        display_text_set_color(s_ota_status_label,
                                    status->ota_target_version[0] != '\0' ? lv_color_hex(0xF59E0B) : lv_color_hex(0x0D8A59),
                                    0);
        lv_obj_set_style_bg_color(s_ota_start_btn, lv_color_hex(0x20BF7A), 0);
        lv_obj_set_style_bg_color(s_ota_start_btn, lv_color_hex(0x0D8A59), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(s_ota_start_btn, lv_color_hex(0x20BF7A), 0);
        display_text_set_color(s_ota_start_btn, lv_color_hex(0xFFFFFF), 0);
        display_text_set_color(s_ota_start_btn_label, lv_color_hex(0xFFFFFF), 0);
        display_text_set(s_ota_start_btn_label,
                          status->ota_target_version[0] != '\0' ? "立即升级" : "检查更新");
    }
}

static lv_obj_t *display_create_header(lv_obj_t *page,
                                                const char *title,
                                                lv_event_cb_t back_cb)
{
    lv_obj_t *header = lv_obj_create(page);
    lv_obj_t *label = NULL;
    bool is_device_header = strcmp(title, "TiRTC+ESP32-S3") == 0;

    lv_obj_set_pos(header, 8, 6);
    lv_obj_set_size(header, 304, 28);
    lv_obj_set_style_bg_color(header, lv_color_hex(0xF5E7D3), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    if (back_cb != NULL) {
        lv_obj_t *btn = lv_btn_create(header);
        lv_obj_set_pos(btn, 0, 0);
        lv_obj_set_size(btn, 54, 28);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xD97135), 0);
        lv_obj_add_event_cb(btn, back_cb, LV_EVENT_CLICKED, NULL);
        label = lv_label_create(btn);
        display_text_set(label, "Back");
        lv_obj_center(label);
    }

    label = lv_label_create(header);
    lv_obj_set_width(label, is_device_header ? 172 : 120);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    display_text_set_color(label, lv_color_hex(0x183642), 0);
    lv_obj_set_style_text_font(label, display_ascii_font(is_device_header ? 14U : 12U), 0);
    display_text_set(label, title);
    if (is_device_header) {
        lv_obj_align(label, LV_ALIGN_CENTER, -20, 0);
    } else {
        lv_obj_center(label);
    }
    return header;
}

static lv_obj_t *display_create_home_img(lv_obj_t *parent,
                                                  const lv_img_dsc_t *src,
                                                  lv_coord_t x,
                                                  lv_coord_t y)
{
    lv_obj_t *img = lv_img_create(parent);

    lv_img_set_src(img, src);
    lv_obj_set_pos(img, x, y);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);

    return img;
}

static lv_obj_t *display_create_home_centered_img(lv_obj_t *parent,
                                                           const lv_img_dsc_t *src,
                                                           lv_coord_t area_x,
                                                           lv_coord_t area_y,
                                                           lv_coord_t area_w,
                                                           lv_coord_t area_h)
{
    lv_coord_t img_w = (lv_coord_t)src->header.w;
    lv_coord_t img_h = (lv_coord_t)src->header.h;
    lv_coord_t x = area_x + ((area_w - img_w) / 2);
    lv_coord_t y = area_y + ((area_h - img_h) / 2);

    return display_create_home_img(parent, src, x, y);
}

static lv_obj_t *display_create_home_signal_bar(lv_obj_t *parent,
                                                         lv_coord_t x,
                                                         lv_coord_t y,
                                                         lv_coord_t height)
{
    lv_obj_t *bar = lv_obj_create(parent);

    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, 4, height);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x20BF7A), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    return bar;
}

static lv_obj_t *display_create_home_wifi_x_line(lv_obj_t *parent,
                                                          const lv_point_t *points)
{
    lv_obj_t *line = lv_line_create(parent);

    lv_line_set_points(line, points, 2);
    lv_obj_set_pos(line, 313, 6);
    lv_obj_set_style_line_width(line, 2, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(0xF6494C), 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);

    return line;
}

static void display_update_home_clock(void)
{
    char time_text[9] = "--:--:--";
    time_t now = 0;
    struct tm local_tm = {0};
    static bool timezone_configured = false;

    if (s_home_time_label == NULL) {
        return;
    }

    if (!timezone_configured) {
        setenv("TZ", "CST-8", 1);
        tzset();
        timezone_configured = true;
    }

    time(&now);
    if (now == s_home_clock_last_second) {
        return;
    }
    s_home_clock_last_second = now;

    if (now >= (time_t)DISPLAY_MIN_VALID_UNIX_TIME &&
        localtime_r(&now, &local_tm) != NULL) {
        snprintf(time_text,
                 sizeof(time_text),
                 "%02d:%02d:%02d",
                 local_tm.tm_hour,
                 local_tm.tm_min,
                 local_tm.tm_sec);
    }

    display_text_set(s_home_time_label, time_text);
}

static uint8_t display_home_wifi_level(const display_status_t *status)
{
    return display_wifi_status_level(status);
}

static void display_update_home_wifi_status(const display_status_t *status)
{
    uint8_t level = display_home_wifi_level(status);
    bool connected = level > 0;

    if (s_home_wifi_status_valid &&
        s_home_wifi_connected == connected &&
        s_home_wifi_level == level) {
        return;
    }
    s_home_wifi_status_valid = true;
    s_home_wifi_connected = connected;
    s_home_wifi_level = level;

    for (uint8_t index = 0; index < 3; ++index) {
        if (s_home_wifi_bars[index] == NULL) {
            continue;
        }

        lv_obj_set_style_bg_color(s_home_wifi_bars[index],
                                  (connected && index < level)
                                      ? lv_color_hex(0x20BF7A)
                                      : lv_color_hex(0xBCCAD8),
                                  0);
    }

    for (uint8_t index = 0; index < 2; ++index) {
        if (s_home_wifi_x_lines[index] == NULL) {
            continue;
        }
        if (connected) {
            lv_obj_add_flag(s_home_wifi_x_lines[index], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_home_wifi_x_lines[index], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void display_update_home_status_bar(const display_status_t *status)
{
    display_update_home_clock();
    display_update_home_wifi_status(status);
    display_update_wifi_indicators(status);
}

static void display_create_home_header(lv_obj_t *parent)
{
    lv_obj_t *header = lv_obj_create(parent);
    static const lv_point_t wifi_x_line_a[] = {
        {0, 0},
        {6, 6},
    };
    static const lv_point_t wifi_x_line_b[] = {
        {6, 0},
        {0, 6},
    };

    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, DISPLAY_DRIVER_WIDTH, 28);
    lv_obj_set_style_bg_color(header, lv_color_hex(0xF7FBFF), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_color(header, lv_color_hex(0xD5E0EB), 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    s_home_time_label = lv_label_create(header);
    lv_obj_set_pos(s_home_time_label, 8, 6);
    lv_obj_set_width(s_home_time_label, 58);
    lv_label_set_long_mode(s_home_time_label, LV_LABEL_LONG_CLIP);
    display_text_set_color(s_home_time_label, lv_color_hex(0x10243E), 0);
#if LV_FONT_MONTSERRAT_12
    lv_obj_set_style_text_font(s_home_time_label, &lv_font_montserrat_12, 0);
#endif
    display_text_set(s_home_time_label, "--:--:--");

    display_create_home_centered_img(header, &home_text_title_img, 66, 0, 188, 28);

    s_home_wifi_bars[0] = display_create_home_signal_bar(header, 294, 16, 6);
    s_home_wifi_bars[1] = display_create_home_signal_bar(header, 301, 12, 10);
    s_home_wifi_bars[2] = display_create_home_signal_bar(header, 308, 8, 14);
    s_home_wifi_x_lines[0] = display_create_home_wifi_x_line(header, wifi_x_line_a);
    s_home_wifi_x_lines[1] = display_create_home_wifi_x_line(header, wifi_x_line_b);
    display_update_home_status_bar(&s_last_status);
}

static lv_obj_t *display_create_home_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);

    lv_obj_remove_style_all(page);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_size(page, DISPLAY_DRIVER_WIDTH, 212);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    return page;
}

static lv_obj_t *display_create_home_nav_button(lv_obj_t *parent,
                                                bool next_page,
                                                lv_event_cb_t cb)
{
    static const lv_point_t left_points[] = {
        {15, 0},
        {3, 10},
        {15, 20},
        {15, 0},
    };
    static const lv_point_t right_points[] = {
        {3, 0},
        {15, 10},
        {3, 20},
        {3, 0},
    };
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_t *icon = NULL;

    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, next_page ? 270 : 0, 190);
    lv_obj_set_size(btn, 50, 50);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_STATE_DISABLED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xD5E0EB), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }

    icon = lv_line_create(btn);
    lv_line_set_points(icon, next_page ? right_points : left_points, 4);
    lv_obj_set_style_line_color(icon, lv_color_hex(0x1768B7), 0);
    lv_obj_set_style_line_width(icon, 4, 0);
    lv_obj_set_style_line_rounded(icon, true, 0);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(icon);

    return btn;
}

static lv_obj_t *display_create_home_app_tile(lv_obj_t *parent,
                                                       lv_coord_t x,
                                                       const lv_img_dsc_t *icon_src,
                                                       const lv_img_dsc_t *title_src,
                                                       const lv_img_dsc_t *subtitle_src,
                                                       lv_color_t accent,
                                                       lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_t *top_bar = NULL;

    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, 8);
    lv_obj_set_size(btn, 96, 172);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xF7FBFF), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xD5E0EB), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xB9CADB), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_translate_y(btn, 1, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }

    top_bar = lv_obj_create(btn);
    lv_obj_remove_style_all(top_bar);
    lv_obj_set_pos(top_bar, 7, 18);
    lv_obj_set_size(top_bar, 82, 5);
    lv_obj_set_style_radius(top_bar, 8, 0);
    lv_obj_set_style_bg_color(top_bar, accent, 0);
    lv_obj_set_style_bg_opa(top_bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *icon = display_create_home_img(btn, icon_src, 22, 40);
    if (icon != NULL) {
        lv_img_set_pivot(icon,
                         (lv_coord_t)icon_src->header.w / 2,
                         (lv_coord_t)icon_src->header.h / 2);
        lv_img_set_zoom(icon, DISPLAY_HOME_ICON_ZOOM);
    }
    display_create_home_centered_img(btn, title_src, 7, 100, 82, 22);
    display_create_home_centered_img(btn, subtitle_src, 7, 130, 82, 14);

    return btn;
}

static void display_build_home_page(lv_obj_t *screen)
{
    lv_obj_t *home_page_1 = NULL;
    lv_obj_t *home_page_2 = NULL;
    lv_obj_t *track = NULL;

    s_home_page = lv_obj_create(screen);
    lv_obj_remove_style_all(s_home_page);
    lv_obj_set_size(s_home_page, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_color(s_home_page, lv_color_hex(0xF3F8FB), 0);
    lv_obj_set_style_bg_opa(s_home_page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_home_page, 0, 0);
    lv_obj_clear_flag(s_home_page, LV_OBJ_FLAG_SCROLLABLE);

    display_create_home_header(s_home_page);

    s_home_carousel = lv_obj_create(s_home_page);
    lv_obj_remove_style_all(s_home_carousel);
    lv_obj_set_pos(s_home_carousel, 0, 28);
    lv_obj_set_size(s_home_carousel, DISPLAY_DRIVER_WIDTH, 212);
    lv_obj_set_style_bg_opa(s_home_carousel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_home_carousel, 0, 0);
    lv_obj_clear_flag(s_home_carousel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_home_carousel, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(s_home_carousel, LV_SCROLLBAR_MODE_OFF);

    home_page_1 = display_create_home_page(s_home_carousel);
    s_home_content_pages[0] = home_page_1;
    display_create_home_app_tile(home_page_1,
                                          8,
                                          &home_icon_view_img,
                                          &home_text_view_img,
                                          &home_text_view_desc_img,
                                          lv_color_hex(0xF6494C),
                                          display_home_view_btn_cb);
    display_create_home_app_tile(home_page_1,
                                          112,
                                          &home_icon_call_img,
                                          &home_text_call_img,
                                          &home_text_call_desc_img,
                                          lv_color_hex(0x1296DB),
                                          display_home_call_btn_cb);
    display_create_home_app_tile(home_page_1,
                                          216,
                                          &home_icon_wechat_img,
                                          &home_text_wechat_img,
                                          &home_text_wechat_desc_img,
                                          lv_color_hex(0x24DB5A),
                                          display_home_wechat_btn_cb);

    home_page_2 = display_create_home_page(s_home_carousel);
    s_home_content_pages[1] = home_page_2;
    lv_obj_add_flag(home_page_2, LV_OBJ_FLAG_HIDDEN);
    display_create_home_app_tile(home_page_2,
                                          8,
                                          &home_icon_ai_img,
                                          &home_text_ai_img,
                                          &home_text_ai_desc_img,
                                          lv_color_hex(0x009D9A),
                                          display_home_ai_btn_cb);
    display_create_home_app_tile(home_page_2,
                                          112,
                                          &home_icon_settings_img,
                                          &home_text_settings_img,
                                          &home_text_settings_desc_img,
                                          lv_color_hex(0x64758A),
                                          display_home_settings_btn_cb);

    track = lv_obj_create(s_home_page);
    lv_obj_remove_style_all(track);
    lv_obj_set_pos(track, 142, 220);
    lv_obj_set_size(track, 37, 8);
    lv_obj_set_style_bg_opa(track, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (uint8_t index = 0; index < 2; ++index) {
        lv_obj_t *dot = lv_obj_create(track);
        lv_obj_remove_style_all(dot);
        s_home_indicator_dots[index] = dot;
        lv_obj_set_style_radius(dot, 8, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    s_home_nav_prev_btn = display_create_home_nav_button(s_home_page, false, display_home_prev_btn_cb);
    s_home_nav_next_btn = display_create_home_nav_button(s_home_page, true, display_home_next_btn_cb);
    s_home_indicator_second_page = false;
    s_home_indicator_valid = false;
    display_update_home_indicators();
    display_build_binding_prompt_overlay(s_home_page);
    display_update_binding_prompt(&s_last_status);
}

static void display_build_system_page(lv_obj_t *screen)
{
    s_system_page = lv_obj_create(screen);
    display_prepare_figma_page(s_system_page);
    lv_obj_add_flag(s_system_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_system_page,
                                      "设置",
                                      display_system_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x000000),
                                      NULL);

    s_system_ram_label = display_create_figma_text(s_system_page,
                                                   "RAM --K / --K",
                                                   12,
                                                   29,
                                                   296,
                                                   lv_color_hex(0x64758A),
                                                   10,
                                                   LV_TEXT_ALIGN_RIGHT);
    display_update_system_ram_label();

    display_create_settings_row(s_system_page, 42, "Wi-Fi 设置", display_system_wifi_btn_cb);
    display_create_settings_row(s_system_page, 80, "网络测试", display_system_network_test_btn_cb);
    display_create_settings_row(s_system_page, 118, "TiRTC 配置", display_system_tirtc_config_btn_cb);
    display_create_settings_row(s_system_page, 156, "TiRTC 测试", display_system_tirtc_test_btn_cb);
    display_create_settings_row(s_system_page, 194, "关于 / OTA", display_system_ota_btn_cb);
}

static void display_build_call_page(lv_obj_t *screen)
{
    lv_obj_t *qr_card = NULL;

    s_call_page = lv_obj_create(screen);
    display_prepare_figma_page(s_call_page);
    lv_obj_add_flag(s_call_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_call_page,
                                      "呼叫",
                                      display_call_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x21C783),
                                      NULL);

    display_create_call_menu_button(s_call_page,
                                    8,
                                    36,
                                    104,
                                    92,
                                    "添加联系人",
                                    NULL,
                                    true,
                                    display_call_add_btn_cb);
    display_create_call_menu_button(s_call_page,
                                    8,
                                    140,
                                    104,
                                    92,
                                    "联系人列表",
                                    NULL,
                                    false,
                                    display_call_list_btn_cb);

    qr_card = display_create_figma_box(s_call_page,
                                       120,
                                       36,
                                       192,
                                       196,
                                       lv_color_hex(0xF7FBFE),
                                       lv_color_hex(0xD6E4EF),
                                       8);
    lv_obj_set_style_shadow_width(qr_card, 8, 0);
    lv_obj_set_style_shadow_ofs_y(qr_card, 2, 0);
    lv_obj_set_style_shadow_color(qr_card, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_shadow_opa(qr_card, LV_OPA_10, 0);
    display_create_call_qr(qr_card, 4, 6, 184);
}

static void display_build_wechat_page(lv_obj_t *screen)
{
    lv_obj_t *qr_card = NULL;

    s_wechat_page = lv_obj_create(screen);
    display_prepare_figma_page(s_wechat_page);
    lv_obj_add_flag(s_wechat_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_wechat_page,
                                      "微信呼叫",
                                      display_call_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x21C783),
                                      NULL);

    display_create_call_menu_button(s_wechat_page,
                                    8,
                                    36,
                                    104,
                                    92,
                                    "添加微信",
                                    "联系人",
                                    true,
                                    display_wechat_add_btn_cb);
    display_create_call_menu_button(s_wechat_page,
                                    8,
                                    140,
                                    104,
                                    92,
                                    "微信",
                                    "联系人",
                                    false,
                                    display_wechat_list_btn_cb);

    qr_card = display_create_figma_box(s_wechat_page,
                                       120,
                                       36,
                                       192,
                                       196,
                                       lv_color_hex(0xF7FBFE),
                                       lv_color_hex(0xD6E4EF),
                                       8);
    lv_obj_set_style_shadow_width(qr_card, 8, 0);
    lv_obj_set_style_shadow_ofs_y(qr_card, 2, 0);
    lv_obj_set_style_shadow_color(qr_card, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_shadow_opa(qr_card, LV_OPA_10, 0);
    display_create_wechat_qr(qr_card, 4, 6, 184);
}

static void display_build_call_add_page(lv_obj_t *screen)
{
    s_call_add_page = lv_obj_create(screen);
    display_prepare_figma_page(s_call_add_page);
    lv_obj_add_flag(s_call_add_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_call_add_page,
                                      "添加联系人",
                                      display_call_child_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x21C783),
                                      NULL);

    s_call_add_value_labels[DISPLAY_CALL_ADD_FIELD_DEVICE_ID] =
        display_create_call_add_field_row(s_call_add_page, 42, DISPLAY_CALL_ADD_FIELD_DEVICE_ID);
    s_call_add_value_labels[DISPLAY_CALL_ADD_FIELD_PAIR_KEY] =
        display_create_call_add_field_row(s_call_add_page, 84, DISPLAY_CALL_ADD_FIELD_PAIR_KEY);
    display_create_figma_button(s_call_add_page,
                                8,
                                136,
                                148,
                                34,
                                lv_color_hex(0x21C783),
                                lv_color_hex(0x21C783),
                                "扫码添加联系人",
                                lv_color_hex(0xFFFFFF),
                                12,
                                display_call_scan_btn_cb);
    display_create_figma_button(s_call_add_page,
                                164,
                                136,
                                148,
                                34,
                                lv_color_hex(0xE9F5FF),
                                lv_color_hex(0x2F82D7),
                                "查看扫码信息",
                                lv_color_hex(0x2F82D7),
                                12,
                                display_call_scan_info_btn_cb);
    display_create_figma_button(s_call_add_page,
                                8,
                                180,
                                304,
                                34,
                                lv_color_hex(0x21C783),
                                lv_color_hex(0x21C783),
                                "确认添加",
                                lv_color_hex(0xFFFFFF),
                                12,
                                display_call_confirm_add_btn_cb);

    display_update_call_add_field_labels();
    display_create_call_scan_info_overlay(s_call_add_page);
}

static void display_build_call_add_edit_page(lv_obj_t *screen)
{
    s_call_add_edit_page = lv_obj_create(screen);
    display_prepare_figma_page(s_call_add_edit_page);
    lv_obj_add_flag(s_call_add_edit_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_call_add_edit_page,
                                      "添加联系人",
                                      display_call_add_edit_back_btn_cb,
                                      "保存",
                                      lv_color_hex(0x20BF7A),
                                      display_call_add_edit_save_btn_cb);

    s_call_add_edit_hint_label = display_create_figma_text(s_call_add_edit_page,
                                                           "Device ID",
                                                           8,
                                                           36,
                                                           196,
                                                           lv_color_hex(0x64758A),
                                                           12,
                                                           LV_TEXT_ALIGN_LEFT);
    s_call_add_edit_length_label = display_create_figma_text(s_call_add_edit_page,
                                                             "0/63",
                                                             230,
                                                             36,
                                                             82,
                                                             lv_color_hex(0x64758A),
                                                             12,
                                                             LV_TEXT_ALIGN_RIGHT);

    s_call_add_edit_ta = lv_textarea_create(s_call_add_edit_page);
    lv_obj_set_pos(s_call_add_edit_ta, 8, DISPLAY_UUID_INPUT_TOP);
    lv_obj_set_size(s_call_add_edit_ta, DISPLAY_UUID_INPUT_WIDTH, DISPLAY_UUID_INPUT_HEIGHT);
    lv_textarea_set_one_line(s_call_add_edit_ta, true);
    lv_textarea_set_max_length(s_call_add_edit_ta, DISPLAY_CALL_CONTACT_PAIR_KEY_MAX - 1U);
    lv_textarea_set_placeholder_text(s_call_add_edit_ta, "Device ID");
    lv_obj_set_style_radius(s_call_add_edit_ta, 8, 0);
    lv_obj_set_style_border_width(s_call_add_edit_ta, 1, 0);
    lv_obj_set_style_border_color(s_call_add_edit_ta, lv_color_hex(0xD5E0EB), 0);
    lv_obj_set_style_bg_color(s_call_add_edit_ta, lv_color_hex(0xFFFFFF), 0);
    display_text_set_color(s_call_add_edit_ta, lv_color_hex(0x10243E), 0);
    lv_obj_set_style_text_font(s_call_add_edit_ta, display_ascii_font(12), 0);
    lv_obj_set_style_pad_left(s_call_add_edit_ta, 12, 0);
    lv_obj_set_style_pad_right(s_call_add_edit_ta, 12, 0);
    lv_obj_add_event_cb(s_call_add_edit_ta,
                        display_call_add_edit_textarea_event_cb,
                        LV_EVENT_ALL,
                        NULL);

    s_call_add_edit_status_label = display_create_figma_text(s_call_add_edit_page,
                                                             "点击保存生效",
                                                             8,
                                                             DISPLAY_UUID_STATUS_TOP,
                                                             DISPLAY_UUID_STATUS_WIDTH,
                                                             lv_color_hex(0x0D8A59),
                                                             12,
                                                             LV_TEXT_ALIGN_LEFT);

    s_call_add_edit_keyboard = lv_keyboard_create(s_call_add_edit_page);
    lv_obj_set_pos(s_call_add_edit_keyboard, DISPLAY_UUID_KEYBOARD_LEFT, DISPLAY_UUID_KEYBOARD_TOP);
    lv_obj_set_size(s_call_add_edit_keyboard, DISPLAY_UUID_KEYBOARD_WIDTH, DISPLAY_UUID_KEYBOARD_HEIGHT);
    lv_obj_set_style_bg_opa(s_call_add_edit_keyboard, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_call_add_edit_keyboard, 0, 0);
    lv_obj_set_style_pad_row(s_call_add_edit_keyboard, 3, 0);
    lv_obj_set_style_pad_column(s_call_add_edit_keyboard, 2, 0);
    lv_obj_set_style_border_width(s_call_add_edit_keyboard, 0, 0);
    lv_obj_set_style_radius(s_call_add_edit_keyboard, 0, 0);
    lv_obj_set_style_pad_all(s_call_add_edit_keyboard, 0, LV_PART_ITEMS);
    display_text_set_color(s_call_add_edit_keyboard, lv_color_hex(0x10243E), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_call_add_edit_keyboard, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_call_add_edit_keyboard,
                              lv_color_hex(0xD7EAFB),
                              LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_call_add_edit_keyboard, 5, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_call_add_edit_keyboard, 0, LV_PART_ITEMS);
    lv_keyboard_set_map(s_call_add_edit_keyboard,
                        LV_KEYBOARD_MODE_USER_1,
                        (const char **)s_wifi_keyboard_map_lc,
                        s_wifi_keyboard_ctrl_lc_map);
    lv_keyboard_set_map(s_call_add_edit_keyboard,
                        LV_KEYBOARD_MODE_USER_2,
                        (const char **)s_wifi_keyboard_map_uc,
                        s_wifi_keyboard_ctrl_uc_map);
    lv_keyboard_set_map(s_call_add_edit_keyboard,
                        LV_KEYBOARD_MODE_USER_3,
                        (const char **)s_wifi_keyboard_map_spec,
                        s_wifi_keyboard_ctrl_spec_map);
    lv_obj_remove_event_cb(s_call_add_edit_keyboard, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(s_call_add_edit_keyboard,
                        display_keyboard_value_event_cb,
                        LV_EVENT_VALUE_CHANGED,
                        NULL);
    lv_obj_add_event_cb(s_call_add_edit_keyboard,
                        display_keyboard_draw_part_event_cb,
                        LV_EVENT_DRAW_PART_BEGIN,
                        NULL);
    lv_obj_add_flag(s_call_add_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_call_add_edit_keyboard, display_keyboard_event_cb, LV_EVENT_ALL, NULL);
}

static void display_build_call_scan_page(lv_obj_t *screen)
{
    s_call_scan_page = lv_obj_create(screen);
    lv_obj_remove_style_all(s_call_scan_page);
    lv_obj_set_pos(s_call_scan_page, 0, 0);
    lv_obj_set_size(s_call_scan_page, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_color(s_call_scan_page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_call_scan_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_call_scan_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_call_scan_page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_call_scan_page, display_call_scan_tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_call_scan_page, LV_OBJ_FLAG_HIDDEN);

    s_call_scan_img = lv_img_create(s_call_scan_page);
    lv_obj_center(s_call_scan_img);
    lv_obj_add_flag(s_call_scan_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_call_scan_img, display_call_scan_tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_call_scan_img, LV_OBJ_FLAG_HIDDEN);
}

static void display_build_call_list_page(lv_obj_t *screen)
{
    s_call_list_page = lv_obj_create(screen);
    display_prepare_figma_page(s_call_list_page);
    lv_obj_add_flag(s_call_list_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_call_list_page,
                                      "联系人列表",
                                      display_call_child_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x21C783),
                                      NULL);

    for (uint8_t index = 0; index < s_call_contact_count; ++index) {
        display_create_call_contact_row(s_call_list_page, index, 36 + ((lv_coord_t)index * 50));
    }
}

static void display_build_call_active_page(lv_obj_t *screen)
{
    s_call_active_page = lv_obj_create(screen);
    display_prepare_figma_page(s_call_active_page);
    lv_obj_add_flag(s_call_active_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_call_active_page,
                                      "通话",
                                      display_call_child_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x21C783),
                                      NULL);

    (void)display_create_call_duration_row(s_call_active_page, 38, &s_call_duration_label);
    display_create_call_volume_row(s_call_active_page,
                                   80,
                                   "麦克风",
                                   "62",
                                   DISPLAY_CALL_VOLUME_MIC_DOWN,
                                   DISPLAY_CALL_VOLUME_MIC_UP,
                                   &s_call_mic_value_label,
                                   display_call_volume_btn_cb);
    display_create_call_volume_row(s_call_active_page,
                                   124,
                                   "扬声器",
                                   "70",
                                   DISPLAY_CALL_VOLUME_SPEAKER_DOWN,
                                   DISPLAY_CALL_VOLUME_SPEAKER_UP,
                                   &s_call_speaker_value_label,
                                   display_call_volume_btn_cb);
    lv_obj_t *hangup_btn = display_create_figma_button(s_call_active_page,
                                                       8,
                                                       168,
                                                       304,
                                                       38,
                                                       lv_color_hex(0xFFE7E7),
                                                       lv_color_hex(0xF15A5A),
                                                       "挂断",
                                                       lv_color_hex(0xE44747),
                                                       16,
                                                       display_call_hangup_btn_cb);
    lv_obj_set_style_radius(hangup_btn, 8, 0);
    display_update_call_active_page(&s_last_status);
}

static void display_build_wechat_add_page(lv_obj_t *screen)
{
    s_wechat_add_page = lv_obj_create(screen);
    display_prepare_figma_page(s_wechat_add_page);
    lv_obj_add_flag(s_wechat_add_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_wechat_add_page,
                                      "添加微信联系人",
                                      display_wechat_child_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x21C783),
                                      NULL);

    s_wechat_add_open_id_label = display_create_wechat_add_field_row(s_wechat_add_page, 42);
    display_create_figma_button(s_wechat_add_page,
                                8,
                                94,
                                148,
                                34,
                                lv_color_hex(0x21C783),
                                lv_color_hex(0x21C783),
                                "扫码添加",
                                lv_color_hex(0xFFFFFF),
                                12,
                                display_wechat_scan_btn_cb);
    display_create_figma_button(s_wechat_add_page,
                                164,
                                94,
                                148,
                                34,
                                lv_color_hex(0xE9F5FF),
                                lv_color_hex(0x2F82D7),
                                "查看扫码信息",
                                lv_color_hex(0x2F82D7),
                                12,
                                display_wechat_scan_info_btn_cb);
    display_create_figma_button(s_wechat_add_page,
                                8,
                                138,
                                304,
                                34,
                                lv_color_hex(0x21C783),
                                lv_color_hex(0x21C783),
                                "确认添加",
                                lv_color_hex(0xFFFFFF),
                                12,
                                display_wechat_confirm_add_btn_cb);
    display_update_wechat_add_field_label();
    display_create_wechat_scan_info_overlay(s_wechat_add_page);
}

static void display_build_wechat_add_edit_page(lv_obj_t *screen)
{
    s_wechat_add_edit_page = lv_obj_create(screen);
    display_prepare_figma_page(s_wechat_add_edit_page);
    lv_obj_add_flag(s_wechat_add_edit_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_wechat_add_edit_page,
                                      "添加微信联系人",
                                      display_wechat_add_edit_back_btn_cb,
                                      "保存",
                                      lv_color_hex(0x20BF7A),
                                      display_wechat_add_edit_save_btn_cb);

    s_wechat_add_edit_hint_label = display_create_figma_text(s_wechat_add_edit_page,
                                                             "OpenID",
                                                             8,
                                                             36,
                                                             196,
                                                             lv_color_hex(0x64758A),
                                                             12,
                                                             LV_TEXT_ALIGN_LEFT);
    s_wechat_add_edit_length_label = display_create_figma_text(s_wechat_add_edit_page,
                                                               "0/28",
                                                               230,
                                                               36,
                                                               82,
                                                               lv_color_hex(0x64758A),
                                                               12,
                                                               LV_TEXT_ALIGN_RIGHT);

    s_wechat_add_edit_ta = lv_textarea_create(s_wechat_add_edit_page);
    lv_obj_set_pos(s_wechat_add_edit_ta, 8, DISPLAY_UUID_INPUT_TOP);
    lv_obj_set_size(s_wechat_add_edit_ta, DISPLAY_UUID_INPUT_WIDTH, DISPLAY_UUID_INPUT_HEIGHT);
    lv_textarea_set_one_line(s_wechat_add_edit_ta, true);
    lv_textarea_set_max_length(s_wechat_add_edit_ta, DISPLAY_WECHAT_OPEN_ID_LENGTH);
    lv_textarea_set_accepted_chars(s_wechat_add_edit_ta, DISPLAY_WECHAT_OPEN_ID_ACCEPTED_CHARS);
    lv_textarea_set_placeholder_text(s_wechat_add_edit_ta, "28位微信Open ID");
    lv_obj_set_style_radius(s_wechat_add_edit_ta, 8, 0);
    lv_obj_set_style_border_width(s_wechat_add_edit_ta, 1, 0);
    lv_obj_set_style_border_color(s_wechat_add_edit_ta, lv_color_hex(0xD5E0EB), 0);
    lv_obj_set_style_bg_color(s_wechat_add_edit_ta, lv_color_hex(0xFFFFFF), 0);
    display_text_set_color(s_wechat_add_edit_ta, lv_color_hex(0x10243E), 0);
    lv_obj_set_style_text_font(s_wechat_add_edit_ta, display_ascii_font(12), 0);
    lv_obj_set_style_pad_left(s_wechat_add_edit_ta, 12, 0);
    lv_obj_set_style_pad_right(s_wechat_add_edit_ta, 12, 0);
    lv_obj_add_event_cb(s_wechat_add_edit_ta,
                        display_wechat_add_edit_textarea_event_cb,
                        LV_EVENT_ALL,
                        NULL);

    s_wechat_add_edit_status_label = display_create_figma_text(s_wechat_add_edit_page,
                                                               "点击保存生效",
                                                               8,
                                                               DISPLAY_UUID_STATUS_TOP,
                                                               DISPLAY_UUID_STATUS_WIDTH,
                                                               lv_color_hex(0x0D8A59),
                                                               12,
                                                               LV_TEXT_ALIGN_LEFT);

    s_wechat_add_edit_keyboard = lv_keyboard_create(s_wechat_add_edit_page);
    lv_obj_set_pos(s_wechat_add_edit_keyboard, DISPLAY_UUID_KEYBOARD_LEFT, DISPLAY_UUID_KEYBOARD_TOP);
    lv_obj_set_size(s_wechat_add_edit_keyboard, DISPLAY_UUID_KEYBOARD_WIDTH, DISPLAY_UUID_KEYBOARD_HEIGHT);
    lv_obj_set_style_bg_opa(s_wechat_add_edit_keyboard, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_wechat_add_edit_keyboard, 0, 0);
    lv_obj_set_style_pad_row(s_wechat_add_edit_keyboard, 3, 0);
    lv_obj_set_style_pad_column(s_wechat_add_edit_keyboard, 2, 0);
    lv_obj_set_style_border_width(s_wechat_add_edit_keyboard, 0, 0);
    lv_obj_set_style_radius(s_wechat_add_edit_keyboard, 0, 0);
    lv_obj_set_style_pad_all(s_wechat_add_edit_keyboard, 0, LV_PART_ITEMS);
    display_text_set_color(s_wechat_add_edit_keyboard, lv_color_hex(0x10243E), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_wechat_add_edit_keyboard, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_wechat_add_edit_keyboard,
                              lv_color_hex(0xD7EAFB),
                              LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_wechat_add_edit_keyboard, 5, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_wechat_add_edit_keyboard, 0, LV_PART_ITEMS);
    lv_keyboard_set_map(s_wechat_add_edit_keyboard,
                        LV_KEYBOARD_MODE_USER_1,
                        (const char **)s_wifi_keyboard_map_lc,
                        s_wifi_keyboard_ctrl_lc_map);
    lv_keyboard_set_map(s_wechat_add_edit_keyboard,
                        LV_KEYBOARD_MODE_USER_2,
                        (const char **)s_wifi_keyboard_map_uc,
                        s_wifi_keyboard_ctrl_uc_map);
    lv_keyboard_set_map(s_wechat_add_edit_keyboard,
                        LV_KEYBOARD_MODE_USER_3,
                        (const char **)s_wifi_keyboard_map_spec,
                        s_wifi_keyboard_ctrl_spec_map);
    lv_obj_remove_event_cb(s_wechat_add_edit_keyboard, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(s_wechat_add_edit_keyboard,
                        display_keyboard_value_event_cb,
                        LV_EVENT_VALUE_CHANGED,
                        NULL);
    lv_obj_add_event_cb(s_wechat_add_edit_keyboard,
                        display_keyboard_draw_part_event_cb,
                        LV_EVENT_DRAW_PART_BEGIN,
                        NULL);
    lv_obj_add_flag(s_wechat_add_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_wechat_add_edit_keyboard, display_keyboard_event_cb, LV_EVENT_ALL, NULL);
}

static void display_build_wechat_list_page(lv_obj_t *screen)
{
    s_wechat_list_page = lv_obj_create(screen);
    display_prepare_figma_page(s_wechat_list_page);
    lv_obj_add_flag(s_wechat_list_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_wechat_list_page,
                                      "微信联系人",
                                      display_wechat_child_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x21C783),
                                      NULL);

    for (uint8_t index = 0; index < DISPLAY_WECHAT_CONTACT_COUNT; ++index) {
        display_create_wechat_contact_row(s_wechat_list_page, index, 36 + ((lv_coord_t)index * 50));
    }

    s_wechat_empty_label = display_create_ai_text(s_wechat_list_page,
                                                  "No WeChat contacts",
                                                  8,
                                                  104,
                                                  304,
                                                  lv_color_hex(0x65768A),
                                                  LV_TEXT_ALIGN_CENTER);
    display_update_wechat_contact_list(&s_last_status);
}

static void display_build_wechat_active_page(lv_obj_t *screen)
{
    s_wechat_active_page = lv_obj_create(screen);
    display_prepare_figma_page(s_wechat_active_page);
    lv_obj_add_flag(s_wechat_active_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_wechat_active_page,
                                      "微信通话",
                                      display_wechat_child_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x21C783),
                                      NULL);

    (void)display_create_call_duration_row(s_wechat_active_page, 38, &s_wechat_duration_label);
    display_create_call_volume_row(s_wechat_active_page,
                                   80,
                                   "麦克风",
                                   "62",
                                   DISPLAY_CALL_VOLUME_MIC_DOWN,
                                   DISPLAY_CALL_VOLUME_MIC_UP,
                                   &s_wechat_mic_value_label,
                                   display_wechat_volume_btn_cb);
    display_create_call_volume_row(s_wechat_active_page,
                                   124,
                                   "扬声器",
                                   "70",
                                   DISPLAY_CALL_VOLUME_SPEAKER_DOWN,
                                   DISPLAY_CALL_VOLUME_SPEAKER_UP,
                                   &s_wechat_speaker_value_label,
                                   display_wechat_volume_btn_cb);
    lv_obj_t *hangup_btn = display_create_figma_button(s_wechat_active_page,
                                                       8,
                                                       168,
                                                       304,
                                                       38,
                                                       lv_color_hex(0xFFE7E7),
                                                       lv_color_hex(0xF15A5A),
                                                       "挂断",
                                                       lv_color_hex(0xE44747),
                                                       16,
                                                       display_wechat_hangup_btn_cb);
    lv_obj_set_style_radius(hangup_btn, 8, 0);
    display_update_wechat_active_page(&s_last_status);
}

static lv_obj_t *display_create_tirtc_config_field(lv_obj_t *parent,
                                                            lv_coord_t y,
                                                            const char *label,
                                                            const char *value,
                                                            display_tirtc_config_field_t field)
{
    (void)field;

    lv_obj_t *row = display_create_figma_box(parent,
                                             0,
                                             y,
                                             304,
                                             42,
                                             lv_color_hex(0xFFFFFF),
                                             lv_color_hex(0xD5E0EB),
                                             6);

    display_create_figma_text(row,
                              label,
                              10,
                              7,
                              250,
                              lv_color_hex(0x64758A),
                              12,
                              LV_TEXT_ALIGN_LEFT);
    lv_obj_t *value_label = display_create_figma_text(row,
                                                      value,
                                                      10,
                                                      22,
                                                      246,
                                                      lv_color_hex(0x10243E),
                                                      12,
                                                      LV_TEXT_ALIGN_LEFT);
    return value_label;
}

static void display_build_network_test_page(lv_obj_t *screen)
{
    lv_obj_t *summary = NULL;
    lv_obj_t *result = NULL;

    s_network_test_page = lv_obj_create(screen);
    display_prepare_figma_page(s_network_test_page);
    lv_obj_add_flag(s_network_test_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_network_test_page,
                                      "网络测试",
                                      display_system_child_back_btn_cb,
                                      "重测",
                                      lv_color_hex(0x1768B7),
                                      display_network_test_start_btn_cb);

    summary = display_create_figma_box(s_network_test_page,
                                       8,
                                       34,
                                       304,
                                       24,
                                       lv_color_hex(0xE7F1FB),
                                       lv_color_hex(0xD5E0EB),
                                       6);
    s_network_summary_wifi_label = display_create_figma_text(summary,
                                                             "Wi-Fi --",
                                                             8,
                                                             4,
                                                             148,
                                                             lv_color_hex(0x10243E),
                                                             12,
                                                             LV_TEXT_ALIGN_LEFT);
    s_network_summary_ip_label = display_create_figma_text(summary,
                                                           "IP --",
                                                           116,
                                                           4,
                                                           180,
                                                           lv_color_hex(0x10243E),
                                                           12,
                                                           LV_TEXT_ALIGN_LEFT);

    display_create_check_row(s_network_test_page, 63, "网关", "等待", lv_color_hex(0xFFFFFF), lv_color_hex(0xF59E0B), &s_network_gateway_value_label);
    display_create_check_row(s_network_test_page, 91, "DNS", "等待", lv_color_hex(0xFFFFFF), lv_color_hex(0xF59E0B), &s_network_dns_value_label);
    display_create_check_row(s_network_test_page, 119, "外网", "未测试", lv_color_hex(0xFFFFFF), lv_color_hex(0xF59E0B), &s_network_wan_value_label);
    s_network_service_row =
        display_create_check_row(s_network_test_page, 147, "TiRTC 服务", "--", lv_color_hex(0xFFFFFF), lv_color_hex(0xF59E0B), &s_network_service_value_label);
    display_create_check_row(s_network_test_page, 175, "丢包", "--", lv_color_hex(0xFFFFFF), lv_color_hex(0x0D8A59), &s_network_loss_value_label);

    result = display_create_figma_box(s_network_test_page,
                                      8,
                                      204,
                                      304,
                                      30,
                                      lv_color_hex(0xFFF2D8),
                                      lv_color_hex(0xF59E0B),
                                      6);
    s_network_result_box = result;
    s_network_result_label = display_create_figma_text(result,
                                                       "基础网络待测",
                                                       10,
                                                       7,
                                                       150,
                                                       lv_color_hex(0xF59E0B),
                                                       12,
                                                       LV_TEXT_ALIGN_LEFT);
    s_network_result_detail_label = display_create_figma_text(result,
                                                              "点击重测",
                                                              174,
                                                              7,
                                                              120,
                                                              lv_color_hex(0x64758A),
                                                              12,
                                                              LV_TEXT_ALIGN_RIGHT);

    display_update_network_test_page(&s_last_status);
}

static void display_build_ai_chat_page(lv_obj_t *screen)
{
    lv_obj_t *card = NULL;

    s_ai_chat_page = lv_obj_create(screen);
    display_prepare_figma_page(s_ai_chat_page);
    lv_obj_set_style_bg_color(s_ai_chat_page, lv_color_hex(0xE8F3FA), 0);
    lv_obj_add_flag(s_ai_chat_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_ai_header(s_ai_chat_page,
                                   "AI 对讲",
                                   display_ai_back_btn_cb,
                                   true,
                                   true);

    s_ai_avatar_img = lv_img_create(s_ai_chat_page);
    if (s_ai_avatar_img != NULL) {
        lv_img_set_src(s_ai_avatar_img,
                       ai_chat_avatar_asset_get(DISPLAY_AI_AVATAR_BUDDY, AI_CHAT_AVATAR_STATE_RESTING));
        lv_obj_set_pos(s_ai_avatar_img, DISPLAY_AI_AVATAR_IMG_X, DISPLAY_AI_AVATAR_IMG_Y);
        lv_obj_clear_flag(s_ai_avatar_img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        s_ai_avatar_last_variant = DISPLAY_AI_AVATAR_BUDDY;
        s_ai_avatar_last_state = AI_CHAT_AVATAR_STATE_RESTING;
    }
    card = display_create_figma_box(s_ai_chat_page,
                                    8,
                                    36,
                                    304,
                                    196,
                                    lv_color_hex(0xF7FBFE),
                                    lv_color_hex(0xD6E4EF),
                                    8);
    if (card != NULL) {
        lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    }

    s_ai_caption_bar = display_create_figma_box(s_ai_chat_page,
                                                DISPLAY_AI_SINGLE_CAPTION_BAR_X,
                                                DISPLAY_AI_SINGLE_CAPTION_BAR_BOTTOM_Y - DISPLAY_AI_SINGLE_CAPTION_BAR_MIN_HEIGHT,
                                                DISPLAY_AI_SINGLE_CAPTION_BAR_WIDTH,
                                                DISPLAY_AI_SINGLE_CAPTION_BAR_MIN_HEIGHT,
                                                lv_color_hex(0xF7FBFE),
                                                lv_color_hex(0xD6E4EF),
                                                8);
    s_ai_single_caption_label =
        display_create_ai_chat_caption_text(s_ai_caption_bar,
                                            "",
                                            DISPLAY_AI_SINGLE_CAPTION_TEXT_X,
                                            DISPLAY_AI_SINGLE_CAPTION_TEXT_Y,
                                            DISPLAY_AI_SINGLE_CAPTION_TEXT_WIDTH,
                                            lv_color_hex(0x1768B7),
                                            LV_TEXT_ALIGN_CENTER,
                                            NULL);
    if (s_ai_single_caption_label != NULL) {
        display_layout_ai_single_caption("");
    }

    s_ai_message_touching = false;
    s_ai_message_list = lv_obj_create(card);
    lv_obj_remove_style_all(s_ai_message_list);
    lv_obj_set_pos(s_ai_message_list, 0, 0);
    lv_obj_set_size(s_ai_message_list, 304, 196);
    lv_obj_set_style_bg_opa(s_ai_message_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ai_message_list, 0, 0);
    lv_obj_set_style_pad_all(s_ai_message_list, 0, 0);
    lv_obj_add_flag(s_ai_message_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_ai_message_list, LV_DIR_VER);
    lv_obj_clear_flag(s_ai_message_list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scrollbar_mode(s_ai_message_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(s_ai_message_list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_ai_message_list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_ai_message_list, lv_color_hex(0x8FB5CE), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_ai_message_list, LV_OPA_80, LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(s_ai_message_list, display_ai_chat_message_list_event_cb, LV_EVENT_ALL, NULL);

    s_ai_message_layout_count = 0;
    s_ai_message_content_height = DISPLAY_AI_CHAT_VIEWPORT_HEIGHT;
    s_ai_new_chat_button_y = DISPLAY_AI_CHAT_BUBBLE_TOP_Y;
    s_ai_message_layout_generation = 1;
    s_ai_message_layout_font_ready = false;
    s_ai_message_layout_new_button_visible = false;
    for (uint8_t index = 0; index < DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX; ++index) {
        s_ai_visible_message_indices[index] = UINT8_MAX;
        s_ai_visible_message_generations[index] = 0;
        s_ai_visible_message_hashes[index] = 0;
        s_ai_visible_message_y[index] = 0;
        s_ai_visible_message_bottom_y[index] = 0;
    }

    s_ai_message_scroll_spacer = lv_obj_create(s_ai_message_list);
    if (s_ai_message_scroll_spacer != NULL) {
        lv_obj_remove_style_all(s_ai_message_scroll_spacer);
        lv_obj_set_pos(s_ai_message_scroll_spacer, 0, 0);
        lv_obj_set_size(s_ai_message_scroll_spacer, 1, DISPLAY_AI_CHAT_VIEWPORT_HEIGHT);
        lv_obj_set_style_bg_opa(s_ai_message_scroll_spacer, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(s_ai_message_scroll_spacer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_ai_message_scroll_spacer,
                        LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    for (size_t index = 0; index < DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX; ++index) {
        s_ai_message_boxes[index] =
            display_create_figma_box(s_ai_message_list,
                                     DISPLAY_AI_CHAT_BUBBLE_LEFT_X,
                                     DISPLAY_AI_CHAT_BUBBLE_TOP_Y,
                                     DISPLAY_AI_CHAT_MIN_TEXT_WIDTH +
                                         DISPLAY_AI_CHAT_BUBBLE_TEXT_X +
                                         DISPLAY_AI_CHAT_BUBBLE_PAD_RIGHT,
                                     36,
                                     lv_color_hex(0x2F82D7),
                                     lv_color_hex(0x2F82D7),
                                     DISPLAY_AI_CHAT_BUBBLE_RADIUS);
        if (s_ai_message_boxes[index] != NULL) {
            lv_obj_clear_flag(s_ai_message_boxes[index],
                              LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(s_ai_message_boxes[index],
                            LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_add_flag(s_ai_message_boxes[index], LV_OBJ_FLAG_HIDDEN);
        }
        s_ai_message_labels[index] =
            display_create_ai_chat_caption_text(s_ai_message_boxes[index],
                                                "",
                                                DISPLAY_AI_CHAT_BUBBLE_TEXT_X,
                                                DISPLAY_AI_CHAT_BUBBLE_TEXT_Y,
                                                DISPLAY_AI_CHAT_MIN_TEXT_WIDTH,
                                                lv_color_hex(0xFFFFFF),
                                                LV_TEXT_ALIGN_LEFT,
                                                s_ai_message_bold_labels[index]);
        display_set_ai_chat_caption_long_mode(s_ai_message_labels[index],
                                              s_ai_message_bold_labels[index]);
    }

    s_ai_new_chat_btn = lv_btn_create(s_ai_message_list);
    if (s_ai_new_chat_btn != NULL) {
        lv_obj_remove_style_all(s_ai_new_chat_btn);
        lv_obj_set_size(s_ai_new_chat_btn,
                        DISPLAY_AI_CHAT_NEW_BUTTON_WIDTH,
                        DISPLAY_AI_CHAT_NEW_BUTTON_HEIGHT);
        lv_obj_set_style_radius(s_ai_new_chat_btn, 8, 0);
        lv_obj_set_style_bg_color(s_ai_new_chat_btn, lv_color_hex(0x21C783), 0);
        lv_obj_set_style_bg_color(s_ai_new_chat_btn, lv_color_hex(0x18A76B), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(s_ai_new_chat_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_ai_new_chat_btn, 0, 0);
        lv_obj_clear_flag(s_ai_new_chat_btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_ai_new_chat_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(s_ai_new_chat_btn, display_ai_start_new_btn_cb, LV_EVENT_CLICKED, NULL);
        s_ai_new_chat_btn_label =
            display_create_ai_chat_caption_text(s_ai_new_chat_btn,
                                                "开始新对话",
                                                0,
                                                11,
                                                DISPLAY_AI_CHAT_NEW_BUTTON_WIDTH,
                                                lv_color_hex(0xFFFFFF),
                                                LV_TEXT_ALIGN_CENTER,
                                                NULL);
        if (s_ai_new_chat_btn_label != NULL) {
            lv_obj_clear_flag(s_ai_new_chat_btn_label, LV_OBJ_FLAG_CLICKABLE);
            lv_label_set_long_mode(s_ai_new_chat_btn_label, LV_LABEL_LONG_CLIP);
        }
    }

    s_ai_scroll_up_btn =
        display_create_ai_chat_scroll_button(s_ai_chat_page,
                                             DISPLAY_AI_CHAT_SCROLL_BUTTON_UP_Y,
                                             LV_SYMBOL_UP,
                                             DISPLAY_AI_SCROLL_PAGE_UP);
    s_ai_scroll_down_btn =
        display_create_ai_chat_scroll_button(s_ai_chat_page,
                                             DISPLAY_AI_CHAT_SCROLL_BUTTON_DOWN_Y,
                                             LV_SYMBOL_DOWN,
                                             DISPLAY_AI_SCROLL_PAGE_DOWN);
    s_ai_scroll_bottom_btn =
        display_create_ai_chat_scroll_button(s_ai_chat_page,
                                             DISPLAY_AI_CHAT_SCROLL_BUTTON_BOTTOM_Y,
                                             LV_SYMBOL_DOWNLOAD,
                                             DISPLAY_AI_SCROLL_BOTTOM);

    s_ai_new_chat_btn = lv_btn_create(s_ai_chat_page);
    if (s_ai_new_chat_btn != NULL) {
        lv_obj_remove_style_all(s_ai_new_chat_btn);
        lv_obj_set_pos(s_ai_new_chat_btn, 71, 194);
        lv_obj_set_size(s_ai_new_chat_btn, DISPLAY_AI_CHAT_NEW_BUTTON_WIDTH, 34);
        lv_obj_set_style_radius(s_ai_new_chat_btn, 8, 0);
        lv_obj_set_style_bg_color(s_ai_new_chat_btn, lv_color_hex(0x21C783), 0);
        lv_obj_set_style_bg_color(s_ai_new_chat_btn, lv_color_hex(0x18A76B), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(s_ai_new_chat_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_ai_new_chat_btn, 0, 0);
        lv_obj_clear_flag(s_ai_new_chat_btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_ai_new_chat_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(s_ai_new_chat_btn, display_ai_start_new_btn_cb, LV_EVENT_CLICKED, NULL);
        s_ai_new_chat_btn_label =
            display_create_ai_chat_caption_text(s_ai_new_chat_btn,
                                                "开始新对话",
                                                0,
                                                8,
                                                DISPLAY_AI_CHAT_NEW_BUTTON_WIDTH,
                                                lv_color_hex(0xFFFFFF),
                                                LV_TEXT_ALIGN_CENTER,
                                                NULL);
        if (s_ai_new_chat_btn_label != NULL) {
            lv_obj_clear_flag(s_ai_new_chat_btn_label, LV_OBJ_FLAG_CLICKABLE);
            lv_label_set_long_mode(s_ai_new_chat_btn_label, LV_LABEL_LONG_CLIP);
        }
    }

    display_update_ai_chat_page(&s_last_status);
}

static void display_build_ai_chat_settings_page(lv_obj_t *screen)
{
    lv_obj_t *mic_row = NULL;
    lv_obj_t *speaker_row = NULL;
    lv_obj_t *avatar_row = NULL;
    lv_obj_t *info_panel = NULL;
    lv_obj_t *value_box = NULL;

    s_ai_chat_settings_page = lv_obj_create(screen);
    display_prepare_figma_page(s_ai_chat_settings_page);
    lv_obj_set_style_bg_color(s_ai_chat_settings_page, lv_color_hex(0xEBF6FC), 0);
    lv_obj_add_flag(s_ai_chat_settings_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_ai_header(s_ai_chat_settings_page,
                                   "AI 对讲设置",
                                   display_ai_settings_back_btn_cb,
                                   false,
                                   false);

    mic_row = display_create_figma_box(s_ai_chat_settings_page,
                                       8,
                                       38,
                                       304,
                                       44,
                                       lv_color_hex(0xFFFFFF),
                                       lv_color_hex(0xD2E1EC),
                                       8);
    display_create_ai_static_text(mic_row,
                                  "麦克风音量",
                                  13,
                                  13,
                                  120,
                                  lv_color_hex(0x11233C),
                                  12,
                                  LV_TEXT_ALIGN_LEFT);
    (void)display_create_ai_setting_button(mic_row,
                                           165,
                                           7,
                                           34,
                                           28,
                                           "-",
                                           DISPLAY_AI_SETTING_MIC_DOWN);
    value_box = display_create_figma_box(mic_row,
                                         207,
                                         7,
                                         44,
                                         28,
                                         lv_color_hex(0xE5FAF0),
                                         lv_color_hex(0xE5FAF0),
                                         7);
    s_ai_settings_mic_value_label = display_create_figma_text(value_box,
                                                              "80",
                                                              0,
                                                              6,
                                                              44,
                                                              lv_color_hex(0x23C17D),
                                                              13,
                                                              LV_TEXT_ALIGN_CENTER);
    (void)display_create_ai_setting_button(mic_row,
                                           259,
                                           7,
                                           34,
                                           28,
                                           "+",
                                           DISPLAY_AI_SETTING_MIC_UP);

    speaker_row = display_create_figma_box(s_ai_chat_settings_page,
                                           8,
                                           90,
                                           304,
                                           44,
                                           lv_color_hex(0xFFFFFF),
                                           lv_color_hex(0xD2E1EC),
                                           8);
    display_create_ai_static_text(speaker_row,
                                  "扬声器音量",
                                  13,
                                  13,
                                  120,
                                  lv_color_hex(0x11233C),
                                  12,
                                  LV_TEXT_ALIGN_LEFT);
    (void)display_create_ai_setting_button(speaker_row,
                                           165,
                                           7,
                                           34,
                                           28,
                                           "-",
                                           DISPLAY_AI_SETTING_SPEAKER_DOWN);
    value_box = display_create_figma_box(speaker_row,
                                         207,
                                         7,
                                         44,
                                         28,
                                         lv_color_hex(0xE5FAF0),
                                         lv_color_hex(0xE5FAF0),
                                         7);
    s_ai_settings_speaker_value_label = display_create_figma_text(value_box,
                                                                  "70",
                                                                  0,
                                                                  6,
                                                                  44,
                                                                  lv_color_hex(0x23C17D),
                                                                  13,
                                                                  LV_TEXT_ALIGN_CENTER);
    (void)display_create_ai_setting_button(speaker_row,
                                           259,
                                           7,
                                           34,
                                           28,
                                           "+",
                                           DISPLAY_AI_SETTING_SPEAKER_UP);

    avatar_row = display_create_figma_box(s_ai_chat_settings_page,
                                          8,
                                          142,
                                          304,
                                          44,
                                          lv_color_hex(0xFFFFFF),
                                          lv_color_hex(0xD2E1EC),
                                          8);
    display_create_ai_static_text(avatar_row,
                                  "角色形象",
                                  13,
                                  13,
                                  92,
                                  lv_color_hex(0x11233C),
                                  12,
                                  LV_TEXT_ALIGN_LEFT);
    (void)display_create_ai_avatar_choice_button(avatar_row,
                                                 142,
                                                 6,
                                                 DISPLAY_AI_AVATAR_BUDDY,
                                                 DISPLAY_AI_SETTING_AVATAR_BUDDY);
    (void)display_create_ai_avatar_choice_button(avatar_row,
                                                 226,
                                                 6,
                                                 DISPLAY_AI_AVATAR_SPROUT,
                                                 DISPLAY_AI_SETTING_AVATAR_SPROUT);

    info_panel = display_create_figma_box(s_ai_chat_settings_page,
                                          8,
                                          194,
                                          304,
                                          34,
                                          lv_color_hex(0xFFFFFF),
                                          lv_color_hex(0xD2E1EC),
                                          8);
    display_create_ai_static_text(info_panel,
                                  "对讲方式",
                                  13,
                                  10,
                                  120,
                                  lv_color_hex(0x11233C),
                                  12,
                                  LV_TEXT_ALIGN_LEFT);
    display_create_ai_static_text(info_panel,
                                  "默认常听，直接说话。",
                                  13,
                                  34,
                                  260,
                                  lv_color_hex(0x60768E),
                                  12,
                                  LV_TEXT_ALIGN_LEFT);
    display_create_ai_static_text(info_panel,
                                  "AI 回复时按侧键打断。",
                                  13,
                                  58,
                                  260,
                                  lv_color_hex(0x60768E),
                                  12,
                                  LV_TEXT_ALIGN_LEFT);
    display_create_ai_static_text(info_panel,
                                  "断开后按侧键重连。",
                                  13,
                                  76,
                                  260,
                                  lv_color_hex(0x60768E),
                                  11,
                                  LV_TEXT_ALIGN_LEFT);

    display_update_ai_chat_settings_page(&s_last_status);
}

static void display_build_tirtc_config_page(lv_obj_t *screen)
{
    lv_obj_t *fields = NULL;

    s_tirtc_config_page = lv_obj_create(screen);
    display_prepare_figma_page(s_tirtc_config_page);
    lv_obj_add_flag(s_tirtc_config_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_tirtc_config_page,
                                      "TiRTC 配置",
                                      display_system_child_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x000000),
                                      NULL);

    fields = lv_obj_create(s_tirtc_config_page);
    lv_obj_remove_style_all(fields);
    lv_obj_set_pos(fields, 8, 40);
    lv_obj_set_size(fields, 304, 154);
    lv_obj_set_style_bg_opa(fields, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(fields, 0, 0);
    lv_obj_set_scroll_dir(fields, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(fields, LV_SCROLLBAR_MODE_OFF);

    s_tirtc_config_value_labels[DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_ID] =
        display_create_tirtc_config_field(fields,
                                          0,
                                          "Device ID",
                                          "Unbound",
                                          DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_ID);
    s_tirtc_config_value_labels[DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_SECRET] =
        display_create_tirtc_config_field(fields,
                                          46,
                                          "Binding",
                                          "Idle",
                                          DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_SECRET);
    s_tirtc_config_value_labels[DISPLAY_TIRTC_CONFIG_FIELD_TOKEN_SUBJECT] =
        display_create_tirtc_config_field(fields,
                                          92,
                                          "Token Subject",
                                          "Not set",
                                          DISPLAY_TIRTC_CONFIG_FIELD_TOKEN_SUBJECT);
    s_tirtc_config_value_labels[DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_ID] =
        display_create_tirtc_config_field(fields,
                                          138,
                                          "Token API",
                                          "Service issued",
                                          DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_ID);
    s_tirtc_config_value_labels[DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_SECRET] =
        display_create_tirtc_config_field(fields,
                                          184,
                                          "Credential",
                                          "Managed by binding",
                                          DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_SECRET);

    (void)display_create_figma_button(s_tirtc_config_page,
                                      8,
                                      202,
                                      304,
                                      30,
                                      lv_color_hex(0xFFE7E7),
                                      lv_color_hex(0xF15A5A),
                                      "重置绑定",
                                      lv_color_hex(0xE44747),
                                      14,
                                      display_tirtc_config_scan_btn_cb);

    display_update_tirtc_config_page(&s_last_status);
}

static void display_build_tirtc_config_edit_page(lv_obj_t *screen)
{
    s_tirtc_config_edit_page = lv_obj_create(screen);
    display_prepare_figma_page(s_tirtc_config_edit_page);
    lv_obj_add_flag(s_tirtc_config_edit_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_tirtc_config_edit_page,
                                      "编辑配置",
                                      display_tirtc_config_edit_back_btn_cb,
                                      "保存",
                                      lv_color_hex(0x20BF7A),
                                      display_tirtc_config_edit_save_btn_cb);

    s_tirtc_edit_hint_label = display_create_figma_text(s_tirtc_config_edit_page,
                                                        "Device ID",
                                                        8,
                                                        36,
                                                        196,
                                                        lv_color_hex(0x64758A),
                                                        12,
                                                        LV_TEXT_ALIGN_LEFT);
    s_tirtc_edit_length_label = display_create_figma_text(s_tirtc_config_edit_page,
                                                          "0/127",
                                                          230,
                                                          36,
                                                          82,
                                                          lv_color_hex(0x64758A),
                                                          12,
                                                          LV_TEXT_ALIGN_RIGHT);

    s_tirtc_edit_ta = lv_textarea_create(s_tirtc_config_edit_page);
    lv_obj_set_pos(s_tirtc_edit_ta, 8, DISPLAY_UUID_INPUT_TOP);
    lv_obj_set_size(s_tirtc_edit_ta, DISPLAY_UUID_INPUT_WIDTH, DISPLAY_UUID_INPUT_HEIGHT);
    lv_textarea_set_one_line(s_tirtc_edit_ta, true);
    lv_textarea_set_max_length(s_tirtc_edit_ta, DISPLAY_TIRTC_CONFIG_TEXT_MAX - 1U);
    lv_textarea_set_placeholder_text(s_tirtc_edit_ta, "Device ID");
    lv_obj_set_style_radius(s_tirtc_edit_ta, 8, 0);
    lv_obj_set_style_border_width(s_tirtc_edit_ta, 1, 0);
    lv_obj_set_style_border_color(s_tirtc_edit_ta, lv_color_hex(0xD5E0EB), 0);
    lv_obj_set_style_bg_color(s_tirtc_edit_ta, lv_color_hex(0xFFFFFF), 0);
    display_text_set_color(s_tirtc_edit_ta, lv_color_hex(0x10243E), 0);
    lv_obj_set_style_text_font(s_tirtc_edit_ta, display_ascii_font(12), 0);
    lv_obj_set_style_pad_left(s_tirtc_edit_ta, 12, 0);
    lv_obj_set_style_pad_right(s_tirtc_edit_ta, 12, 0);
    lv_obj_add_event_cb(s_tirtc_edit_ta, display_tirtc_edit_textarea_event_cb, LV_EVENT_ALL, NULL);

    s_tirtc_edit_status_label = display_create_figma_text(s_tirtc_config_edit_page,
                                                          "点击保存生效",
                                                          8,
                                                          DISPLAY_UUID_STATUS_TOP,
                                                          DISPLAY_UUID_STATUS_WIDTH,
                                                          lv_color_hex(0x0D8A59),
                                                          12,
                                                          LV_TEXT_ALIGN_LEFT);

    s_tirtc_edit_keyboard = lv_keyboard_create(s_tirtc_config_edit_page);
    lv_obj_set_pos(s_tirtc_edit_keyboard, DISPLAY_UUID_KEYBOARD_LEFT, DISPLAY_UUID_KEYBOARD_TOP);
    lv_obj_set_size(s_tirtc_edit_keyboard, DISPLAY_UUID_KEYBOARD_WIDTH, DISPLAY_UUID_KEYBOARD_HEIGHT);
    lv_obj_set_style_bg_opa(s_tirtc_edit_keyboard, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_tirtc_edit_keyboard, 0, 0);
    lv_obj_set_style_pad_row(s_tirtc_edit_keyboard, 3, 0);
    lv_obj_set_style_pad_column(s_tirtc_edit_keyboard, 2, 0);
    lv_obj_set_style_border_width(s_tirtc_edit_keyboard, 0, 0);
    lv_obj_set_style_radius(s_tirtc_edit_keyboard, 0, 0);
    lv_obj_set_style_pad_all(s_tirtc_edit_keyboard, 0, LV_PART_ITEMS);
    display_text_set_color(s_tirtc_edit_keyboard, lv_color_hex(0x10243E), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_tirtc_edit_keyboard, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_tirtc_edit_keyboard, lv_color_hex(0xD7EAFB), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_tirtc_edit_keyboard, 5, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_tirtc_edit_keyboard, 0, LV_PART_ITEMS);
    lv_keyboard_set_map(s_tirtc_edit_keyboard, LV_KEYBOARD_MODE_USER_1, (const char **)s_wifi_keyboard_map_lc, s_wifi_keyboard_ctrl_lc_map);
    lv_keyboard_set_map(s_tirtc_edit_keyboard, LV_KEYBOARD_MODE_USER_2, (const char **)s_wifi_keyboard_map_uc, s_wifi_keyboard_ctrl_uc_map);
    lv_keyboard_set_map(s_tirtc_edit_keyboard, LV_KEYBOARD_MODE_USER_3, (const char **)s_wifi_keyboard_map_spec, s_wifi_keyboard_ctrl_spec_map);
    lv_obj_remove_event_cb(s_tirtc_edit_keyboard, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(s_tirtc_edit_keyboard, display_keyboard_value_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_tirtc_edit_keyboard, display_keyboard_draw_part_event_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_add_flag(s_tirtc_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_tirtc_edit_keyboard, display_keyboard_event_cb, LV_EVENT_ALL, NULL);
}

static void display_build_ota_page(lv_obj_t *screen)
{
    lv_obj_t *card = NULL;
    lv_obj_t *label = NULL;

    s_ota_page = lv_obj_create(screen);
    lv_obj_set_size(s_ota_page, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_opa(s_ota_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ota_page, 0, 0);
    lv_obj_set_style_pad_all(s_ota_page, 0, 0);
    lv_obj_clear_flag(s_ota_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ota_page, LV_OBJ_FLAG_HIDDEN);

    display_prepare_figma_page(s_ota_page);
    (void)display_create_figma_header(s_ota_page,
                                      "关于 / OTA",
                                      display_system_child_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x000000),
                                      NULL);

    (void)display_create_info_row(s_ota_page,
                                  40,
                                  "固件版本",
                                  "--",
                                  lv_color_hex(0x10243E),
                                  &s_ota_version_label);
    (void)display_create_info_row(s_ota_page,
                                  82,
                                  "协议版本",
                                  DISPLAY_TIRTC_VERSION_TEXT,
                                  lv_color_hex(0x10243E),
                                  &s_ota_second_value_label);
    s_ota_second_label = lv_obj_get_child(lv_obj_get_parent(s_ota_second_value_label), 0);
    (void)display_create_info_row(s_ota_page,
                                  124,
                                  "OTA 状态",
                                  "可检查",
                                  lv_color_hex(0x0D8A59),
                                  &s_ota_status_label);

    s_ota_start_btn = display_create_figma_button(s_ota_page,
                                                  8,
                                                  166,
                                                  304,
                                                  38,
                                                  lv_color_hex(0x20BF7A),
                                                  lv_color_hex(0x20BF7A),
                                                  "检查更新",
                                                  lv_color_hex(0xFFFFFF),
                                                  14,
                                                  display_ota_start_btn_cb);
    s_ota_start_btn_label = lv_obj_get_child(s_ota_start_btn, 0);

    s_ota_reboot_btn = display_create_figma_button(s_ota_page,
                                                   8,
                                                   166,
                                                   304,
                                                   38,
                                                   lv_color_hex(0x20BF7A),
                                                   lv_color_hex(0x20BF7A),
                                                   "重启生效",
                                                   lv_color_hex(0xFFFFFF),
                                                   14,
                                                   display_ota_reboot_btn_cb);
    s_ota_reboot_btn_label = lv_obj_get_child(s_ota_reboot_btn, 0);

    s_ota_action_panel = display_create_figma_box(s_ota_page,
                                                  8,
                                                  166,
                                                  304,
                                                  58,
                                                  lv_color_hex(0xFFFFFF),
                                                  lv_color_hex(0xD5E0EB),
                                                  6);
    s_ota_progress_title_label = display_create_figma_text(s_ota_action_panel,
                                                           "正在检查更新",
                                                           122,
                                                           20,
                                                           150,
                                                           lv_color_hex(0x10243E),
                                                           12,
                                                           LV_TEXT_ALIGN_LEFT);
    s_ota_progress_percent_label = display_create_figma_text(s_ota_action_panel,
                                                             "0%",
                                                             234,
                                                             7,
                                                             60,
                                                             lv_color_hex(0x64758A),
                                                             12,
                                                             LV_TEXT_ALIGN_RIGHT);
    s_ota_progress_bar = lv_bar_create(s_ota_action_panel);
    lv_obj_set_pos(s_ota_progress_bar, 10, 28);
    lv_obj_set_size(s_ota_progress_bar, 284, 8);
    lv_bar_set_range(s_ota_progress_bar, 0, 100);
    lv_obj_set_style_radius(s_ota_progress_bar, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ota_progress_bar, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_ota_progress_bar, lv_color_hex(0xE2EAF1), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ota_progress_bar, lv_color_hex(0x20BF7A), LV_PART_INDICATOR);
    s_ota_progress_hint_label = display_create_figma_text(s_ota_action_panel,
                                                          "升级中请保持供电",
                                                          10,
                                                          38,
                                                          180,
                                                          lv_color_hex(0x64758A),
                                                          12,
                                                          LV_TEXT_ALIGN_LEFT);

    display_update_ota_page(&s_last_status);
    return;

    (void)display_create_header(s_ota_page, "OTA", display_system_back_btn_cb);

    card = lv_obj_create(s_ota_page);
    lv_obj_set_pos(card, 18, 46);
    lv_obj_set_size(card, 284, 186);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xF4EFE5), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    label = lv_label_create(card);
    lv_obj_set_pos(label, 14, 10);
    display_text_set_color(label, lv_color_hex(0x183642), 0);
#if LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
#endif
    display_text_set(label, "Local firmware update");

    s_ota_version_label = lv_label_create(card);
    lv_obj_set_pos(s_ota_version_label, 14, 36);
    lv_obj_set_width(s_ota_version_label, 252);
    lv_label_set_long_mode(s_ota_version_label, LV_LABEL_LONG_DOT);
    display_text_set_color(s_ota_version_label, lv_color_hex(0x48656F), 0);
#if LV_FONT_MONTSERRAT_12
    lv_obj_set_style_text_font(s_ota_version_label, &lv_font_montserrat_12, 0);
#endif

    s_ota_url_label = lv_label_create(card);
    lv_obj_set_pos(s_ota_url_label, 14, 56);
    lv_obj_set_width(s_ota_url_label, 252);
    lv_label_set_long_mode(s_ota_url_label, LV_LABEL_LONG_DOT);
    display_text_set_color(s_ota_url_label, lv_color_hex(0x48656F), 0);
#if LV_FONT_MONTSERRAT_12
    lv_obj_set_style_text_font(s_ota_url_label, &lv_font_montserrat_12, 0);
#endif

    s_ota_progress_bar = lv_bar_create(card);
    lv_obj_set_pos(s_ota_progress_bar, 14, 82);
    lv_obj_set_size(s_ota_progress_bar, 252, 12);
    lv_bar_set_range(s_ota_progress_bar, 0, 100);
    lv_obj_set_style_radius(s_ota_progress_bar, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ota_progress_bar, 8, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_ota_progress_bar, lv_color_hex(0xD8CBB8), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ota_progress_bar, lv_color_hex(0x2E8F6B), LV_PART_INDICATOR);

    s_ota_size_label = lv_label_create(card);
    lv_obj_set_pos(s_ota_size_label, 14, 102);
    lv_obj_set_width(s_ota_size_label, 252);
    lv_label_set_long_mode(s_ota_size_label, LV_LABEL_LONG_DOT);
    display_text_set_color(s_ota_size_label, lv_color_hex(0x48656F), 0);
#if LV_FONT_MONTSERRAT_12
    lv_obj_set_style_text_font(s_ota_size_label, &lv_font_montserrat_12, 0);
#endif

    s_ota_status_label = lv_label_create(card);
    lv_obj_set_pos(s_ota_status_label, 14, 122);
    lv_obj_set_width(s_ota_status_label, 252);
    lv_label_set_long_mode(s_ota_status_label, LV_LABEL_LONG_WRAP);
    display_text_set_color(s_ota_status_label, lv_color_hex(0x183642), 0);
    lv_obj_set_style_text_line_space(s_ota_status_label, 1, 0);
#if LV_FONT_MONTSERRAT_12
    lv_obj_set_style_text_font(s_ota_status_label, &lv_font_montserrat_12, 0);
#endif

    s_ota_start_btn = lv_btn_create(card);
    lv_obj_set_pos(s_ota_start_btn, 14, 154);
    lv_obj_set_size(s_ota_start_btn, 116, 28);
    lv_obj_set_style_radius(s_ota_start_btn, 8, 0);
    lv_obj_add_event_cb(s_ota_start_btn, display_ota_start_btn_cb, LV_EVENT_CLICKED, NULL);
    s_ota_start_btn_label = lv_label_create(s_ota_start_btn);
    display_text_set(s_ota_start_btn_label, "Start");
    lv_obj_center(s_ota_start_btn_label);

    s_ota_reboot_btn = lv_btn_create(card);
    lv_obj_set_pos(s_ota_reboot_btn, 150, 154);
    lv_obj_set_size(s_ota_reboot_btn, 116, 28);
    lv_obj_set_style_radius(s_ota_reboot_btn, 8, 0);
    lv_obj_add_event_cb(s_ota_reboot_btn, display_ota_reboot_btn_cb, LV_EVENT_CLICKED, NULL);
    s_ota_reboot_btn_label = lv_label_create(s_ota_reboot_btn);
    display_text_set(s_ota_reboot_btn_label, "Wait");
    lv_obj_center(s_ota_reboot_btn_label);

    display_update_ota_page(&s_last_status);
}

static void display_build_test_page(lv_obj_t *screen)
{
    lv_obj_t *card = NULL;

    s_test_page = lv_obj_create(screen);
    lv_obj_set_size(s_test_page, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_opa(s_test_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_test_page, 0, 0);
    lv_obj_set_style_pad_all(s_test_page, 0, 0);
    lv_obj_clear_flag(s_test_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_test_page, LV_OBJ_FLAG_HIDDEN);

    display_prepare_figma_page(s_test_page);
    (void)display_create_figma_header(s_test_page,
                                      "TiRTC 测试",
                                      display_system_child_back_btn_cb,
                                      "开始",
                                      lv_color_hex(0x1768B7),
                                      display_tirtc_test_start_btn_cb);

    lv_obj_t *summary = display_create_figma_box(s_test_page,
                                                 8,
                                                 34,
                                                 304,
                                                 24,
                                                 lv_color_hex(0xE7F1FB),
                                                 lv_color_hex(0xD5E0EB),
                                                 6);
    s_tirtc_test_session_label = display_create_figma_text(summary,
                                                           "会话 TEST",
                                                           8,
                                                           4,
                                                           150,
                                                           lv_color_hex(0x10243E),
                                                           12,
                                                           LV_TEXT_ALIGN_LEFT);
    s_tirtc_test_link_label = display_create_figma_text(summary,
                                                        "音频链路",
                                                        196,
                                                        4,
                                                        100,
                                                        lv_color_hex(0x10243E),
                                                        12,
                                                        LV_TEXT_ALIGN_RIGHT);

    display_create_check_row(s_test_page, 63, "获取 Token", "等待", lv_color_hex(0xFFFFFF), lv_color_hex(0xF59E0B), &s_tirtc_test_token_value_label);
    display_create_check_row(s_test_page, 91, "连接房间", "等待", lv_color_hex(0xFFFFFF), lv_color_hex(0xF59E0B), &s_tirtc_test_room_value_label);
    display_create_check_row(s_test_page, 119, "发送音频", "等待", lv_color_hex(0xFFFFFF), lv_color_hex(0xF59E0B), &s_tirtc_test_tx_audio_value_label);
    display_create_check_row(s_test_page, 147, "接收音频", "等待", lv_color_hex(0xFFF2D8), lv_color_hex(0xF59E0B), &s_tirtc_test_rx_audio_value_label);
    display_create_check_row(s_test_page, 175, "断开测试", "就绪", lv_color_hex(0xFFFFFF), lv_color_hex(0x0D8A59), &s_tirtc_test_disconnect_value_label);

    lv_obj_t *result = display_create_figma_box(s_test_page,
                                                8,
                                                204,
                                                304,
                                                30,
                                                lv_color_hex(0xFFF2D8),
                                                lv_color_hex(0xF59E0B),
                                                6);
    s_tirtc_test_result_label = display_create_figma_text(result,
                                                          "测试就绪",
                                                          10,
                                                          7,
                                                          150,
                                                          lv_color_hex(0xF59E0B),
                                                          12,
                                                          LV_TEXT_ALIGN_LEFT);
    s_tirtc_test_result_detail_label = display_create_figma_text(result,
                                                                 "点击开始",
                                                                 174,
                                                                 7,
                                                                 120,
                                                                 lv_color_hex(0x64758A),
                                                                 12,
                                                                 LV_TEXT_ALIGN_RIGHT);

    display_update_test_page(&s_last_status);
    return;

    (void)display_create_header(s_test_page, "Test", display_system_back_btn_cb);

    card = lv_obj_create(s_test_page);
    lv_obj_set_pos(card, 18, 48);
    lv_obj_set_size(card, 284, 170);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xF4EFE5), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    s_test_status_label = lv_label_create(card);
    lv_obj_set_pos(s_test_status_label, 0, 0);
    lv_obj_set_width(s_test_status_label, 252);
    lv_obj_set_style_text_font(s_test_status_label, display_ascii_font(12U), 0);
    display_text_set_color(s_test_status_label, lv_color_hex(0x30464F), 0);
    lv_label_set_long_mode(s_test_status_label, LV_LABEL_LONG_WRAP);
    display_text_set(s_test_status_label, "Ready");

    s_test_video_btn = lv_btn_create(card);
    lv_obj_set_pos(s_test_video_btn, 0, 114);
    lv_obj_set_size(s_test_video_btn, 118, 34);
    lv_obj_set_style_radius(s_test_video_btn, 16, 0);
    lv_obj_set_style_bg_color(s_test_video_btn, lv_color_hex(0x2E8F6B), 0);
    lv_obj_set_style_border_width(s_test_video_btn, 0, 0);
    lv_obj_add_event_cb(s_test_video_btn, display_test_video_btn_cb, LV_EVENT_CLICKED, NULL);

    s_test_video_btn_label = lv_label_create(s_test_video_btn);
    lv_obj_set_style_text_font(s_test_video_btn_label, &lv_font_montserrat_14, 0);
    display_text_set_color(s_test_video_btn_label, lv_color_hex(0xFFFFFF), 0);
    display_text_set(s_test_video_btn_label, "Video");
    lv_obj_center(s_test_video_btn_label);

    s_test_audio_btn = lv_btn_create(card);
    lv_obj_set_pos(s_test_audio_btn, 134, 114);
    lv_obj_set_size(s_test_audio_btn, 118, 34);
    lv_obj_set_style_radius(s_test_audio_btn, 16, 0);
    lv_obj_set_style_bg_color(s_test_audio_btn, lv_color_hex(0x1E4F62), 0);
    lv_obj_set_style_border_width(s_test_audio_btn, 0, 0);
    lv_obj_add_event_cb(s_test_audio_btn, display_test_audio_btn_cb, LV_EVENT_CLICKED, NULL);

    s_test_audio_btn_label = lv_label_create(s_test_audio_btn);
    lv_obj_set_style_text_font(s_test_audio_btn_label, &lv_font_montserrat_14, 0);
    display_text_set_color(s_test_audio_btn_label, lv_color_hex(0xFFFFFF), 0);
    display_text_set(s_test_audio_btn_label, "Audio");
    lv_obj_center(s_test_audio_btn_label);
}

static void display_build_main_page(lv_obj_t *screen)
{
    lv_obj_t *status_card = NULL;
    lv_obj_t *qr_card = NULL;
    lv_obj_t *qr_box = NULL;

    s_main_page = lv_obj_create(screen);
    lv_obj_set_size(s_main_page, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_opa(s_main_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_main_page, 0, 0);
    lv_obj_set_style_pad_all(s_main_page, 0, 0);
    lv_obj_clear_flag(s_main_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_main_page, LV_OBJ_FLAG_HIDDEN);

    display_prepare_figma_page(s_main_page);
    (void)display_create_figma_header(s_main_page,
                                      "查看",
                                      display_device_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x20BF7A),
                                      NULL);

    status_card = display_create_figma_box(s_main_page,
                                           8,
                                           36,
                                           142,
                                           48,
                                           lv_color_hex(0xFFFFFF),
                                           lv_color_hex(0xD5E0EB),
                                           8);
    display_create_device_status_row(status_card,
                                     5,
                                     "连接状态",
                                     "未连接",
                                     lv_color_hex(0xBCCAD8),
                                     &s_device_connection_dot,
                                     &s_device_connection_value_label);
    display_create_device_status_row(status_card,
                                     24,
                                     "开门指示",
                                     "未开门",
                                     lv_color_hex(0xF59E0B),
                                     &s_device_door_dot,
                                     &s_device_door_value_label);

    display_create_device_volume_card(s_main_page,
                                      91,
                                      "扬声器音量",
                                      "0",
                                      DISPLAY_DEVICE_VOLUME_RECEIVE_DOWN,
                                      DISPLAY_DEVICE_VOLUME_RECEIVE_UP,
                                      DISPLAY_DEVICE_VOLUME_RECEIVE_MUTE,
                                      &s_device_receive_volume_label,
                                      &s_device_receive_mute_label);
    display_create_device_volume_card(s_main_page,
                                      165,
                                      "麦克风音量",
                                      "0",
                                      DISPLAY_DEVICE_VOLUME_SEND_DOWN,
                                      DISPLAY_DEVICE_VOLUME_SEND_UP,
                                      DISPLAY_DEVICE_VOLUME_SEND_MUTE,
                                      &s_device_send_volume_label,
                                      &s_device_send_mute_label);

    qr_card = display_create_figma_box(s_main_page,
                                       158,
                                       36,
                                       154,
                                       176,
                                       lv_color_hex(0xFFFFFF),
                                       lv_color_hex(0xD5E0EB),
                                       8);
    qr_box = display_create_figma_box(qr_card,
                                      2,
                                      6,
                                      DISPLAY_DEVICE_QR_SIZE,
                                      DISPLAY_DEVICE_QR_SIZE,
                                      lv_color_hex(0xFFFFFF),
                                      lv_color_hex(0xFFFFFF),
                                      0);
    lv_obj_add_flag(qr_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(qr_box, display_device_qr_tap_cb, LV_EVENT_CLICKED, NULL);
    display_create_device_qr(qr_box);
    s_main_device_id_label = display_create_figma_text(qr_card,
                                                       "--",
                                                       DISPLAY_MAIN_DEVICE_ID_LABEL_X,
                                                       DISPLAY_MAIN_DEVICE_ID_LABEL_Y,
                                                       DISPLAY_MAIN_DEVICE_ID_LABEL_WIDTH,
                                                       lv_color_hex(0x64758A),
                                                       DISPLAY_MAIN_DEVICE_ID_LABEL_FONT_SIZE,
                                                       LV_TEXT_ALIGN_CENTER);
    if (s_main_device_id_label != NULL) {
        lv_label_set_long_mode(s_main_device_id_label, LV_LABEL_LONG_DOT);
        lv_obj_clear_flag(s_main_device_id_label, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }
    display_update_main_page(&s_last_status);

}

static void display_build_uuid_edit_page(lv_obj_t *screen)
{
    s_uuid_edit_page = lv_obj_create(screen);
    lv_obj_set_size(s_uuid_edit_page, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_opa(s_uuid_edit_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_uuid_edit_page, 0, 0);
    lv_obj_set_style_pad_all(s_uuid_edit_page, 0, 0);
    lv_obj_clear_flag(s_uuid_edit_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_uuid_edit_page, LV_OBJ_FLAG_HIDDEN);

    display_prepare_figma_page(s_uuid_edit_page);
    (void)display_create_figma_header(s_uuid_edit_page,
                                      "Device ID",
                                      display_uuid_back_btn_cb,
                                      "保存",
                                      lv_color_hex(0x20BF7A),
                                      display_uuid_save_btn_cb);

    s_uuid_edit_hint_label = display_create_figma_text(s_uuid_edit_page,
                                                       "A-Z / 0-9 / 符号",
                                                       DISPLAY_UUID_HINT_LEFT,
                                                       DISPLAY_UUID_HINT_TOP,
                                                       DISPLAY_UUID_HINT_WIDTH,
                                                       lv_color_hex(0x64758A),
                                                       12,
                                                       LV_TEXT_ALIGN_LEFT);
    s_uuid_edit_length_label = display_create_figma_text(s_uuid_edit_page,
                                                         "",
                                                         DISPLAY_DRIVER_WIDTH - DISPLAY_UUID_HINT_LEFT - DISPLAY_UUID_LENGTH_WIDTH,
                                                         DISPLAY_UUID_HINT_TOP,
                                                         DISPLAY_UUID_LENGTH_WIDTH,
                                                         lv_color_hex(0x64758A),
                                                         12,
                                                         LV_TEXT_ALIGN_RIGHT);

    s_uuid_ta = lv_textarea_create(s_uuid_edit_page);
    lv_obj_set_pos(s_uuid_ta, 8, DISPLAY_UUID_INPUT_TOP);
    lv_obj_set_size(s_uuid_ta, DISPLAY_UUID_INPUT_WIDTH, DISPLAY_UUID_INPUT_HEIGHT);
    lv_textarea_set_one_line(s_uuid_ta, true);
    lv_textarea_set_max_length(s_uuid_ta, DEVICE_UUID_EDIT_MAX_LEN);
    lv_textarea_set_accepted_chars(s_uuid_ta, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    lv_textarea_set_placeholder_text(s_uuid_ta, "Device ID");
    lv_obj_set_style_radius(s_uuid_ta, 8, 0);
    lv_obj_set_style_border_width(s_uuid_ta, 1, 0);
    lv_obj_set_style_border_color(s_uuid_ta, lv_color_hex(0xD5E0EB), 0);
    lv_obj_set_style_bg_color(s_uuid_ta, lv_color_hex(0xFFFFFF), 0);
    display_text_set_color(s_uuid_ta, lv_color_hex(0x10243E), 0);
    lv_obj_set_style_text_font(s_uuid_ta, display_ascii_font(12), 0);
    lv_obj_set_style_pad_left(s_uuid_ta, 12, 0);
    lv_obj_set_style_pad_right(s_uuid_ta, 12, 0);

    s_uuid_edit_status_label = display_create_figma_text(s_uuid_edit_page,
                                                         "请输入 4-12 位",
                                                         8,
                                                         DISPLAY_UUID_STATUS_TOP,
                                                         DISPLAY_UUID_STATUS_WIDTH,
                                                         lv_color_hex(0x64758A),
                                                         12,
                                                         LV_TEXT_ALIGN_LEFT);

    s_uuid_keyboard = lv_btnmatrix_create(s_uuid_edit_page);
    lv_obj_set_pos(s_uuid_keyboard, DISPLAY_UUID_KEYBOARD_LEFT, DISPLAY_UUID_KEYBOARD_TOP);
    lv_obj_set_size(s_uuid_keyboard, DISPLAY_UUID_KEYBOARD_WIDTH, DISPLAY_UUID_KEYBOARD_HEIGHT);
    lv_btnmatrix_set_map(s_uuid_keyboard, (const char **)s_uuid_keyboard_map);
    lv_obj_set_style_bg_opa(s_uuid_keyboard, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_uuid_keyboard, 0, 0);
    lv_obj_set_style_pad_all(s_uuid_keyboard, 0, 0);
    lv_obj_set_style_pad_row(s_uuid_keyboard, 3, 0);
    lv_obj_set_style_pad_column(s_uuid_keyboard, 2, 0);
    lv_obj_set_style_radius(s_uuid_keyboard, 6, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_uuid_keyboard, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_uuid_keyboard, lv_color_hex(0xD7EAFB), LV_PART_ITEMS | LV_STATE_PRESSED);
    display_text_set_color(s_uuid_keyboard, lv_color_hex(0x10243E), LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_uuid_keyboard, 0, LV_PART_ITEMS);
    lv_obj_clear_flag(s_uuid_keyboard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_uuid_keyboard, display_uuid_keyboard_value_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_uuid_keyboard,
                        display_uuid_keyboard_draw_part_event_cb,
                        LV_EVENT_DRAW_PART_BEGIN,
                        NULL);

    display_update_uuid_edit_feedback(NULL, lv_color_hex(0x64758A));
}

static void display_build_wifi_page(lv_obj_t *screen)
{
    lv_obj_t *header = NULL;
    lv_obj_t *label = NULL;
    lv_obj_t *btn = NULL;
    lv_obj_t *list_panel = NULL;

    s_wifi_page = lv_obj_create(screen);
    lv_obj_set_size(s_wifi_page, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_opa(s_wifi_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wifi_page, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_page, 0, 0);
    lv_obj_clear_flag(s_wifi_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wifi_page, LV_OBJ_FLAG_HIDDEN);

    display_prepare_figma_page(s_wifi_page);
    (void)display_create_figma_header(s_wifi_page,
                                      "Wi-Fi 设置",
                                      display_wifi_back_btn_cb,
                                      "刷新",
                                      lv_color_hex(0x1768B7),
                                      display_wifi_refresh_btn_cb);

    s_wifi_connection_state_label = display_create_figma_text(s_wifi_page,
                                                              "未连接 Wi-Fi",
                                                              8,
                                                              37,
                                                              210,
                                                              lv_color_hex(0x64758A),
                                                              12,
                                                              LV_TEXT_ALIGN_LEFT);
    s_wifi_scan_state_label = display_create_figma_text(s_wifi_page,
                                                        "扫描中",
                                                        242,
                                                        37,
                                                        70,
                                                        lv_color_hex(0x1768B7),
                                                        12,
                                                        LV_TEXT_ALIGN_RIGHT);
    if (s_wifi_scan_state_label != NULL) {
        lv_obj_add_flag(s_wifi_scan_state_label, LV_OBJ_FLAG_HIDDEN);
    }
    s_wifi_scan_count_label = lv_label_create(s_wifi_page);
    lv_obj_set_pos(s_wifi_scan_count_label, 242, 37);
    lv_obj_set_width(s_wifi_scan_count_label, 70);
    lv_label_set_long_mode(s_wifi_scan_count_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_wifi_scan_count_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_wifi_scan_count_label, display_ascii_font(12), 0);
    display_text_set_color(s_wifi_scan_count_label, lv_color_hex(0x64758A), 0);
    display_text_set(s_wifi_scan_count_label, "0 APs");

    list_panel = display_create_figma_box(s_wifi_page,
                                          8,
                                          58,
                                          304,
                                          174,
                                          lv_color_hex(0xE7F1FB),
                                          lv_color_hex(0xD5E0EB),
                                          6);

    s_wifi_list = lv_obj_create(list_panel);
    lv_obj_remove_style_all(s_wifi_list);
    lv_obj_set_pos(s_wifi_list, 0, 0);
    lv_obj_set_size(s_wifi_list, 304, 174);
    lv_obj_set_style_bg_opa(s_wifi_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_wifi_list, 6, 0);
    lv_obj_set_style_pad_row(s_wifi_list, 8, 0);
    lv_obj_set_flex_flow(s_wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_wifi_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_wifi_list, LV_DIR_VER);
    lv_obj_clear_flag(s_wifi_list, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scrollbar_mode(s_wifi_list, LV_SCROLLBAR_MODE_OFF);

    for (uint16_t index = 0; index < DISPLAY_WIFI_SCAN_MAX; ++index) {
        lv_obj_t *row = lv_btn_create(s_wifi_list);
        lv_obj_t *label_row = lv_label_create(row);
        lv_obj_t *label_rssi = lv_label_create(row);

        lv_obj_set_width(row, 292);
        display_style_wifi_list_button(row);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(row,
                            display_wifi_ap_select_cb,
                            LV_EVENT_CLICKED,
                            (void *)(uintptr_t)index);
        lv_obj_set_width(label_row, 178);
        lv_label_set_long_mode(label_row, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(label_row, LV_TEXT_ALIGN_LEFT, 0);
        display_text_set_color(label_row, lv_color_hex(0x10243E), 0);
        lv_obj_set_style_text_font(label_row, display_ascii_font(14), 0);
        display_text_set(label_row, "");
        lv_obj_align(label_row, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_set_width(label_rssi, 70);
        lv_obj_set_style_text_align(label_rssi, LV_TEXT_ALIGN_RIGHT, 0);
        display_text_set_color(label_rssi, lv_color_hex(0xF59E0B), 0);
        lv_obj_set_style_text_font(label_rssi, display_ascii_font(12), 0);
        display_text_set(label_rssi, "");
        lv_obj_align(label_rssi, LV_ALIGN_RIGHT_MID, 0, 0);

        s_wifi_list_buttons[index] = row;
        s_wifi_list_ssid_labels[index] = label_row;
        s_wifi_list_rssi_labels[index] = label_rssi;
    }

    display_update_wifi_scan_state(&s_last_status);
    display_refresh_wifi_list(&s_last_status);
    return;

    header = lv_obj_create(s_wifi_page);
    lv_obj_set_pos(header, 8, 6);
    lv_obj_set_size(header, 304, 28);
    lv_obj_set_style_bg_color(header, lv_color_hex(0xF5E7D3), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    btn = lv_btn_create(header);
    lv_obj_set_pos(btn, 0, 0);
    lv_obj_set_size(btn, 60, 28);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xD97135), 0);
    lv_obj_add_event_cb(btn, display_wifi_back_btn_cb, LV_EVENT_CLICKED, NULL);
    label = lv_label_create(btn);
    display_text_set(label, "Back");
    lv_obj_center(label);

    label = lv_label_create(header);
    lv_obj_set_pos(label, 72, 6);
    display_text_set_color(label, lv_color_hex(0x183642), 0);
    display_text_set(label, "WiFi Settings");

    btn = lv_btn_create(header);
    lv_obj_set_pos(btn, 228, 0);
    lv_obj_set_size(btn, 76, 28);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x52708B), 0);
    lv_obj_add_event_cb(btn, display_wifi_refresh_btn_cb, LV_EVENT_CLICKED, NULL);
    label = lv_label_create(btn);
    display_text_set(label, "Refresh");
    lv_obj_center(label);

    s_wifi_connection_state_label = lv_label_create(s_wifi_page);
    lv_obj_set_pos(s_wifi_connection_state_label, 10, 40);
    lv_obj_set_width(s_wifi_connection_state_label, DISPLAY_WIFI_STATUS_LEFT_WIDTH);
    lv_label_set_long_mode(s_wifi_connection_state_label, LV_LABEL_LONG_DOT);
    display_text_set_color(s_wifi_connection_state_label, lv_color_hex(0x48656F), 0);
    display_text_set(s_wifi_connection_state_label, "WiFi offline");

    s_wifi_scan_state_label = lv_label_create(s_wifi_page);
    lv_obj_set_pos(s_wifi_scan_state_label, 214, 40);
    lv_obj_set_width(s_wifi_scan_state_label, DISPLAY_WIFI_STATUS_RIGHT_WIDTH);
    lv_obj_set_style_text_align(s_wifi_scan_state_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_wifi_scan_state_label, LV_LABEL_LONG_DOT);
    display_text_set_color(s_wifi_scan_state_label, lv_color_hex(0x52708B), 0);
    display_text_set(s_wifi_scan_state_label, "Scanning");

    list_panel = display_create_card(s_wifi_page, 8, 58, 304, 174);
    lv_obj_set_style_radius(list_panel, 0, 0);
    lv_obj_set_style_border_width(list_panel, 0, 0);
    lv_obj_set_style_pad_all(list_panel, 0, 0);

    s_wifi_list = lv_obj_create(list_panel);
    lv_obj_set_pos(s_wifi_list, 0, 0);
    lv_obj_set_size(s_wifi_list, 288, 158);
    lv_obj_set_style_bg_color(s_wifi_list, lv_color_hex(0x173440), 0);
    lv_obj_set_style_border_width(s_wifi_list, 0, 0);
    lv_obj_set_style_radius(s_wifi_list, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_list, 8, 0);
    lv_obj_set_style_pad_row(s_wifi_list, 8, 0);
    lv_obj_set_flex_flow(s_wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_wifi_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_wifi_list, LV_DIR_VER);
    lv_obj_clear_flag(s_wifi_list, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scrollbar_mode(s_wifi_list, LV_SCROLLBAR_MODE_OFF);

    for (uint16_t index = 0; index < DISPLAY_WIFI_SCAN_MAX; ++index) {
        lv_obj_t *row = lv_btn_create(s_wifi_list);
        lv_obj_t *label_row = lv_label_create(row);
        lv_obj_t *label_rssi = lv_label_create(row);

        lv_obj_set_width(row, lv_pct(100));
        display_style_wifi_list_button(row);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(row,
                            display_wifi_ap_select_cb,
                            LV_EVENT_CLICKED,
                            (void *)(uintptr_t)index);
        lv_obj_set_width(label_row, DISPLAY_WIFI_LIST_SSID_WIDTH);
        lv_label_set_long_mode(label_row, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(label_row, LV_TEXT_ALIGN_LEFT, 0);
        display_text_set_color(label_row, lv_color_hex(0x183642), 0);
        display_text_set(label_row, "");
        lv_obj_align(label_row, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_set_width(label_rssi, DISPLAY_WIFI_LIST_RSSI_WIDTH);
        lv_obj_set_style_text_align(label_rssi, LV_TEXT_ALIGN_RIGHT, 0);
        display_text_set_color(label_rssi, lv_color_hex(0x48656F), 0);
        display_text_set(label_rssi, "");
        lv_obj_align(label_rssi, LV_ALIGN_RIGHT_MID, 0, 0);

        s_wifi_list_buttons[index] = row;
        s_wifi_list_ssid_labels[index] = label_row;
        s_wifi_list_rssi_labels[index] = label_rssi;
    }
}

static void display_build_wifi_connect_page(lv_obj_t *screen)
{
    s_wifi_connect_page = lv_obj_create(screen);
    lv_obj_set_size(s_wifi_connect_page, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_opa(s_wifi_connect_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wifi_connect_page, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_connect_page, 0, 0);
    lv_obj_clear_flag(s_wifi_connect_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wifi_connect_page, LV_OBJ_FLAG_HIDDEN);

    display_prepare_figma_page(s_wifi_connect_page);
    (void)display_create_figma_header(s_wifi_connect_page,
                                      "连接 Wi-Fi",
                                      display_wifi_connect_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x000000),
                                      NULL);

    s_wifi_connect_hint_label = display_create_figma_text(s_wifi_connect_page,
                                                          "输入密码加入",
                                                          DISPLAY_WIFI_CONNECT_HINT_LEFT,
                                                          DISPLAY_WIFI_CONNECT_HINT_TOP,
                                                          DISPLAY_WIFI_CONNECT_HINT_WIDTH,
                                                          lv_color_hex(0x64758A),
                                                          12,
                                                          LV_TEXT_ALIGN_LEFT);
    s_wifi_connect_rssi_label = display_create_figma_text(s_wifi_connect_page,
                                                          "",
                                                          DISPLAY_DRIVER_WIDTH - DISPLAY_WIFI_CONNECT_HINT_LEFT - DISPLAY_WIFI_CONNECT_RSSI_WIDTH,
                                                          DISPLAY_WIFI_CONNECT_HINT_TOP,
                                                          DISPLAY_WIFI_CONNECT_RSSI_WIDTH,
                                                          lv_color_hex(0xF59E0B),
                                                          12,
                                                          LV_TEXT_ALIGN_RIGHT);

    s_password_ta = lv_textarea_create(s_wifi_connect_page);
    lv_obj_set_pos(s_password_ta, 8, DISPLAY_WIFI_CONNECT_INPUT_TOP);
    lv_obj_set_size(s_password_ta,
                    DISPLAY_WIFI_CONNECT_INPUT_WIDTH,
                    DISPLAY_WIFI_CONNECT_INPUT_HEIGHT);
    lv_textarea_set_one_line(s_password_ta, true);
    lv_textarea_set_password_mode(s_password_ta, false);
    lv_textarea_set_placeholder_text(s_password_ta, "Password");
    lv_obj_set_style_radius(s_password_ta, 8, 0);
    lv_obj_set_style_border_width(s_password_ta, 1, 0);
    lv_obj_set_style_border_color(s_password_ta, lv_color_hex(0xD5E0EB), 0);
    lv_obj_set_style_bg_color(s_password_ta, lv_color_hex(0xFFFFFF), 0);
    display_text_set_color(s_password_ta, lv_color_hex(0x10243E), 0);
    lv_obj_set_style_text_font(s_password_ta, display_ascii_font(12), 0);
    lv_obj_set_style_pad_left(s_password_ta, 12, 0);
    lv_obj_set_style_pad_right(s_password_ta, 12, 0);
    lv_obj_add_event_cb(s_password_ta, display_textarea_event_cb, LV_EVENT_ALL, NULL);

    s_wifi_connect_details_label = display_create_figma_text(s_wifi_connect_page,
                                                             "",
                                                             8,
                                                             DISPLAY_WIFI_CONNECT_DETAILS_TOP,
                                                             DISPLAY_WIFI_CONNECT_DETAILS_WIDTH,
                                                             lv_color_hex(0x64758A),
                                                             12,
                                                             LV_TEXT_ALIGN_LEFT);
    lv_obj_add_flag(s_wifi_connect_details_label, LV_OBJ_FLAG_HIDDEN);

    s_keyboard = lv_btnmatrix_create(s_wifi_connect_page);
    lv_obj_set_style_bg_opa(s_keyboard, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_keyboard, 0, 0);
    lv_obj_set_style_border_width(s_keyboard, 0, 0);
    lv_obj_set_style_radius(s_keyboard, 0, 0);
    lv_obj_set_style_pad_row(s_keyboard, 3, 0);
    lv_obj_set_style_pad_column(s_keyboard, 2, 0);
    lv_obj_set_style_pad_all(s_keyboard, 0, LV_PART_ITEMS);
    display_text_set_color(s_keyboard, lv_color_hex(0x10243E), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(0xD7EAFB), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_keyboard, 5, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_keyboard, 0, LV_PART_ITEMS);
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_SCROLLABLE);
    display_set_wifi_keyboard_mode(LV_KEYBOARD_MODE_USER_1);
    lv_obj_add_event_cb(s_keyboard, display_keyboard_value_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_keyboard, display_keyboard_draw_part_event_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_keyboard, display_keyboard_event_cb, LV_EVENT_ALL, NULL);
    display_layout_wifi_keyboard();
}

static void display_build_ui(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xF3F8FB), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    display_build_home_page(screen);
    display_build_main_page(screen);

    display_show_home_page();
}

static void display_refresh_timer(lv_timer_t *timer)
{
    (void)timer;

    display_status_t *status = &s_refresh_status;
    display_status_t *previous_status = &s_refresh_previous_status;
    bool main_page_visible = display_page_is_visible(s_main_page);
    bool call_page_visible = display_page_is_visible(s_call_page);
    bool call_list_page_visible = display_page_is_visible(s_call_list_page);
    bool call_active_page_visible = display_page_is_visible(s_call_active_page);
    bool wechat_page_visible = display_page_is_visible(s_wechat_page);
    bool wechat_list_page_visible = display_page_is_visible(s_wechat_list_page);
    bool wechat_active_page_visible = display_page_is_visible(s_wechat_active_page);
    bool network_test_page_visible = display_page_is_visible(s_network_test_page);
    bool tirtc_config_page_visible = display_page_is_visible(s_tirtc_config_page);
    bool test_page_visible = display_page_is_visible(s_test_page);
    bool ota_page_visible = display_page_is_visible(s_ota_page);
    bool ai_chat_page_visible = display_page_is_visible(s_ai_chat_page);
    bool ai_settings_page_visible = display_page_is_visible(s_ai_chat_settings_page);
    bool wifi_page_visible = display_page_is_visible(s_wifi_page);
    bool wifi_connect_page_visible = display_page_is_visible(s_wifi_connect_page);
    bool wifi_status_changed = false;
    bool wifi_scan_refresh_due = false;
    int64_t now_us = esp_timer_get_time();
    int64_t refresh_start_us = now_us;

    *previous_status = s_last_status;
    if (s_snapshot_provider != NULL) {
        s_snapshot_provider(status, s_snapshot_ctx);
    } else {
        memset(status, 0, sizeof(*status));
    }
    s_last_status = *status;
    if (display_sync_call_contacts_from_status(status)) {
        display_invalidate_call_list_page();
        if (call_list_page_visible) {
            display_show_call_list_page();
        }
    }

    display_update_home_status_bar(status);
    display_update_binding_prompt(status);

    if (main_page_visible) {
        display_update_main_page(status);
    }

    if (call_page_visible) {
        display_update_call_page(status);
    }

    if (call_active_page_visible) {
        display_update_call_active_page(status);
    }

    if (wechat_page_visible) {
        display_update_wechat_page(status);
    }

    if (wechat_list_page_visible) {
        display_update_wechat_contact_list(status);
    }

    if (wechat_active_page_visible) {
        display_update_wechat_active_page(status);
        if (!display_wechat_call_state_keeps_active_page(status->wechat_call_state)) {
            s_wechat_active_started_us = 0;
            display_show_wechat_page();
        }
    }

    if (wifi_page_visible || wifi_connect_page_visible || s_wifi_connect_pending || s_last_wifi_scan_request_us > 0) {
        wifi_status_changed = !display_wifi_scan_equals(status, previous_status) ||
                              status->network_connected != previous_status->network_connected ||
                              status->network_connect_failed != previous_status->network_connect_failed ||
                              strcmp(status->network_ssid, previous_status->network_ssid) != 0;

        if (s_last_wifi_scan_request_us > 0) {
            int64_t scan_elapsed_us = now_us - s_last_wifi_scan_request_us;
            wifi_scan_refresh_due = true;
            if ((!status->wifi_scan_in_progress &&
                 scan_elapsed_us > DISPLAY_WIFI_SCAN_REFRESH_GRACE_US) ||
                scan_elapsed_us > DISPLAY_WIFI_SCAN_REFRESH_TIMEOUT_US) {
                s_last_wifi_scan_request_us = 0;
            }
        }

        if (wifi_page_visible && (wifi_status_changed || wifi_scan_refresh_due)) {
            display_update_wifi_scan_state(status);
            display_refresh_wifi_list(status);
        }

        if (wifi_connect_page_visible || s_wifi_connect_pending) {
            display_update_wifi_connect_feedback(status);
        }
        if (wifi_connect_page_visible && wifi_status_changed) {
            display_update_wifi_connect_details_line(status);
            display_update_wifi_connect_status_line(status);
        }
    }

    if (network_test_page_visible) {
        display_update_network_test_page(status);
    }
    if (tirtc_config_page_visible) {
        display_update_tirtc_config_page(status);
    }
    if (test_page_visible) {
        display_update_test_page(status);
    }
    if (ota_page_visible || status->ota_running) {
        display_update_ota_page(status);
    }
    if (ai_chat_page_visible) {
        display_update_ai_chat_page(status);
    }
    if (ai_settings_page_visible) {
        display_update_ai_chat_settings_page(status);
    }

    if (status->network_connected &&
        !previous_status->network_connected &&
        !status->ping_running &&
        s_actions.on_ping_test != NULL) {
        esp_err_t ping_ret = s_actions.on_ping_test(s_actions.ctx);
        if (ping_ret != ESP_OK) {
            ESP_LOGW(TAG, "auto ping start failed: %s", esp_err_to_name(ping_ret));
        }
    }

    if (status->wechat_incoming_call_pending) {
        display_show_call_alert(true);
    } else if (status->rtc_incoming_call_pending) {
        display_show_call_alert(false);
    } else {
        display_hide_call_alert();
    }

    int64_t refresh_elapsed_us = esp_timer_get_time() - refresh_start_us;
    if (refresh_elapsed_us > DISPLAY_REFRESH_SLOW_LOG_US &&
        refresh_start_us - s_refresh_slow_last_log_us > DISPLAY_REFRESH_SLOW_LOG_INTERVAL_US) {
        s_refresh_slow_last_log_us = refresh_start_us;
        ESP_LOGD(TAG,
                 "LVGL refresh slow: elapsed=%lldus home=%d main=%d ai=%d wifi=%d ota=%d",
                 (long long)refresh_elapsed_us,
                 display_page_is_visible(s_home_page) ? 1 : 0,
                 main_page_visible ? 1 : 0,
                 ai_chat_page_visible ? 1 : 0,
                 (wifi_page_visible || wifi_connect_page_visible) ? 1 : 0,
                 ota_page_visible ? 1 : 0);
    }
}

esp_err_t display_init(const display_actions_t *actions)
{
    display_driver_handles_t driver_handles = {0};

    if (s_display_initialized) {
        return ESP_OK;
    }

    if (actions != NULL) {
        s_actions = *actions;
    }

    ESP_RETURN_ON_ERROR(display_allocate_status_buffers(), TAG, "display status buffers alloc failed");
    ESP_RETURN_ON_ERROR(display_driver_init(&driver_handles), TAG, "screen driver init failed");
    s_display = driver_handles.display;
    s_touch_indev = driver_handles.touch_indev;
    if (s_display == NULL || s_touch_indev == NULL) {
        return ESP_FAIL;
    }

    if (!lvgl_port_lock(0)) {
        ESP_LOGW(TAG, "lvgl lock busy during init");
    }
    display_build_ui();
    lv_timer_create(display_refresh_timer, 250, NULL);
    lvgl_port_unlock();

    s_display_initialized = true;
    return ESP_OK;
}

void display_set_snapshot_provider(display_snapshot_cb_t cb, void *ctx)
{
    s_snapshot_provider = cb;
    s_snapshot_ctx = ctx;
}

static lv_obj_t *display_find_tap_target_locked(const lv_point_t *point)
{
    lv_obj_t *target = NULL;

    if (s_display == NULL || point == NULL) {
        return NULL;
    }

    target = lv_indev_search_obj(lv_disp_get_layer_sys(s_display), (lv_point_t *)point);
    if (target == NULL) {
        target = lv_indev_search_obj(lv_disp_get_layer_top(s_display), (lv_point_t *)point);
    }
    if (target == NULL) {
        target = lv_indev_search_obj(lv_disp_get_scr_act(s_display), (lv_point_t *)point);
    }

    return target;
}

static lv_obj_t *display_promote_tap_target_locked(lv_obj_t *target)
{
    while (target != NULL) {
        if (target->spec_attr != NULL && target->spec_attr->event_dsc_cnt > 0) {
            return target;
        }
        target = lv_obj_get_parent(target);
    }

    return NULL;
}

static lv_obj_t *display_find_scroll_target_locked(lv_obj_t *target)
{
    while (target != NULL) {
        if (lv_obj_has_flag(target, LV_OBJ_FLAG_SCROLLABLE)) {
            return target;
        }
        target = lv_obj_get_parent(target);
    }

    return NULL;
}

static uint16_t display_button_matrix_button_at_point_locked(lv_obj_t *button_matrix_obj,
                                                             const lv_point_t *point)
{
    if (button_matrix_obj == NULL || point == NULL) {
        return LV_BTNMATRIX_BTN_NONE;
    }

    lv_obj_update_layout(button_matrix_obj);

    lv_btnmatrix_t *button_matrix = (lv_btnmatrix_t *)button_matrix_obj;
    if (button_matrix->button_areas == NULL) {
        return LV_BTNMATRIX_BTN_NONE;
    }

    lv_area_t matrix_area;
    lv_obj_get_coords(button_matrix_obj, &matrix_area);

    for (uint16_t button_id = 0; button_id < button_matrix->btn_cnt; ++button_id) {
        lv_area_t button_area = button_matrix->button_areas[button_id];
        button_area.x1 += matrix_area.x1;
        button_area.x2 += matrix_area.x1;
        button_area.y1 += matrix_area.y1;
        button_area.y2 += matrix_area.y1;

        if (point->x >= button_area.x1 && point->x <= button_area.x2 &&
            point->y >= button_area.y1 && point->y <= button_area.y2) {
            return button_id;
        }
    }

    return LV_BTNMATRIX_BTN_NONE;
}

static bool display_is_keyboard_object(const lv_obj_t *target)
{
    return target != NULL &&
           (target == s_keyboard ||
            target == s_tirtc_edit_keyboard ||
            target == s_call_add_edit_keyboard ||
            target == s_wechat_add_edit_keyboard);
}

static esp_err_t display_dispatch_tap_locked(lv_obj_t *target, const lv_point_t *point)
{
    if (target == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if (target == s_password_ta ||
        target == s_uuid_ta ||
        target == s_tirtc_edit_ta ||
        target == s_call_add_edit_ta ||
        target == s_wechat_add_edit_ta) {
        return lv_event_send(target, LV_EVENT_FOCUSED, s_touch_indev) == LV_RES_OK ? ESP_OK : ESP_FAIL;
    }

    if (display_is_keyboard_object(target)) {
        uint16_t button_id = display_button_matrix_button_at_point_locked(target, point);
        if (button_id == LV_BTNMATRIX_BTN_NONE) {
            return ESP_ERR_NOT_FOUND;
        }
        lv_btnmatrix_set_selected_btn(target, button_id);
        lv_res_t ret = lv_event_send(target, LV_EVENT_VALUE_CHANGED, s_touch_indev);
        lv_btnmatrix_set_selected_btn(target, LV_BTNMATRIX_BTN_NONE);
        return ret == LV_RES_OK ? ESP_OK : ESP_FAIL;
    }

    if (lv_event_send(target, LV_EVENT_PRESSED, s_touch_indev) != LV_RES_OK) {
        return ESP_FAIL;
    }
    if (lv_event_send(target, LV_EVENT_RELEASED, s_touch_indev) != LV_RES_OK) {
        return ESP_FAIL;
    }
    if (lv_event_send(target, LV_EVENT_SHORT_CLICKED, s_touch_indev) != LV_RES_OK) {
        return ESP_FAIL;
    }
    return lv_event_send(target, LV_EVENT_CLICKED, s_touch_indev) == LV_RES_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t display_debug_tap_in_lvgl(uint16_t x, uint16_t y)
{
    lv_point_t point = {
        .x = (lv_coord_t)x,
        .y = (lv_coord_t)y,
    };
    lv_obj_t *target = display_find_tap_target_locked(&point);
    target = display_promote_tap_target_locked(target);
    return display_dispatch_tap_locked(target, &point);
}

static esp_err_t display_debug_scroll_in_lvgl(uint16_t x, uint16_t y, int16_t dx, int16_t dy)
{
    lv_point_t point = {
        .x = (lv_coord_t)x,
        .y = (lv_coord_t)y,
    };
    lv_obj_t *target = display_find_tap_target_locked(&point);
    lv_obj_t *scroll_target = display_find_scroll_target_locked(target);
    if (scroll_target == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if (scroll_target == s_home_carousel && abs(dx) >= abs(dy) && abs(dx) >= 40) {
        display_home_scroll_to_page(dx < 0);
        return ESP_OK;
    }

    lv_obj_scroll_by(scroll_target, (lv_coord_t)-dx, (lv_coord_t)-dy, LV_ANIM_OFF);
    lv_obj_update_snap(scroll_target, LV_ANIM_OFF);
    if (scroll_target == s_home_carousel) {
        display_update_home_indicators();
    }

    return ESP_OK;
}

esp_err_t display_capture_bmp(uint8_t **bmp_data, size_t *bmp_size)
{
    if (bmp_data == NULL || bmp_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_display_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    *bmp_data = NULL;
    *bmp_size = 0;

    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *screen = lv_scr_act();
    uint32_t snapshot_size = lv_snapshot_buf_size_needed(screen, LV_IMG_CF_TRUE_COLOR);
    if (snapshot_size == 0) {
        lvgl_port_unlock();
        return ESP_FAIL;
    }

    uint8_t *snapshot_buf = heap_caps_malloc(snapshot_size, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (snapshot_buf == NULL) {
        snapshot_buf = malloc(snapshot_size);
    }
    if (snapshot_buf == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }

    lv_img_dsc_t snapshot = {0};
    if (lv_snapshot_take_to_buf(screen,
                                LV_IMG_CF_TRUE_COLOR,
                                &snapshot,
                                snapshot_buf,
                                snapshot_size) != LV_RES_OK) {
        free(snapshot_buf);
        lvgl_port_unlock();
        return ESP_FAIL;
    }

    uint32_t width = snapshot.header.w;
    uint32_t height = snapshot.header.h;
    uint32_t row_stride = ((width * 3U) + 3U) & ~3U;
    size_t total_size = 54U + (size_t)row_stride * height;

    lvgl_port_unlock();

    uint8_t *buffer = heap_caps_malloc(total_size, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (buffer == NULL) {
        buffer = malloc(total_size);
    }
    if (buffer == NULL) {
        free(snapshot_buf);
        return ESP_ERR_NO_MEM;
    }

    memset(buffer, 0, total_size);
    buffer[0] = 'B';
    buffer[1] = 'M';
    display_write_u32_le(&buffer[2], (uint32_t)total_size);
    display_write_u32_le(&buffer[10], 54U);
    display_write_u32_le(&buffer[14], 40U);
    display_write_u32_le(&buffer[18], width);
    display_write_u32_le(&buffer[22], height);
    display_write_u16_le(&buffer[26], 1U);
    display_write_u16_le(&buffer[28], 24U);
    display_write_u32_le(&buffer[34], row_stride * height);
    display_write_u32_le(&buffer[38], 2835U);
    display_write_u32_le(&buffer[42], 2835U);

    for (uint32_t y = 0; y < height; ++y) {
        const lv_color_t *src_row = (const lv_color_t *)(snapshot.data + ((size_t)y * width * sizeof(lv_color_t)));
        uint8_t *dst_row = buffer + 54U + ((size_t)(height - 1U - y) * row_stride);

        for (uint32_t x = 0; x < width; ++x) {
            lv_color32_t color32 = {.full = lv_color_to32(src_row[x])};
            dst_row[x * 3U + 0U] = color32.ch.blue;
            dst_row[x * 3U + 1U] = color32.ch.green;
            dst_row[x * 3U + 2U] = color32.ch.red;
        }
    }

    free(snapshot_buf);

    *bmp_data = buffer;
    *bmp_size = total_size;
    return ESP_OK;
}

esp_err_t display_debug_tap(uint16_t x, uint16_t y)
{
    if (!s_display_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (x >= display_driver_width() || y >= display_driver_height()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lvgl_port_lock(100)) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = display_debug_tap_in_lvgl(x, y);

    lvgl_port_unlock();
    return ret;
}

static void display_debug_tap_async_cb(void *user_data)
{
    display_debug_tap_request_t *request = (display_debug_tap_request_t *)user_data;

    if (request == NULL) {
        return;
    }

    (void)display_debug_tap_in_lvgl(request->x, request->y);
    free(request);
}

esp_err_t display_debug_tap_async(uint16_t x, uint16_t y)
{
    if (!s_display_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (x >= display_driver_width() || y >= display_driver_height()) {
        return ESP_ERR_INVALID_ARG;
    }

    display_debug_tap_request_t *request = malloc(sizeof(*request));
    if (request == NULL) {
        return ESP_ERR_NO_MEM;
    }

    request->x = x;
    request->y = y;

    if (!lvgl_port_lock(100)) {
        ESP_LOGW(TAG, "debug tap failed: lvgl lock timeout x=%u y=%u", x, y);
        free(request);
        return ESP_ERR_TIMEOUT;
    }

    lv_res_t async_ret = lv_async_call(display_debug_tap_async_cb, request);
    lvgl_port_unlock();
    if (async_ret != LV_RES_OK) {
        ESP_LOGW(TAG, "debug tap failed: async dispatch x=%u y=%u", x, y);
        free(request);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void display_debug_scroll_async_cb(void *user_data)
{
    display_debug_scroll_request_t *request = (display_debug_scroll_request_t *)user_data;

    if (request == NULL) {
        return;
    }

    (void)display_debug_scroll_in_lvgl(request->x, request->y, request->dx, request->dy);
    free(request);
}

esp_err_t display_debug_scroll_async(uint16_t x, uint16_t y, int16_t dx, int16_t dy)
{
    if (!s_display_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (x >= display_driver_width() || y >= display_driver_height()) {
        return ESP_ERR_INVALID_ARG;
    }

    display_debug_scroll_request_t *request = malloc(sizeof(*request));
    if (request == NULL) {
        return ESP_ERR_NO_MEM;
    }

    request->x = x;
    request->y = y;
    request->dx = dx;
    request->dy = dy;

    if (!lvgl_port_lock(100)) {
        ESP_LOGW(TAG, "debug scroll failed: lvgl lock timeout x=%u y=%u dx=%d dy=%d", x, y, dx, dy);
        free(request);
        return ESP_ERR_TIMEOUT;
    }

    lv_res_t async_ret = lv_async_call(display_debug_scroll_async_cb, request);
    lvgl_port_unlock();
    if (async_ret != LV_RES_OK) {
        ESP_LOGW(TAG, "debug scroll failed: async dispatch x=%u y=%u dx=%d dy=%d", x, y, dx, dy);
        free(request);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t display_show_remote_video_frame_rgb565(const uint16_t *pixels, uint16_t width, uint16_t height)
{
    if (!s_display_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pixels == NULL || width == 0 || height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lvgl_port_lock(100)) {
        return ESP_ERR_TIMEOUT;
    }

    display_show_remote_video_locked(pixels, width, height);
    lvgl_port_unlock();
    return ESP_OK;
}
esp_err_t display_clear_remote_video(void)
{
    if (!s_display_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lvgl_port_lock(100)) {
        return ESP_ERR_TIMEOUT;
    }

    display_clear_remote_video_locked(NULL);
    lvgl_port_unlock();
    return ESP_OK;
}
