#ifndef TIRTC_SAMPLE_AVI_H
#define TIRTC_SAMPLE_AVI_H

#include <stdbool.h>
#include <stdint.h>

/* Format contract produced by tools/transcode_sample_media.sh. */
#define TIRTC_SAMPLE_AVI_VIDEO_WIDTH 1280U
#define TIRTC_SAMPLE_AVI_VIDEO_HEIGHT 720U
#define TIRTC_SAMPLE_AVI_VIDEO_FPS 15U
#define TIRTC_SAMPLE_AVI_AUDIO_BYTES_PER_MS 8U

typedef struct {
    const uint8_t *data;
    uint32_t length;
    uint32_t movi_start;
    uint32_t movi_end;
    uint32_t video_frames;
    uint32_t audio_bytes;
    uint32_t duration_ms;
} tirtc_sample_avi_t;

typedef struct {
    uint32_t offset;
    uint32_t loop;
    uint32_t video_index;
    uint32_t audio_bytes;
    uint32_t audio_chunk_offset;
    uint32_t audio_chunk_remaining;
} tirtc_sample_avi_cursor_t;

bool tirtc_sample_avi_open(tirtc_sample_avi_t *avi, const uint8_t *data,
                           uint32_t length, uint32_t max_packet_bytes);
void tirtc_sample_avi_cursor_reset(const tirtc_sample_avi_t *avi,
                                   tirtc_sample_avi_cursor_t *cursor);
bool tirtc_sample_avi_next_video(const tirtc_sample_avi_t *avi,
                                 tirtc_sample_avi_cursor_t *cursor,
                                 const uint8_t **data, uint32_t *length,
                                 uint32_t *timestamp_ms);
bool tirtc_sample_avi_next_audio(const tirtc_sample_avi_t *avi,
                                 tirtc_sample_avi_cursor_t *cursor,
                                 uint8_t *data, uint32_t capacity,
                                 uint32_t *length, uint32_t *timestamp_ms);
uint32_t tirtc_sample_avi_video_position_ms(
    const tirtc_sample_avi_t *avi,
    const tirtc_sample_avi_cursor_t *cursor);
uint32_t tirtc_sample_avi_audio_position_ms(
    const tirtc_sample_avi_t *avi,
    const tirtc_sample_avi_cursor_t *cursor);

#endif
