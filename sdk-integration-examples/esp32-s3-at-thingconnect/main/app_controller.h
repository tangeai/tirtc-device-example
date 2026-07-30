#ifndef APP_CONTROLLER_H
#define APP_CONTROLLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_TEXT_SMALL 65
#define APP_TEXT_MEDIUM 129
#define APP_EVENT_PAYLOAD_SIZE 1025

typedef enum {
    APP_STATE_BOOTING = 0,
    APP_STATE_NEED_WIFI,
    APP_STATE_NETWORKING,
    APP_STATE_NEED_BINDING,
    APP_STATE_BINDING,
    APP_STATE_STARTING_PLATFORM,
    APP_STATE_READY,
    APP_STATE_RESTARTING,
    APP_STATE_ERROR,
} app_state_t;

typedef enum {
    APP_INTENT_WIFI_SET = 0,
    APP_INTENT_WIFI_CLEAR,
    APP_INTENT_BIND_START,
    APP_INTENT_AI_START,
    APP_INTENT_AI_STOP,
    APP_INTENT_AI_INTERRUPT,
    APP_INTENT_AI_SUBMIT,
    APP_INTENT_AI_UPDATE,
    APP_INTENT_AI_ACTION_RESULT,
    APP_INTENT_CALL_START,
    APP_INTENT_CALL_ACCEPT,
    APP_INTENT_CALL_REJECT,
    APP_INTENT_CALL_CANCEL,
    APP_INTENT_CALL_HANGUP,
    APP_INTENT_CONTACTS_LIST,
    APP_INTENT_PENDING_LIST,
    APP_INTENT_CONTACT_REQUEST,
    APP_INTENT_CONTACT_RESPOND,
    APP_INTENT_CONTACT_REMARK,
    APP_INTENT_CONTACT_DELETE,
    APP_INTENT_RESTART,
} app_intent_type_t;

typedef struct {
    app_intent_type_t type;
    uint32_t request_id;
    bool accepted;
    char first[APP_TEXT_MEDIUM];
    char second[APP_TEXT_MEDIUM];
    char third[APP_TEXT_MEDIUM];
} app_intent_t;

typedef struct {
    app_state_t state;
    uint32_t generation;
    bool wifi_configured;
    bool wifi_online;
    bool platform_ready;
    bool mqtt_online;
    bool tirtc_ready;
    char ssid[33];
    char ip_address[48];
    char device_id[APP_TEXT_SMALL];
    char verification_code[17];
    char session_owner[8];
    char session_state[24];
    int last_error;
} app_snapshot_t;

typedef struct {
    uint32_t app_generation;
    uint32_t revision;
    uint32_t session_generation;
    uint32_t origin_request_id;
    int64_t deadline_remaining_ms;
    bool pending_incoming_call;
    bool caller;
    bool ai_update_pending;
    bool ai_action_pending;
    bool ai_call_handoff_pending;
    int call_type;
    char owner[8];
    char state[24];
    char room_id[APP_TEXT_MEDIUM];
    char peer_id[APP_TEXT_MEDIUM];
    char ai_session_id[APP_TEXT_MEDIUM];
    int64_t ai_update_deadline_remaining_ms;
    int64_t ai_action_deadline_remaining_ms;
    int64_t ai_call_handoff_deadline_remaining_ms;
    char ai_action_id[APP_TEXT_MEDIUM];
    char ai_call_handoff_phase[24];
} app_session_snapshot_t;

typedef struct {
    bool connected;
    int adapter_state;
    int active_profile;
    int measured_profile;
    uint32_t active_session_generation;
    uint32_t measured_session_generation;
    uint32_t connection_generation;
    uint32_t tx_audio_frames;
    uint32_t tx_audio_bytes;
    uint32_t tx_video_frames;
    uint32_t tx_video_bytes;
    uint32_t rx_audio_frames;
    uint32_t rx_audio_bytes;
    uint32_t rx_video_frames;
    uint32_t rx_video_bytes;
    uint32_t send_errors;
    uint32_t first_tx_ms;
    uint32_t last_tx_ms;
    uint32_t first_rx_ms;
    uint32_t last_rx_ms;
    bool connect_request_pending;
    bool connect_callback_pending;
    uint32_t accept_callbacks_pending;
    uint32_t disconnects_pending;
    uint32_t connection_users;
    bool incoming_armed;
} app_media_snapshot_t;

typedef enum {
    APP_EVENT_SYSTEM = 0,
    APP_EVENT_WIFI,
    APP_EVENT_BIND,
    APP_EVENT_PLATFORM,
    APP_EVENT_TIRTC,
    APP_EVENT_SESSION,
    APP_EVENT_ERROR,
} app_event_domain_t;

typedef struct {
    app_event_domain_t domain;
    uint32_t generation;
    uint32_t request_id;
    int code;
    int value1;
    int value2;
    int value3;
    bool flag;
    char name[32];
    char first[APP_TEXT_MEDIUM];
    char second[APP_TEXT_MEDIUM];
    char payload[APP_EVENT_PAYLOAD_SIZE];
} app_event_t;

typedef void (*app_event_callback_t)(const app_event_t *event, void *user_data);

esp_err_t app_controller_init(app_event_callback_t callback, void *user_data);
esp_err_t app_controller_submit(const app_intent_t *intent);
void app_controller_snapshot(app_snapshot_t *snapshot);
esp_err_t app_controller_session_snapshot(app_session_snapshot_t *snapshot);
esp_err_t app_controller_media_snapshot(app_media_snapshot_t *snapshot);
const char *app_state_name(app_state_t state);

#ifdef __cplusplus
}
#endif

#endif
