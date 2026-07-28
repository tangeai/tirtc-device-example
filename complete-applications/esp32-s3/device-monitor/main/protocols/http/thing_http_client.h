#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    const char *value;
} thing_http_header_t;

typedef struct {
    const char *url;
    const char *method;
    const char *body;
    const thing_http_header_t *headers;
    size_t header_count;
    uint32_t timeout_ms;
    uint8_t retry_count;
    uint32_t retry_delay_ms;
    const char *trace_name;
} thing_http_request_t;

typedef struct {
    uint32_t retry_after_sec;
} thing_http_response_info_t;

esp_err_t thing_http_join_url(char *out, size_t out_size, const char *base_url, const char *path);
bool thing_http_error_is_recoverable(esp_err_t ret);
esp_err_t thing_http_request_json(const thing_http_request_t *request,
                                  char *response_buf,
                                  size_t response_buf_size,
                                  int *status_code);
esp_err_t thing_http_request_json_ex(const thing_http_request_t *request,
                                     char *response_buf,
                                     size_t response_buf_size,
                                     int *status_code,
                                     thing_http_response_info_t *response_info);

#ifdef __cplusplus
}
#endif
