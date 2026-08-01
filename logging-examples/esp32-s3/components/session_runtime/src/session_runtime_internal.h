#ifndef SESSION_RUNTIME_INTERNAL_H
#define SESSION_RUNTIME_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "session_runtime.h"
#include "session_contact_resolver.h"
#include "session_handoff_guard.h"
#include "tirtc_adapter.h"

#define SESSION_COMMAND_CALL_CONFIRM 0x2000U
#define SESSION_COMMAND_CALL_HANGUP 0x2001U
#define SESSION_COMMAND_AI 0x2100U

#define SESSION_INTERNAL_ARGUMENT_MAX 513
#define SESSION_INTERNAL_PAYLOAD_MAX 4097
#define SESSION_DEVICE_CONTACT_CACHE_MAX 32

typedef enum {
    SESSION_AI_PHASE_NONE = 0,
    SESSION_AI_PHASE_TOKEN,
    SESSION_AI_PHASE_WHIP,
    SESSION_AI_PHASE_DELAY,
    SESSION_AI_PHASE_START_RESPONSE,
    SESSION_AI_PHASE_ACTIVE,
    SESSION_AI_PHASE_ENDING,
} session_ai_phase_t;

typedef enum {
    SESSION_CALL_PHASE_NONE = 0,
    SESSION_CALL_PHASE_REQUEST,
    SESSION_CALL_PHASE_OUTGOING_WAIT,
    SESSION_CALL_PHASE_RINGING,
    SESSION_CALL_PHASE_ACCEPT_TOKEN,
    SESSION_CALL_PHASE_CONNECTING,
    SESSION_CALL_PHASE_WAIT_CONFIRM,
    SESSION_CALL_PHASE_ACTIVE,
    SESSION_CALL_PHASE_RECOVER,
    SESSION_CALL_PHASE_ENDING,
} session_call_phase_t;

typedef enum {
    SESSION_HTTP_NONE = 0,
    SESSION_HTTP_AI_TOKEN,
    SESSION_HTTP_CALL_REQUEST,
    SESSION_HTTP_CALL_ACCEPT,
    SESSION_HTTP_CALL_BUSY_REJECT,
    SESSION_HTTP_CALL_REJECT,
    SESSION_HTTP_CALL_CANCEL,
    SESSION_HTTP_CALL_HANGUP,
    SESSION_HTTP_CALL_RECOVER,
    SESSION_HTTP_CONTACTS_LIST,
    SESSION_HTTP_AI_CALL_CONTACTS,
    SESSION_HTTP_CONTACTS_PENDING,
    SESSION_HTTP_CONTACTS_REQUEST,
    SESSION_HTTP_CONTACTS_RESPOND,
    SESSION_HTTP_CONTACTS_REMARK,
    SESSION_HTTP_CONTACTS_DELETE,
} session_http_request_t;

typedef enum {
    SESSION_INT_ADAPTER_CONNECTION = 0,
    SESSION_INT_ADAPTER_COMMAND,
    SESSION_INT_PLATFORM_SIGNAL,
    SESSION_INT_HTTP_RESPONSE,
    SESSION_INT_MEDIA_ERROR,
    SESSION_INT_MEDIA_PROMPT_DONE,
    SESSION_INT_ABORT,
    SESSION_INT_AI_START,
    SESSION_INT_AI_INTERRUPT,
    SESSION_INT_AI_SUBMIT,
    SESSION_INT_AI_UPDATE,
    SESSION_INT_AI_PROMPT,
    SESSION_INT_AI_ACTION_RESULT,
    SESSION_INT_AI_STOP,
    SESSION_INT_CALL_START,
    SESSION_INT_CALL_ACCEPT,
    SESSION_INT_CALL_REJECT,
    SESSION_INT_CALL_CANCEL,
    SESSION_INT_CALL_HANGUP,
    SESSION_INT_CALL_RECOVER,
    SESSION_INT_CONTACTS_LIST,
    SESSION_INT_CONTACTS_PENDING,
    SESSION_INT_CONTACTS_REQUEST,
    SESSION_INT_CONTACTS_RESPOND,
    SESSION_INT_CONTACTS_REMARK,
    SESSION_INT_CONTACTS_DELETE,
} session_internal_event_type_t;

typedef struct {
    session_internal_event_type_t type;
    session_http_request_t http_request;
    uint32_t generation;
    uint32_t request_cookie;
    uint32_t connection_generation;
    uint32_t origin_request_id;
    uint32_t command;
    uint32_t length;
    int status;
    bool flag;
    bool truncated;
    session_runtime_call_type_t call_type;
    tirtc_adapter_connection_event_t connection;
    char first[SESSION_INTERNAL_ARGUMENT_MAX];
    char second[SESSION_INTERNAL_ARGUMENT_MAX];
    char payload[SESSION_INTERNAL_PAYLOAD_MAX];
} session_internal_event_t;

typedef struct {
    bool active;
    bool final;
    bool seq_valid;
    uint32_t last_seq;
    char utterance_id[SESSION_RUNTIME_REQUEST_ID_MAX];
    char text[SESSION_RUNTIME_TEXT_MAX];
} session_caption_group_t;

typedef struct {
    session_ai_phase_t phase;
    char peer_id[SESSION_RUNTIME_AI_PEER_MAX];
    char role_id[65];
    char start_request_id[SESSION_RUNTIME_REQUEST_ID_MAX];
    char session_id[SESSION_RUNTIME_ID_MAX];
    char action_id[SESSION_RUNTIME_REQUEST_ID_MAX];
    char action_id_json[SESSION_RUNTIME_JSON_MAX];
    char update_request_id[SESSION_RUNTIME_REQUEST_ID_MAX];
    uint32_t update_origin_request_id;
    uint32_t prompt_origin_request_id;
    int64_t action_deadline_ms;
    int64_t update_deadline_ms;
    int64_t prompt_deadline_ms;
    bool prompt_pending;
    bool prompt_draining;
    char prompt_name[24];
    session_caption_group_t captions[2];
} session_ai_context_t;

typedef struct {
    session_call_phase_t phase;
    char room_id[SESSION_RUNTIME_ID_MAX];
    char peer_id[SESSION_RUNTIME_ID_MAX];
    char peer_name[SESSION_RUNTIME_ID_MAX];
    char target_id[SESSION_RUNTIME_ID_MAX];
    char connect_token[SESSION_RUNTIME_TOKEN_MAX];
    char confirm_room_id[SESSION_RUNTIME_ID_MAX];
    session_runtime_call_type_t call_type;
    bool caller;
    bool p2p_connected;
    bool room_confirmed;
} session_call_context_t;

typedef struct {
    session_runtime_handoff_phase_t phase;
    uint32_t ai_generation;
    uint32_t contacts_request_cookie;
    uint8_t retry_count;
    uint8_t adapter_retry_count;
    uint8_t call_start_retry_count;
    uint8_t busy_response_retry_count;
    bool response_ok;
    int failure_status;
    int64_t deadline_ms;
    int64_t terminal_deadline_ms;
    int64_t busy_response_retry_at_ms;
    int64_t busy_response_deadline_ms;
    char action_id_json[SESSION_RUNTIME_JSON_MAX];
    char busy_action_id_json[SESSION_RUNTIME_JSON_MAX];
    char completed_busy_action_id_json[SESSION_RUNTIME_JSON_MAX];
    char failure_phase[65];
    char failure_message[SESSION_RUNTIME_TEXT_MAX];
    char target[SESSION_CONTACT_LABEL_MAX];
    char target_device_id[SESSION_RUNTIME_ID_MAX];
} session_ai_call_handoff_t;

typedef struct {
    device_service_t owner;
    device_session_state_t state;
    uint32_t generation;
    uint32_t revision;
    uint32_t last_connection_generation;
    uint32_t event_origin_request_id;
    uint32_t last_origin_request_id;
    uint32_t session_origin_request_id;
    uint32_t rpc_sequence;
    uint32_t http_sequence;
    int64_t deadline_ms;
    session_ai_context_t ai;
    session_call_context_t call;
    session_ai_call_handoff_t ai_call_handoff;
    session_device_contact_t
        device_contacts[SESSION_DEVICE_CONTACT_CACHE_MAX];
    char pending_device_contacts[SESSION_DEVICE_CONTACT_CACHE_MAX]
                                 [SESSION_RUNTIME_ID_MAX];
    uint8_t device_contact_count;
    uint8_t pending_device_contact_count;
    bool device_contact_cache_complete;
} session_context_t;

int64_t session_now_ms(void);
void session_secure_zero(void *memory, size_t size);
bool session_copy_string(char *destination,
                         size_t destination_size,
                         const char *source);
bool session_copy_json(char *destination,
                       size_t destination_size,
                       const char *source);
bool session_json_string(const cJSON *object,
                         const char *name,
                         char *destination,
                         size_t destination_size);
int session_json_response_code(const cJSON *root);
const cJSON *session_json_response_data(const cJSON *root);
void session_make_rpc_id(session_context_t *context,
                         const char *prefix,
                         char *destination,
                         size_t destination_size);

void session_emit(session_context_t *context,
                  const session_runtime_event_t *event);
void session_refresh_snapshot(session_context_t *context);
void session_emit_operation(session_context_t *context,
                            const char *name,
                            int status,
                            const char *text,
                            const char *json);
void session_emit_diagnostic(session_context_t *context,
                             int status,
                             const char *text);
void session_set_state(session_context_t *context,
                       device_session_state_t state,
                       const char *reason,
                       int status);
bool session_claim(session_context_t *context,
                   device_service_t owner,
                   device_session_state_t initial_state,
                   tirtc_adapter_media_profile_t media_profile,
                   bool uplink_video_enabled,
                   const char *reason);
void session_finish(session_context_t *context,
                    const char *reason,
                    int status);

esp_err_t session_submit_http(session_context_t *context,
                              session_http_request_t request,
                              uint32_t generation,
                              const char *method,
                              const char *path,
                              const char *json_body);
esp_err_t session_submit_http_correlated(session_context_t *context,
                                         session_http_request_t request,
                                         uint32_t generation,
                                         uint32_t request_cookie,
                                         const char *method,
                                         const char *path,
                                         const char *json_body);

bool session_ai_handles(const session_internal_event_t *event,
                        const session_context_t *context);
void session_ai_handle(session_context_t *context,
                       const session_internal_event_t *event);
void session_ai_tick(session_context_t *context, int64_t current_ms);
int session_ai_complete_action(session_context_t *context,
                               bool ok,
                               const char *payload);
int session_ai_send_action_response(session_context_t *context,
                                    const char *id_json,
                                    bool ok,
                                    const char *payload);
void session_ai_end_for_call_handoff(session_context_t *context);

bool session_ai_call_begin(session_context_t *context,
                           const cJSON *params);
bool session_ai_call_reserved(const session_context_t *context);
bool session_ai_call_response_frozen(const session_context_t *context);
bool session_ai_call_owns_action(const session_context_t *context);
void session_ai_call_reject_concurrent_action(
    session_context_t *context,
    const char *action_id_json);
void session_ai_call_contacts_response(
    session_context_t *context,
    const session_internal_event_t *event,
    bool cache_valid);
bool session_ai_call_prepare_ai_finish(session_context_t *context);
void session_ai_call_tick(session_context_t *context, int64_t current_ms);
void session_ai_call_cancel(session_context_t *context);

bool session_call_handles(const session_internal_event_t *event,
                          const session_context_t *context);
void session_call_handle(session_context_t *context,
                         const session_internal_event_t *event);
bool session_call_start_handoff(session_context_t *context,
                                const char *target_device_id);
void session_call_tick(session_context_t *context, int64_t current_ms);

#endif
