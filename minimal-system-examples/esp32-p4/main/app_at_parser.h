#ifndef APP_AT_PARSER_H
#define APP_AT_PARSER_H

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_AT_MAX_FIELDS 4
#define APP_AT_FIELD_SIZE 129

typedef struct {
    size_t count;
    char values[APP_AT_MAX_FIELDS][APP_AT_FIELD_SIZE];
} app_at_fields_t;

/* Parses comma-separated AT arguments. Quoted fields support \", \\, \r,
 * \n and \t. Unquoted fields are trimmed. Empty quoted fields are allowed. */
esp_err_t app_at_parse_fields(const char *arguments,
                              size_t length,
                              app_at_fields_t *fields);

/* Escapes a UTF-8 payload for one quoted URC field. */
esp_err_t app_at_escape(const char *input, char *output, size_t output_size);

#ifdef __cplusplus
}
#endif

#endif
