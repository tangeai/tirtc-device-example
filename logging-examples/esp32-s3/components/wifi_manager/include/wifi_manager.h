#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_MANAGER_SSID_MAX 32
#define WIFI_MANAGER_PASSWORD_MAX 64
#define WIFI_MANAGER_IPV4_STRING_MAX 16

typedef struct {
    char ssid[WIFI_MANAGER_SSID_MAX + 1];
    char password[WIFI_MANAGER_PASSWORD_MAX + 1];
} wifi_manager_credentials_t;

typedef enum {
    WIFI_MANAGER_STATE_UNINITIALIZED = 0,
    WIFI_MANAGER_STATE_IDLE,
    WIFI_MANAGER_STATE_WAITING_FOR_CREDENTIALS,
    WIFI_MANAGER_STATE_CONNECTING,
    WIFI_MANAGER_STATE_CONNECTED,
    WIFI_MANAGER_STATE_DISCONNECTED,
} wifi_manager_state_t;

typedef struct {
    wifi_manager_state_t state;
    bool initialized;
    bool started;
    bool credentials_configured;
    bool connected;
    char ssid[WIFI_MANAGER_SSID_MAX + 1];
    char ip[WIFI_MANAGER_IPV4_STRING_MAX];
    char netmask[WIFI_MANAGER_IPV4_STRING_MAX];
    char gateway[WIFI_MANAGER_IPV4_STRING_MAX];
    uint16_t last_disconnect_reason;
    uint32_t reconnect_attempt;
} wifi_manager_snapshot_t;

typedef enum {
    WIFI_MANAGER_EVENT_SNAPSHOT = 0,
    WIFI_MANAGER_EVENT_INITIALIZED,
    WIFI_MANAGER_EVENT_DEINITIALIZED,
    WIFI_MANAGER_EVENT_STARTED,
    WIFI_MANAGER_EVENT_STOPPED,
    WIFI_MANAGER_EVENT_CREDENTIALS_UPDATED,
    WIFI_MANAGER_EVENT_CREDENTIALS_CLEARED,
    WIFI_MANAGER_EVENT_CONNECTING,
    WIFI_MANAGER_EVENT_CONNECTED,
    WIFI_MANAGER_EVENT_DISCONNECTED,
} wifi_manager_event_type_t;

typedef struct {
    wifi_manager_event_type_t type;
    wifi_manager_snapshot_t snapshot;
} wifi_manager_event_t;

/*
 * Observer callbacks run synchronously on the caller, Wi-Fi event-loop, or
 * reconnect timer task. Keep them short and queue business work elsewhere.
 * The event is valid only for the duration of the callback.
 */
typedef void (*wifi_manager_observer_t)(
    const wifi_manager_event_t *event,
    void *context);

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_start(void);
esp_err_t wifi_manager_stop(void);
esp_err_t wifi_manager_deinit(void);

esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password);
esp_err_t wifi_manager_get_credentials(wifi_manager_credentials_t *credentials);
esp_err_t wifi_manager_clear_credentials(void);
esp_err_t wifi_manager_get_snapshot(wifi_manager_snapshot_t *snapshot);
esp_err_t wifi_manager_set_observer(
    wifi_manager_observer_t observer,
    void *context);

bool wifi_manager_credentials_valid(const char *ssid,
                                    const char *password,
                                    char *error,
                                    size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
