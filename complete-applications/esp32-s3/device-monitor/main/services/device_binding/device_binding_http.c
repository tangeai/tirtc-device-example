#include "device_binding_http.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "thing_http_client.h"

static const char *TAG = "binding_http";

#define DEVICE_BINDING_HTTP_TIMEOUT_MS 10000
#define DEVICE_BINDING_HTTP_RESPONSE_MAX_LEN 4096
#define DEVICE_BINDING_HTTP_URL_MAX_LEN 256
#define DEVICE_BINDING_HTTP_BODY_MAX_LEN 320
#define DEVICE_BINDING_HTTP_CODE_OK 200
#define DEVICE_BINDING_HTTP_CODE_VERIFY_PENDING 40901
#define DEVICE_BINDING_HTTP_CODE_RATE_LIMIT 429
#define DEVICE_BINDING_HTTP_DEFAULT_RETRY_AFTER_SEC 10U
#define DEVICE_BINDING_HTTP_NONCE_HEX_LEN 16
#define DEVICE_BINDING_HTTP_SIGNATURE_MAX 96

static bool device_binding_http_has_value(const char *value)
{
    return value != NULL && value[0] != '\0';
}

static void device_binding_http_make_nonce(char *nonce, size_t nonce_size)
{
    if (nonce == NULL || nonce_size == 0U) {
        return;
    }

    nonce[0] = '\0';
    uint8_t raw[8];
    esp_fill_random(raw, sizeof(raw));
    for (size_t index = 0; index < sizeof(raw) && (index * 2U + 2U) < nonce_size; ++index) {
        snprintf(nonce + index * 2U, 3, "%02x", raw[index]);
    }
}

static esp_err_t device_binding_http_sign(const char *device_id,
                                          const char *device_key,
                                          const char *timestamp,
                                          const char *nonce,
                                          char *signature,
                                          size_t signature_size)
{
    char raw[256];
    uint8_t digest[32];
    size_t encoded_len = 0;

    if (!device_binding_http_has_value(device_id) ||
        !device_binding_http_has_value(device_key) ||
        !device_binding_http_has_value(timestamp) ||
        !device_binding_http_has_value(nonce) ||
        signature == NULL || signature_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(raw, sizeof(raw), "%s%s%s", device_id, timestamp, nonce);
    if (written <= 0 || written >= (int)sizeof(raw)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == NULL) {
        return ESP_FAIL;
    }
    if (mbedtls_md_hmac(md_info,
                        (const uint8_t *)device_key,
                        strlen(device_key),
                        (const uint8_t *)raw,
                        strlen(raw),
                        digest) != 0) {
        return ESP_FAIL;
    }
    if (mbedtls_base64_encode((uint8_t *)signature,
                              signature_size - 1U,
                              &encoded_len,
                              digest,
                              sizeof(digest)) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    signature[encoded_len] = '\0';
    return ESP_OK;
}

static const cJSON *device_binding_pick_data_object(const cJSON *root)
{
    const cJSON *data = NULL;

    if (!cJSON_IsObject(root)) {
        return NULL;
    }
    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    return cJSON_IsObject(data) ? data : root;
}

static esp_err_t device_binding_parse_report_response(const char *json,
                                                      device_binding_http_report_result_t *result)
{
    cJSON *root = NULL;
    const cJSON *code = NULL;
    const cJSON *data = NULL;
    const cJSON *verify_code = NULL;
    const cJSON *temp_token = NULL;
    const cJSON *temp_client_id = NULL;

    if (json == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    data = device_binding_pick_data_object(root);
    if (data == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(result, 0, sizeof(*result));
    code = cJSON_GetObjectItemCaseSensitive(root, "code");
    result->service_code = cJSON_IsNumber(code) ? code->valueint : 0;

    verify_code = cJSON_GetObjectItemCaseSensitive(data, "code");
    temp_token = cJSON_GetObjectItemCaseSensitive(data, "temp_token");
    temp_client_id = cJSON_GetObjectItemCaseSensitive(data, "temp_client_id");
    if (!cJSON_IsString(verify_code) || verify_code->valuestring[0] == '\0' ||
        !cJSON_IsString(temp_token) || temp_token->valuestring[0] == '\0' ||
        !cJSON_IsString(temp_client_id) || temp_client_id->valuestring[0] == '\0') {
        if (result->service_code == DEVICE_BINDING_HTTP_CODE_VERIFY_PENDING ||
            result->service_code == DEVICE_BINDING_HTTP_CODE_RATE_LIMIT) {
            result->type = DEVICE_BINDING_HTTP_REPORT_RETRY_AFTER;
            cJSON_Delete(root);
            return ESP_OK;
        }
        if (result->service_code != 0 && result->service_code != DEVICE_BINDING_HTTP_CODE_OK) {
            ESP_LOGW(TAG, "binding report service error: code=%d", result->service_code);
            cJSON_Delete(root);
            return ESP_FAIL;
        }
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    result->type = DEVICE_BINDING_HTTP_REPORT_UNBOUND;
    strlcpy(result->code, verify_code->valuestring, sizeof(result->code));
    strlcpy(result->temp_token, temp_token->valuestring, sizeof(result->temp_token));
    strlcpy(result->temp_client_id, temp_client_id->valuestring, sizeof(result->temp_client_id));
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t device_binding_http_report(const char *api_base,
                                     const char *mac,
                                     const char *device_id,
                                     const char *device_key,
                                     device_binding_http_report_result_t *result)
{
    char url[DEVICE_BINDING_HTTP_URL_MAX_LEN] = {0};
    char body[DEVICE_BINDING_HTTP_BODY_MAX_LEN] = {0};
    char timestamp[24] = {0};
    char nonce[DEVICE_BINDING_HTTP_NONCE_HEX_LEN + 1] = {0};
    char signature[DEVICE_BINDING_HTTP_SIGNATURE_MAX] = {0};
    thing_http_header_t headers[4] = {0};
    size_t header_count = 0U;
    thing_http_response_info_t response_info = {0};
    char *response = NULL;
    int status_code = 0;
    esp_err_t ret = ESP_OK;
    bool has_device_id = device_binding_http_has_value(device_id);
    bool has_device_key = device_binding_http_has_value(device_key);

    if (api_base == NULL || api_base[0] == '\0' ||
        mac == NULL || mac[0] == '\0' ||
        result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (has_device_id != has_device_key) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = thing_http_join_url(url, sizeof(url), api_base, "/v1/device/report");
    if (ret != ESP_OK) {
        return ret;
    }
    int body_len = snprintf(body, sizeof(body), "{\"mac\":\"%s\"}", mac);
    if (body_len <= 0 || body_len >= (int)sizeof(body)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (has_device_id) {
        snprintf(timestamp, sizeof(timestamp), "%lld", (long long)time(NULL));
        device_binding_http_make_nonce(nonce, sizeof(nonce));
        ret = device_binding_http_sign(device_id,
                                       device_key,
                                       timestamp,
                                       nonce,
                                       signature,
                                       sizeof(signature));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "binding report signature failed: %s", esp_err_to_name(ret));
            return ret;
        }
        headers[0] = (thing_http_header_t){.name = "X-Device-Id", .value = device_id};
        headers[1] = (thing_http_header_t){.name = "X-Timestamp", .value = timestamp};
        headers[2] = (thing_http_header_t){.name = "X-Nonce", .value = nonce};
        headers[3] = (thing_http_header_t){.name = "X-Signature", .value = signature};
        header_count = sizeof(headers) / sizeof(headers[0]);
    }

    response = heap_caps_calloc(1,
                                DEVICE_BINDING_HTTP_RESPONSE_MAX_LEN,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (response == NULL) {
        response = calloc(1, DEVICE_BINDING_HTTP_RESPONSE_MAX_LEN);
    }
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGD(TAG,
             "binding report begin: signed=%d device_id_len=%u",
             has_device_id ? 1 : 0,
             has_device_id ? (unsigned)strlen(device_id) : 0U);
    const thing_http_request_t request = {
        .url = url,
        .method = "POST",
        .body = body,
        .headers = headers,
        .header_count = header_count,
        .timeout_ms = DEVICE_BINDING_HTTP_TIMEOUT_MS,
        .retry_count = 0,
        .trace_name = "binding-report",
    };
    ret = thing_http_request_json_ex(&request,
                                     response,
                                     DEVICE_BINDING_HTTP_RESPONSE_MAX_LEN,
                                     &status_code,
                                     &response_info);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "binding report request failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ret = device_binding_parse_report_response(response, result);
    if (ret == ESP_OK && result->type == DEVICE_BINDING_HTTP_REPORT_RETRY_AFTER) {
        result->retry_after_sec = response_info.retry_after_sec != 0U ?
                                  response_info.retry_after_sec :
                                  DEVICE_BINDING_HTTP_DEFAULT_RETRY_AFTER_SEC;
        ESP_LOGD(TAG,
                 "binding report pending: service_code=%d retry_after=%us",
                 result->service_code,
                 (unsigned)result->retry_after_sec);
        ret = ESP_OK;
        goto cleanup;
    }
    if (status_code != 200 && ret != ESP_OK) {
        ESP_LOGW(TAG, "binding report HTTP status=%d body_len=%u",
                 status_code,
                 (unsigned)strlen(response));
        goto cleanup;
    }
    if (status_code != 200) {
        ESP_LOGW(TAG, "binding report HTTP status=%d body_len=%u",
                 status_code,
                 (unsigned)strlen(response));
        ret = ESP_FAIL;
        goto cleanup;
    }
cleanup:
    free(response);
    return ret;
}
