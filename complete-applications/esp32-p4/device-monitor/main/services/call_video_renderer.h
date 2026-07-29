#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define CALL_VIDEO_DECODE_MAX_WIDTH   480U
#define CALL_VIDEO_DECODE_MAX_HEIGHT  320U
#define CALL_VIDEO_RENDER_WIDTH       480U
#define CALL_VIDEO_RENDER_HEIGHT      320U

typedef enum {
    CALL_VIDEO_CODEC_H264 = 0,
    CALL_VIDEO_CODEC_MJPEG,
} call_video_codec_t;

typedef struct {
    bool running;
    bool waiting_for_key_frame;
    bool frame_ready;
    call_video_codec_t codec;
    uint16_t source_width;
    uint16_t source_height;
    uint32_t received_frames;
    uint64_t received_bytes;
    uint32_t submitted_frames;
    uint32_t decoded_frames;
    uint32_t converted_frames;
    uint32_t presented_frames;
    uint32_t dropped_frames;
    uint32_t conversion_dropped_frames;
    uint32_t decode_failures;
    uint32_t conversion_failures;
    uint64_t conversion_time_us;
    uint32_t conversion_max_us;
    uint64_t conversion_pack_time_us;
    uint32_t conversion_pack_max_us;
    uint64_t conversion_ppa_time_us;
    uint32_t conversion_ppa_max_us;
    uint64_t conversion_swap_time_us;
    uint32_t conversion_swap_max_us;
    uint32_t discontinuities;
    uint32_t input_overflows;
    uint32_t latest_sequence;
    uint32_t queue_depth;
    uint32_t conversion_queue_depth;
} call_video_renderer_stats_t;

/* Reserves the long-lived compressed, decoded, and RGB frame pools in PSRAM. */
esp_err_t call_video_renderer_prewarm(void);
/* Reserves the P4 hardware JPEG driver's small internal DMA working set before
 * networking and RTC fragment internal RAM. The decoder is reused by calls. */
esp_err_t call_video_renderer_prewarm_mjpeg_decoder(void);
esp_err_t call_video_renderer_start_for_codec(call_video_codec_t codec);
esp_err_t call_video_renderer_start(void);
esp_err_t call_video_renderer_stop(void);
void call_video_renderer_flush(void);
bool call_video_renderer_requires_key_frame(void);
esp_err_t call_video_renderer_submit_h264(const uint8_t *data,
                                          size_t data_len,
                                          bool key_frame,
                                          uint32_t pts);
esp_err_t call_video_renderer_submit_mjpeg(const uint8_t *data,
                                           size_t data_len,
                                           uint32_t pts);

/* Claims the newest converted RGB565 viewport and releases any older queued
 * viewports. The returned PSRAM pointer stays pinned until the next successful
 * claim or release, so the converter never overwrites a frame in flight. */
esp_err_t call_video_renderer_present_next_rgb565(const uint16_t **pixels,
                                                   size_t *pixel_count,
                                                   uint32_t *sequence);
void call_video_renderer_release_presented_rgb565(void);
void call_video_renderer_get_stats(call_video_renderer_stats_t *stats);
