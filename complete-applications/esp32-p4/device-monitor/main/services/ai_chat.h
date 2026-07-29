#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ai_chat_events.h"

#define AI_CHAT_DEVICE_ID_MAX       128U
#define AI_CHAT_USER_ID_MAX         128U
#define AI_CHAT_ROLE_ID_MAX         128U
#define AI_CHAT_PEER_ID_MAX         2048U
#define AI_CHAT_TOKEN_MAX           1536U
#define AI_CHAT_SECRET_MAX          128U
#define AI_CHAT_API_BASE_MAX        160U
#define AI_CHAT_DEVICE_MAC_MAX      18U
#define AI_CHAT_STATUS_TEXT_MAX     96U
#define AI_CHAT_MESSAGE_HISTORY_MAX 100
#define AI_CHAT_MESSAGE_SNAPSHOT_MAX AI_CHAT_MESSAGE_HISTORY_MAX

#if AI_CHAT_MESSAGE_HISTORY_MAX > 255
#error "AI Chat message history count must fit in uint8_t"
#endif

#if AI_CHAT_MESSAGE_SNAPSHOT_MAX > AI_CHAT_MESSAGE_HISTORY_MAX
#error "AI Chat message snapshot count cannot exceed history count"
#endif

typedef enum {
    AI_CHAT_STATE_IDLE = 0,
    AI_CHAT_STATE_STARTING,
    AI_CHAT_STATE_TOKEN,
    AI_CHAT_STATE_CONNECTING,
    AI_CHAT_STATE_CONNECTED,
    AI_CHAT_STATE_STARTING_SESSION,
    AI_CHAT_STATE_IN_SESSION,
    AI_CHAT_STATE_STOPPING,
    AI_CHAT_STATE_ERROR,
} ai_chat_state_t;

typedef void (*ai_chat_media_active_cb_t)(bool active, void *ctx);

typedef struct {
    bool enabled;
    bool video_enabled;
    char device_id[AI_CHAT_DEVICE_ID_MAX];
    char user_id[AI_CHAT_USER_ID_MAX];
    char role_id[AI_CHAT_ROLE_ID_MAX];
    char device_key[AI_CHAT_SECRET_MAX];
    char device_mac[AI_CHAT_DEVICE_MAC_MAX];
    char token_api_base[AI_CHAT_API_BASE_MAX];
    ai_chat_media_active_cb_t media_active_cb;
    void *media_active_ctx;
} ai_chat_config_t;

typedef struct {
    uint8_t caption_type;
    int64_t utterance_id;
    bool final;
    char text[AI_CHAT_CAPTION_TEXT_MAX];
} ai_chat_message_t;

typedef struct {
    ai_chat_state_t state;
    bool active;
    bool listening;
    bool cloud_speaking;
    bool video_active;
    uint32_t tx_audio_frames;
    uint32_t tx_audio_failures;
    uint32_t tx_video_frames;
    uint32_t tx_video_failures;
    uint32_t rx_commands;
    char role_id[AI_CHAT_ROLE_ID_MAX];
    char session_id[AI_CHAT_SESSION_ID_MAX];
    char asr_caption[AI_CHAT_CAPTION_TEXT_MAX];
    char tts_caption[AI_CHAT_CAPTION_TEXT_MAX];
    uint8_t message_count;
    ai_chat_message_t messages[AI_CHAT_MESSAGE_SNAPSHOT_MAX];
    char status[AI_CHAT_STATUS_TEXT_MAX];
    int last_error;
} ai_chat_snapshot_t;

esp_err_t ai_chat_init(const ai_chat_config_t *config);
esp_err_t ai_chat_configure(const ai_chat_config_t *config);
esp_err_t ai_chat_open(void);
esp_err_t ai_chat_close(void);
esp_err_t ai_chat_clear_messages(void);
esp_err_t ai_chat_handle_control_button(bool pressed);
bool ai_chat_owns_control_button(void);
bool ai_chat_can_start(void);
void ai_chat_get_snapshot(ai_chat_snapshot_t *snapshot);
const char *ai_chat_state_name(ai_chat_state_t state);
