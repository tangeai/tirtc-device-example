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

typedef enum {
    AUDIO_ECHO_SUPPRESSION_BALANCED = 0,
    AUDIO_ECHO_SUPPRESSION_STRONG,
} audio_echo_suppression_t;

typedef struct {
    bool ready;
    bool capture_enabled;
    bool speaker_enabled;
    uint32_t capture_frames;
    uint32_t capture_interval_us;
    uint32_t capture_interval_max_us;
    uint32_t capture_read_us;
    uint32_t capture_process_us;
    uint32_t capture_dispatch_us;
    uint32_t capture_pipeline_wait_us;
    uint32_t capture_pipeline_wait_max_us;
    uint32_t capture_pipeline_high_water;
    uint32_t capture_pipeline_overruns;
    uint32_t capture_read_errors;
    bool capture_window_valid;
    uint32_t capture_window_updated_ms;
    int32_t capture_raw_ch0_dbfs_x10;
    int32_t capture_raw_ch1_dbfs_x10;
    int32_t capture_pre_peak_dbfs_x10;
    int32_t capture_pre_rms_dbfs_x10;
    int32_t capture_post_peak_dbfs_x10;
    int32_t capture_post_rms_dbfs_x10;
    uint16_t capture_base_gain_q8;
    uint16_t capture_auto_gain_q8;
    uint16_t capture_auto_gain_target_peak;
    uint32_t echo_reference_frames;
    uint32_t echo_active_frames;
    uint32_t echo_bypass_frames;
    uint32_t echo_warmup_frames;
    uint32_t echo_warmup_passthrough_frames;
    uint32_t echo_near_end_detected_frames;
    uint32_t echo_near_end_frames;
    uint32_t echo_near_decisions;
    uint32_t echo_near_reject_low_peak;
    uint32_t echo_near_reject_low_retained;
    uint32_t echo_near_reject_low_nlp;
    uint32_t echo_near_reject_high_coherence;
    uint8_t echo_near_retained_energy_percent;
    uint8_t echo_near_nlp_reduction_percent;
    uint8_t echo_near_reference_coherence_percent;
    uint32_t echo_far_end_guard_frames;
    uint32_t echo_ref_peak;
    uint32_t echo_mic_peak;
    uint32_t echo_linear_peak;
    uint32_t echo_out_peak;
    uint8_t echo_suppress_percent;
    uint32_t speaker_write_frames;
    uint32_t speaker_prepare_errors;
    uint32_t speaker_write_errors;
    uint32_t speaker_prepare_last_us;
    uint32_t speaker_prepare_max_us;
    uint32_t speaker_write_last_us;
    uint32_t speaker_write_max_us;
    uint32_t input_level;
    uint32_t output_level;
    uint8_t speaker_volume_percent;
    uint8_t capture_gain_percent;
    uint8_t capture_codec_gain_percent;
    uint8_t capture_upload_gain_percent;
    uint16_t capture_auto_gain_max_percent;
    uint16_t capture_effective_auto_gain_max_percent;
    bool echo_continuous_processing;
    bool echo_near_end_protection_enabled;
    bool far_end_gain_guard_enabled;
    uint8_t far_end_upload_gain_percent;
    uint16_t far_end_auto_gain_max_percent;
    audio_echo_suppression_t echo_suppression;
    bool capture_high_pass_filter_enabled;
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
    bool echo_continuous_processing;
    bool echo_near_end_protection_enabled;
    bool far_end_gain_guard_enabled;
    uint8_t far_end_upload_gain_percent;
    uint16_t far_end_auto_gain_max_percent;
    audio_echo_suppression_t echo_suppression;
    bool echo_diagnostics_enabled;
    bool high_pass_filter_enabled;
    bool noise_gate_enabled;
    uint16_t noise_gate_open_peak;
    uint16_t noise_gate_close_peak;
    uint8_t noise_gate_attenuation_percent;
} audio_capture_processing_config_t;
