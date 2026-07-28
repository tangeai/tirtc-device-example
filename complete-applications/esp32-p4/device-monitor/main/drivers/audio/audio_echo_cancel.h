#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool active;
    uint32_t ref_peak;
    uint32_t mic_peak;
    uint32_t out_peak;
    uint8_t suppress_percent;
    uint16_t delay_samples;
    uint32_t process_us;
} audio_echo_cancel_metrics_t;

esp_err_t audio_echo_cancel_prepare(void);
esp_err_t audio_echo_cancel_set_active(bool active);
void audio_echo_cancel_feed_playback(const int16_t *samples,
                                     size_t sample_count,
                                     uint8_t channels);
void audio_echo_cancel_process_capture(int16_t *samples,
                                       size_t sample_count,
                                       audio_echo_cancel_metrics_t *metrics);
void audio_echo_cancel_reset(void);
void audio_echo_cancel_deinit(void);
