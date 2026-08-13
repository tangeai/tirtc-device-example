#ifndef PLATFORM_CLIENT_H
#define PLATFORM_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  PLATFORM_SERVICE_DEVICE = 0,
  PLATFORM_SERVICE_AI,
  PLATFORM_SERVICE_CALL,
} platform_service_t;

#define PLATFORM_HTTP_METHOD_GET "GET"
#define PLATFORM_HTTP_METHOD_POST "POST"
#define PLATFORM_HTTP_METHOD_PUT "PUT"
#define PLATFORM_HTTP_METHOD_DELETE "DELETE"

typedef struct {
  const char *device_id;
  const char *device_secret;
  const char *client_id;
  const char *mac_address;
  const char *discovery_url;
} platform_client_config_t;

typedef struct {
  char device_id[65];
  char device_secret[257];
} platform_provision_result_t;

typedef esp_err_t (*platform_credentials_persist_callback_t)(
    const platform_provision_result_t *credentials, void *user_data);

typedef struct {
  int64_t expires_at_unix;
  char mac_address[18];
  char code[17];
  char temp_token[1024];
  char temp_client_id[65];
} platform_pending_provision_t;

typedef esp_err_t (*platform_pending_provision_load_callback_t)(
    platform_pending_provision_t *pending, void *user_data);
typedef esp_err_t (*platform_pending_provision_save_callback_t)(
    const platform_pending_provision_t *pending, void *user_data);
typedef esp_err_t (*platform_pending_provision_clear_callback_t)(
    void *user_data);

typedef struct {
  const char *mac_address;
  const char *existing_device_id;
  const char *existing_device_secret;
  const char *discovery_url;
  unsigned timeout_seconds;
  platform_credentials_persist_callback_t persist_credentials;
  void *persist_user_data;
  platform_pending_provision_load_callback_t load_pending;
  platform_pending_provision_save_callback_t save_pending;
  platform_pending_provision_clear_callback_t clear_pending;
  void *pending_user_data;
} platform_provision_config_t;

typedef void (*platform_response_callback_t)(const char *body, void *user_data);
/* Return ESP_OK only after the complete message has been copied into a
 * reliable business queue. Command messages are application-ACKed only after
 * this callback succeeds. */
typedef esp_err_t (*platform_signal_callback_t)(const char *json, size_t length,
                                                void *user_data);

typedef enum {
  PLATFORM_CLIENT_EVENT_DISCOVERY_READY = 0,
  PLATFORM_CLIENT_EVENT_AUTH_READY,
  PLATFORM_CLIENT_EVENT_MQTT_CONNECTED,
  PLATFORM_CLIENT_EVENT_MQTT_DISCONNECTED,
  PLATFORM_CLIENT_EVENT_PROVISION_CODE,
  PLATFORM_CLIENT_EVENT_PROVISION_PROGRESS,
  PLATFORM_CLIENT_EVENT_REBIND_REQUIRED,
  PLATFORM_CLIENT_EVENT_ERROR,
} platform_client_event_type_t;

typedef enum {
  PLATFORM_PROVISION_PROGRESS_MQTT_CONNECTED = 1,
  PLATFORM_PROVISION_PROGRESS_SUBSCRIBED,
  PLATFORM_PROVISION_PROGRESS_MESSAGE_RECEIVED,
  PLATFORM_PROVISION_PROGRESS_GRANT_VALIDATED,
  PLATFORM_PROVISION_PROGRESS_CREDENTIALS_PERSISTED,
  PLATFORM_PROVISION_PROGRESS_ACK_CONFIRMED,
} platform_provision_progress_t;

typedef enum {
  PLATFORM_CLIENT_EVENT_SOURCE_DISCOVERY = 0,
  PLATFORM_CLIENT_EVENT_SOURCE_AUTH,
  PLATFORM_CLIENT_EVENT_SOURCE_MQTT,
  PLATFORM_CLIENT_EVENT_SOURCE_PROVISION,
  PLATFORM_CLIENT_EVENT_SOURCE_REQUEST,
  PLATFORM_CLIENT_EVENT_SOURCE_INTERNAL,
} platform_client_event_source_t;

typedef struct {
  platform_client_event_type_t type;
  platform_client_event_source_t source;
  esp_err_t error;
  int status_code;
  unsigned reason_code;
  char provision_code[17];
} platform_client_event_t;

/* Observer callbacks may originate from the caller, platform worker or MQTT
 * event task. They must not block and must copy any data they retain. */
typedef void (*platform_client_observer_t)(const platform_client_event_t *event,
                                           void *user_data);

/* Starts service discovery, signed device login, the request worker and MQTT.
 * This function performs network I/O and must run outside app_main. */
esp_err_t platform_client_start(const platform_client_config_t *config);

/* Stops MQTT and the internal request/token worker, then clears volatile
 * identity/token state. Persistent credentials owned by the caller are not
 * modified. The client may be started again afterwards. */
esp_err_t platform_client_stop(void);

/* First-boot / rebind flow. The binding HTTP and temporary MQTT endpoints are
 * obtained from one service-discovery response. auth_grant is ACKed at QoS 1
 * before the server's short delivery deadline; credentials are persisted only
 * after the ACK PUBACK arrives. Existing credentials are optional and enable
 * a signed report after server-side unbind. */
esp_err_t platform_client_provision(const platform_provision_config_t *config,
                                    platform_provision_result_t *result);
bool platform_client_ready(void);
bool platform_client_mqtt_connected(void);
bool platform_client_provisioning(void);
const char *platform_client_verification_code(void);
/* Active authenticated identity. The pointer remains owned by this component
 * and is cleared by platform_client_stop(). */
const char *platform_client_device_id(void);
const char *platform_client_tirtc_endpoint(void);

/* The callback runs in the platform request task; response text is only valid
 * during the callback. method must be GET, POST, PUT or DELETE. */
esp_err_t platform_client_request_ex(platform_service_t service,
                                     const char *method, const char *path,
                                     const char *body,
                                     platform_response_callback_t callback,
                                     void *user_data);

/* Compatibility wrapper: GET when json_body is NULL, POST otherwise. */
esp_err_t platform_client_request(platform_service_t service, const char *path,
                                  const char *json_body,
                                  platform_response_callback_t callback,
                                  void *user_data);

void platform_client_set_signal_handler(platform_signal_callback_t callback,
                                        void *user_data);
void platform_client_set_observer(platform_client_observer_t observer,
                                  void *user_data);

#ifdef __cplusplus
}
#endif

#endif
