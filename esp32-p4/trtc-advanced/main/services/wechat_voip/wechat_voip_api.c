#include "wechat_voip_api.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "device_auth_http.h"
#include "esp_check.h"
#include "esp_log.h"
#include "thing_http_client.h"
#include "wechat_voip_config.h"

static const char *TAG = "wx_voip_api";
static const char *ACTIVE_CALL_ROOM_TYPE = WECHAT_VOIP_LOCAL_VIDEO_ENABLE ? "video" : "voice";

enum {
    VOIP_HTTP_URL_MAX_LEN = 256,
    VOIP_HTTP_RESPONSE_MAX_LEN = 2048,
    VOIP_HTTP_BODY_MAX_LEN = 768,
    DEVICE_AUDIO_RATE = 8000,
    DEVICE_AUDIO_CHANNELS = 1,
    DEVICE_CALLING_TIMEOUT_SEC = 30,
};

static void copy_str(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strlcpy(dst, src, dst_size);
}

static bool response_code_ok(cJSON *root)
{
    cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    int code_value = cJSON_IsNumber(code) ? code->valueint : -1;
    return code_value == 0 || code_value == 200;
}

static const char *json_string_any(cJSON *root, const char *name1, const char *name2)
{
    if (root == NULL || name1 == NULL) {
        return NULL;
    }
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, name1));
    if ((value == NULL || value[0] == '\0') && name2 != NULL) {
        value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, name2));
    }
    return value;
}

static const char *json_string_any4(cJSON *root,
                                    const char *name1,
                                    const char *name2,
                                    const char *name3,
                                    const char *name4)
{
    const char *names[] = {name1, name2, name3, name4};
    if (root == NULL) {
        return NULL;
    }
    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, names[index]));
        if (value != NULL && value[0] != '\0') {
            return value;
        }
    }
    return NULL;
}

static esp_err_t voip_http_request(const char *api_base,
                                   const char *path,
                                   const char *method,
                                   const char *body,
                                   const char *mqtt_token,
                                   char *response,
                                   size_t response_size,
                                   int *status)
{
    char url[VOIP_HTTP_URL_MAX_LEN] = {0};
    ESP_RETURN_ON_ERROR(thing_http_join_url(url, sizeof(url), api_base, path),
                        TAG,
                        "voip url build failed");

    char authorization[DEVICE_AUTH_MQTT_TOKEN_MAX_LEN + 16] = {0};
    thing_http_header_t headers[1];
    size_t header_count = 0;
    if (mqtt_token != NULL && mqtt_token[0] != '\0') {
        snprintf(authorization, sizeof(authorization), "Bearer %s", mqtt_token);
        headers[0].name = "Authorization";
        headers[0].value = authorization;
        header_count = 1;
    }

    const thing_http_request_t request = {
        .url = url,
        .method = method,
        .body = body,
        .headers = headers,
        .header_count = header_count,
    };
    return thing_http_request_json(&request, response, response_size, status);
}

static esp_err_t parse_and_check_reply(const char *response, const char *operation)
{
    cJSON *root = cJSON_Parse(response);
    if (root == NULL) {
        ESP_LOGW(TAG, "%s response is not JSON: %.120s", operation, response != NULL ? response : "");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!response_code_ok(root)) {
        cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "msg");
        ESP_LOGW(TAG, "%s rejected: code=%d msg=%s",
                 operation,
                 cJSON_IsNumber(code) ? code->valueint : -1,
                 cJSON_GetStringValue(msg) != NULL ? cJSON_GetStringValue(msg) : "");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t wechat_voip_api_report_profile(const char *api_base, const char *mqtt_token)
{
    char response[VOIP_HTTP_RESPONSE_MAX_LEN] = {0};
    int status = 0;
    char body[192];
    const bool video_enabled = WECHAT_VOIP_LOCAL_VIDEO_ENABLE != 0;
    const unsigned video_width = video_enabled ? WECHAT_VOIP_VIDEO_WIDTH : 1U;
    const unsigned video_height = video_enabled ? WECHAT_VOIP_VIDEO_HEIGHT : 1U;
    const char *video_media = video_enabled ? WECHAT_VOIP_VIDEO_MEDIA : "";
    snprintf(body,
             sizeof(body),
             "{\"screen_width\":%u,\"screen_height\":%u,"
             "\"audio_rate\":%u,\"audio_channels\":%u,"
             "\"video_mt\":\"%s\",\"no_video\":%s,"
             "\"calling_timeout_sec\":%u}",
             video_width,
             video_height,
             DEVICE_AUDIO_RATE,
             DEVICE_AUDIO_CHANNELS,
             video_media,
             video_enabled ? "false" : "true",
             DEVICE_CALLING_TIMEOUT_SEC);

    ESP_LOGI(TAG,
             "report voip profile: video=%u codec=%s size=%ux%u audio=%uHz/%uch",
             video_enabled ? 1U : 0U,
             video_enabled ? video_media : "none",
             video_width,
             video_height,
             (unsigned)DEVICE_AUDIO_RATE,
             (unsigned)DEVICE_AUDIO_CHANNELS);
    esp_err_t ret = voip_http_request(api_base,
                                      "/v1/voip/device/profile",
                                      "POST",
                                      body,
                                      mqtt_token,
                                      response,
                                      sizeof(response),
                                      &status);
    if (ret != ESP_OK) {
        return ret;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "report profile HTTP status=%d body_len=%u",
                 status,
                 (unsigned)strlen(response));
        return ESP_FAIL;
    }
    return parse_and_check_reply(response, "report profile");
}

esp_err_t wechat_voip_api_fetch_callers(const char *api_base,
                                        const char *mqtt_token,
                                        wechat_voip_api_caller_cb_t caller_cb,
                                        void *ctx,
                                        int *caller_count)
{
    char response[VOIP_HTTP_RESPONSE_MAX_LEN] = {0};
    int status = 0;

    esp_err_t ret = voip_http_request(api_base,
                                      "/v1/voip/device/callers",
                                      "GET",
                                      NULL,
                                      mqtt_token,
                                      response,
                                      sizeof(response),
                                      &status);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "fetch callers failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "fetch callers HTTP status=%d body_len=%u",
                 status,
                 (unsigned)strlen(response));
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(response);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!response_code_ok(root)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *list = cJSON_IsObject(data) ? cJSON_GetObjectItemCaseSensitive(data, "list") : NULL;
    if (!cJSON_IsArray(list)) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "fetch callers response missing list");
        return ESP_ERR_INVALID_RESPONSE;
    }

    int count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, list) {
        wechat_voip_auth_user_t caller = {0};
        copy_str(caller.openid,
                 sizeof(caller.openid),
                 json_string_any4(item,
                                  "wx_open_id",
                                  "wxa_open_id",
                                  "wx_user_openid",
                                  "wxa_user_openid"));
        copy_str(caller.model_id, sizeof(caller.model_id), json_string_any(item, "wx_model_id", "wxa_model_id"));
        copy_str(caller.app_id, sizeof(caller.app_id), json_string_any(item, "wx_app_id", "wxa_app_id"));
        if (caller_cb != NULL) {
            caller_cb(&caller, ctx);
        }
        count++;
    }
    cJSON_Delete(root);
    if (caller_count != NULL) {
        *caller_count = count;
    }
    ESP_LOGI(TAG, "callers refreshed: count=%d", count);
    return ESP_OK;
}

esp_err_t wechat_voip_api_request_call(const char *api_base,
                                       const char *mqtt_token,
                                       const char *device_id,
                                       const wechat_voip_auth_user_t *target,
                                       int wx_version_type)
{
    if (device_id == NULL || device_id[0] == '\0' ||
        target == NULL || target->openid[0] == '\0' || target->model_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char body[VOIP_HTTP_BODY_MAX_LEN];
    const unsigned local_camera_status = WECHAT_VOIP_LOCAL_VIDEO_ENABLE ? 1U : 0U;
    int written = 0;
    if (target->app_id[0] != '\0') {
        written = snprintf(body,
                           sizeof(body),
                           "{\"device_id\":\"%s\","
                           "\"wx_app_id\":\"%s\","
                           "\"wx_user_openid\":\"%s\","
                           "\"wx_model_id\":\"%s\","
                           "\"wx_room_type\":\"%s\","
                           "\"wx_version_type\":%d,"
                           "\"calling_timeout_sec\":%u,"
                           "\"wx_caller_camera_status\":%u,"
                           "\"wx_listener_camera_status\":0}",
                           device_id,
                           target->app_id,
                           target->openid,
                           target->model_id,
                           ACTIVE_CALL_ROOM_TYPE,
                           wx_version_type,
                           (unsigned)DEVICE_CALLING_TIMEOUT_SEC,
                           local_camera_status);
    } else {
        written = snprintf(body,
                           sizeof(body),
                           "{\"device_id\":\"%s\","
                           "\"wx_user_openid\":\"%s\","
                           "\"wx_model_id\":\"%s\","
                           "\"wx_room_type\":\"%s\","
                           "\"wx_version_type\":%d,"
                           "\"calling_timeout_sec\":%u,"
                           "\"wx_caller_camera_status\":%u,"
                           "\"wx_listener_camera_status\":0}",
                           device_id,
                           target->openid,
                           target->model_id,
                           ACTIVE_CALL_ROOM_TYPE,
                           wx_version_type,
                           (unsigned)DEVICE_CALLING_TIMEOUT_SEC,
                           local_camera_status);
    }
    if (written <= 0 || written >= (int)sizeof(body)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char response[VOIP_HTTP_RESPONSE_MAX_LEN] = {0};
    int status = 0;
    ESP_LOGI(TAG,
             "request wechat call: room=%s local_camera=%u openid_len=%u model_id_len=%u version_type=%d calling_timeout=%us",
             ACTIVE_CALL_ROOM_TYPE,
             local_camera_status,
             (unsigned)strlen(target->openid),
             (unsigned)strlen(target->model_id),
             wx_version_type,
             (unsigned)DEVICE_CALLING_TIMEOUT_SEC);
    esp_err_t ret = voip_http_request(api_base,
                                      "/v1/voip/device/call",
                                      "POST",
                                      body,
                                      mqtt_token,
                                      response,
                                      sizeof(response),
                                      &status);
    if (ret != ESP_OK) {
        return ret;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "device call HTTP status=%d body_len=%u",
                 status,
                 (unsigned)strlen(response));
        return ESP_FAIL;
    }
    return parse_and_check_reply(response, "device call");
}

esp_err_t wechat_voip_api_report_auth(const char *api_base,
                                      const char *device_id,
                                      const char *open_id)
{
    if (device_id == NULL || device_id[0] == '\0' || open_id == NULL || open_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char body[VOIP_HTTP_BODY_MAX_LEN];
    int written = snprintf(body,
                           sizeof(body),
                           "{\"device_id\":\"%s\",\"wx_open_id\":\"%s\"}",
                           device_id,
                           open_id);
    if (written <= 0 || written >= (int)sizeof(body)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char response[VOIP_HTTP_RESPONSE_MAX_LEN] = {0};
    int status = 0;
    esp_err_t ret = voip_http_request(api_base,
                                      "/v1/voip/user/report-auth",
                                      "POST",
                                      body,
                                      NULL,
                                      response,
                                      sizeof(response),
                                      &status);
    if (ret != ESP_OK) {
        return ret;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "report auth HTTP status=%d body_len=%u",
                 status,
                 (unsigned)strlen(response));
        return ESP_FAIL;
    }
    return parse_and_check_reply(response, "report auth");
}

esp_err_t wechat_voip_api_delete_auth(const char *api_base,
                                      const char *device_id,
                                      const wechat_voip_auth_user_t *removed)
{
    if (device_id == NULL || device_id[0] == '\0' ||
        removed == NULL || removed->openid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char body[VOIP_HTTP_BODY_MAX_LEN];
    int written = 0;
    if (removed->app_id[0] != '\0') {
        written = snprintf(body,
                           sizeof(body),
                           "{\"device_id\":\"%s\",\"wx_open_id\":\"%s\",\"wx_app_id\":\"%s\"}",
                           device_id,
                           removed->openid,
                           removed->app_id);
    } else {
        written = snprintf(body,
                           sizeof(body),
                           "{\"device_id\":\"%s\",\"wx_open_id\":\"%s\"}",
                           device_id,
                           removed->openid);
    }
    if (written <= 0 || written >= (int)sizeof(body)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char response[VOIP_HTTP_RESPONSE_MAX_LEN] = {0};
    int status = 0;
    esp_err_t ret = voip_http_request(api_base,
                                      "/v1/voip/user/delete-auth",
                                      "POST",
                                      body,
                                      NULL,
                                      response,
                                      sizeof(response),
                                      &status);
    if (ret != ESP_OK) {
        return ret;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "delete auth HTTP status=%d body_len=%u",
                 status,
                 (unsigned)strlen(response));
        return ESP_FAIL;
    }
    return parse_and_check_reply(response, "delete auth");
}
