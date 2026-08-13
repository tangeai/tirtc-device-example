#include "app_at_parser.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool is_space(char value)
{
    return value == ' ' || value == '\t';
}

static esp_err_t append_char(char *field, size_t *size, char value)
{
    if (*size + 1U >= APP_AT_FIELD_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    field[(*size)++] = value;
    field[*size] = '\0';
    return ESP_OK;
}

static esp_err_t parse_quoted(const char *arguments,
                              size_t length,
                              size_t *position,
                              char *field)
{
    size_t size = 0;
    ++(*position);
    while (*position < length) {
        char value = arguments[(*position)++];
        if (value == '"') {
            while (*position < length && is_space(arguments[*position])) {
                ++(*position);
            }
            return ESP_OK;
        }
        if (value == '\\') {
            if (*position >= length) {
                return ESP_ERR_INVALID_ARG;
            }
            char escaped = arguments[(*position)++];
            switch (escaped) {
            case '"':
            case '\\':
                value = escaped;
                break;
            case 'r':
                value = '\r';
                break;
            case 'n':
                value = '\n';
                break;
            case 't':
                value = '\t';
                break;
            default:
                return ESP_ERR_INVALID_ARG;
            }
        }
        if ((uint8_t)value < 0x20U && value != '\r' &&
            value != '\n' && value != '\t') {
            return ESP_ERR_INVALID_ARG;
        }
        esp_err_t err = append_char(field, &size, value);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t parse_unquoted(const char *arguments,
                                size_t length,
                                size_t *position,
                                char *field)
{
    size_t size = 0;
    while (*position < length && arguments[*position] != ',') {
        char value = arguments[(*position)++];
        if (value == '"' || value == '\\' ||
            ((uint8_t)value < 0x20U && value != '\t')) {
            return ESP_ERR_INVALID_ARG;
        }
        esp_err_t err = append_char(field, &size, value);
        if (err != ESP_OK) {
            return err;
        }
    }
    while (size > 0 && is_space(field[size - 1U])) {
        field[--size] = '\0';
    }
    return size == 0 ? ESP_ERR_INVALID_ARG : ESP_OK;
}

esp_err_t app_at_parse_fields(const char *arguments,
                              size_t length,
                              app_at_fields_t *fields)
{
    if (arguments == NULL || fields == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(fields, 0, sizeof(*fields));

    size_t position = 0;
    while (position < length) {
        if (fields->count >= APP_AT_MAX_FIELDS) {
            return ESP_ERR_INVALID_SIZE;
        }
        while (position < length && is_space(arguments[position])) {
            ++position;
        }
        if (position >= length) {
            return ESP_ERR_INVALID_ARG;
        }

        char *field = fields->values[fields->count];
        esp_err_t err = arguments[position] == '"'
                            ? parse_quoted(arguments, length, &position, field)
                            : parse_unquoted(arguments, length, &position, field);
        if (err != ESP_OK) {
            return err;
        }
        ++fields->count;

        if (position == length) {
            break;
        }
        if (arguments[position] != ',') {
            return ESP_ERR_INVALID_ARG;
        }
        ++position;
        if (position == length) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return fields->count > 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t app_at_escape(const char *input, char *output, size_t output_size)
{
    if (input == NULL || output == NULL || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t used = 0;
    for (const unsigned char *cursor = (const unsigned char *)input;
         *cursor != '\0';
         ++cursor) {
        const char *escape = NULL;
        switch (*cursor) {
        case '"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\t':
            escape = "\\t";
            break;
        default:
            if (*cursor < 0x20U) {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        }
        size_t add = escape == NULL ? 1U : 2U;
        if (used + add >= output_size) {
            output[0] = '\0';
            return ESP_ERR_INVALID_SIZE;
        }
        if (escape == NULL) {
            output[used++] = (char)*cursor;
        } else {
            output[used++] = escape[0];
            output[used++] = escape[1];
        }
    }
    output[used] = '\0';
    return ESP_OK;
}
