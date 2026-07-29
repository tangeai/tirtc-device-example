/*
 * 测试用本地 token 签发。
 *
 * 这段代码实现公开的连接 token 签名算法，便于在没有业务服务端时快速验证。
 * 正式产品不要把 SecretKeyId 固化在固件里。
 */
#include "tirtc_token.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

#include "tirtc_config.h"

static const char *TAG = "tirtc_token";

#define TOKEN_NONCE_BYTES 16U
#define TOKEN_MAX_TTL_SECONDS 86400
#define TOKEN_PAYLOAD_JSON_MAX_LEN 512U
#define TOKEN_PAYLOAD_B64_MAX_LEN 768U
#define TOKEN_BASE64_WORK_MAX_LEN 768U
#define TOKEN_SIGNATURE_MAX_LEN 64U
#define TOKEN_DEVICE_ID_MAX_LEN 128U

static bool is_blank_or_placeholder(const char *value, const char *placeholder)
{
    return value == NULL || value[0] == '\0' || strcmp(value, placeholder) == 0;
}

static esp_err_t normalize_device_id(const char *remote_id, char *device_id, size_t device_id_size)
{
    const char *scheme = "device://";
    const size_t scheme_len = strlen(scheme);

    if (remote_id == NULL || device_id == NULL || device_id_size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const char *start = remote_id;
    while (isspace((unsigned char)*start))
    {
        start++;
    }

    const char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1]))
    {
        end--;
    }

    if ((size_t)(end - start) >= scheme_len && strncmp(start, scheme, scheme_len) == 0)
    {
        start += scheme_len;
        while (start < end && isspace((unsigned char)*start))
        {
            start++;
        }
    }
    else if (strstr(start, "://") != NULL)
    {
        ESP_LOGE(TAG, "remote_id 只能是设备 ID 或 device:// 形式");
        return ESP_ERR_INVALID_ARG;
    }

    if (start >= end)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t device_id_len = (size_t)(end - start);
    if (device_id_len >= device_id_size)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(device_id, start, device_id_len);
    device_id[device_id_len] = '\0';
    return ESP_OK;
}

static void fill_nonce(uint8_t nonce[TOKEN_NONCE_BYTES])
{
    for (size_t i = 0U; i < TOKEN_NONCE_BYTES; i += sizeof(uint32_t))
    {
        uint32_t random_value = esp_random();
        size_t copy_len = TOKEN_NONCE_BYTES - i;
        if (copy_len > sizeof(random_value))
        {
            copy_len = sizeof(random_value);
        }
        memcpy(nonce + i, &random_value, copy_len);
    }
}

static esp_err_t base64url_encode(const uint8_t *data, size_t data_len, char *out, size_t out_size)
{
    uint8_t base64[TOKEN_BASE64_WORK_MAX_LEN];
    size_t encoded_len = 0U;
    size_t write_index = 0U;

    if (data == NULL || out == NULL || out_size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (4U * ((data_len + 2U) / 3U) + 1U > sizeof(base64))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    if (mbedtls_base64_encode(base64, sizeof(base64) - 1U, &encoded_len, data, data_len) != 0)
    {
        return ESP_FAIL;
    }

    for (size_t read_index = 0U; read_index < encoded_len; ++read_index)
    {
        char current = (char)base64[read_index];

        if (current == '=')
        {
            continue;
        }
        if (current == '+')
        {
            current = '-';
        }
        else if (current == '/')
        {
            current = '_';
        }

        if (write_index + 1U >= out_size)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        out[write_index++] = current;
    }

    out[write_index] = '\0';
    return ESP_OK;
}

static esp_err_t hmac_sha256_base64url(const char *key,
                                       const char *message,
                                       char *out,
                                       size_t out_size)
{
    uint8_t digest[32];
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    if (key == NULL || message == NULL || out == NULL || out_size == 0U || md_info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (mbedtls_md_hmac(md_info,
                        (const uint8_t *)key,
                        strlen(key),
                        (const uint8_t *)message,
                        strlen(message),
                        digest) != 0)
    {
        return ESP_FAIL;
    }

    return base64url_encode(digest, sizeof(digest), out, out_size);
}

static esp_err_t current_unix_time(time_t *now)
{
    if (now == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    time(now);
    if (*now < 1600000000)
    {
        ESP_LOGE(TAG, "系统时间未同步，无法生成带 iat/exp 的 token");
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

esp_err_t tirtc_token_fetch_connect(const char *peer_id, char *out_token, size_t out_token_size)
{
    char device_id[TOKEN_DEVICE_ID_MAX_LEN];
    char nonce_b64[TOKEN_SIGNATURE_MAX_LEN];
    char payload_json[TOKEN_PAYLOAD_JSON_MAX_LEN];
    char payload_b64[TOKEN_PAYLOAD_B64_MAX_LEN];
    char app_sig[TOKEN_SIGNATURE_MAX_LEN];
    uint8_t nonce[TOKEN_NONCE_BYTES];
    time_t now;
    int written;

    if (peer_id == NULL || peer_id[0] == '\0' || out_token == NULL || out_token_size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (is_blank_or_placeholder(TIRTC_TOKEN_ACCESS_ID, "your_token_access_id") ||
        is_blank_or_placeholder(TIRTC_TOKEN_SECRET_KEY, "your_token_secret_key"))
    {
        ESP_LOGE(TAG, "请先配置 access_id 和 secret_key");
        return ESP_ERR_INVALID_STATE;
    }

    if (TIRTC_TOKEN_TTL_SECONDS <= 0 || TIRTC_TOKEN_TTL_SECONDS > TOKEN_MAX_TTL_SECONDS)
    {
        ESP_LOGE(TAG, "TIRTC_TOKEN_TTL_SECONDS 需要在 1 到 %d 秒之间", TOKEN_MAX_TTL_SECONDS);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = normalize_device_id(peer_id, device_id, sizeof(device_id));
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = current_unix_time(&now);
    if (ret != ESP_OK)
    {
        return ret;
    }

    fill_nonce(nonce);
    ret = base64url_encode(nonce, sizeof(nonce), nonce_b64, sizeof(nonce_b64));
    if (ret != ESP_OK)
    {
        return ret;
    }

    written = snprintf(payload_json,
                       sizeof(payload_json),
                       "{\"sub\":\"%s\",\"scope\":\"connect:device://%s\","
                       "\"iss\":\"%s\",\"iat\":%lld,\"exp\":%lld,\"nonce\":\"%s\"}",
                       TIRTC_TOKEN_SUBJECT,
                       device_id,
                       TIRTC_TOKEN_ACCESS_ID,
                       (long long)now,
                       (long long)(now + TIRTC_TOKEN_TTL_SECONDS),
                       nonce_b64);
    if (written < 0 || (size_t)written >= sizeof(payload_json))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    ret = base64url_encode((const uint8_t *)payload_json,
                           (size_t)written,
                           payload_b64,
                           sizeof(payload_b64));
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = hmac_sha256_base64url(TIRTC_TOKEN_SECRET_KEY, payload_b64, app_sig, sizeof(app_sig));
    if (ret != ESP_OK)
    {
        return ret;
    }

    written = snprintf(out_token, out_token_size, "v1.%s.%s", payload_b64, app_sig);
    if (written < 0 || (size_t)written >= out_token_size)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGW(TAG, "本地签发 token 只用于快速验证，生产环境请改由业务服务端签发");
    ESP_LOGI(TAG, "本地 token 已生成 remote_id=%s subject=%s ttl=%d",
             device_id,
             TIRTC_TOKEN_SUBJECT,
             TIRTC_TOKEN_TTL_SECONDS);
    return ESP_OK;
}
