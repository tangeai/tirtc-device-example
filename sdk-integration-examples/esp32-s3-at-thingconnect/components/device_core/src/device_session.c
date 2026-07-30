#include "device/device_session.h"

bool device_session_video_enabled(const device_media_config_t *media,
                                  device_service_t service,
                                  device_media_direction_t direction)
{
    if (media == NULL) {
        return false;
    }

    if (direction == DEVICE_MEDIA_UPLINK && service != DEVICE_SERVICE_NONE) {
        return media->video.uplink_enabled;
    }

    if (direction != DEVICE_MEDIA_DOWNLINK || !media->video.downlink_enabled) {
        return false;
    }

    /* This log-oriented example has no AI video decoder/display pipeline. */
    return service == DEVICE_SERVICE_CALL;
}
const char *device_service_name(device_service_t service)
{
    switch (service) {
    case DEVICE_SERVICE_NONE: return "none";
    case DEVICE_SERVICE_AI: return "ai";
    case DEVICE_SERVICE_CALL: return "device-call";
    default: return "unknown";
    }
}

const char *device_session_state_name(device_session_state_t state)
{
    switch (state) {
    case DEVICE_SESSION_OFFLINE: return "offline";
    case DEVICE_SESSION_IDLE: return "idle";
    case DEVICE_SESSION_AI_CONNECTING: return "ai-connecting";
    case DEVICE_SESSION_AI_STARTING: return "ai-starting";
    case DEVICE_SESSION_AI_ACTIVE: return "ai-active";
    case DEVICE_SESSION_RINGING: return "ringing";
    case DEVICE_SESSION_CALLING: return "calling";
    case DEVICE_SESSION_CALL_CONNECTING: return "call-connecting";
    case DEVICE_SESSION_IN_CALL: return "in-call";
    case DEVICE_SESSION_RECOVERING: return "recovering";
    case DEVICE_SESSION_ENDING: return "ending";
    default: return "unknown";
    }
}
