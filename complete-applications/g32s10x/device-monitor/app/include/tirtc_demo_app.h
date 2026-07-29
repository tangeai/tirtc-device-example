#ifndef _TIRTC_DEMO_APP_H
#define _TIRTC_DEMO_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TIRTC_G32_APP_VERSION "0.1.1"

#define TIRTC_DEMO_ERR_SERVICE_PENDING (-1000)
#define TIRTC_DEMO_ERR_CONFIG_REQUIRED (-1001)
#define TIRTC_DEMO_ERR_NETWORK_PENDING (-1002)
#define TIRTC_DEMO_ERR_UNSUPPORTED (-1004)

#define TIRTC_DEMO_ENDPOINT_MAX 160
#define TIRTC_DEMO_DEVICE_ID_MAX 128
#define TIRTC_DEMO_DEVICE_SECRET_MAX 192
#define TIRTC_DEMO_WECHAT_OPENID_MAX 128
#define TIRTC_DEMO_WECHAT_APP_ID_MAX 96
#define TIRTC_DEMO_WECHAT_MODEL_ID_MAX 96
#define TIRTC_DEMO_AI_MESSAGE_MAX 8
#define TIRTC_DEMO_AI_CAPTION_MAX 256
#define TIRTC_DEMO_AI_AVATAR_COUNT 2
#define TIRTC_DEMO_DIAGNOSTIC_SUMMARY_MAX 192
#define TIRTC_DEMO_CONFIG_PATH "/data/tirtc/config.json"
#define TIRTC_DEMO_DEFAULT_ENDPOINT "http://ep-tirtc.tange365.com"
#define TIRTC_DEMO_UNCONFIGURED_TEXT "未配置"

typedef enum {
    TIRTC_DEMO_STATE_IDLE = 0,
    TIRTC_DEMO_STATE_READY,
    TIRTC_DEMO_STATE_UNCONFIGURED,
    TIRTC_DEMO_STATE_CONNECTED,
    TIRTC_DEMO_STATE_CALLING,
    TIRTC_DEMO_STATE_IN_CALL,
    TIRTC_DEMO_STATE_AI_TALK,
    TIRTC_DEMO_STATE_ERROR,
} tirtc_demo_state_t;

typedef struct {
    tirtc_demo_state_t state;
    int last_error;
    const char *stage;
    const char *message;
} tirtc_demo_status_t;

typedef enum {
    TIRTC_DEMO_VOLUME_RX = 0,
    TIRTC_DEMO_VOLUME_TX,
} tirtc_demo_volume_t;

typedef enum {
    TIRTC_DEMO_CALL_OUTGOING = 0,
    TIRTC_DEMO_CALL_ACCEPT,
    TIRTC_DEMO_CALL_REJECT,
    TIRTC_DEMO_CALL_HANGUP,
} tirtc_demo_call_action_t;

typedef enum {
    TIRTC_DEMO_AI_CONNECT = 0,
    TIRTC_DEMO_AI_START_TX,
    TIRTC_DEMO_AI_STOP_TX,
    TIRTC_DEMO_AI_INTERRUPT,
    TIRTC_DEMO_AI_NEW_CHAT,
} tirtc_demo_ai_action_t;

typedef struct {
    uint8_t caption_type;
    int64_t utterance_id;
    bool final;
    char text[TIRTC_DEMO_AI_CAPTION_MAX];
} tirtc_demo_ai_message_t;

typedef struct {
    const char *name;
    const char *device_id;
    const char *last_call;
    bool online;
} tirtc_demo_contact_t;

typedef struct {
    const char *name;
    const char *open_id;
    const char *app_id;
    const char *model_id;
} tirtc_demo_wechat_contact_t;

typedef struct {
    char endpoint[TIRTC_DEMO_ENDPOINT_MAX];
    char device_id[TIRTC_DEMO_DEVICE_ID_MAX];
    char device_secret[TIRTC_DEMO_DEVICE_SECRET_MAX];
    uint8_t ai_avatar;
} tirtc_demo_config_t;

typedef struct {
    char endpoint[TIRTC_DEMO_ENDPOINT_MAX];
    char peer_id[TIRTC_DEMO_DEVICE_ID_MAX];
    char codec[8];
    int sample_rate;
    int channels;
    int packet_ms;
} tirtc_demo_settings_t;

typedef struct {
    bool network_ready;
    int network_rssi;
    bool identity_ready;
    bool token_ready;
    bool cloud_online;
    bool binding_waiting;
    bool contacts_ready;
    bool sdk_ready;
    bool service_starting;
    bool peer_connected;
    bool incoming_call;
    bool call_active;
    bool audio_tx_active;
    bool audio_rx_active;
    bool ai_ready;
    bool ai_talking;
    bool wechat_ready;
    bool wechat_contacts_ready;
    bool wechat_incoming;
    bool video_tx_active;
    bool video_rx_active;
    bool door_open;
    bool diagnostic_running;
    uint8_t ai_avatar;
    uint32_t cloud_generation;
    uint32_t ai_generation;
    uint32_t diagnostic_generation;
    uint32_t call_duration_s;
    uint32_t rx_video_frames;
    uint32_t rx_video_dropped;
    const char *peer_id;
    const char *call_phase;
    const char *binding_code;
    const char *session_type;
    const char *ai_caption;
    const char *diagnostic_summary;
} tirtc_demo_runtime_t;

void tirtc_demo_app_enter(void);
void tirtc_demo_app_exit(void);
int tirtc_demo_app_return_home(void);
void tirtc_demo_app_service_init(void);
int tirtc_demo_app_start(void);
int tirtc_demo_app_restart(void);
int tirtc_demo_app_disconnect(void);
int tirtc_demo_app_refresh_network(void);
int tirtc_demo_app_request_binding(void);
int tirtc_demo_app_reset_binding(void);
int tirtc_demo_app_run_diagnostics(void);
int tirtc_demo_app_refresh_contacts(void);
int tirtc_demo_app_request_contact(const char *target_device_id);
int tirtc_demo_app_adjust_volume(tirtc_demo_volume_t volume, int delta);
int tirtc_demo_app_toggle_mute(tirtc_demo_volume_t volume);
int tirtc_demo_app_call_contact(size_t index);
int tirtc_demo_app_delete_contact(size_t index);
int tirtc_demo_app_handle_call(tirtc_demo_call_action_t action);
int tirtc_demo_app_handle_ai(tirtc_demo_ai_action_t action);
int tirtc_demo_app_set_ai_avatar(uint8_t avatar);
int tirtc_demo_app_refresh_wechat(void);
int tirtc_demo_app_call_wechat(size_t index);
int tirtc_demo_app_add_wechat_contact(const char *open_id);
int tirtc_demo_app_delete_wechat_contact(size_t index);
int tirtc_demo_app_accept_wechat(void);
int tirtc_demo_app_reject_wechat(void);
int tirtc_demo_app_prepare_external_media(void);
void tirtc_demo_app_get_status(tirtc_demo_status_t *status);
void tirtc_demo_app_get_runtime(tirtc_demo_runtime_t *runtime);
void tirtc_demo_app_get_settings(tirtc_demo_settings_t *settings);
void tirtc_demo_app_get_config(tirtc_demo_config_t *config);
int tirtc_demo_app_save_config(const tirtc_demo_config_t *config);
bool tirtc_demo_app_config_ready(void);
void tirtc_demo_app_copy_device_id(char *device_id, size_t capacity);
int tirtc_demo_app_get_volume(tirtc_demo_volume_t volume);
bool tirtc_demo_app_is_muted(tirtc_demo_volume_t volume);
size_t tirtc_demo_app_get_contacts(const tirtc_demo_contact_t **contacts);
size_t tirtc_demo_app_get_wechat_contacts(
    const tirtc_demo_wechat_contact_t **contacts);
size_t tirtc_demo_app_copy_ai_messages(tirtc_demo_ai_message_t *messages,
                                       size_t capacity);
void tirtc_demo_app_copy_ai_caption(char *caption, size_t capacity);
const char *tirtc_demo_app_state_name(tirtc_demo_state_t state);
const char *tirtc_demo_app_sdk_version(void);

#ifdef __cplusplus
}
#endif

#endif
