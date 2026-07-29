#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t sample_rate_hz;
    uint8_t channels;
    uint8_t bits_per_sample;
} audio_format_t;

typedef void (*audio_capture_frame_cb_t)(const uint8_t *data,
                                         size_t data_len,
                                         const audio_format_t *format,
                                         void *ctx);

typedef struct {
    bool ready;
    bool capture_enabled;
    bool speaker_enabled;
    uint32_t capture_frames;
    uint32_t input_level;
    uint32_t output_level;
    uint8_t speaker_volume_percent;
    uint8_t capture_gain_percent;
    bool aec_active;
    bool aec_reference_active;
    uint32_t aec_process_frames;
    uint64_t aec_process_us_total;
    uint32_t aec_process_us_max;
} audio_stats_t;

typedef struct {
    uint32_t prepare_ms;
    uint32_t write_ms;
    uint32_t data_bytes;
} audio_playback_timing_t;
