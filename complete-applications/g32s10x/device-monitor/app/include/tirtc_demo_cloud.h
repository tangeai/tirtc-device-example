#ifndef _TIRTC_DEMO_CLOUD_H
#define _TIRTC_DEMO_CLOUD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TIRTC_DEMO_CLOUD_ENABLE
#define TIRTC_DEMO_CLOUD_ENABLE 1
#endif

#ifndef TIRTC_DEMO_CLOUD_API_BASE
#define TIRTC_DEMO_CLOUD_API_BASE "http://mqtt-demo.tange-ai.com"
#endif

#ifndef TIRTC_DEMO_CLOUD_API_HOST
#define TIRTC_DEMO_CLOUD_API_HOST "mqtt-demo.tange-ai.com"
#endif

#ifndef TIRTC_DEMO_CLOUD_API_PORT
#define TIRTC_DEMO_CLOUD_API_PORT 80
#endif

#ifndef TIRTC_DEMO_CLOUD_MQTT_HOST
#define TIRTC_DEMO_CLOUD_MQTT_HOST "mqtt-demo.tange-ai.com"
#endif

#ifndef TIRTC_DEMO_CLOUD_MQTT_PORT
#define TIRTC_DEMO_CLOUD_MQTT_PORT 8883
#endif

#define TIRTC_DEMO_CLOUD_DEVICE_ID_MAX 128
#define TIRTC_DEMO_CLOUD_DEVICE_SECRET_MAX 192
#define TIRTC_DEMO_CLOUD_TOKEN_MAX 1024
#define TIRTC_DEMO_CLOUD_PEER_ID_MAX 2048
#define TIRTC_DEMO_CLOUD_CONNECT_TOKEN_MAX 1536
#define TIRTC_DEMO_CLOUD_ROOM_ID_MAX 96
#define TIRTC_DEMO_CLOUD_CONTACT_MAX 5
#define TIRTC_DEMO_CLOUD_CONTACT_NAME_MAX 96
#define TIRTC_DEMO_CLOUD_BINDING_CODE_MAX 32
#define TIRTC_DEMO_CLOUD_WX_OPENID_MAX 128
#define TIRTC_DEMO_CLOUD_WX_APP_ID_MAX 96
#define TIRTC_DEMO_CLOUD_WX_MODEL_ID_MAX 96
#define TIRTC_DEMO_CLOUD_WX_SESSION_TOKEN_MAX 1024
#define TIRTC_DEMO_CLOUD_WX_PAYLOAD_MAX 256
#define TIRTC_DEMO_CLOUD_AI_ROLE_ID_MAX 128

#define TIRTC_DEMO_CLOUD_ERR_PENDING (-2000)
#define TIRTC_DEMO_CLOUD_ERR_NOT_READY (-2001)
#define TIRTC_DEMO_CLOUD_ERR_BUSY (-2002)
#define TIRTC_DEMO_CLOUD_ERR_UNSUPPORTED (-2004)

typedef enum {
    TIRTC_DEMO_CLOUD_CALL_IDLE = 0,
    TIRTC_DEMO_CLOUD_CALL_OUTGOING,
    TIRTC_DEMO_CLOUD_CALL_INCOMING,
    TIRTC_DEMO_CLOUD_CALL_CONNECTING,
    TIRTC_DEMO_CLOUD_CALL_ACTIVE,
} tirtc_demo_cloud_call_state_t;

typedef enum {
    TIRTC_DEMO_CLOUD_SESSION_NONE = 0,
    TIRTC_DEMO_CLOUD_SESSION_DEVICE,
    TIRTC_DEMO_CLOUD_SESSION_WECHAT,
    TIRTC_DEMO_CLOUD_SESSION_AI,
} tirtc_demo_cloud_session_type_t;

typedef struct {
    char device_id[TIRTC_DEMO_CLOUD_DEVICE_ID_MAX];
    char remark[TIRTC_DEMO_CLOUD_CONTACT_NAME_MAX];
    bool online;
} tirtc_demo_cloud_contact_t;

typedef struct {
    char name[TIRTC_DEMO_CLOUD_CONTACT_NAME_MAX];
    char open_id[TIRTC_DEMO_CLOUD_WX_OPENID_MAX];
    char app_id[TIRTC_DEMO_CLOUD_WX_APP_ID_MAX];
    char model_id[TIRTC_DEMO_CLOUD_WX_MODEL_ID_MAX];
} tirtc_demo_cloud_wechat_contact_t;

typedef struct {
    tirtc_demo_cloud_session_type_t type;
    bool incoming;
    char peer_id[TIRTC_DEMO_CLOUD_PEER_ID_MAX];
    char token[TIRTC_DEMO_CLOUD_CONNECT_TOKEN_MAX];
    char room_id[TIRTC_DEMO_CLOUD_ROOM_ID_MAX];
    char role_id[TIRTC_DEMO_CLOUD_AI_ROLE_ID_MAX];
    char wx_app_id[TIRTC_DEMO_CLOUD_WX_APP_ID_MAX];
    char wx_model_id[TIRTC_DEMO_CLOUD_WX_MODEL_ID_MAX];
    char wx_open_id[TIRTC_DEMO_CLOUD_WX_OPENID_MAX];
    char wx_session_token[TIRTC_DEMO_CLOUD_WX_SESSION_TOKEN_MAX];
    char wx_payload[TIRTC_DEMO_CLOUD_WX_PAYLOAD_MAX];
} tirtc_demo_cloud_session_t;

typedef struct {
    uint32_t status_generation;
    int last_error;
    int http_status;
    int business_code;
    bool identity_ready;
    bool time_ready;
    bool token_ready;
    bool mqtt_connected;
    bool binding_waiting;
    bool contacts_ready;
    bool incoming_call;
    bool call_active;
    bool wechat_ready;
    bool wechat_contacts_ready;
    bool wechat_incoming;
    bool ai_token_ready;
    tirtc_demo_cloud_session_type_t session_type;
    tirtc_demo_cloud_call_state_t call_state;
    char stage[32];
    char message[160];
    char binding_code[TIRTC_DEMO_CLOUD_BINDING_CODE_MAX];
    char peer_id[TIRTC_DEMO_CLOUD_DEVICE_ID_MAX];
    char room_id[TIRTC_DEMO_CLOUD_ROOM_ID_MAX];
    size_t contact_count;
    tirtc_demo_cloud_contact_t contacts[TIRTC_DEMO_CLOUD_CONTACT_MAX];
    size_t wechat_contact_count;
    tirtc_demo_cloud_wechat_contact_t
        wechat_contacts[TIRTC_DEMO_CLOUD_CONTACT_MAX];
} tirtc_demo_cloud_snapshot_t;

typedef struct {
    bool mqtt_connected;
    bool binding_waiting;
    tirtc_demo_cloud_session_type_t session_type;
    tirtc_demo_cloud_call_state_t call_state;
} tirtc_demo_cloud_runtime_state_t;

typedef int (*tirtc_demo_cloud_save_identity_fn)(const char *device_id,
                                                  const char *device_secret,
                                                  void *context);
typedef void (*tirtc_demo_cloud_identity_activated_fn)(void *context);
typedef void (*tirtc_demo_cloud_session_ready_fn)(
    const tirtc_demo_cloud_session_t *session, void *context);

typedef struct {
    tirtc_demo_cloud_save_identity_fn save_identity;
    tirtc_demo_cloud_identity_activated_fn identity_activated;
    tirtc_demo_cloud_session_ready_fn session_ready;
    void *context;
} tirtc_demo_cloud_callbacks_t;

int tirtc_demo_cloud_init(const tirtc_demo_cloud_callbacks_t *callbacks);
int tirtc_demo_cloud_start(void);
int tirtc_demo_cloud_get_physical_client_id(char *output, size_t output_size);
int tirtc_demo_cloud_apply_identity(const char *device_id,
                                    const char *device_secret);
int tirtc_demo_cloud_request_binding(void);
int tirtc_demo_cloud_refresh_contacts(void);
int tirtc_demo_cloud_request_contact(const char *target_device_id);
int tirtc_demo_cloud_call(const char *target_device_id);
int tirtc_demo_cloud_accept(void);
int tirtc_demo_cloud_reject(void);
int tirtc_demo_cloud_hangup(void);
int tirtc_demo_cloud_refresh_wechat_profile(void);
int tirtc_demo_cloud_refresh_wechat_contacts(void);
int tirtc_demo_cloud_call_wechat(const char *open_id,
                                 const char *app_id,
                                 const char *model_id);
int tirtc_demo_cloud_add_wechat_contact(const char *open_id);
int tirtc_demo_cloud_delete_wechat_contact(const char *open_id);
int tirtc_demo_cloud_request_ai_session(void);
void tirtc_demo_cloud_notify_rtc_connected(void);
void tirtc_demo_cloud_notify_rtc_disconnected(void);
void tirtc_demo_cloud_notify_room_confirmed(const char *room_id);
void tirtc_demo_cloud_notify_session_connecting(
    tirtc_demo_cloud_session_type_t type);
void tirtc_demo_cloud_notify_session_active(tirtc_demo_cloud_session_type_t type);
void tirtc_demo_cloud_notify_session_ended(tirtc_demo_cloud_session_type_t type,
                                            const char *reason);
void tirtc_demo_cloud_get_snapshot(tirtc_demo_cloud_snapshot_t *snapshot);
void tirtc_demo_cloud_get_runtime_state(
    tirtc_demo_cloud_runtime_state_t *state);

#ifdef __cplusplus
}
#endif

#endif
