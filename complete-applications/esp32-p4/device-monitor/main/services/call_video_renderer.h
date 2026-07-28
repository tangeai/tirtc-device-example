#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define CALL_VIDEO_DECODE_MAX_WIDTH   480U
#define CALL_VIDEO_DECODE_MAX_HEIGHT  320U
#define CALL_VIDEO_RENDER_SOURCE_X    0U
#define CALL_VIDEO_RENDER_SOURCE_Y    52U
#define CALL_VIDEO_RENDER_WIDTH       480U
#define CALL_VIDEO_RENDER_HEIGHT      218U

typedef struct {
    bool running;
    bool waiting_for_key_frame;
    bool frame_ready;
    uint16_t source_width;
    uint16_t source_height;
    uint32_t submitted_frames;
    uint32_t decoded_frames;
    uint32_t dropped_frames;
    uint32_t decode_failures;
    uint32_t latest_sequence;
    uint32_t queue_depth;
} call_video_renderer_stats_t;

/* Reserves the long-lived compressed, decoded, and RGB frame pools in PSRAM.
 * Decoder state and realtime task stacks are intentionally left session-owned
 * because they consume scarce internal RAM. */
esp_err_t call_video_renderer_prewarm(void);
esp_err_t call_video_renderer_start(void);
esp_err_t call_video_renderer_stop(void);
void call_video_renderer_flush(void);
esp_err_t call_video_renderer_submit_h264(const uint8_t *data,
                                          size_t data_len,
                                          bool key_frame,
                                          uint32_t pts);

/* Claims the oldest converted RGB565 viewport from the bounded presentation
 * FIFO. The returned PSRAM pointer stays pinned until the next successful
 * claim or release, so the converter never overwrites a frame in flight. */
esp_err_t call_video_renderer_present_next_rgb565(const uint16_t **pixels,
                                                   size_t *pixel_count,
                                                   uint32_t *sequence);
void call_video_renderer_release_presented_rgb565(void);
void call_video_renderer_get_stats(call_video_renderer_stats_t *stats);
