#include "device/device_utf8.h"

#include <stdint.h>
#include <string.h>

static bool continuation(uint8_t byte)
{
    return (byte & 0xC0U) == 0x80U;
}

static bool decode_scalar(const uint8_t *text,
                          size_t *length,
                          uint32_t *scalar)
{
    uint8_t first = text[0];
    if (first < 0x80U) {
        *length = 1U;
        *scalar = first;
        return true;
    }
    if (first >= 0xC2U && first <= 0xDFU) {
        if (text[1] == 0U || !continuation(text[1])) {
            return false;
        }
        *length = 2U;
        *scalar = ((uint32_t)(first & 0x1FU) << 6U) |
                  (uint32_t)(text[1] & 0x3FU);
        return true;
    }
    if (first >= 0xE0U && first <= 0xEFU) {
        uint8_t second = text[1];
        uint8_t third = second == 0U ? 0U : text[2];
        if (second == 0U || third == 0U ||
            !continuation(second) || !continuation(third) ||
            (first == 0xE0U && second < 0xA0U) ||
            (first == 0xEDU && second >= 0xA0U)) {
            return false;
        }
        *length = 3U;
        *scalar = ((uint32_t)(first & 0x0FU) << 12U) |
                  ((uint32_t)(second & 0x3FU) << 6U) |
                  (uint32_t)(third & 0x3FU);
        return true;
    }
    if (first >= 0xF0U && first <= 0xF4U) {
        uint8_t second = text[1];
        uint8_t third = second == 0U ? 0U : text[2];
        uint8_t fourth = third == 0U ? 0U : text[3];
        if (second == 0U || third == 0U || fourth == 0U ||
            !continuation(second) || !continuation(third) ||
            !continuation(fourth) ||
            (first == 0xF0U && second < 0x90U) ||
            (first == 0xF4U && second > 0x8FU)) {
            return false;
        }
        *length = 4U;
        *scalar = ((uint32_t)(first & 0x07U) << 18U) |
                  ((uint32_t)(second & 0x3FU) << 12U) |
                  ((uint32_t)(third & 0x3FU) << 6U) |
                  (uint32_t)(fourth & 0x3FU);
        return true;
    }
    return false;
}

bool device_utf8_validate(const char *text)
{
    if (text == NULL) {
        return false;
    }
    const uint8_t *cursor = (const uint8_t *)text;
    while (*cursor != 0U) {
        size_t length = 0U;
        uint32_t scalar = 0U;
        if (!decode_scalar(cursor, &length, &scalar)) {
            return false;
        }
        cursor += length;
    }
    return true;
}

static bool line_control(uint32_t scalar)
{
    return scalar < 0x20U ||
           (scalar >= 0x7FU && scalar <= 0x9FU) ||
           scalar == 0x2028U ||
           scalar == 0x2029U;
}

size_t device_utf8_sanitize_line(const char *source,
                                 char *destination,
                                 size_t destination_size,
                                 size_t byte_limit)
{
    if (destination == NULL || destination_size == 0U) {
        return 0U;
    }
    destination[0] = '\0';
    if (source == NULL) {
        return 0U;
    }

    const uint8_t *cursor = (const uint8_t *)source;
    size_t output = 0U;
    bool truncated = false;
    size_t maximum = byte_limit < destination_size - 1U
                         ? byte_limit
                         : destination_size - 1U;
    while (*cursor != 0U) {
        size_t length = 0U;
        uint32_t scalar = 0U;
        bool valid = decode_scalar(cursor, &length, &scalar);
        size_t write_length = valid && !line_control(scalar) ? length : 1U;
        if (output + write_length > maximum) {
            truncated = true;
            break;
        }
        if (!valid) {
            destination[output++] = '?';
            cursor++;
        } else if (line_control(scalar)) {
            destination[output++] = ' ';
            cursor += length;
        } else {
            memcpy(destination + output, cursor, length);
            output += length;
            cursor += length;
        }
    }
    if (truncated && maximum >= 3U) {
        while (output + 3U > maximum && output > 0U) {
            output--;
            while (output > 0U &&
                   (((uint8_t)destination[output] & 0xC0U) == 0x80U)) {
                output--;
            }
        }
        memcpy(destination + output, "...", 3U);
        output += 3U;
    }
    destination[output] = '\0';
    return output;
}
