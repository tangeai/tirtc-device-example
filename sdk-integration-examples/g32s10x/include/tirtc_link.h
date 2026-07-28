#ifndef TIRTC_LINK_H
#define TIRTC_LINK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TIRTC_LINK_STATE_IDLE = 0,
    TIRTC_LINK_STATE_WAIT_WIFI_CONFIG,
    TIRTC_LINK_STATE_WAIT_NETWORK,
    TIRTC_LINK_STATE_SYNC_TIME,
    TIRTC_LINK_STATE_WAIT_TIRTC_CONFIG,
    TIRTC_LINK_STATE_STARTING,
    TIRTC_LINK_STATE_LISTENING,
    TIRTC_LINK_STATE_CONNECTING,
    TIRTC_LINK_STATE_CONNECTED,
    TIRTC_LINK_STATE_ERROR,
} tirtc_link_state_t;

typedef struct {
    tirtc_link_state_t state;
    int last_error;
    bool network_ready;
    bool clock_ready;
    bool sdk_ready;
    bool peer_connected;
    uint32_t accepted_connections;
    uint32_t received_commands;
} tirtc_link_status_t;

/*
 * Start the self-contained demo service.
 *
 * The function creates the control and sample-media workers, then returns.
 * WiFi join, NTP sync and TiRTC start continue asynchronously. Calling it
 * more than once is safe; only the first call creates workers.
 */
void tirtc_link_service_init(void);

/*
 * Queue one outbound TiRTC connection request.
 * remote_device_id and token are copied before this function returns.
 * A zero return means queued, not connected; observe status/callback logs for
 * the final result. Inbound-only products do not need this API.
 */
int tirtc_link_connect(const char *remote_device_id, const char *token);

/* Queue disconnection of the current peer. */
int tirtc_link_disconnect(void);

/* Copy a lock-protected service snapshot into caller-owned storage. */
void tirtc_link_get_status(tirtc_link_status_t *status);

/* Stable ASCII state name intended for telemetry and external diagnostics. */
const char *tirtc_link_state_name(tirtc_link_state_t state);

#ifdef __cplusplus
}
#endif

#endif
