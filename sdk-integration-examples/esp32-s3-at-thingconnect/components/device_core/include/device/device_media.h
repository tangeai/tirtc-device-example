#ifndef DEVICE_MEDIA_H
#define DEVICE_MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_MEDIA_ASSET_PATH_MAX 128
#define DEVICE_VIDEO_OBJECT_FIT_MAX 8
#define DEVICE_MEDIA_REFERENCE_DURATION_MS 10000U

typedef enum {
    DEVICE_AUDIO_CODEC_G711A = 0,
    DEVICE_AUDIO_CODEC_AMR_NB,
    DEVICE_AUDIO_CODEC_AMR_WB,
    DEVICE_AUDIO_CODEC_OPUS,
} device_audio_codec_t;

typedef enum {
    DEVICE_VIDEO_CODEC_MJPEG = 0,
    DEVICE_VIDEO_CODEC_H264,
    DEVICE_VIDEO_CODEC_H265_RESERVED,
} device_video_codec_t;

typedef struct {
    char asset_path[DEVICE_MEDIA_ASSET_PATH_MAX];
    device_audio_codec_t codec;
    uint32_t sample_rate_hz;
    uint8_t channels;
    uint16_t packet_ms;
    uint32_t duration_ms;
    uint32_t packet_count;
} device_audio_config_t;

typedef struct {
    char asset_path[DEVICE_MEDIA_ASSET_PATH_MAX];
    device_video_codec_t codec;
    uint16_t width;
    uint16_t height;
    uint16_t camera_rotation;
    double aspect_ratio;
    char object_fit[DEVICE_VIDEO_OBJECT_FIT_MAX];
    bool hor_mirror;
    bool vert_mirror;
    uint16_t fps;
    uint32_t duration_ms;
    uint32_t frame_count;
    bool uplink_enabled;
    bool downlink_enabled;
} device_video_config_t;

/* One device has one recorded-media profile shared by AI and device calls. */
typedef struct {
    device_audio_config_t audio;
    device_video_config_t video;
} device_media_config_t;

void device_media_config_set_defaults(device_media_config_t *config);

/* Returns true when the configuration is supported by the current reference profile. */
bool device_media_config_validate(const device_media_config_t *config,
                                  char *error,
                                  size_t error_size);

const char *device_audio_codec_name(device_audio_codec_t codec);
const char *device_video_codec_name(device_video_codec_t codec);

#ifdef __cplusplus
}
#endif

#endif
