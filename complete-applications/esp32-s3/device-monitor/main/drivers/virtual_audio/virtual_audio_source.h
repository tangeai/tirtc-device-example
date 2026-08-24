#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tirtc_session.h"

#define VIRTUAL_AUDIO_OK              0
#define VIRTUAL_AUDIO_ERR_INVALID_ARG -1
#define VIRTUAL_AUDIO_ERR_IO          -2
#define VIRTUAL_AUDIO_ERR_NOT_TEST    -3
#define VIRTUAL_AUDIO_ERR_CHECKSUM    -4

typedef struct {
    uint32_t sequence;
    uint32_t payload_crc32;
} virtual_audio_packet_info_t;

typedef struct {
    uint8_t *packet_buffer;
    size_t packet_length;
    size_t packet_capacity;
    tirtc_session_audio_format_t format;
    uint32_t phase;
    uint32_t sequence;
    uint32_t last_sequence;
    bool last_sequence_valid;
} virtual_audio_source_t;

int virtual_audio_source_open(virtual_audio_source_t *source);
void virtual_audio_source_reset(virtual_audio_source_t *source);
void virtual_audio_source_close(virtual_audio_source_t *source);
int virtual_audio_source_next_packet(virtual_audio_source_t *source,
                                     const uint8_t **data_ptr,
                                     size_t *data_len,
                                     const tirtc_session_audio_format_t **format,
                                     uint32_t *duration_us);
int virtual_audio_source_parse_packet(const uint8_t *data,
                                      size_t data_len,
                                      virtual_audio_packet_info_t *info);
