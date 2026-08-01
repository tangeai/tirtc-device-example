#include "device/device_media_file.h"

#include <string.h>

device_media_file_result_t device_g711_file_open(device_g711_file_t *source,
                                                 const char *path,
                                                 size_t packet_size)
{
    if (source == NULL || path == NULL || path[0] == '\0' || packet_size == 0) {
        return DEVICE_MEDIA_FILE_INVALID;
    }

    memset(source, 0, sizeof(*source));
    source->file = fopen(path, "rb");
    if (source->file == NULL) {
        return DEVICE_MEDIA_FILE_IO_ERROR;
    }
    source->packet_size = packet_size;
    return DEVICE_MEDIA_FILE_OK;
}

device_media_file_result_t device_g711_file_next(device_g711_file_t *source,
                                                 uint8_t *buffer,
                                                 size_t capacity,
                                                 size_t *size,
                                                 bool loop)
{
    if (size != NULL) {
        *size = 0;
    }
    if (source == NULL || source->file == NULL || buffer == NULL || size == NULL) {
        return DEVICE_MEDIA_FILE_INVALID;
    }
    if (capacity < source->packet_size) {
        return DEVICE_MEDIA_FILE_BUFFER_TOO_SMALL;
    }

    size_t count = fread(buffer, 1, source->packet_size, source->file);
    if (count == 0 && feof(source->file) && loop) {
        clearerr(source->file);
        rewind(source->file);
        source->packet_index = 0;
        count = fread(buffer, 1, source->packet_size, source->file);
    }
    if (count == source->packet_size) {
        *size = count;
        source->packet_index++;
        return DEVICE_MEDIA_FILE_OK;
    }
    if (ferror(source->file)) {
        return DEVICE_MEDIA_FILE_IO_ERROR;
    }
    return count == 0 ? DEVICE_MEDIA_FILE_EOF : DEVICE_MEDIA_FILE_INVALID;
}

void device_g711_file_close(device_g711_file_t *source)
{
    if (source == NULL) {
        return;
    }
    if (source->file != NULL) {
        fclose(source->file);
    }
    memset(source, 0, sizeof(*source));
}

static const uint8_t k_amr_nb_payload_bytes[16] = {
    12, 13, 15, 17, 19, 20, 26, 31, 5, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0,
};

static const uint8_t k_amr_wb_payload_bytes[16] = {
    17, 23, 32, 36, 40, 46, 50, 58, 60, 5, 0xff, 0xff, 0xff, 0xff, 0xff, 0,
};

device_media_file_result_t device_amr_file_open(device_amr_file_t *source,
                                                const char *path,
                                                bool wideband)
{
    static const uint8_t narrow_magic[] = "#!AMR\n";
    static const uint8_t wide_magic[] = "#!AMR-WB\n";
    if (source == NULL || path == NULL || path[0] == '\0') {
        return DEVICE_MEDIA_FILE_INVALID;
    }
    memset(source, 0, sizeof(*source));
    source->file = fopen(path, "rb");
    if (source->file == NULL) {
        return DEVICE_MEDIA_FILE_IO_ERROR;
    }
    const uint8_t *magic = wideband ? wide_magic : narrow_magic;
    size_t magic_size = wideband ? sizeof(wide_magic) - 1U : sizeof(narrow_magic) - 1U;
    uint8_t header[sizeof(wide_magic) - 1U];
    if (fread(header, 1, magic_size, source->file) != magic_size ||
        memcmp(header, magic, magic_size) != 0) {
        device_amr_file_close(source);
        return DEVICE_MEDIA_FILE_INVALID;
    }
    source->wideband = wideband;
    source->data_offset = (long)magic_size;
    return DEVICE_MEDIA_FILE_OK;
}

static device_media_file_result_t read_amr_frame(device_amr_file_t *source,
                                                 uint8_t *buffer,
                                                 size_t capacity,
                                                 size_t *size)
{
    int toc = fgetc(source->file);
    if (toc == EOF) {
        return ferror(source->file) ? DEVICE_MEDIA_FILE_IO_ERROR : DEVICE_MEDIA_FILE_EOF;
    }
    unsigned frame_type = ((unsigned)toc >> 3U) & 0x0fU;
    uint8_t payload_size = source->wideband
                               ? k_amr_wb_payload_bytes[frame_type]
                               : k_amr_nb_payload_bytes[frame_type];
    if (payload_size == 0xffU) {
        return DEVICE_MEDIA_FILE_INVALID;
    }
    size_t frame_size = (size_t)payload_size + 1U;
    if (frame_size > capacity) {
        return DEVICE_MEDIA_FILE_BUFFER_TOO_SMALL;
    }
    buffer[0] = (uint8_t)toc;
    if (payload_size > 0 &&
        fread(buffer + 1, 1, payload_size, source->file) != payload_size) {
        return ferror(source->file) ? DEVICE_MEDIA_FILE_IO_ERROR
                                    : DEVICE_MEDIA_FILE_INVALID;
    }
    *size = frame_size;
    source->frame_index++;
    return DEVICE_MEDIA_FILE_OK;
}

device_media_file_result_t device_amr_file_next(device_amr_file_t *source,
                                                uint8_t *buffer,
                                                size_t capacity,
                                                size_t *size,
                                                bool loop)
{
    if (size != NULL) {
        *size = 0;
    }
    if (source == NULL || source->file == NULL || buffer == NULL || size == NULL) {
        return DEVICE_MEDIA_FILE_INVALID;
    }
    device_media_file_result_t result = read_amr_frame(source, buffer, capacity, size);
    if (result == DEVICE_MEDIA_FILE_EOF && loop) {
        clearerr(source->file);
        if (fseek(source->file, source->data_offset, SEEK_SET) != 0) {
            return DEVICE_MEDIA_FILE_IO_ERROR;
        }
        source->frame_index = 0;
        result = read_amr_frame(source, buffer, capacity, size);
    }
    return result;
}

void device_amr_file_close(device_amr_file_t *source)
{
    if (source == NULL) {
        return;
    }
    if (source->file != NULL) {
        fclose(source->file);
    }
    memset(source, 0, sizeof(*source));
}

#define OPUS_PACKET_MAGIC "TIRTCOPUS1\n"

device_media_file_result_t device_opus_packet_file_open(
    device_opus_packet_file_t *source,
    const char *path)
{
    if (source == NULL || path == NULL || path[0] == '\0') {
        return DEVICE_MEDIA_FILE_INVALID;
    }
    memset(source, 0, sizeof(*source));
    source->file = fopen(path, "rb");
    if (source->file == NULL) {
        return DEVICE_MEDIA_FILE_IO_ERROR;
    }
    const size_t magic_size = sizeof(OPUS_PACKET_MAGIC) - 1U;
    uint8_t header[sizeof(OPUS_PACKET_MAGIC) - 1U];
    if (fread(header, 1, magic_size, source->file) != magic_size ||
        memcmp(header, OPUS_PACKET_MAGIC, magic_size) != 0) {
        device_opus_packet_file_close(source);
        return DEVICE_MEDIA_FILE_INVALID;
    }
    source->data_offset = (long)magic_size;
    return DEVICE_MEDIA_FILE_OK;
}

static device_media_file_result_t read_opus_packet(device_opus_packet_file_t *source,
                                                   uint8_t *buffer,
                                                   size_t capacity,
                                                   size_t *size)
{
    int high = fgetc(source->file);
    if (high == EOF) {
        return ferror(source->file) ? DEVICE_MEDIA_FILE_IO_ERROR : DEVICE_MEDIA_FILE_EOF;
    }
    int low = fgetc(source->file);
    if (low == EOF) {
        return ferror(source->file) ? DEVICE_MEDIA_FILE_IO_ERROR
                                    : DEVICE_MEDIA_FILE_INVALID;
    }
    size_t packet_size = ((size_t)(uint8_t)high << 8U) | (uint8_t)low;
    if (packet_size == 0) {
        return DEVICE_MEDIA_FILE_INVALID;
    }
    if (packet_size > capacity) {
        return DEVICE_MEDIA_FILE_BUFFER_TOO_SMALL;
    }
    if (fread(buffer, 1, packet_size, source->file) != packet_size) {
        return ferror(source->file) ? DEVICE_MEDIA_FILE_IO_ERROR
                                    : DEVICE_MEDIA_FILE_INVALID;
    }
    *size = packet_size;
    source->packet_index++;
    return DEVICE_MEDIA_FILE_OK;
}

device_media_file_result_t device_opus_packet_file_next(
    device_opus_packet_file_t *source,
    uint8_t *buffer,
    size_t capacity,
    size_t *size,
    bool loop)
{
    if (size != NULL) {
        *size = 0;
    }
    if (source == NULL || source->file == NULL || buffer == NULL || size == NULL) {
        return DEVICE_MEDIA_FILE_INVALID;
    }
    device_media_file_result_t result = read_opus_packet(source, buffer, capacity, size);
    if (result == DEVICE_MEDIA_FILE_EOF && loop) {
        clearerr(source->file);
        if (fseek(source->file, source->data_offset, SEEK_SET) != 0) {
            return DEVICE_MEDIA_FILE_IO_ERROR;
        }
        source->packet_index = 0;
        result = read_opus_packet(source, buffer, capacity, size);
    }
    return result;
}

void device_opus_packet_file_close(device_opus_packet_file_t *source)
{
    if (source == NULL) {
        return;
    }
    if (source->file != NULL) {
        fclose(source->file);
    }
    memset(source, 0, sizeof(*source));
}

static device_media_file_result_t read_mjpeg_frame(device_mjpeg_file_t *source,
                                                   uint8_t *buffer,
                                                   size_t capacity,
                                                   size_t *size)
{
    bool in_frame = false;
    int previous = -1;
    size_t used = 0;

    for (;;) {
        int current = fgetc(source->file);
        if (current == EOF) {
            if (ferror(source->file)) {
                return DEVICE_MEDIA_FILE_IO_ERROR;
            }
            return in_frame ? DEVICE_MEDIA_FILE_INVALID : DEVICE_MEDIA_FILE_EOF;
        }

        if (!in_frame) {
            if (previous == 0xff && current == 0xd8) {
                if (capacity < 2) {
                    return DEVICE_MEDIA_FILE_BUFFER_TOO_SMALL;
                }
                buffer[0] = 0xff;
                buffer[1] = 0xd8;
                used = 2;
                in_frame = true;
            }
            previous = current;
            continue;
        }

        if (used >= capacity) {
            return DEVICE_MEDIA_FILE_BUFFER_TOO_SMALL;
        }
        buffer[used++] = (uint8_t)current;
        if (previous == 0xff && current == 0xd9) {
            *size = used;
            source->frame_index++;
            return DEVICE_MEDIA_FILE_OK;
        }
        previous = current;
    }
}

device_media_file_result_t device_mjpeg_file_open(device_mjpeg_file_t *source,
                                                  const char *path)
{
    if (source == NULL || path == NULL || path[0] == '\0') {
        return DEVICE_MEDIA_FILE_INVALID;
    }

    memset(source, 0, sizeof(*source));
    source->file = fopen(path, "rb");
    if (source->file == NULL) {
        return DEVICE_MEDIA_FILE_IO_ERROR;
    }
    return DEVICE_MEDIA_FILE_OK;
}

device_media_file_result_t device_mjpeg_file_next(device_mjpeg_file_t *source,
                                                  uint8_t *buffer,
                                                  size_t capacity,
                                                  size_t *size,
                                                  bool loop)
{
    if (size != NULL) {
        *size = 0;
    }
    if (source == NULL || source->file == NULL || buffer == NULL || size == NULL) {
        return DEVICE_MEDIA_FILE_INVALID;
    }

    device_media_file_result_t result = read_mjpeg_frame(source, buffer, capacity, size);
    if (result == DEVICE_MEDIA_FILE_EOF && loop) {
        clearerr(source->file);
        rewind(source->file);
        source->frame_index = 0;
        result = read_mjpeg_frame(source, buffer, capacity, size);
    }
    return result;
}

void device_mjpeg_file_close(device_mjpeg_file_t *source)
{
    if (source == NULL) {
        return;
    }
    if (source->file != NULL) {
        fclose(source->file);
    }
    memset(source, 0, sizeof(*source));
}

static bool find_h264_start_code(FILE *file, long *offset)
{
    int zero_count = 0;
    for (;;) {
        int byte = fgetc(file);
        if (byte == EOF) {
            return false;
        }
        if (byte == 0) {
            zero_count++;
            continue;
        }
        if (byte == 1 && zero_count >= 2) {
            long after = ftell(file);
            int start_code_size = zero_count >= 3 ? 4 : 3;
            *offset = after - start_code_size;
            return true;
        }
        zero_count = 0;
    }
}

static bool h264_is_slice(int nal_type)
{
    return nal_type >= 1 && nal_type <= 5;
}

typedef struct {
    FILE *file;
    uint8_t current_byte;
    uint8_t bits_remaining;
    uint8_t consecutive_zeros;
} h264_rbsp_reader_t;

static bool h264_rbsp_read_bit(h264_rbsp_reader_t *reader, uint8_t *bit)
{
    if (reader->bits_remaining == 0U) {
        int byte;
        for (;;) {
            byte = fgetc(reader->file);
            if (byte == EOF) {
                return false;
            }
            if (reader->consecutive_zeros >= 2U && byte == 0x03) {
                continue;
            }
            reader->consecutive_zeros =
                byte == 0 ? (uint8_t)(reader->consecutive_zeros + 1U) : 0U;
            break;
        }
        reader->current_byte = (uint8_t)byte;
        reader->bits_remaining = 8U;
    }
    reader->bits_remaining--;
    *bit = (reader->current_byte >> reader->bits_remaining) & 1U;
    return true;
}

static bool h264_read_first_mb_in_slice(FILE *file, uint32_t *first_mb)
{
    h264_rbsp_reader_t reader = {
        .file = file,
    };
    uint8_t bit = 0;
    unsigned leading_zero_bits = 0;
    while (leading_zero_bits < 31U) {
        if (!h264_rbsp_read_bit(&reader, &bit)) {
            return false;
        }
        if (bit != 0U) {
            break;
        }
        leading_zero_bits++;
    }
    if (bit == 0U || leading_zero_bits >= 31U) {
        return false;
    }

    uint32_t suffix = 0;
    for (unsigned index = 0; index < leading_zero_bits; ++index) {
        if (!h264_rbsp_read_bit(&reader, &bit)) {
            return false;
        }
        suffix = (suffix << 1U) | bit;
    }
    *first_mb = ((1UL << leading_zero_bits) - 1UL) + suffix;
    return true;
}

static bool h264_is_access_unit_prefix(int nal_type)
{
    return nal_type == 6 || nal_type == 7 || nal_type == 8 ||
           nal_type == 9 || (nal_type >= 14 && nal_type <= 18);
}

static device_media_file_result_t read_h264_access_unit(device_h264_file_t *source,
                                                        uint8_t *buffer,
                                                        size_t capacity,
                                                        size_t *size,
                                                        bool *key_frame)
{
    long access_unit_start;
    if (!find_h264_start_code(source->file, &access_unit_start)) {
        return ferror(source->file) ? DEVICE_MEDIA_FILE_IO_ERROR : DEVICE_MEDIA_FILE_EOF;
    }

    int nal_header = fgetc(source->file);
    if (nal_header == EOF) {
        return DEVICE_MEDIA_FILE_INVALID;
    }
    int nal_type = nal_header & 0x1f;
    bool has_slice = h264_is_slice(nal_type);
    bool has_idr = nal_type == 5;
    long access_unit_end = -1;

    for (;;) {
        long next_start;
        if (!find_h264_start_code(source->file, &next_start)) {
            if (ferror(source->file)) {
                return DEVICE_MEDIA_FILE_IO_ERROR;
            }
            access_unit_end = ftell(source->file);
            break;
        }

        nal_header = fgetc(source->file);
        if (nal_header == EOF) {
            return DEVICE_MEDIA_FILE_INVALID;
        }
        int next_type = nal_header & 0x1f;
        bool next_is_slice = h264_is_slice(next_type);
        uint32_t first_mb = 0;
        if (next_is_slice &&
            !h264_read_first_mb_in_slice(source->file, &first_mb)) {
            return DEVICE_MEDIA_FILE_INVALID;
        }
        bool starts_new_access_unit =
            has_slice &&
            (h264_is_access_unit_prefix(next_type) ||
             (next_is_slice && first_mb == 0U));
        if (starts_new_access_unit) {
            access_unit_end = next_start;
            break;
        }
        has_slice = has_slice || next_is_slice;
        has_idr = has_idr || next_type == 5;
    }

    if (access_unit_end <= access_unit_start) {
        return DEVICE_MEDIA_FILE_INVALID;
    }
    size_t access_unit_size = (size_t)(access_unit_end - access_unit_start);
    if (access_unit_size > capacity) {
        (void)fseek(source->file, access_unit_end, SEEK_SET);
        return DEVICE_MEDIA_FILE_BUFFER_TOO_SMALL;
    }
    clearerr(source->file);
    if (fseek(source->file, access_unit_start, SEEK_SET) != 0 ||
        fread(buffer, 1, access_unit_size, source->file) != access_unit_size ||
        fseek(source->file, access_unit_end, SEEK_SET) != 0) {
        return DEVICE_MEDIA_FILE_IO_ERROR;
    }

    *size = access_unit_size;
    *key_frame = has_idr;
    source->frame_index++;
    return DEVICE_MEDIA_FILE_OK;
}

device_media_file_result_t device_h264_file_open(device_h264_file_t *source,
                                                 const char *path)
{
    if (source == NULL || path == NULL || path[0] == '\0') {
        return DEVICE_MEDIA_FILE_INVALID;
    }
    memset(source, 0, sizeof(*source));
    source->file = fopen(path, "rb");
    if (source->file == NULL) {
        return DEVICE_MEDIA_FILE_IO_ERROR;
    }
    return DEVICE_MEDIA_FILE_OK;
}

device_media_file_result_t device_h264_file_next(device_h264_file_t *source,
                                                 uint8_t *buffer,
                                                 size_t capacity,
                                                 size_t *size,
                                                 bool *key_frame,
                                                 bool loop)
{
    if (size != NULL) {
        *size = 0;
    }
    if (key_frame != NULL) {
        *key_frame = false;
    }
    if (source == NULL || source->file == NULL || buffer == NULL ||
        size == NULL || key_frame == NULL || capacity == 0) {
        return DEVICE_MEDIA_FILE_INVALID;
    }

    device_media_file_result_t result = read_h264_access_unit(
        source, buffer, capacity, size, key_frame);
    if (result == DEVICE_MEDIA_FILE_EOF && loop) {
        clearerr(source->file);
        rewind(source->file);
        source->frame_index = 0;
        result = read_h264_access_unit(source, buffer, capacity, size, key_frame);
    }
    return result;
}

void device_h264_file_close(device_h264_file_t *source)
{
    if (source == NULL) {
        return;
    }
    if (source->file != NULL) {
        fclose(source->file);
    }
    memset(source, 0, sizeof(*source));
}

const char *device_media_file_result_name(device_media_file_result_t result)
{
    switch (result) {
    case DEVICE_MEDIA_FILE_OK: return "ok";
    case DEVICE_MEDIA_FILE_EOF: return "eof";
    case DEVICE_MEDIA_FILE_IO_ERROR: return "io-error";
    case DEVICE_MEDIA_FILE_INVALID: return "invalid";
    case DEVICE_MEDIA_FILE_BUFFER_TOO_SMALL: return "buffer-too-small";
    default: return "unknown";
    }
}
