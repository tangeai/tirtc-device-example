#ifndef SESSION_RUNTIME_H
#define SESSION_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device/device_session.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SESSION_RUNTIME_ID_MAX 129
#define SESSION_RUNTIME_AI_PEER_MAX 2049
#define SESSION_RUNTIME_TOKEN_MAX 1537
#define SESSION_RUNTIME_TEXT_MAX 513
#define SESSION_RUNTIME_JSON_MAX 1025
#define SESSION_RUNTIME_REQUEST_ID_MAX 129
#define SESSION_RUNTIME_PEER_MESSAGE_MAX 129

typedef enum {
    SESSION_RUNTIME_EVENT_STATE = 0,
    SESSION_RUNTIME_EVENT_OPERATION,
    SESSION_RUNTIME_EVENT_AI_CAPTION,
    SESSION_RUNTIME_EVENT_AI_EVENT,
    SESSION_RUNTIME_EVENT_AI_ACTION,
    SESSION_RUNTIME_EVENT_CALL_INCOMING,
    SESSION_RUNTIME_EVENT_CALL_EVENT,
    SESSION_RUNTIME_EVENT_VIEW_EVENT,
    SESSION_RUNTIME_EVENT_CONTACT,
    SESSION_RUNTIME_EVENT_CONTACTS_DONE,
    SESSION_RUNTIME_EVENT_DIAGNOSTIC,
} session_runtime_event_type_t;

typedef enum {
    SESSION_RUNTIME_CALL_AUDIO = 0,
    SESSION_RUNTIME_CALL_VIDEO,
} session_runtime_call_type_t;

typedef enum {
    SESSION_RUNTIME_HANDOFF_NONE = 0,
    SESSION_RUNTIME_HANDOFF_RESOLVING,
    SESSION_RUNTIME_HANDOFF_ACTION_RESPONSE,
    SESSION_RUNTIME_HANDOFF_RESPONSE_DRAIN,
    SESSION_RUNTIME_HANDOFF_ADAPTER_DRAIN,
} session_runtime_handoff_phase_t;

typedef struct {
    device_service_t owner;
    device_session_state_t state;
    uint32_t generation;
    uint32_t revision;
    uint32_t origin_request_id;
    int64_t deadline_ms;
    bool pending_incoming_call;
    bool caller;
    bool ai_update_pending;
    bool ai_action_pending;
    bool ai_call_handoff_pending;
    session_runtime_call_type_t call_type;
    session_runtime_handoff_phase_t ai_call_handoff_phase;
    int64_t ai_update_deadline_ms;
    int64_t ai_action_deadline_ms;
    int64_t ai_call_handoff_deadline_ms;
    char room_id[SESSION_RUNTIME_ID_MAX];
    char peer_id[SESSION_RUNTIME_ID_MAX];
    char ai_session_id[SESSION_RUNTIME_ID_MAX];
    char ai_action_id[SESSION_RUNTIME_REQUEST_ID_MAX];
} session_runtime_snapshot_t;

typedef struct {
    session_runtime_event_type_t type;
    device_service_t owner;
    device_session_state_t state;
    uint32_t generation;
    uint32_t origin_request_id;
    int status;
    uint32_t index;
    uint32_t count;
    uint32_t seq_num;
    int caption_type;
    int caption_mode;
    bool final;
    bool online;
    char name[65];
    char request_id[SESSION_RUNTIME_REQUEST_ID_MAX];
    char room_id[SESSION_RUNTIME_ID_MAX];
    char peer_id[SESSION_RUNTIME_ID_MAX];
    char text[SESSION_RUNTIME_TEXT_MAX];
    char json[SESSION_RUNTIME_JSON_MAX];
} session_runtime_event_t;

/* Observer runs in session_task. It must copy data and return without blocking. */
typedef void (*session_runtime_observer_t)(const session_runtime_event_t *event,
                                           void *user_data);

esp_err_t session_runtime_start(void);
void session_runtime_set_observer(session_runtime_observer_t observer,
                                  void *user_data);
esp_err_t session_runtime_get_snapshot(session_runtime_snapshot_t *snapshot);
device_session_state_t session_runtime_state(void);
device_service_t session_runtime_service(void);
const char *session_runtime_handoff_phase_name(
    session_runtime_handoff_phase_t phase);

/* Network/identity teardown intent. Safe to call from another task. */
esp_err_t session_runtime_abort(const char *reason, uint32_t origin_request_id);

esp_err_t session_runtime_ai_start(uint32_t origin_request_id);
esp_err_t session_runtime_ai_interrupt(uint32_t origin_request_id);
esp_err_t session_runtime_ai_submit_speech(uint32_t origin_request_id);
esp_err_t session_runtime_ai_update_config(const char *extra_params_json,
                                           uint32_t origin_request_id);
esp_err_t session_runtime_ai_prompt(const char *preset,
                                    uint32_t origin_request_id);
esp_err_t session_runtime_ai_action_result(const char *action_id,
                                           bool ok,
                                           const char *result_json_or_message,
                                           uint32_t origin_request_id);
esp_err_t session_runtime_ai_stop(uint32_t origin_request_id);

/* The session layer obtains all call tokens from the platform. */
esp_err_t session_runtime_call_start(const char *target_device_id,
                                     session_runtime_call_type_t call_type,
                                     uint32_t origin_request_id);
esp_err_t session_runtime_call_accept(uint32_t origin_request_id);
esp_err_t session_runtime_call_reject(uint32_t origin_request_id);
esp_err_t session_runtime_call_cancel(uint32_t origin_request_id);
esp_err_t session_runtime_call_hangup(uint32_t origin_request_id);
/* Sends one UTF-8 line to the peer of the active device call. */
esp_err_t session_runtime_call_send_message(const char *message,
                                            uint32_t origin_request_id);
esp_err_t session_runtime_call_recover(uint32_t origin_request_id);

esp_err_t session_runtime_contacts_list(uint32_t origin_request_id);
esp_err_t session_runtime_contacts_pending(uint32_t origin_request_id);
esp_err_t session_runtime_contacts_request(const char *target_device_id,
                                           uint32_t origin_request_id);
esp_err_t session_runtime_contacts_respond(const char *peer_device_id,
                                           bool accept,
                                           uint32_t origin_request_id);
esp_err_t session_runtime_contacts_remark(const char *peer_device_id,
                                          const char *remark,
                                          uint32_t origin_request_id);
esp_err_t session_runtime_contacts_delete(const char *peer_device_id,
                                          uint32_t origin_request_id);

#ifdef __cplusplus
}
#endif

#endif
