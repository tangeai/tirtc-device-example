#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "audio_types.h"

typedef enum {
    MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY = 0,
    MEDIA_SINK_AUDIO_PROFILE_DEVICE_CALL,
    MEDIA_SINK_AUDIO_PROFILE_JITTER_SAFE,
    MEDIA_SINK_AUDIO_PROFILE_IPC_TALKBACK,
} media_sink_audio_profile_t;

typedef struct {
    uint32_t buffered_ms;
    uint32_t queued_packets;
    bool playback_active;
    bool source_quiet;
} media_sink_audio_drain_status_t;

typedef struct {
    media_sink_audio_profile_t profile;
    uint32_t source_packet_ms;
    uint32_t prebuffer_ms;
    uint32_t target_ms;
    uint32_t buffered_ms;
    uint32_t queued_packets;
    uint32_t jitter_ewma_ms;
    uint32_t jitter_peak_ms;
    uint32_t jitter_boost_ms;
    uint32_t max_arrival_gap_ms;
    uint32_t first_play_delay_ms;
    uint32_t rx_packets;
    uint32_t rx_ms;
    uint32_t played_packets;
    uint32_t played_ms;
    uint32_t play_drop_packets;
    uint32_t queue_drop_packets;
    uint32_t trim_drop_packets;
    uint32_t underflow_events;
    uint32_t active_underflow_events;
    uint32_t underflow_grace_waits;
    uint32_t underflow_grace_recoveries;
    uint32_t delayed_burst_events;
    uint32_t source_late_events;
    uint32_t source_late_ms;
    uint32_t max_source_late_ms;
    int32_t source_clock_error_ms;
    int32_t source_gap_pending_ms;
    uint32_t source_gap_pending_packets;
    uint32_t source_gap_fill_events;
    uint32_t source_gap_fill_ms;
    uint32_t concealment_events;
    uint32_t concealed_ms;
    uint32_t clock_recovery_events;
    uint32_t clock_recovery_frames;
    bool playback_pacing_enabled;
    uint32_t pacing_wait_events;
    uint32_t pacing_wait_ms;
    uint32_t pacing_wait_max_ms;
    uint32_t pacing_late_events;
    uint32_t pacing_late_ms;
    uint32_t pacing_late_max_ms;
    uint32_t integrity_rx_frames;
    uint32_t integrity_buffered_frames;
    uint32_t integrity_played_frames;
    uint32_t integrity_checksum_failures;
    uint32_t integrity_missing_frames;
    uint32_t integrity_duplicate_frames;
    uint32_t integrity_reordered_frames;
    uint32_t integrity_first_sequence;
    uint32_t integrity_last_sequence;
    bool integrity_sequence_valid;
    uint32_t integrity_queue_dropped_frames;
    uint32_t integrity_trimmed_frames;
    uint32_t integrity_play_failures;
    uint32_t integrity_play_missing_frames;
    uint32_t integrity_play_duplicate_frames;
    uint32_t integrity_play_reordered_frames;
    uint32_t integrity_first_play_sequence;
    uint32_t integrity_last_play_sequence;
    bool integrity_play_sequence_valid;
    bool playback_active;
    bool talkspurt_active;
} media_sink_audio_diagnostics_t;

esp_err_t media_sink_init(void);
void media_sink_set_audio_profile(media_sink_audio_profile_t profile);
void media_sink_set_remote_audio_talkspurt(bool active);
bool media_sink_remote_audio_is_drained(media_sink_audio_drain_status_t *status);
bool media_sink_get_audio_diagnostics(media_sink_audio_diagnostics_t *diagnostics);
void media_sink_reset_audio_integrity_stats(void);
esp_err_t media_sink_submit_remote_audio(const uint8_t *data,
                                         size_t data_len,
                                         const audio_format_t *format,
                                         uint32_t source_timestamp_ms);
esp_err_t media_sink_submit_remote_audio_owned(uint8_t *data,
                                               size_t data_len,
                                               const audio_format_t *format,
                                               uint32_t source_timestamp_ms);
void media_sink_flush(void);
