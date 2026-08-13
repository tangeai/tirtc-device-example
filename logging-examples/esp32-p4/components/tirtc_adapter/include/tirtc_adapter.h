#ifndef TIRTC_ADAPTER_H
#define TIRTC_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device/device_media.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TIRTC_ADAPTER_IDLE = 0,
    TIRTC_ADAPTER_STARTING,
    TIRTC_ADAPTER_RUNNING,
    TIRTC_ADAPTER_STOPPING,
    TIRTC_ADAPTER_STOPPED,
    TIRTC_ADAPTER_ERROR,
} tirtc_adapter_state_t;

typedef enum {
    TIRTC_ADAPTER_MEDIA_NONE = 0,
    TIRTC_ADAPTER_MEDIA_AI,
    TIRTC_ADAPTER_MEDIA_CALL,
    TIRTC_ADAPTER_MEDIA_VIEW,
} tirtc_adapter_media_profile_t;

typedef struct {
    const char *device_id;
    const char *device_secret;
    const char *client_id;
    const char *service_endpoint;
    uint32_t max_send_buffer_bytes;
    int max_connections;
    int log_level;
} tirtc_adapter_config_t;

typedef struct {
    bool connected;
    bool incoming;
    int error;
    uint32_t session_generation;
    uint32_t connection_generation;
} tirtc_adapter_connection_event_t;

bool tirtc_adapter_is_remote_close_error(int error);

typedef struct {
    bool connected;
    tirtc_adapter_state_t adapter_state;
    tirtc_adapter_media_profile_t active_profile;
    tirtc_adapter_media_profile_t measured_profile;
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
} tirtc_adapter_metrics_t;

typedef struct {
    uint32_t command;
    const void *data;
    uint32_t length;
    uint32_t session_generation;
    uint32_t connection_generation;
} tirtc_adapter_command_event_t;

typedef struct {
    /* Event objects and their payload are valid only for the callback call. */
    void (*on_connection_changed)(const tirtc_adapter_connection_event_t *event,
                                  void *user_data);
    void (*on_command)(const tirtc_adapter_command_event_t *event,
                       void *user_data);
    void *user_data;
} tirtc_adapter_event_handlers_t;

typedef void (*tirtc_adapter_service_callback_t)(const char *body, void *user_data);

const char *tirtc_adapter_version(void);
const char *tirtc_adapter_build_info(void);
tirtc_adapter_state_t tirtc_adapter_state(void);
int tirtc_adapter_last_start_error(void);
const char *tirtc_adapter_last_start_stage(void);
bool tirtc_adapter_restart_required(void);

/* Call after networking and credentials are ready. No credential is logged. */
int tirtc_adapter_start(const tirtc_adapter_config_t *config);

/* Stop is asynchronous. Wait for STOPPED before calling deinit. */
int tirtc_adapter_request_stop(void);
int tirtc_adapter_deinit(void);

bool tirtc_adapter_has_connection(void);
uint32_t tirtc_adapter_connection_generation(void);
int tirtc_adapter_set_media_profile(tirtc_adapter_media_profile_t profile,
                                    uint32_t session_generation,
                                    bool downlink_video_enabled);
/* Arm one incoming call connection for this session; consumed on acceptance. */
int tirtc_adapter_expect_incoming(uint32_t session_generation);
/* Reserve the idle listener for one authenticated Web viewing connection. */
int tirtc_adapter_arm_view(uint32_t session_generation);
int tirtc_adapter_cancel_view(uint32_t session_generation);
tirtc_adapter_media_profile_t tirtc_adapter_media_profile(void);
uint32_t tirtc_adapter_media_session_generation(void);
void tirtc_adapter_get_metrics(tirtc_adapter_metrics_t *metrics);
int tirtc_adapter_connect(const char *remote_id,
                          const char *token,
                          uint32_t session_generation);
/* Disconnect is accepted asynchronously and executed outside SDK callbacks. */
int tirtc_adapter_disconnect(void);
int tirtc_adapter_cancel_connect(void);
int tirtc_adapter_whip_connect(const char *service_description,
                               const char *token,
                               uint32_t session_generation);
int tirtc_adapter_send_command(uint32_t session_generation,
                               uint32_t command,
                               const void *data,
                               uint32_t length);
int tirtc_adapter_get_send_buffer_used(uint32_t session_generation,
                                       size_t *used_bytes);
/* Call only from the session task after a connection is established. */
int tirtc_adapter_subscribe_downlink(uint32_t session_generation,
                                    bool audio,
                                    bool video);
int tirtc_adapter_service_request(const char *path,
                                  const char *json_body,
                                  const char *token,
                                  tirtc_adapter_service_callback_t callback,
                                  void *user_data);
void tirtc_adapter_set_event_handlers(const tirtc_adapter_event_handlers_t *handlers);

int tirtc_adapter_send_audio(tirtc_adapter_media_profile_t profile,
                             uint32_t session_generation,
                             const device_audio_config_t *config,
                             uint32_t timestamp_ms,
                             const void *data,
                             uint32_t length);
int tirtc_adapter_send_video(tirtc_adapter_media_profile_t profile,
                             uint32_t session_generation,
                             const device_video_config_t *config,
                             uint32_t timestamp_ms,
                             bool key_frame,
                             const void *data,
                             uint32_t length);

#ifdef __cplusplus
}
#endif

#endif
