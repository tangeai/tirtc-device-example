#include "thing_http_client.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "thing_http";

#define THING_HTTP_DEFAULT_TIMEOUT_MS 10000U
#define THING_HTTP_BUFFER_SIZE       2048
#define THING_HTTP_TX_BUFFER_SIZE    2048
#define THING_HTTP_DEFAULT_RETRY_DELAY_MS 500U
#define THING_HTTP_RETRY_DELAY_STEP_MS    1000U

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int64_t start_us;
    int64_t connected_us;
    int64_t headers_sent_us;
    int64_t first_header_us;
    int64_t first_data_us;
    int64_t finish_us;
    int64_t disconnected_us;
    int data_events;
    int header_events;
    int disconnect_events;
    esp_err_t last_event_error;
    uint32_t retry_after_sec;
} thing_http_response_t;

static bool thing_http_is_https(const char *url)
{
    return url != NULL && strncmp(url, "https://", 8) == 0;
}

esp_err_t thing_http_join_url(char *out, size_t out_size, const char *base_url, const char *path)
{
    if (out == NULL || out_size == 0 || base_url == NULL || base_url[0] == '\0' ||
        path == NULL || path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    size_t base_len = strlen(base_url);
    while (base_len > 0 && base_url[base_len - 1] == '/') {
        base_len--;
    }

    int written = snprintf(out, out_size, "%.*s%s", (int)base_len, base_url, path);
    return written > 0 && written < (int)out_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static int64_t thing_http_elapsed_ms(const thing_http_response_t *response)
{
    if (response == NULL || response->start_us == 0) {
        return 0;
    }
    return (esp_timer_get_time() - response->start_us) / 1000;
}

static const char *thing_http_last_stage(const thing_http_response_t *response)
{
    if (response == NULL) {
        return "none";
    }
    if (response->finish_us != 0) {
        return "finish";
    }
    if (response->first_data_us != 0) {
        return "body";
    }
    if (response->first_header_us != 0) {
        return "header";
    }
    if (response->headers_sent_us != 0) {
        return "sent";
    }
    if (response->connected_us != 0) {
        return "connected";
    }
    return "init";
}

static esp_err_t thing_http_event_handler(esp_http_client_event_t *event)
{
    if (event == NULL) {
        return ESP_OK;
    }

    thing_http_response_t *response = (thing_http_response_t *)event->user_data;
    if (response == NULL) {
        return ESP_OK;
    }

    switch (event->event_id) {
    case HTTP_EVENT_ERROR:
        response->last_event_error = ESP_FAIL;
        ESP_LOGW(TAG, "event error: elapsed=%lldms stage=%s",
                 (long long)thing_http_elapsed_ms(response),
                 thing_http_last_stage(response));
        return ESP_OK;
    case HTTP_EVENT_ON_CONNECTED:
        response->connected_us = esp_timer_get_time();
        return ESP_OK;
    case HTTP_EVENT_HEADERS_SENT:
        response->headers_sent_us = esp_timer_get_time();
        return ESP_OK;
    case HTTP_EVENT_ON_HEADER:
        response->header_events++;
        if (response->first_header_us == 0) {
            response->first_header_us = esp_timer_get_time();
        }
        if (event->header_key != NULL && event->header_value != NULL &&
            strcasecmp(event->header_key, "Retry-After") == 0) {
            unsigned long retry_after = strtoul(event->header_value, NULL, 10);
            if (retry_after > 0UL && retry_after <= 3600UL) {
                response->retry_after_sec = (uint32_t)retry_after;
            }
        }
        return ESP_OK;
    case HTTP_EVENT_ON_DATA:
        if (event->data == NULL || event->data_len <= 0 || response->data == NULL) {
            return ESP_OK;
        }
        response->data_events++;
        if (response->first_data_us == 0) {
            response->first_data_us = esp_timer_get_time();
        }
        if (response->len + (size_t)event->data_len + 1 > response->cap) {
            response->last_event_error = ESP_ERR_NO_MEM;
            return ESP_ERR_NO_MEM;
        }
        memcpy(response->data + response->len, event->data, (size_t)event->data_len);
        response->len += (size_t)event->data_len;
        response->data[response->len] = '\0';
        return ESP_OK;
    case HTTP_EVENT_ON_FINISH:
        response->finish_us = esp_timer_get_time();
        return ESP_OK;
    case HTTP_EVENT_DISCONNECTED:
        response->disconnect_events++;
        response->disconnected_us = esp_timer_get_time();
        return ESP_OK;
    default:
        return ESP_OK;
    }
}

static esp_err_t thing_http_method_from_string(const char *method,
                                               esp_http_client_method_t *http_method)
{
    if (http_method == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (method == NULL || strcmp(method, "POST") == 0) {
        *http_method = HTTP_METHOD_POST;
        return ESP_OK;
    }
    if (strcmp(method, "GET") == 0) {
        *http_method = HTTP_METHOD_GET;
        return ESP_OK;
    }
    if (strcmp(method, "PUT") == 0) {
        *http_method = HTTP_METHOD_PUT;
        return ESP_OK;
    }
    if (strcmp(method, "DELETE") == 0) {
        *http_method = HTTP_METHOD_DELETE;
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static bool thing_http_method_has_json_body(esp_http_client_method_t method)
{
    return method == HTTP_METHOD_POST ||
           method == HTTP_METHOD_PUT ||
           method == HTTP_METHOD_DELETE;
}

bool thing_http_error_is_recoverable(esp_err_t ret)
{
    switch (ret) {
    case ESP_ERR_TIMEOUT:
    case ESP_ERR_HTTP_CONNECT:
    case ESP_ERR_HTTP_WRITE_DATA:
    case ESP_ERR_HTTP_FETCH_HEADER:
    case ESP_ERR_HTTP_CONNECTING:
    case ESP_ERR_HTTP_EAGAIN:
    case ESP_ERR_HTTP_CONNECTION_CLOSED:
    case ESP_ERR_HTTP_READ_TIMEOUT:
    case ESP_ERR_HTTP_INCOMPLETE_DATA:
        return true;
    default:
        return false;
    }
}

static bool thing_http_should_retry(const thing_http_request_t *request,
                                    esp_err_t ret,
                                    int status,
                                    const thing_http_response_t *response)
{
    if (request == NULL || request->retry_count == 0 || ret == ESP_OK || response == NULL) {
        return false;
    }

    /*
     * The transport layer cannot prove that a mutating request is idempotent.
     * In particular, signed Device Report/Token requests carry a one-time
     * nonce; replaying the same headers can turn a lost response into a nonce
     * replay error.  Their service owner retries the whole operation instead,
     * which regenerates the signature material.
     */
    if (request->method == NULL || strcmp(request->method, "GET") != 0) {
        return false;
    }
    return status == 0 &&
           response->len == 0 &&
           response->first_header_us == 0 &&
           response->last_event_error != ESP_ERR_NO_MEM;
}

esp_err_t thing_http_request_json(const thing_http_request_t *request,
                                  char *response_buf,
                                  size_t response_buf_size,
                                  int *status_code)
{
    return thing_http_request_json_ex(request,
                                      response_buf,
                                      response_buf_size,
                                      status_code,
                                      NULL);
}

esp_err_t thing_http_request_json_ex(const thing_http_request_t *request,
                                     char *response_buf,
                                     size_t response_buf_size,
                                     int *status_code,
                                     thing_http_response_info_t *response_info)
{
    if (request == NULL || request->url == NULL || request->url[0] == '\0' ||
        response_buf == NULL || response_buf_size < 2 ||
        (request->header_count > 0 && request->headers == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t timeout_ms = request->timeout_ms != 0 ? request->timeout_ms : THING_HTTP_DEFAULT_TIMEOUT_MS;
    uint8_t attempts = (uint8_t)(request->retry_count + 1U);
    if (attempts == 0) {
        attempts = 1;
    }
    esp_err_t ret = ESP_OK;
    int status = 0;
    esp_http_client_method_t http_method = HTTP_METHOD_POST;

    ret = thing_http_method_from_string(request->method, &http_method);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "unsupported HTTP method: %s",
                 request->method != NULL ? request->method : "(null)");
        return ret;
    }

    for (uint8_t attempt = 1; attempt <= attempts; ++attempt) {
        thing_http_response_t response = {
            .data = response_buf,
            .cap = response_buf_size,
            .start_us = esp_timer_get_time(),
        };
        response_buf[0] = '\0';
        if (status_code != NULL) {
            *status_code = 0;
        }
        if (response_info != NULL) {
            memset(response_info, 0, sizeof(*response_info));
        }

        esp_http_client_config_t config = {
            .url = request->url,
            .method = http_method,
            .event_handler = thing_http_event_handler,
            .user_data = &response,
            .timeout_ms = (int)timeout_ms,
            .buffer_size = THING_HTTP_BUFFER_SIZE,
            .buffer_size_tx = THING_HTTP_TX_BUFFER_SIZE,
            .crt_bundle_attach = thing_http_is_https(request->url) ? esp_crt_bundle_attach : NULL,
        };

        ESP_LOGD(TAG,
                 "request begin: trace=%s method=%s attempt=%u/%u timeout=%ums internal_free=%u largest=%u",
                 request->trace_name != NULL ? request->trace_name : "-",
                 request->method != NULL ? request->method : "POST",
                 (unsigned)attempt,
                 (unsigned)attempts,
                 (unsigned)timeout_ms,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == NULL) {
            ESP_LOGW(TAG, "request init failed: trace=%s", request->trace_name != NULL ? request->trace_name : "-");
            return ESP_ERR_NO_MEM;
        }

        esp_http_client_set_header(client, "Connection", "close");
        for (size_t index = 0; index < request->header_count; ++index) {
            const thing_http_header_t *header = &request->headers[index];
            if (header->name != NULL && header->value != NULL) {
                esp_http_client_set_header(client, header->name, header->value);
            }
        }
        if (thing_http_method_has_json_body(config.method)) {
            const char *body = request->body != NULL ? request->body : "";
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, body, (int)strlen(body));
        }

        ret = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        if (ret == ESP_OK && status_code != NULL) {
            *status_code = status;
        }
        if (response_info != NULL) {
            response_info->retry_after_sec = response.retry_after_sec;
        }
        int64_t elapsed_ms = thing_http_elapsed_ms(&response);
        ESP_LOGD(TAG,
                 "request done: trace=%s ret=%s status=%d elapsed=%lldms stage=%s bytes=%u hdr=%d data=%d disc=%d event_err=%s internal_free=%u largest=%u",
                 request->trace_name != NULL ? request->trace_name : "-",
                 esp_err_to_name(ret),
                 status,
                 (long long)elapsed_ms,
                 thing_http_last_stage(&response),
                 (unsigned)response.len,
                 response.header_events,
                 response.data_events,
                 response.disconnect_events,
                 esp_err_to_name(response.last_event_error),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        esp_http_client_cleanup(client);

        if (attempt < attempts && thing_http_should_retry(request, ret, status, &response)) {
            uint32_t delay_ms = request->retry_delay_ms != 0 ?
                                request->retry_delay_ms :
                                THING_HTTP_DEFAULT_RETRY_DELAY_MS;
            delay_ms += (uint32_t)(attempt - 1U) * THING_HTTP_RETRY_DELAY_STEP_MS;
            ESP_LOGW(TAG,
                     "request retry: trace=%s ret=%s stage=%s wait_ms=%u",
                     request->trace_name != NULL ? request->trace_name : "-",
                     esp_err_to_name(ret),
                     thing_http_last_stage(&response),
                     (unsigned)delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            continue;
        }
        break;
    }
    return ret;
}
