#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    MEDIA_GOVERNOR_PROFILE_IDLE = 0,
    MEDIA_GOVERNOR_PROFILE_QR_SCAN,
    MEDIA_GOVERNOR_PROFILE_LOCAL_PREVIEW,
    MEDIA_GOVERNOR_PROFILE_RTC_AUDIO,
    MEDIA_GOVERNOR_PROFILE_RTC_AV_SAFE,
    MEDIA_GOVERNOR_PROFILE_RTC_AV_PREVIEW,
    MEDIA_GOVERNOR_PROFILE_AI_CHAT,
} media_governor_profile_t;

typedef enum {
    MEDIA_GOVERNOR_WEAK_NETWORK_OFF = 0,
    MEDIA_GOVERNOR_WEAK_NETWORK_FRAMERATE_PRIORITY,
    MEDIA_GOVERNOR_WEAK_NETWORK_RESOLUTION_PRIORITY,
} media_governor_weak_network_mode_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t fps;
    uint32_t bitrate_bps;
    media_governor_weak_network_mode_t weak_network_mode;
    uint8_t weak_network_level;
    uint8_t h264_min_qp;
    uint8_t h264_max_qp;
} media_governor_video_config_t;

typedef struct {
    uint16_t capture_width;
    uint16_t capture_height;
    uint8_t capture_fps;
    uint8_t rtc_video_fps;
    uint16_t rtc_width;
    uint16_t rtc_height;
    uint32_t h264_bitrate_bps;
    uint8_t h264_min_qp;
    uint8_t h264_max_qp;
    size_t h264_output_buffer_bytes;
    size_t h264_max_delta_payload_bytes;
    size_t dma_free_min_bytes;
    size_t dma_largest_min_bytes;
} media_governor_camera_policy_t;

typedef struct {
    bool defer_audio_for_local_video;
    bool prepare_playback_while_video_first;
} media_governor_rtc_policy_t;

esp_err_t media_governor_init(void);
void media_governor_set_profile(media_governor_profile_t profile);
media_governor_profile_t media_governor_get_profile(void);
esp_err_t media_governor_set_rtc_video_config(const media_governor_video_config_t *config);
void media_governor_build_device_call_video_config(media_governor_video_config_t *config);
void media_governor_build_camera_policy(const media_governor_video_config_t *config,
                                        media_governor_camera_policy_t *policy);
esp_err_t media_governor_apply_weak_network_level(media_governor_weak_network_mode_t mode, uint8_t level);
/* Auto adaptation is for future weak-network callbacks and is disabled by default. */
bool media_governor_auto_adaptation_enabled(void);
esp_err_t media_governor_apply_auto_weak_network_level(media_governor_weak_network_mode_t mode, uint8_t level);
void media_governor_get_rtc_video_config(media_governor_video_config_t *config);
void media_governor_get_camera_policy(media_governor_camera_policy_t *policy);
void media_governor_get_rtc_av_camera_policy(media_governor_camera_policy_t *policy);
void media_governor_get_rtc_policy(media_governor_rtc_policy_t *policy);
void media_governor_note_network_backpressure(void);
bool media_governor_is_network_backpressured(void);
const char *media_governor_profile_name(media_governor_profile_t profile);
