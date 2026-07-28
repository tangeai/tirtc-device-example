#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "audio_types.h"

typedef struct {
    bool initialized;
    uint32_t audio_queue_len;
    uint32_t audio_queue_capacity;
    uint32_t audio_buffered_ms;
    uint32_t audio_jitter_boost_ms;
    size_t audio_pcm_used_bytes;
    size_t audio_pcm_capacity;
} media_sink_stats_t;

esp_err_t media_sink_init(void);
esp_err_t media_sink_submit_remote_audio(const uint8_t *data,
                                                  size_t data_len,
                                                  const audio_format_t *format);
esp_err_t media_sink_submit_remote_audio_owned(uint8_t *data,
                                                        size_t data_len,
                                                        const audio_format_t *format);
esp_err_t media_sink_submit_remote_video_jpeg(const uint8_t *data, size_t data_len);
void media_sink_flush(void);
void media_sink_get_stats(media_sink_stats_t *stats);
