#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct video_frame_converter *video_frame_converter_handle_t;

typedef enum {
    VIDEO_FRAME_CONVERTER_MODE_SOFTWARE = 0,
    VIDEO_FRAME_CONVERTER_MODE_PPA,
} video_frame_converter_mode_t;

typedef struct {
    uint16_t output_width;
    uint16_t output_height;
    /* Optional even-aligned source crop. A zero width/height selects the
     * complete source frame. Exact-size crops stay on the PPA fast path. */
    uint16_t source_crop_x;
    uint16_t source_crop_y;
    uint16_t source_crop_width;
    uint16_t source_crop_height;
    bool output_rgb565_byte_swap;
} video_frame_converter_config_t;

typedef struct {
    uint32_t ppa_frames;
    uint32_t software_frames;
    uint32_t ppa_failures;
    uint64_t pack_time_us;
    uint32_t pack_max_us;
    uint64_t ppa_time_us;
    uint32_t ppa_max_us;
    uint64_t swap_time_us;
    uint32_t swap_max_us;
    uint64_t software_time_us;
    uint32_t software_max_us;
} video_frame_converter_stats_t;

esp_err_t video_frame_converter_create(const video_frame_converter_config_t *config,
                                       video_frame_converter_handle_t *out_handle);
void video_frame_converter_destroy(video_frame_converter_handle_t handle);

esp_err_t video_frame_converter_i420_to_rgb565(video_frame_converter_handle_t handle,
                                               const uint8_t *i420,
                                               uint16_t source_width,
                                               uint16_t source_height,
                                               uint16_t *output,
                                               video_frame_converter_mode_t *mode_used);

video_frame_converter_mode_t video_frame_converter_get_mode(video_frame_converter_handle_t handle);
void video_frame_converter_get_stats(video_frame_converter_handle_t handle,
                                     video_frame_converter_stats_t *stats);
const char *video_frame_converter_mode_name(video_frame_converter_mode_t mode);
