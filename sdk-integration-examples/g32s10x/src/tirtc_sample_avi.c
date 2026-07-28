#include <stddef.h>
#include <string.h>

#include "tirtc_sample_avi.h"

#define AVI_HEADER_SIZE 12U
#define AVI_CHUNK_HEADER_SIZE 8U

typedef enum {
    AVI_PACKET_VIDEO,
    AVI_PACKET_AUDIO,
} avi_packet_type_t;

static uint32_t avi_read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static bool avi_fourcc(const uint8_t *data, const char *fourcc)
{
    return data[0] == (uint8_t)fourcc[0] &&
           data[1] == (uint8_t)fourcc[1] &&
           data[2] == (uint8_t)fourcc[2] &&
           data[3] == (uint8_t)fourcc[3];
}

static bool avi_chunk_matches(const uint8_t *chunk, avi_packet_type_t type)
{
    if (type == AVI_PACKET_AUDIO) {
        return chunk[2] == (uint8_t)'w' && chunk[3] == (uint8_t)'b';
    }
    return chunk[2] == (uint8_t)'d' &&
           (chunk[3] == (uint8_t)'c' || chunk[3] == (uint8_t)'b');
}

static bool avi_find_movi(tirtc_sample_avi_t *avi)
{
    uint32_t offset = AVI_HEADER_SIZE;

    if (avi->length < AVI_HEADER_SIZE || !avi_fourcc(avi->data, "RIFF") ||
        !avi_fourcc(avi->data + 8U, "AVI ")) {
        return false;
    }
    while (offset + AVI_CHUNK_HEADER_SIZE <= avi->length) {
        const uint8_t *chunk = avi->data + offset;
        uint32_t length = avi_read_le32(chunk + 4U);
        uint64_t end = (uint64_t)offset + AVI_CHUNK_HEADER_SIZE + length;

        if (end > avi->length) {
            return false;
        }
        if (avi_fourcc(chunk, "LIST") && length >= 4U &&
            avi_fourcc(chunk + AVI_CHUNK_HEADER_SIZE, "movi")) {
            avi->movi_start = offset + AVI_CHUNK_HEADER_SIZE + 4U;
            avi->movi_end = (uint32_t)end;
            return avi->movi_start < avi->movi_end;
        }
        offset = (uint32_t)(end + (length & 1U));
    }
    return false;
}

bool tirtc_sample_avi_open(tirtc_sample_avi_t *avi, const uint8_t *data,
                           uint32_t length, uint32_t max_packet_bytes)
{
    uint32_t offset;

    if (avi == NULL || data == NULL || length == 0U ||
        max_packet_bytes == 0U) {
        return false;
    }
    memset(avi, 0, sizeof(*avi));
    avi->data = data;
    avi->length = length;
    if (!avi_find_movi(avi)) {
        return false;
    }

    offset = avi->movi_start;
    while (offset + AVI_CHUNK_HEADER_SIZE <= avi->movi_end) {
        const uint8_t *chunk = avi->data + offset;
        uint32_t chunk_length = avi_read_le32(chunk + 4U);
        uint64_t payload = (uint64_t)offset + AVI_CHUNK_HEADER_SIZE;
        uint64_t end = payload + chunk_length;

        if (end > avi->movi_end) {
            return false;
        }
        if (avi_chunk_matches(chunk, AVI_PACKET_VIDEO)) {
            const uint8_t *frame = avi->data + (uint32_t)payload;

            if (chunk_length < 4U || chunk_length > max_packet_bytes ||
                frame[0] != 0xffU || frame[1] != 0xd8U ||
                frame[chunk_length - 2U] != 0xffU ||
                frame[chunk_length - 1U] != 0xd9U) {
                return false;
            }
            ++avi->video_frames;
        } else if (avi_chunk_matches(chunk, AVI_PACKET_AUDIO)) {
            if (chunk_length == 0U || chunk_length > max_packet_bytes ||
                UINT32_MAX - avi->audio_bytes < chunk_length) {
                return false;
            }
            avi->audio_bytes += chunk_length;
        }
        offset = (uint32_t)(end + (chunk_length & 1U));
    }
    if (avi->video_frames == 0U || avi->audio_bytes == 0U) {
        return false;
    }
    avi->duration_ms =
        (avi->video_frames * 1000U) / TIRTC_SAMPLE_AVI_VIDEO_FPS;
    return avi->audio_bytes ==
           avi->duration_ms * TIRTC_SAMPLE_AVI_AUDIO_BYTES_PER_MS;
}

void tirtc_sample_avi_cursor_reset(const tirtc_sample_avi_t *avi,
                                   tirtc_sample_avi_cursor_t *cursor)
{
    if (avi == NULL || cursor == NULL) {
        return;
    }
    memset(cursor, 0, sizeof(*cursor));
    cursor->offset = avi->movi_start;
}

uint32_t tirtc_sample_avi_video_position_ms(
    const tirtc_sample_avi_t *avi,
    const tirtc_sample_avi_cursor_t *cursor)
{
    if (avi == NULL || cursor == NULL) {
        return 0U;
    }
    return cursor->loop * avi->duration_ms +
           (cursor->video_index * 1000U) / TIRTC_SAMPLE_AVI_VIDEO_FPS;
}

uint32_t tirtc_sample_avi_audio_position_ms(
    const tirtc_sample_avi_t *avi,
    const tirtc_sample_avi_cursor_t *cursor)
{
    if (avi == NULL || cursor == NULL) {
        return 0U;
    }
    return cursor->loop * avi->duration_ms +
           cursor->audio_bytes / TIRTC_SAMPLE_AVI_AUDIO_BYTES_PER_MS;
}

bool tirtc_sample_avi_next_video(const tirtc_sample_avi_t *avi,
                                 tirtc_sample_avi_cursor_t *cursor,
                                 const uint8_t **data, uint32_t *length,
                                 uint32_t *timestamp_ms)
{
    unsigned int pass;

    if (avi == NULL || cursor == NULL || data == NULL || length == NULL ||
        timestamp_ms == NULL) {
        return false;
    }
    for (pass = 0U; pass < 2U; ++pass) {
        while (cursor->offset + AVI_CHUNK_HEADER_SIZE <= avi->movi_end) {
            const uint8_t *chunk = avi->data + cursor->offset;
            uint32_t chunk_length = avi_read_le32(chunk + 4U);
            uint64_t payload =
                (uint64_t)cursor->offset + AVI_CHUNK_HEADER_SIZE;
            uint64_t end = payload + chunk_length;

            if (end > avi->movi_end) {
                return false;
            }
            cursor->offset = (uint32_t)(end + (chunk_length & 1U));
            if (!avi_chunk_matches(chunk, AVI_PACKET_VIDEO)) {
                continue;
            }
            *data = avi->data + (uint32_t)payload;
            *length = chunk_length;
            *timestamp_ms = tirtc_sample_avi_video_position_ms(avi, cursor);
            ++cursor->video_index;
            return true;
        }
        cursor->offset = avi->movi_start;
        ++cursor->loop;
        cursor->video_index = 0U;
    }
    return false;
}

static bool avi_open_next_audio_chunk(
    const tirtc_sample_avi_t *avi, tirtc_sample_avi_cursor_t *cursor)
{
    unsigned int pass;

    for (pass = 0U; pass < 2U; ++pass) {
        while (cursor->offset + AVI_CHUNK_HEADER_SIZE <= avi->movi_end) {
            const uint8_t *chunk = avi->data + cursor->offset;
            uint32_t chunk_length = avi_read_le32(chunk + 4U);
            uint64_t payload =
                (uint64_t)cursor->offset + AVI_CHUNK_HEADER_SIZE;
            uint64_t end = payload + chunk_length;

            if (end > avi->movi_end) {
                return false;
            }
            cursor->offset = (uint32_t)(end + (chunk_length & 1U));
            if (!avi_chunk_matches(chunk, AVI_PACKET_AUDIO)) {
                continue;
            }
            cursor->audio_chunk_offset = (uint32_t)payload;
            cursor->audio_chunk_remaining = chunk_length;
            return true;
        }
        cursor->offset = avi->movi_start;
        ++cursor->loop;
        cursor->audio_bytes = 0U;
        cursor->audio_chunk_offset = 0U;
        cursor->audio_chunk_remaining = 0U;
    }
    return false;
}

bool tirtc_sample_avi_next_audio(const tirtc_sample_avi_t *avi,
                                 tirtc_sample_avi_cursor_t *cursor,
                                 uint8_t *data, uint32_t capacity,
                                 uint32_t *length, uint32_t *timestamp_ms)
{
    uint32_t produced = 0U;

    if (avi == NULL || cursor == NULL || data == NULL || length == NULL ||
        timestamp_ms == NULL || capacity == 0U) {
        return false;
    }
    *timestamp_ms = tirtc_sample_avi_audio_position_ms(avi, cursor);
    while (produced < capacity) {
        uint32_t copy_length;

        if (cursor->audio_chunk_remaining == 0U &&
            !avi_open_next_audio_chunk(avi, cursor)) {
            return false;
        }
        copy_length = capacity - produced;
        if (copy_length > cursor->audio_chunk_remaining) {
            copy_length = cursor->audio_chunk_remaining;
        }
        memcpy(data + produced, avi->data + cursor->audio_chunk_offset,
               copy_length);
        produced += copy_length;
        cursor->audio_chunk_offset += copy_length;
        cursor->audio_chunk_remaining -= copy_length;
        cursor->audio_bytes += copy_length;
    }
    *length = produced;
    return true;
}
