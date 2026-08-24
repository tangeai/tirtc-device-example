#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "audio_types.h"

typedef enum {
    AUDIO_ECHO_NEAR_REJECT_NONE = 0,
    AUDIO_ECHO_NEAR_REJECT_LOW_LINEAR_PEAK,
    AUDIO_ECHO_NEAR_REJECT_LOW_RETAINED_ENERGY,
    AUDIO_ECHO_NEAR_REJECT_LOW_NLP_REDUCTION,
    AUDIO_ECHO_NEAR_REJECT_HIGH_REFERENCE_COHERENCE,
} audio_echo_near_reject_reason_t;

#define AUDIO_ECHO_CANCEL_DEBUG_CHANNEL_COUNT 6U

typedef struct {
    bool allocated;
    bool recording;
    bool complete;
    uint32_t sample_rate_hz;
    uint8_t channel_count;
    size_t captured_frames;
    size_t capacity_frames;
    size_t data_bytes;
} audio_echo_cancel_debug_capture_status_t;

typedef struct {
    bool active;
    bool reference_active;
    bool output_bypassed;
    bool warming_up;
    bool warmup_near_end_passthrough;
    bool near_end_detected;
    bool near_end_protected;
    uint32_t ref_peak;
    uint32_t mic_peak;
    uint32_t linear_peak;
    uint32_t out_peak;
    uint8_t suppress_percent;
    uint16_t delay_samples;
    uint32_t near_decision_sequence;
    uint8_t near_retained_energy_percent;
    uint8_t near_nlp_reduction_percent;
    uint8_t near_reference_coherence_percent;
    audio_echo_near_reject_reason_t near_reject_reason;
} audio_echo_cancel_metrics_t;

bool audio_echo_cancel_preload(void);
void audio_echo_cancel_set_suppression(audio_echo_suppression_t suppression);
void audio_echo_cancel_feed_playback(const int16_t *samples,
                                     size_t sample_count,
                                     uint8_t channels);
void audio_echo_cancel_process_capture(int16_t *samples,
                                       size_t sample_count,
                                       audio_echo_cancel_metrics_t *metrics);
void audio_echo_cancel_process_capture_with_reference(int16_t *samples,
                                                      const int16_t *reference,
                                                      size_t sample_count,
                                                      bool continuous_processing,
                                                      bool near_end_protection_enabled,
                                                      audio_echo_cancel_metrics_t *metrics);
esp_err_t audio_echo_cancel_debug_capture_start(uint32_t duration_ms);
void audio_echo_cancel_debug_capture_get_status(
    audio_echo_cancel_debug_capture_status_t *status);
esp_err_t audio_echo_cancel_debug_capture_read(size_t offset,
                                               void *buffer,
                                               size_t buffer_size,
                                               size_t *bytes_read);
esp_err_t audio_echo_cancel_debug_capture_free(void);
esp_err_t audio_echo_cancel_debug_injection_set(uint32_t frequency_hz,
                                                uint16_t amplitude);
void audio_echo_cancel_reset(void);
void audio_echo_cancel_deinit(void);
