#ifndef TIRTC_LINK_PLATFORM_H
#define TIRTC_LINK_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    TIRTC_LINK_E_WIFI_NOT_CONFIGURED = -41001,
    TIRTC_LINK_E_WIFI_DISCONNECTED = -41002,
    TIRTC_LINK_E_NTP_NETWORK = -41003,
    TIRTC_LINK_E_NTP_DNS = -41004,
    TIRTC_LINK_E_NTP_SOCKET = -41005,
    TIRTC_LINK_E_NTP_SOCKET_OPTION = -41006,
    TIRTC_LINK_E_NTP_SEND = -41007,
    TIRTC_LINK_E_NTP_RECEIVE = -41008,
    TIRTC_LINK_E_NTP_RESPONSE = -41009,
    TIRTC_LINK_E_NTP_TIMESTAMP = -41010,
    TIRTC_LINK_E_CLIENT_ID = -41011,
    TIRTC_LINK_E_IDENTITY_MISSING = -41012,
} tirtc_link_platform_error_t;

/* Initialize the G32 WiFi adapter. Must be idempotent. */
int tirtc_link_platform_init(void);

/* Drive non-blocking retry work. Called by the TiRTC control worker. */
void tirtc_link_platform_poll(void);

/* Network and wall-clock prerequisites consumed by the control state machine. */
bool tirtc_link_platform_wifi_configured(void);
bool tirtc_link_platform_network_ready(void);
bool tirtc_link_platform_time_ready(void);

/* Perform one bounded NTP attempt and return a TIRTC_LINK_E_NTP_* code. */
int tirtc_link_platform_sync_time(void);

/* Build the stable 12-hex-character client ID from the WiFi MAC address. */
int tirtc_link_platform_get_client_id(char *client_id, size_t capacity);

#endif
