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
    uint8_t capture_codec_gain_percent;
    uint8_t capture_upload_gain_percent;
    uint16_t capture_auto_gain_max_percent;
    bool capture_noise_gate_enabled;
} audio_stats_t;

typedef struct {
    uint32_t prepare_ms;
    uint32_t write_ms;
    uint32_t data_bytes;
} audio_playback_timing_t;

typedef struct {
    uint8_t send_volume_percent;
    uint8_t codec_gain_percent;
    uint8_t upload_gain_percent;
    uint16_t auto_gain_max_percent;
    bool noise_gate_enabled;
    uint16_t noise_gate_open_peak;
    uint16_t noise_gate_close_peak;
    uint8_t noise_gate_attenuation_percent;
} audio_capture_processing_config_t;
