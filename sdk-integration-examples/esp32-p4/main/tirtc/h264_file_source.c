#include "h264_file_source.h"

#include <string.h>

#include "esp_heap_caps.h"

#define H264_READ_CHUNK_BYTES 4096U
#define H264_BUFFER_INIT_BYTES (16U * 1024U)
#define H264_BUFFER_LIMIT_BYTES (512U * 1024U)

#define H264_PARSE_NEED_MORE 2

static int find_nal_unit(const uint8_t *buffer, int size, uint8_t *nal_type, int *nal_start, int *nal_end)
{
    int index = 0;

    if (buffer == NULL || nal_type == NULL || nal_start == NULL || nal_end == NULL || size < 4)
    {
        return 0;
    }

    *nal_start = 0;
    *nal_end = 0;

    while (buffer[index] != 0 || buffer[index + 1] != 0 ||
           !(buffer[index + 2] == 1 || (buffer[index + 2] == 0 && buffer[index + 3] == 1)))
    {
        index++;
        if (size < index + 4)
        {
            return 0;
        }
    }

    *nal_start = index;
    if (buffer[index + 2] == 1)
    {
        *nal_type = buffer[index + 3] & 0x1F;
        index += 4;
    }
    else if (size > index + 4)
    {
        *nal_type = buffer[index + 4] & 0x1F;
        index += 5;
    }
    else
    {
        return 0;
    }

    if (size < index + 4)
    {
        *nal_end = size - 1;
        return -1;
    }

    while (buffer[index] != 0 || buffer[index + 1] != 0 ||
           !(buffer[index + 2] == 1 || (buffer[index + 2] == 0 && buffer[index + 3] == 1)))
    {
        index++;
        if (size < index + 4)
        {
            *nal_end = size - 1;
            return -1;
        }
    }

    *nal_end = index - 1;
    return *nal_end - *nal_start;
}

#define H264_BIT(num, bit) (((num) & (1 << (7 - (bit)))) > 0)

static bool exp_golomb_decode(const uint8_t *buffer, int size, int *bit_offset, int *value)
{
    int total_bits = size << 3;
    int leading_zero_bits = 0;
    int index = 0;
    int suffix = 0;
    int bit_pos = 0;

    if (buffer == NULL || bit_offset == NULL || value == NULL || size <= 0 ||
        *bit_offset < 0 || *bit_offset >= total_bits)
    {
        return false;
    }

    for (index = *bit_offset; index < total_bits && !H264_BIT(buffer[index / 8], index % 8); index++)
    {
        leading_zero_bits++;
    }
    if (index >= total_bits || leading_zero_bits > 30)
    {
        return false;
    }

    bit_pos = *bit_offset + leading_zero_bits + 1;
    if (bit_pos + leading_zero_bits > total_bits)
    {
        return false;
    }

    for (index = 0; index < leading_zero_bits && bit_pos < total_bits; index++)
    {
        suffix = (suffix << 1) + H264_BIT(buffer[bit_pos / 8], bit_pos % 8);
        bit_pos++;
    }

    *bit_offset = bit_pos;
    *value = (1 << leading_zero_bits) - 1 + suffix;
    return true;
}

static int slice_payload_offset(const uint8_t *data, size_t size, int nal_start)
{
    if (data == NULL || nal_start < 0 || size <= (size_t)(nal_start + 4))
    {
        return -1;
    }

    if (data[nal_start + 2] == 1)
    {
        return nal_start + 4;
    }

    if (size <= (size_t)(nal_start + 5))
    {
        return -1;
    }

    return nal_start + 5;
}

static void source_compact(h264_file_source_t *source)
{
    if (source == NULL || source->consumed_bytes == 0)
    {
        return;
    }

    if (source->consumed_bytes >= source->size)
    {
        source->size = 0;
        source->consumed_bytes = 0;
        return;
    }

    memmove(source->data, source->data + source->consumed_bytes, source->size - source->consumed_bytes);
    source->size -= source->consumed_bytes;
    source->consumed_bytes = 0;
}

static int source_reserve(h264_file_source_t *source, size_t required_capacity)
{
    size_t new_capacity = 0;
    uint8_t *new_data = NULL;

    if (source == NULL)
    {
        return H264_FILE_SOURCE_ERR_INVALID_ARG;
    }

    if (required_capacity > H264_BUFFER_LIMIT_BYTES)
    {
        return H264_FILE_SOURCE_ERR_IO;
    }

    if (source->capacity >= required_capacity)
    {
        return H264_FILE_SOURCE_OK;
    }

    new_capacity = source->capacity == 0 ? H264_BUFFER_INIT_BYTES : source->capacity;
    while (new_capacity < required_capacity)
    {
        if (new_capacity >= H264_BUFFER_LIMIT_BYTES)
        {
            return H264_FILE_SOURCE_ERR_IO;
        }

        new_capacity *= 2;
        if (new_capacity > H264_BUFFER_LIMIT_BYTES)
        {
            new_capacity = H264_BUFFER_LIMIT_BYTES;
        }
    }

    new_data = (uint8_t *)heap_caps_malloc(new_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (new_data == NULL)
    {
        new_data = (uint8_t *)heap_caps_malloc(new_capacity, MALLOC_CAP_8BIT);
    }
    if (new_data == NULL)
    {
        return H264_FILE_SOURCE_ERR_NO_MEM;
    }

    if (source->data != NULL && source->size > 0)
    {
        memcpy(new_data, source->data, source->size);
    }

    heap_caps_free(source->data);
    source->data = new_data;
    source->capacity = new_capacity;
    return H264_FILE_SOURCE_OK;
}

static int source_fill(h264_file_source_t *source)
{
    size_t free_space = 0;
    size_t read_size = 0;
    size_t read_bytes = 0;
    int ret = H264_FILE_SOURCE_OK;

    if (source == NULL || source->fp == NULL)
    {
        return H264_FILE_SOURCE_ERR_INVALID_ARG;
    }

    if (source->file_end_reached)
    {
        return H264_FILE_SOURCE_OK;
    }

    source_compact(source);
    if (source->capacity - source->size < H264_READ_CHUNK_BYTES)
    {
        ret = source_reserve(source, source->size + H264_READ_CHUNK_BYTES);
        if (ret != H264_FILE_SOURCE_OK)
        {
            return ret;
        }
    }

    free_space = source->capacity - source->size;
    if (free_space == 0)
    {
        return H264_FILE_SOURCE_ERR_IO;
    }

    read_size = free_space > H264_READ_CHUNK_BYTES ? H264_READ_CHUNK_BYTES : free_space;
    read_bytes = fread(source->data + source->size, 1, read_size, source->fp);
    source->size += read_bytes;

    if (read_bytes == 0)
    {
        if (ferror(source->fp))
        {
            return H264_FILE_SOURCE_ERR_IO;
        }
        if (feof(source->fp))
        {
            source->file_end_reached = true;
            return H264_FILE_SOURCE_OK;
        }
        return H264_FILE_SOURCE_ERR_IO;
    }

    if (read_bytes < read_size && feof(source->fp))
    {
        source->file_end_reached = true;
    }

    return H264_FILE_SOURCE_OK;
}

static int source_locate_nal(const h264_file_source_t *source,
                             int search_offset,
                             uint8_t *nal_type,
                             int *nal_start,
                             int *nal_end)
{
    int local_start = 0;
    int local_end = 0;
    int ret = 0;

    if (source == NULL || source->data == NULL || nal_type == NULL ||
        nal_start == NULL || nal_end == NULL || search_offset < 0)
    {
        return H264_FILE_SOURCE_ERR_INVALID_ARG;
    }

    if ((size_t)search_offset >= source->size)
    {
        return source->file_end_reached ? H264_FILE_SOURCE_EOF : H264_PARSE_NEED_MORE;
    }

    ret = find_nal_unit(source->data + search_offset,
                        (int)(source->size - (size_t)search_offset),
                        nal_type,
                        &local_start,
                        &local_end);
    if (ret == 0)
    {
        return source->file_end_reached ? H264_FILE_SOURCE_EOF : H264_PARSE_NEED_MORE;
    }
    if (ret == -1 && !source->file_end_reached)
    {
        return H264_PARSE_NEED_MORE;
    }

    *nal_start = search_offset + local_start;
    *nal_end = search_offset + local_end;
    return H264_FILE_SOURCE_OK;
}

static int source_find_frame(h264_file_source_t *source, int *frame_start, int *frame_end, bool *is_key_frame)
{
    uint8_t nal_type = 0;
    int nal_start = 0;
    int nal_end = 0;
    int search_offset = 0;
    int offset = 0;
    int bit_offset = 0;
    int first_mb_in_slice = 0;
    int slice_type = 0;
    int prev_first_mb_in_slice = 0;
    int prev_nal_type = 0;
    bool local_is_key_frame = false;
    int ret = H264_FILE_SOURCE_OK;

    if (source == NULL || frame_start == NULL || frame_end == NULL || is_key_frame == NULL)
    {
        return H264_FILE_SOURCE_ERR_INVALID_ARG;
    }

    *frame_start = -1;
    *frame_end = -1;

    while (true)
    {
        ret = source_locate_nal(source, search_offset, &nal_type, &nal_start, &nal_end);
        if (ret != H264_FILE_SOURCE_OK)
        {
            return ret;
        }

        if (*frame_start < 0)
        {
            *frame_start = nal_start;
        }

        if (nal_type == 1 || nal_type == 5)
        {
            break;
        }

        search_offset = nal_end + 1;
    }

    offset = slice_payload_offset(source->data, source->size, nal_start);
    if (offset < 0 || (size_t)offset >= source->size)
    {
        return H264_FILE_SOURCE_ERR_BAD_STREAM;
    }

    bit_offset = 0;
    if (!exp_golomb_decode(source->data + offset,
                           (int)(source->size - (size_t)offset),
                           &bit_offset,
                           &first_mb_in_slice) ||
        !exp_golomb_decode(source->data + offset,
                           (int)(source->size - (size_t)offset),
                           &bit_offset,
                           &slice_type))
    {
        return H264_FILE_SOURCE_ERR_BAD_STREAM;
    }

    if (nal_type == 5)
    {
        local_is_key_frame = true;
    }
    else
    {
        slice_type %= 5;
        local_is_key_frame = (slice_type == 2 || slice_type == 4);
    }

    prev_first_mb_in_slice = first_mb_in_slice;
    prev_nal_type = nal_type;
    search_offset = nal_end + 1;

    while (true)
    {
        ret = source_locate_nal(source, search_offset, &nal_type, &nal_start, &nal_end);
        if (ret == H264_FILE_SOURCE_EOF)
        {
            *frame_end = (int)source->size - 1;
            *is_key_frame = local_is_key_frame;
            return H264_FILE_SOURCE_OK;
        }
        if (ret != H264_FILE_SOURCE_OK)
        {
            return ret;
        }

        if (nal_type != prev_nal_type)
        {
            *frame_end = nal_start - 1;
            *is_key_frame = local_is_key_frame;
            return H264_FILE_SOURCE_OK;
        }

        offset = slice_payload_offset(source->data, source->size, nal_start);
        if (offset < 0 || (size_t)offset >= source->size)
        {
            return H264_FILE_SOURCE_ERR_BAD_STREAM;
        }

        bit_offset = 0;
        if (!exp_golomb_decode(source->data + offset,
                               (int)(source->size - (size_t)offset),
                               &bit_offset,
                               &first_mb_in_slice))
        {
            return H264_FILE_SOURCE_ERR_BAD_STREAM;
        }
        if ((prev_first_mb_in_slice > first_mb_in_slice) ||
            (prev_first_mb_in_slice == first_mb_in_slice && prev_first_mb_in_slice == 0))
        {
            *frame_end = nal_start - 1;
            *is_key_frame = local_is_key_frame;
            return H264_FILE_SOURCE_OK;
        }

        prev_first_mb_in_slice = first_mb_in_slice;
        search_offset = nal_end + 1;
    }
}

int h264_file_source_open(const char *path, h264_file_source_t *source)
{
    int ret = H264_FILE_SOURCE_OK;

    if (path == NULL || path[0] == '\0' || source == NULL)
    {
        return H264_FILE_SOURCE_ERR_INVALID_ARG;
    }

    memset(source, 0, sizeof(*source));
    source->fp = fopen(path, "rb");
    if (source->fp == NULL)
    {
        return H264_FILE_SOURCE_ERR_IO;
    }

    ret = source_reserve(source, H264_BUFFER_INIT_BYTES);
    if (ret != H264_FILE_SOURCE_OK)
    {
        h264_file_source_close(source);
        return ret;
    }

    return H264_FILE_SOURCE_OK;
}

void h264_file_source_reset(h264_file_source_t *source)
{
    if (source == NULL)
    {
        return;
    }

    if (source->fp != NULL)
    {
        clearerr(source->fp);
        if (fseek(source->fp, 0, SEEK_SET) != 0)
        {
            source->size = 0;
            source->consumed_bytes = 0;
            source->file_end_reached = true;
            return;
        }
    }

    source->size = 0;
    source->consumed_bytes = 0;
    source->file_end_reached = false;
}

void h264_file_source_close(h264_file_source_t *source)
{
    if (source == NULL)
    {
        return;
    }

    if (source->fp != NULL)
    {
        fclose(source->fp);
    }

    heap_caps_free(source->data);
    memset(source, 0, sizeof(*source));
}

int h264_file_source_next_frame(h264_file_source_t *source,
                                const uint8_t **data,
                                size_t *length,
                                bool *is_key_frame)
{
    int frame_start = 0;
    int frame_end = 0;
    int ret = H264_FILE_SOURCE_OK;

    if (source == NULL || data == NULL || length == NULL || is_key_frame == NULL)
    {
        return H264_FILE_SOURCE_ERR_INVALID_ARG;
    }
    if (source->fp == NULL || source->data == NULL)
    {
        return H264_FILE_SOURCE_ERR_IO;
    }

    source_compact(source);

    while (true)
    {
        if (source->size == 0 && !source->file_end_reached)
        {
            ret = source_fill(source);
            if (ret != H264_FILE_SOURCE_OK)
            {
                return ret;
            }
        }

        if (source->size == 0 && source->file_end_reached)
        {
            return H264_FILE_SOURCE_EOF;
        }

        ret = source_find_frame(source, &frame_start, &frame_end, is_key_frame);
        if (ret == H264_FILE_SOURCE_OK)
        {
            if (frame_end < frame_start)
            {
                return H264_FILE_SOURCE_ERR_BAD_STREAM;
            }

            *data = source->data + frame_start;
            *length = (size_t)(frame_end - frame_start + 1);
            source->consumed_bytes = (size_t)(frame_end + 1);
            return H264_FILE_SOURCE_OK;
        }

        if (ret == H264_PARSE_NEED_MORE)
        {
            ret = source_fill(source);
            if (ret != H264_FILE_SOURCE_OK)
            {
                return ret;
            }
            continue;
        }

        return ret;
    }
}
