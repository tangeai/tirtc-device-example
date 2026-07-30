#ifndef DEVICE_SESSION_H
#define DEVICE_SESSION_H

#include <stdbool.h>

#include "device/device_media.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    DEVICE_SERVICE_NONE = 0,
    DEVICE_SERVICE_AI,
    DEVICE_SERVICE_CALL,
} device_service_t;

typedef enum {
    DEVICE_SESSION_OFFLINE = 0,
    DEVICE_SESSION_IDLE,
    DEVICE_SESSION_AI_CONNECTING,
    DEVICE_SESSION_AI_STARTING,
    DEVICE_SESSION_AI_ACTIVE,
    DEVICE_SESSION_RINGING,
    DEVICE_SESSION_CALLING,
    DEVICE_SESSION_CALL_CONNECTING,
    DEVICE_SESSION_IN_CALL,
    DEVICE_SESSION_RECOVERING,
    DEVICE_SESSION_ENDING,
} device_session_state_t;

typedef enum {
    DEVICE_MEDIA_UPLINK = 0,
    DEVICE_MEDIA_DOWNLINK,
} device_media_direction_t;

/* Combines the device-wide video switches with protocol-specific direction limits. */
bool device_session_video_enabled(const device_media_config_t *media,
                                  device_service_t service,
                                  device_media_direction_t direction);

const char *device_service_name(device_service_t service);
const char *device_session_state_name(device_session_state_t state);

#ifdef __cplusplus
}
#endif

#endif
