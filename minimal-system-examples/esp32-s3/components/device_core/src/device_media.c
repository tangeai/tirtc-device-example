#include "device/device_media.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static bool fail(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size > 0) {
        (void)snprintf(error, error_size, "%s", message);
    }
    return false;
}

void device_media_config_set_defaults(device_media_config_t *config)
{
    if (config == NULL) {
        return;
    }

    config->audio.codec = DEVICE_AUDIO_CODEC_G711A;
    (void)snprintf(config->audio.asset_path,
                   sizeof(config->audio.asset_path),
                   "%s",
                   "audio_g711a_8khz_mono_20ms_10s_500packets.g711a");
    config->audio.sample_rate_hz = 8000;
    config->audio.channels = 1;
    config->audio.packet_ms = 20;
    config->audio.duration_ms = DEVICE_MEDIA_REFERENCE_DURATION_MS;
    config->audio.packet_count = 500;

    config->video.codec = DEVICE_VIDEO_CODEC_MJPEG;
    (void)snprintf(config->video.asset_path,
                   sizeof(config->video.asset_path),
                   "%s",
                   "video_mjpeg_640x480_8fps_10s_80frames.mjpeg");
    config->video.width = 640;
    config->video.height = 480;
    config->video.camera_rotation = 0;
    config->video.aspect_ratio = 4.0 / 3.0;
    config->video.object_fit[0] = '\0';
    config->video.hor_mirror = false;
    config->video.vert_mirror = false;
    config->video.fps = 8;
    config->video.duration_ms = DEVICE_MEDIA_REFERENCE_DURATION_MS;
    config->video.frame_count = 80;
    config->video.uplink_enabled = true;
    config->video.downlink_enabled = true;
}

bool device_media_config_validate(const device_media_config_t *config,
                                  char *error,
                                  size_t error_size)
{
    if (config == NULL) {
        return fail(error, error_size, "media config is null");
    }
    if (error != NULL && error_size > 0) {
        error[0] = '\0';
    }

    if (config->audio.asset_path[0] == '\0') {
        return fail(error, error_size, "audio asset path is empty");
    }
    if (config->audio.channels != 1) {
        return fail(error, error_size, "the reference profile currently requires mono audio");
    }
    if (config->audio.packet_ms != 20) {
        return fail(error, error_size, "the reference file source currently requires 20 ms audio packets");
    }
    if (config->audio.duration_ms == 0 ||
        config->audio.duration_ms > DEVICE_MEDIA_REFERENCE_DURATION_MS) {
        return fail(error, error_size, "audio duration must be between 1 ms and 10 seconds");
    }
    if (config->audio.packet_count !=
        config->audio.duration_ms / config->audio.packet_ms) {
        return fail(error, error_size, "audio packet count does not match duration and packet interval");
    }

    switch (config->audio.codec) {
    case DEVICE_AUDIO_CODEC_G711A:
    case DEVICE_AUDIO_CODEC_OPUS:
        if (config->audio.sample_rate_hz != 8000 &&
            config->audio.sample_rate_hz != 16000) {
            return fail(error, error_size, "G711A/Opus sample rate must be 8000 or 16000 Hz");
        }
        break;
    case DEVICE_AUDIO_CODEC_AMR_NB:
        if (config->audio.sample_rate_hz != 8000) {
            return fail(error, error_size, "AMR-NB sample rate must be 8000 Hz");
        }
        break;
    case DEVICE_AUDIO_CODEC_AMR_WB:
        if (config->audio.sample_rate_hz != 16000) {
            return fail(error, error_size, "AMR-WB sample rate must be 16000 Hz");
        }
        break;
    default:
        return fail(error, error_size, "unknown audio codec");
    }

    if (config->video.uplink_enabled) {
        if (config->video.asset_path[0] == '\0') {
            return fail(error, error_size, "uplink video asset path is empty");
        }
        if (!((config->video.width == 640 && config->video.height == 480) ||
              (config->video.width == 640 && config->video.height == 360) ||
              (config->video.width == 1280 && config->video.height == 720))) {
            return fail(error,
                        error_size,
                        "video resolution must be 640x480, 640x360, or 1280x720");
        }
        if (config->video.duration_ms == 0 ||
            config->video.duration_ms > DEVICE_MEDIA_REFERENCE_DURATION_MS) {
            return fail(error, error_size, "video duration must be between 1 ms and 10 seconds");
        }
    }
    if (config->video.camera_rotation != 0 &&
        config->video.camera_rotation != 90 &&
        config->video.camera_rotation != 180 &&
        config->video.camera_rotation != 270) {
        return fail(error, error_size, "camera rotation must be 0, 90, 180, or 270");
    }
    if (!isfinite(config->video.aspect_ratio) || config->video.aspect_ratio <= 0) {
        return fail(error, error_size, "video aspect ratio must be greater than 0");
    }
    if (config->video.object_fit[0] != '\0' &&
        strcmp(config->video.object_fit, "fill") != 0 &&
        strcmp(config->video.object_fit, "contain") != 0) {
        return fail(error, error_size, "video object fit must be fill or contain");
    }

    switch (config->video.codec) {
    case DEVICE_VIDEO_CODEC_MJPEG:
        if ((config->video.uplink_enabled || config->video.downlink_enabled) &&
            config->video.fps != 8) {
            return fail(error, error_size, "MJPEG reference profile requires 8 fps");
        }
        if (config->video.uplink_enabled &&
            (config->video.duration_ms * config->video.fps) % 1000U != 0U) {
            return fail(error, error_size, "MJPEG duration must contain a whole number of frames");
        }
        if (config->video.uplink_enabled &&
            config->video.frame_count !=
                config->video.duration_ms * config->video.fps / 1000U) {
            return fail(error, error_size, "MJPEG frame count does not match duration and fps");
        }
        break;
    case DEVICE_VIDEO_CODEC_H264:
        if ((config->video.uplink_enabled || config->video.downlink_enabled) &&
            config->video.fps != 15) {
            return fail(error, error_size, "H264 reference profile requires 15 fps");
        }
        if (config->video.uplink_enabled &&
            (config->video.duration_ms * config->video.fps) % 1000U != 0U) {
            return fail(error, error_size, "H264 duration must contain a whole number of frames");
        }
        if (config->video.uplink_enabled &&
            config->video.frame_count !=
                config->video.duration_ms * config->video.fps / 1000U) {
            return fail(error, error_size, "H264 frame count does not match duration and fps");
        }
        break;
    case DEVICE_VIDEO_CODEC_H265_RESERVED:
        return fail(error, error_size, "H265 is reserved but not implemented");
    default:
        return fail(error, error_size, "unknown video codec");
    }

    return true;
}

const char *device_audio_codec_name(device_audio_codec_t codec)
{
    switch (codec) {
    case DEVICE_AUDIO_CODEC_G711A: return "g711a";
    case DEVICE_AUDIO_CODEC_AMR_NB: return "amr-nb";
    case DEVICE_AUDIO_CODEC_AMR_WB: return "amr-wb";
    case DEVICE_AUDIO_CODEC_OPUS: return "opus";
    default: return "unknown";
    }
}

const char *device_video_codec_name(device_video_codec_t codec)
{
    switch (codec) {
    case DEVICE_VIDEO_CODEC_MJPEG: return "mjpeg";
    case DEVICE_VIDEO_CODEC_H264: return "h264";
    case DEVICE_VIDEO_CODEC_H265_RESERVED: return "h265-reserved";
    default: return "unknown";
    }
}
