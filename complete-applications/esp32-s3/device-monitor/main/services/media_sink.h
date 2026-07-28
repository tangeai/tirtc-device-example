#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "audio_types.h"

typedef enum {
    MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY = 0,
    MEDIA_SINK_AUDIO_PROFILE_JITTER_SAFE,
} media_sink_audio_profile_t;

esp_err_t media_sink_init(void);
void media_sink_set_audio_profile(media_sink_audio_profile_t profile);
esp_err_t media_sink_submit_remote_audio(const uint8_t *data,
                                                  size_t data_len,
                                                  const audio_format_t *format);
esp_err_t media_sink_submit_remote_audio_owned(uint8_t *data,
                                                        size_t data_len,
                                                        const audio_format_t *format);
esp_err_t media_sink_submit_remote_video_jpeg(const uint8_t *data, size_t data_len);
void media_sink_flush(void);
