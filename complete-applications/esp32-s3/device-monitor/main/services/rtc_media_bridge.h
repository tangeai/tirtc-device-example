#pragma once

#include "tirtc_session.h"

typedef struct {
    uint32_t valid_frames;
    uint32_t checksum_failures;
    uint32_t missing_frames;
    uint32_t duplicate_frames;
    uint32_t reordered_frames;
    uint32_t first_sequence;
    uint32_t last_sequence;
    bool sequence_valid;
} rtc_media_bridge_audio_integrity_stats_t;

const tirtc_session_media_ops_t *rtc_media_bridge_get_ops(void);
void *rtc_media_bridge_get_context(void);
void rtc_media_bridge_reset_audio_integrity_stats(void);
void rtc_media_bridge_get_audio_integrity_stats(
    rtc_media_bridge_audio_integrity_stats_t *stats);
