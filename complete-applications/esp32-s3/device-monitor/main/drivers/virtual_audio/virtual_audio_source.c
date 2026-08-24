#include "virtual_audio_source.h"

#include <stdlib.h>
#include <string.h>

#define VIRTUAL_AUDIO_SAMPLE_RATE_HZ 8000U
#define VIRTUAL_AUDIO_CHANNELS       1U
#define VIRTUAL_AUDIO_BITS_PER_SAMPLE 16U
#define VIRTUAL_AUDIO_FRAME_MS       20U
#define VIRTUAL_AUDIO_TONE_HZ        440U
#define VIRTUAL_AUDIO_TONE_AMPLITUDE 12000
#define VIRTUAL_AUDIO_SAMPLES_PER_FRAME \
    ((VIRTUAL_AUDIO_SAMPLE_RATE_HZ * VIRTUAL_AUDIO_FRAME_MS) / 1000U)
#define VIRTUAL_AUDIO_PACKET_BYTES \
    (VIRTUAL_AUDIO_SAMPLES_PER_FRAME * VIRTUAL_AUDIO_CHANNELS * sizeof(int16_t))
#define VIRTUAL_AUDIO_PACKET_MAGIC 0x54445541U

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t sequence;
    uint32_t payload_crc32;
    uint32_t header_crc32;
} virtual_audio_packet_header_t;

static uint32_t virtual_audio_crc32(const uint8_t *data, size_t data_len)
{
    uint32_t crc = UINT32_MAX;

    for (size_t index = 0; index < data_len; ++index) {
        crc ^= data[index];
        for (uint8_t bit = 0; bit < 8U; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

static int virtual_audio_source_reserve(virtual_audio_source_t *source)
{
    uint8_t *new_buffer = NULL;

    if (source == NULL) {
        return VIRTUAL_AUDIO_ERR_INVALID_ARG;
    }
    if (source->packet_capacity >= VIRTUAL_AUDIO_PACKET_BYTES) {
        return VIRTUAL_AUDIO_OK;
    }

    new_buffer = (uint8_t *)realloc(source->packet_buffer, VIRTUAL_AUDIO_PACKET_BYTES);
    if (new_buffer == NULL) {
        return VIRTUAL_AUDIO_ERR_IO;
    }

    source->packet_buffer = new_buffer;
    source->packet_capacity = VIRTUAL_AUDIO_PACKET_BYTES;
    return VIRTUAL_AUDIO_OK;
}

static int16_t virtual_audio_source_next_sample(virtual_audio_source_t *source)
{
    uint32_t phase = source->phase;
    int32_t sample = 0;

    if (phase < (VIRTUAL_AUDIO_SAMPLE_RATE_HZ / 2U)) {
        sample = ((int32_t)phase * 4 * VIRTUAL_AUDIO_TONE_AMPLITUDE /
                  (int32_t)VIRTUAL_AUDIO_SAMPLE_RATE_HZ) -
                 VIRTUAL_AUDIO_TONE_AMPLITUDE;
    } else {
        sample = (3 * VIRTUAL_AUDIO_TONE_AMPLITUDE) -
                 ((int32_t)phase * 4 * VIRTUAL_AUDIO_TONE_AMPLITUDE /
                  (int32_t)VIRTUAL_AUDIO_SAMPLE_RATE_HZ);
    }

    source->phase += VIRTUAL_AUDIO_TONE_HZ;
    while (source->phase >= VIRTUAL_AUDIO_SAMPLE_RATE_HZ) {
        source->phase -= VIRTUAL_AUDIO_SAMPLE_RATE_HZ;
    }

    return (int16_t)sample;
}

int virtual_audio_source_open(virtual_audio_source_t *source)
{
    int rc = VIRTUAL_AUDIO_OK;

    if (source == NULL) {
        return VIRTUAL_AUDIO_ERR_INVALID_ARG;
    }

    memset(source, 0, sizeof(*source));
    source->format.sample_rate_hz = VIRTUAL_AUDIO_SAMPLE_RATE_HZ;
    source->format.channels = VIRTUAL_AUDIO_CHANNELS;
    source->format.bits_per_sample = VIRTUAL_AUDIO_BITS_PER_SAMPLE;

    rc = virtual_audio_source_reserve(source);
    if (rc != VIRTUAL_AUDIO_OK) {
        virtual_audio_source_close(source);
        return rc;
    }
    return VIRTUAL_AUDIO_OK;
}

void virtual_audio_source_reset(virtual_audio_source_t *source)
{
    if (source == NULL) {
        return;
    }

    source->packet_length = 0U;
    source->phase = 0U;
    source->sequence = 0U;
    source->last_sequence = 0U;
    source->last_sequence_valid = false;
}

void virtual_audio_source_close(virtual_audio_source_t *source)
{
    if (source == NULL) {
        return;
    }

    free(source->packet_buffer);
    memset(source, 0, sizeof(*source));
}

int virtual_audio_source_next_packet(virtual_audio_source_t *source,
                                     const uint8_t **data_ptr,
                                     size_t *data_len,
                                     const tirtc_session_audio_format_t **format,
                                     uint32_t *duration_us)
{
    int16_t *samples = NULL;
    int rc = VIRTUAL_AUDIO_OK;

    if (source == NULL || data_ptr == NULL || data_len == NULL || duration_us == NULL) {
        return VIRTUAL_AUDIO_ERR_INVALID_ARG;
    }

    rc = virtual_audio_source_reserve(source);
    if (rc != VIRTUAL_AUDIO_OK) {
        return rc;
    }

    samples = (int16_t *)source->packet_buffer;
    for (size_t index = 0; index < VIRTUAL_AUDIO_SAMPLES_PER_FRAME; ++index) {
        samples[index] = virtual_audio_source_next_sample(source);
    }

    virtual_audio_packet_header_t header = {
        .magic = VIRTUAL_AUDIO_PACKET_MAGIC,
        .sequence = source->sequence,
    };
    header.payload_crc32 = virtual_audio_crc32(source->packet_buffer + sizeof(header),
                                               VIRTUAL_AUDIO_PACKET_BYTES - sizeof(header));
    header.header_crc32 = virtual_audio_crc32((const uint8_t *)&header,
                                              offsetof(virtual_audio_packet_header_t,
                                                       header_crc32));
    memcpy(source->packet_buffer, &header, sizeof(header));
    source->last_sequence = source->sequence;
    source->last_sequence_valid = true;
    source->sequence++;

    source->packet_length = VIRTUAL_AUDIO_PACKET_BYTES;
    *data_ptr = source->packet_buffer;
    *data_len = source->packet_length;
    if (format != NULL) {
        *format = &source->format;
    }
    *duration_us = VIRTUAL_AUDIO_FRAME_MS * 1000U;
    return VIRTUAL_AUDIO_OK;
}

int virtual_audio_source_parse_packet(const uint8_t *data,
                                      size_t data_len,
                                      virtual_audio_packet_info_t *info)
{
    virtual_audio_packet_header_t header = {0};

    if (data == NULL || info == NULL) {
        return VIRTUAL_AUDIO_ERR_INVALID_ARG;
    }
    memset(info, 0, sizeof(*info));
    if (data_len < sizeof(header)) {
        return VIRTUAL_AUDIO_ERR_NOT_TEST;
    }

    memcpy(&header, data, sizeof(header));
    if (header.magic != VIRTUAL_AUDIO_PACKET_MAGIC) {
        return VIRTUAL_AUDIO_ERR_NOT_TEST;
    }

    uint32_t header_crc32 = virtual_audio_crc32(
        (const uint8_t *)&header,
        offsetof(virtual_audio_packet_header_t, header_crc32));
    uint32_t payload_crc32 = virtual_audio_crc32(data + sizeof(header),
                                                 data_len - sizeof(header));
    if (header.header_crc32 != header_crc32 || header.payload_crc32 != payload_crc32) {
        return VIRTUAL_AUDIO_ERR_CHECKSUM;
    }

    info->sequence = header.sequence;
    info->payload_crc32 = header.payload_crc32;
    return VIRTUAL_AUDIO_OK;
}
