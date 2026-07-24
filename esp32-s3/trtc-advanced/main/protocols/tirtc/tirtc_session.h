#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "tiRTC.h"

#define TIRTC_SESSION_ENDPOINT_MAX_LEN        128
#define TIRTC_SESSION_DEVICE_LICENSE_MAX_LEN  192
#define TIRTC_SESSION_SECRET_KEY_MAX_LEN      128
#define TIRTC_SESSION_DEVICE_ID_MAX_LEN       128
#define TIRTC_SESSION_CLIENT_ID_MAX_LEN       64
#define TIRTC_SESSION_CONNECT_TOKEN_MAX_LEN   1536
#define TIRTC_SESSION_SERVICE_CODE_CLIENT_ID_CONFLICT 40305
#define TIRTC_SESSION_DEVICE_AUDIO_STREAM_ID  10U
#define TIRTC_SESSION_H5_TALKBACK_STREAM_ID   14U
#define TIRTC_TOKEN_ACCESS_ID_MAX_LEN 128
#define TIRTC_TOKEN_SUBJECT_MAX_LEN   64

typedef enum {
    TIRTC_SESSION_MODE_LISTEN = 0,
    TIRTC_SESSION_MODE_CONNECT,
} tirtc_session_mode_t;

typedef enum {
    TIRTC_SESSION_STATE_STOPPED = 0,
    TIRTC_SESSION_STATE_STARTING,
    TIRTC_SESSION_STATE_READY,
    TIRTC_SESSION_STATE_CONNECTED,
    TIRTC_SESSION_STATE_MEDIA_BOOTSTRAPPING,
    TIRTC_SESSION_STATE_DISCONNECTING,
    TIRTC_SESSION_STATE_ERROR,
} tirtc_session_state_t;

typedef struct {
    bool connected;
} tirtc_session_network_state_t;

typedef struct {
    uint32_t sample_rate_hz;
    uint8_t channels;
    uint8_t bits_per_sample;
} tirtc_session_audio_format_t;

typedef void (*tirtc_session_capture_frame_cb_t)(const uint8_t *data,
                                                 size_t data_len,
                                                 const tirtc_session_audio_format_t *format,
                                                 void *ctx);
typedef bool (*tirtc_session_command_cb_t)(tirtc_conn_t conn,
                                           uint32_t cmdw,
                                           const void *data,
                                           uint32_t data_len,
                                           void *ctx);
typedef bool (*tirtc_session_message_cb_t)(tirtc_conn_t conn,
                                           uint8_t media,
                                           uint8_t stream_id,
                                           uint8_t flags,
                                           const void *data,
                                           uint32_t data_len,
                                           void *ctx);
typedef void (*tirtc_session_connection_error_cb_t)(tirtc_conn_t conn, int error, void *ctx);
typedef void (*tirtc_session_disconnected_cb_t)(tirtc_conn_t conn, void *ctx);
typedef void (*tirtc_session_start_error_cb_t)(int error,
                                               const char *device_id,
                                               const char *client_id,
                                               void *ctx);

typedef struct {
    tirtc_session_command_cb_t on_command;
    tirtc_session_message_cb_t on_message;
    tirtc_session_connection_error_cb_t on_connection_error;
    tirtc_session_disconnected_cb_t on_disconnected;
    tirtc_session_start_error_cb_t on_start_error;
} tirtc_session_observer_t;

typedef struct {
    esp_err_t (*init)(void *ctx);
    void (*set_capture_frame_cb)(tirtc_session_capture_frame_cb_t cb, void *cb_ctx, void *ctx);
    esp_err_t (*set_capture_enabled)(bool enabled, void *ctx);
    esp_err_t (*prepare_playback_path)(void *ctx);
    esp_err_t (*submit_remote_audio)(uint8_t media,
                                     uint8_t flags,
                                     const uint8_t *data,
                                     size_t data_len,
                                     size_t *playback_data_len,
                                     void *ctx);
    esp_err_t (*submit_remote_video_jpeg)(const uint8_t *data, size_t data_len, void *ctx);
    void (*flush)(void *ctx);
} tirtc_session_media_ops_t;

typedef struct {
    bool (*is_test_video_active)(void *ctx);
    bool (*is_test_audio_active)(void *ctx);
    void (*request_test_audio_restart)(void *ctx);
} tirtc_session_hooks_t;

typedef struct {
    esp_err_t (*set_speaker_volume)(uint8_t percent, void *ctx);
    esp_err_t (*set_door_open)(bool open, void *ctx);
} tirtc_session_control_ops_t;

typedef struct {
    bool enabled;
    tirtc_session_mode_t default_session_mode;
    char service_endpoint[TIRTC_SESSION_ENDPOINT_MAX_LEN];
    char device_id[TIRTC_SESSION_DEVICE_ID_MAX_LEN];
    char client_id[TIRTC_SESSION_CLIENT_ID_MAX_LEN];
    char device_license[TIRTC_SESSION_DEVICE_LICENSE_MAX_LEN];
    char device_secret_key[TIRTC_SESSION_SECRET_KEY_MAX_LEN];
    char remote_device_id[TIRTC_SESSION_DEVICE_ID_MAX_LEN];
    char remote_device_secret_key[TIRTC_SESSION_SECRET_KEY_MAX_LEN];
    char token_access_id[TIRTC_TOKEN_ACCESS_ID_MAX_LEN];
    char token_secret_key[TIRTC_SESSION_SECRET_KEY_MAX_LEN];
    char token_subject[TIRTC_TOKEN_SUBJECT_MAX_LEN];
    uint32_t token_ttl_seconds;
} tirtc_session_config_t;

#define TIRTC_SESSION_DEBUG_TEXT_MAX_LEN 64

typedef struct {
    bool enabled;
    bool sdk_initialized;
    bool sdk_started;
    bool active_connection;
    bool call_active;
    bool incoming_call_pending;
    bool local_video_send_enabled;
    bool local_audio_send_enabled;
    tirtc_session_mode_t session_mode;
    tirtc_session_state_t state;
    uint8_t local_video_stream_id;
    uint8_t local_audio_stream_id;
    uint32_t tx_attempts;
    uint32_t tx_failures;
    uint32_t tx_video_frames;
    uint32_t tx_audio_frames;
    uint32_t rx_video_frames;
    uint32_t rx_audio_frames;
    uint32_t rx_message_frames;
    size_t tx_video_bytes;
    size_t tx_audio_bytes;
    size_t rx_video_bytes;
    size_t rx_audio_bytes;
    size_t rx_message_bytes;
    bool access_hijacking_detected;
    int last_error;
    char last_event[TIRTC_SESSION_DEBUG_TEXT_MAX_LEN];
} tirtc_session_stats_t;

typedef struct {
    bool valid;
    bool call_active;
    bool local_video_send_enabled;
    bool local_audio_send_enabled;
    bool video_stream_active;
    bool audio_stream_active;
    uint8_t rgb[3];
} tirtc_session_peer_state_t;

esp_err_t tirtc_session_init(const tirtc_session_config_t *config);
const char *tirtc_session_error_string(int error);
esp_err_t tirtc_session_configure(const tirtc_session_config_t *config);
void tirtc_session_set_identity_ready(bool ready);
esp_err_t tirtc_session_prepare_sdk(void);
esp_err_t tirtc_session_set_media_bridge(const tirtc_session_media_ops_t *ops, void *ctx);
void tirtc_session_set_hooks(const tirtc_session_hooks_t *hooks, void *ctx);
void tirtc_session_set_control_ops(const tirtc_session_control_ops_t *ops, void *ctx);
esp_err_t tirtc_session_register_observer(const tirtc_session_observer_t *observer, void *ctx);
esp_err_t tirtc_session_start_if_ready(void);
int tirtc_session_whip_connect(const char *service_desc,
                               const char *token,
                               TIRTCCONNECTCALLBACK cb,
                               void *user_data);
int tirtc_session_whip_connect_external(const char *service_desc,
                                        const char *token,
                                        TIRTCCONNECTCALLBACK cb,
                                        void *user_data);
int tirtc_session_service_request(const char *path,
                                  const char *json_body,
                                  const char *token,
                                  TIRTCSERVICEREQUESTCALLBACK cb,
                                  void *user_data);
esp_err_t tirtc_session_connect_peer(const char *remote_device_id,
                                     const char *remote_device_secret_key);
esp_err_t tirtc_session_connect_peer_with_token(const char *remote_device_id,
                                                const char *connect_token);
esp_err_t tirtc_session_restart(void);
esp_err_t tirtc_session_stop(void);
esp_err_t tirtc_session_disconnect(void);
int tirtc_session_disconnect_connection(tirtc_conn_t conn);
esp_err_t tirtc_session_accept_incoming_call(void);
esp_err_t tirtc_session_reject_incoming_call(void);
esp_err_t tirtc_session_hangup(void);
void tirtc_session_on_network_state_changed(const tirtc_session_network_state_t *state);
esp_err_t tirtc_session_set_local_video_send_enabled(bool enabled);
esp_err_t tirtc_session_set_local_audio_send_enabled(bool enabled);
esp_err_t tirtc_session_set_remote_audio_stream_id(uint8_t stream_id);
void tirtc_session_set_next_connection_auto_media(bool enabled);
void tirtc_session_set_next_connection_defer_media(bool enabled);
esp_err_t tirtc_session_track_external_connection(tirtc_conn_t conn, bool auto_media);
esp_err_t tirtc_session_send_command_raw(tirtc_conn_t conn,
                                         uint32_t cmdw,
                                         const void *data,
                                         size_t data_len);
esp_err_t tirtc_session_send_active_command(uint32_t cmdw,
                                            const void *data,
                                            size_t data_len);
esp_err_t tirtc_session_send_rgb(uint8_t red, uint8_t green, uint8_t blue);
esp_err_t tirtc_session_query_peer_state(void);
bool tirtc_session_get_last_peer_state(tirtc_session_peer_state_t *state);
esp_err_t tirtc_session_subscribe_audio(tirtc_conn_t conn, uint8_t stream_id);
esp_err_t tirtc_session_unsubscribe_audio(tirtc_conn_t conn, uint8_t stream_id);
void tirtc_session_flush_remote_media(void);
void tirtc_session_suppress_remote_media(tirtc_conn_t conn, bool suppress);
esp_err_t tirtc_session_send_stream_message(const void *data, size_t data_len);
esp_err_t tirtc_session_send_audio_frame(tirtc_conn_t conn, const TIRTCFRAMEINFO *frame_info, const void *data);
esp_err_t tirtc_session_get_send_buffer_used(tirtc_conn_t conn, size_t *used);
esp_err_t tirtc_session_send_captured_audio_frame(tirtc_conn_t conn,
                                                  const uint8_t *data,
                                                  size_t data_len,
                                                  const tirtc_session_audio_format_t *format,
                                                  uint64_t pts_us);
esp_err_t tirtc_session_send_test_video_frame(const TIRTCFRAMEINFO *frame_info, const uint8_t *data);
esp_err_t tirtc_session_send_test_audio_pcm_frame(const uint8_t *data,
                                                  size_t data_len,
                                                  const tirtc_session_audio_format_t *format,
                                                  uint64_t pts_us);
esp_err_t tirtc_session_set_external_audio_call_active(tirtc_conn_t conn, bool active);
esp_err_t tirtc_session_use_builtin_media(void);
esp_err_t tirtc_session_set_session_mode(tirtc_session_mode_t session_mode);
tirtc_session_mode_t tirtc_session_get_session_mode(void);
tirtc_session_state_t tirtc_session_get_state(void);
void tirtc_session_get_config(tirtc_session_config_t *config);
bool tirtc_session_get_started_auth_debug(char *device_id,
                                          size_t device_id_size,
                                          char *credential_hash,
                                          size_t credential_hash_size,
                                          uint32_t *secret_len);
void tirtc_session_get_stats(tirtc_session_stats_t *stats);
void tirtc_session_refresh_media_policy(void);
