#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <driver/systick.h>
#include <os/thread.h>
#include <spinlock.h>

#include "cJSON.h"
#include "driver/network/wireless.h"
#include "lwip/altcp_tls.h"
#include "lwip/apps/mqtt.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/netdb.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "mbedtls/base64.h"
#include "mbedtls/entropy.h"
#include "mbedtls/md.h"
#include "tirtc_demo_cloud.h"

#define TIRTC_DEMO_CLOUD_WORKER_STACK 24576
#define TIRTC_DEMO_CLOUD_POLL_MS 250U
#define TIRTC_DEMO_CLOUD_HTTP_TIMEOUT_MS 10000U
#define TIRTC_DEMO_CLOUD_HTTP_BODY_MAX 12288U
#define TIRTC_DEMO_CLOUD_MQTT_RX_MAX 4096U
#define TIRTC_DEMO_CLOUD_MQTT_QUEUE_DEPTH 4U
#define TIRTC_DEMO_CLOUD_MQTT_TOPIC_MAX 192U
#define TIRTC_DEMO_CLOUD_RETRY_MS 5000U
#define TIRTC_DEMO_CLOUD_BINDING_POLL_MS 10000U
#define TIRTC_DEMO_CLOUD_RATE_LIMIT_RETRY_MS 30000U
#define TIRTC_DEMO_CLOUD_BIND_RETRY_MAX_MS 60000U
#define TIRTC_DEMO_CLOUD_AUTH_ACK_TIMEOUT_MS 30000U
#define TIRTC_DEMO_CLOUD_HEARTBEAT_MS 30000U
#define TIRTC_DEMO_CLOUD_CALL_TIMEOUT_MS 30000U
#define TIRTC_DEMO_CLOUD_BINDING_TIMEOUT_MS 190000U
#define TIRTC_DEMO_CLOUD_VALID_TIME 1704067200L

#define TIRTC_DEMO_CLOUD_ERR_DNS (-2101)
#define TIRTC_DEMO_CLOUD_ERR_HTTP_CONNECT (-2102)
#define TIRTC_DEMO_CLOUD_ERR_HTTP_IO (-2103)
#define TIRTC_DEMO_CLOUD_ERR_HTTP_RESPONSE (-2104)

#define TIRTC_DEMO_CLOUD_WORK_START (1U << 0)
#define TIRTC_DEMO_CLOUD_WORK_BIND (1U << 1)
#define TIRTC_DEMO_CLOUD_WORK_CONTACTS (1U << 2)
#define TIRTC_DEMO_CLOUD_WORK_CONTACT_REQUEST (1U << 3)
#define TIRTC_DEMO_CLOUD_WORK_CALL (1U << 4)
#define TIRTC_DEMO_CLOUD_WORK_REJECT (1U << 5)
#define TIRTC_DEMO_CLOUD_WORK_HANGUP (1U << 6)
#define TIRTC_DEMO_CLOUD_WORK_MQTT_RX (1U << 7)
#define TIRTC_DEMO_CLOUD_WORK_RECONFIGURE (1U << 8)
#define TIRTC_DEMO_CLOUD_WORK_AUTH_COMPLETE (1U << 9)
#define TIRTC_DEMO_CLOUD_WORK_DEVICE_INFO (1U << 10)
#define TIRTC_DEMO_CLOUD_WORK_WECHAT_PROFILE (1U << 11)
#define TIRTC_DEMO_CLOUD_WORK_WECHAT_CONTACTS (1U << 12)
#define TIRTC_DEMO_CLOUD_WORK_WECHAT_CALL (1U << 13)
#define TIRTC_DEMO_CLOUD_WORK_WECHAT_ADD (1U << 14)
#define TIRTC_DEMO_CLOUD_WORK_WECHAT_DELETE (1U << 15)
#define TIRTC_DEMO_CLOUD_WORK_AI_TOKEN (1U << 16)
#define TIRTC_DEMO_CLOUD_WORK_BIND_POLL (1U << 17)

typedef enum {
    TIRTC_DEMO_CLOUD_MQTT_NONE = 0,
    TIRTC_DEMO_CLOUD_MQTT_TEMP,
    TIRTC_DEMO_CLOUD_MQTT_FORMAL,
} tirtc_demo_cloud_mqtt_mode_t;

typedef enum {
    TIRTC_DEMO_CLOUD_ROOM_CANCEL = 0,
    TIRTC_DEMO_CLOUD_ROOM_REJECT,
    TIRTC_DEMO_CLOUD_ROOM_HANGUP,
} tirtc_demo_cloud_room_action_t;

typedef struct {
    char topic[TIRTC_DEMO_CLOUD_MQTT_TOPIC_MAX];
    char payload[TIRTC_DEMO_CLOUD_MQTT_RX_MAX + 1U];
    size_t length;
} tirtc_demo_cloud_mqtt_message_t;

typedef struct {
    bool initialized;
    bool start_requested;
    bool mqtt_connecting;
    bool mqtt_connected;
    bool mqtt_rx_drop;
    bool contact_request_pending;
    bool call_request_pending;
    bool call_is_caller;
    bool device_info_pending;
    bool wechat_call_pending;
    bool wechat_outgoing_waiting;
    bool wechat_contact_op_pending;
    bool ai_token_pending;
    bool auth_grant_pending;
    bool auth_ack_inflight;
    bool binding_requesting;
    uint8_t binding_retry_count;
    uint32_t work;
    uint32_t heartbeat_seq;
    uint64_t next_retry_ms;
    uint64_t next_binding_retry_ms;
    uint64_t next_binding_poll_ms;
    uint64_t binding_retry_started_ms;
    uint64_t next_heartbeat_ms;
    uint64_t binding_deadline_ms;
    uint64_t call_deadline_ms;
    time_t token_auth_failure_wall;
    thread_ptr_t worker;
    tirtc_demo_cloud_callbacks_t callbacks;
    tirtc_demo_cloud_mqtt_mode_t mqtt_mode;
    tirtc_demo_cloud_mqtt_mode_t mqtt_wanted_mode;
    mqtt_client_t *mqtt_client;
    struct altcp_tls_config *mqtt_tls;
    struct mqtt_connect_client_info_t mqtt_info;
    char device_id[TIRTC_DEMO_CLOUD_DEVICE_ID_MAX];
    char device_secret[TIRTC_DEMO_CLOUD_DEVICE_SECRET_MAX];
    char mqtt_token[TIRTC_DEMO_CLOUD_TOKEN_MAX];
    char temp_client_id[96];
    char temp_token[TIRTC_DEMO_CLOUD_TOKEN_MAX];
    char mqtt_client_id[160];
    char mqtt_user[TIRTC_DEMO_CLOUD_DEVICE_ID_MAX];
    char mqtt_password[TIRTC_DEMO_CLOUD_TOKEN_MAX];
    char mqtt_cmd_topic[TIRTC_DEMO_CLOUD_MQTT_TOPIC_MAX];
    char mqtt_notify_topic[TIRTC_DEMO_CLOUD_MQTT_TOPIC_MAX];
    char mqtt_ack_topic[TIRTC_DEMO_CLOUD_MQTT_TOPIC_MAX];
    char mqtt_up_topic[TIRTC_DEMO_CLOUD_MQTT_TOPIC_MAX];
    char mqtt_rx_topic[TIRTC_DEMO_CLOUD_MQTT_TOPIC_MAX];
    char mqtt_rx_payload[TIRTC_DEMO_CLOUD_MQTT_RX_MAX + 1U];
    size_t mqtt_rx_expected;
    size_t mqtt_rx_length;
    tirtc_demo_cloud_mqtt_message_t
        mqtt_rx_queue[TIRTC_DEMO_CLOUD_MQTT_QUEUE_DEPTH];
    uint8_t mqtt_rx_read;
    uint8_t mqtt_rx_write;
    uint8_t mqtt_rx_count;
    char pending_contact_id[TIRTC_DEMO_CLOUD_DEVICE_ID_MAX];
    char pending_call_id[TIRTC_DEMO_CLOUD_DEVICE_ID_MAX];
    char pending_wechat_open_id[TIRTC_DEMO_CLOUD_WX_OPENID_MAX];
    char pending_wechat_app_id[TIRTC_DEMO_CLOUD_WX_APP_ID_MAX];
    char pending_wechat_model_id[TIRTC_DEMO_CLOUD_WX_MODEL_ID_MAX];
    tirtc_demo_cloud_snapshot_t snapshot;
} tirtc_demo_cloud_runtime_t;

static DEFINE_SPINLOCK(g_tirtc_demo_cloud_lock);

static tirtc_demo_cloud_runtime_t g_cloud = {
    .snapshot = {
        .stage = "待启动",
        .message = "设备业务服务尚未启动。",
        .business_code = -1,
    },
};

static void cloud_clear_call_locked(const char *message);

/* Let’s Encrypt ISRG Root YR, SHA-256
 * E5:7B:7E:6F:15:0C:41:91:02:E8:D5:C0:55:72:9F:F9:
 * 67:B9:D1:A8:29:BF:00:CE:C8:9C:A6:04:EB:F4:A8:6F. */
static const unsigned char g_tirtc_demo_root_yr[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIFKTCCAxGgAwIBAgIRAOxGNJNgz0sP+KmC2Tqpyj0wDQYJKoZIhvcNAQELBQAw\n"
    "LjELMAkGA1UEBhMCVVMxDTALBgNVBAoTBElTUkcxEDAOBgNVBAMTB1Jvb3QgWVIw\n"
    "HhcNMjUwOTAzMDAwMDAwWhcNNDUwOTAyMjM1OTU5WjAuMQswCQYDVQQGEwJVUzEN\n"
    "MAsGA1UEChMESVNSRzEQMA4GA1UEAxMHUm9vdCBZUjCCAiIwDQYJKoZIhvcNAQEB\n"
    "BQADggIPADCCAgoCggIBANvGJnN78CTJdWL3+eGfsLN5TrNBJs+VH9hRXqRbwxu9\n"
    "sGNiB0BD1fcOxbSUQCJIM1xE13Db+5Cw1w0s0EBYsvuIP/6joF0w8cuImbgR1OGg\n"
    "YbSQ4OpzI+DG8SGuTlcE873OCS+kh3srlo6vl43M5OJg4Aeo1sfHp6kTJDoIiFBN\n"
    "JAY+OKfX/FUvYKuhjT+no49lmqmupSBI5PkBQiqrEGtWU5uxU/cQWHGu8jSjFBzn\n"
    "ZqvbNPLMXMLFxCb3WTfrJBXXjqvWG+v4bjzxjjeAtOlU7qarRDvNOyAuQYLln904\n"
    "M+faKx8hnLCpJ15ZqaEgcNlY+9MMWcC5yvL2A2j3l9+2buggZX+dOE91zYmIdawT\n"
    "vSZuVvlbRrAlLxIB6pwMBjneXCjYQ8+3BCCjssbSNpZU3hTcBDdhfAlEDlYr6pEa\n"
    "tnMdmDT5BqnKC92bd0EhM1fbLHioLccLCuievT8ZkPhZrq7Mii7gNXAcUEAR8+lz\n"
    "Yal+9zTg7C5DALyVOeG/CqfRAMn1KSHCR0NSA6P8tn/mGRlnCct5rtVCLnVySVpU\n"
    "6H1qGg3DgTOuskf8eahTMiYbI5ezPJmO5ertalskQ1utp74+eDy92PI4ftHKTbq9\n"
    "IWhH4YZKh3WnJEIt+oQvlYZbY8tpEroKrFB6PFGzrJIDRyts4HqvuH52RFj2zv/B\n"
    "AgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8EBTADAQH/MB0GA1Ud\n"
    "DgQWBBTe51tg0CJtQCh9Pw0B/qS1UrRRlDANBgkqhkiG9w0BAQsFAAOCAgEAWHnf\n"
    "713Bdkq7t5yN2dNIgQakUb94X9WuyhMEHHkgx4oDpSUlnG0w4g94MoqaEUE31ZjR\n"
    "LU7L5LD1g9ujFHTQu8AD215AHMVQFbm6j8hQxdXHAzDajFNQnOlDJrLjzIx176oy\n"
    "AjvUtejZx2NNmdb5fd0WGVGsCdoAJ3N8ozo7ajE8t6vfxStZb4BQ9WYJGHUDrv2N\n"
    "i5tJF6CNiPnlzs3BUfECRbE4JSk+jvy8+VoGiFE8qsH/j78x2fjgQhAQFV7P7Zxy\n"
    "dBTZ1wEkNpZNW2qnaK1SKBLa+xf6E06YRIq5uaI+HWH8SY1y5VbRgzq40EKg3yxP\n"
    "06fz+uYAUIFJoLNfhwRCc3Q6pQVuMX3yAjHAes4gk4moGcLQ5p7HAh39yeylZc1J\n"
    "41sx/jKwLIkPE6Rr1Nf4pxdsxf9SA4yOEiAkDgq04DVxn8hgYFdUtBCuiuVC2heA\n"
    "EiqVEa+8QZjuw8Gj0EbHXcRd1nInvGqRS1o9Is7YBdQN57X1AYveGBNNqjICSb7c\n"
    "awuw1EawTDrs13VUlJVEsbQ0/O/1aaV73mCdOQ8azqL2KTv1Ewu1xbquE2S+kdQU\n"
    "To9TUwat3wUA6cwXh1EfpS/3fJ0aGah5hdpRyoCLDlsSn8tkrjMfFFX0viC+GxHc\n"
    "sI1ANRYvqSFC2X1VRZfDg+wD6E21BccmifG4yWc=\n"
    "-----END CERTIFICATE-----\n";

static void cloud_lock(unsigned long *flags)
{
    spin_lock_irqsave(&g_tirtc_demo_cloud_lock, *flags);
}

static void cloud_unlock(unsigned long flags)
{
    spin_unlock_irqrestore(&g_tirtc_demo_cloud_lock, flags);
}

static uint64_t cloud_now_ms(void)
{
    return systick_get_time_ms();
}

static void cloud_copy(char *dest, size_t dest_size, const char *source)
{
    if (dest == NULL || dest_size == 0U) {
        return;
    }
    snprintf(dest, dest_size, "%s", source != NULL ? source : "");
}

static bool cloud_identity_valid(const char *device_id, const char *device_secret)
{
    return device_id != NULL && device_secret != NULL &&
           device_id[0] != '\0' && device_secret[0] != '\0' &&
           strlen(device_id) < TIRTC_DEMO_CLOUD_DEVICE_ID_MAX &&
           strlen(device_secret) < TIRTC_DEMO_CLOUD_DEVICE_SECRET_MAX;
}

static bool cloud_target_valid(const char *device_id)
{
    size_t length;

    if (device_id == NULL) {
        return false;
    }
    length = strlen(device_id);
    if (length == 0U || length >= TIRTC_DEMO_CLOUD_DEVICE_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        unsigned char value = (unsigned char)device_id[i];
        if (!isalnum(value) && value != '-' && value != '_' &&
            value != '.' && value != ':') {
            return false;
        }
    }
    return true;
}

static bool cloud_json_value_valid(const char *value, size_t max_length,
                                   bool allow_empty)
{
    size_t length;

    if (value == NULL) {
        return allow_empty;
    }
    length = strlen(value);
    if ((!allow_empty && length == 0U) || length >= max_length) {
        return false;
    }
    for (size_t i = 0U; i < length; ++i) {
        unsigned char ch = (unsigned char)value[i];
        if (ch < 0x20U || ch == '"' || ch == '\\') {
            return false;
        }
    }
    return true;
}

static void cloud_set_status_locked(int error, int http_status, int business_code,
                                    const char *stage, const char *message)
{
    g_cloud.snapshot.last_error = error;
    g_cloud.snapshot.http_status = http_status;
    g_cloud.snapshot.business_code = business_code;
    cloud_copy(g_cloud.snapshot.stage, sizeof(g_cloud.snapshot.stage), stage);
    cloud_copy(g_cloud.snapshot.message, sizeof(g_cloud.snapshot.message), message);
    ++g_cloud.snapshot.status_generation;
    if (g_cloud.snapshot.status_generation == 0U) {
        g_cloud.snapshot.status_generation = 1U;
    }
}

static void cloud_set_status(int error, int http_status, int business_code,
                             const char *stage, const char *message)
{
    unsigned long flags;

    cloud_lock(&flags);
    cloud_set_status_locked(error, http_status, business_code, stage, message);
    cloud_unlock(flags);
}

static void cloud_schedule(uint32_t work)
{
    thread_ptr_t worker;
    unsigned long flags;

    cloud_lock(&flags);
    g_cloud.work |= work;
    worker = g_cloud.worker;
    cloud_unlock(flags);
    if (worker != NULL) {
        thread_wakeup(worker);
    }
}

static uint32_t cloud_take_work(void)
{
    uint32_t work;
    unsigned long flags;

    cloud_lock(&flags);
    work = g_cloud.work;
    g_cloud.work = 0U;
    cloud_unlock(flags);
    return work;
}

static bool cloud_network_ready(void)
{
    return wifi_status_get() == JZ_WIFI_STATUS_AVAILABLE;
}

static bool cloud_time_ready(void)
{
    return time(NULL) >= TIRTC_DEMO_CLOUD_VALID_TIME;
}

static int cloud_get_mac(char *output, size_t output_size)
{
    unsigned char mac[6] = {0};
    bool valid = false;

    if (output == NULL || output_size < 18U) {
        return -1;
    }

    LOCK_TCPIP_CORE();
    if (netif_default != NULL && netif_default->hwaddr_len >= sizeof(mac)) {
        memcpy(mac, netif_default->hwaddr, sizeof(mac));
        valid = true;
    }
    UNLOCK_TCPIP_CORE();

    if (!valid) {
        return -1;
    }
    snprintf(output, output_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

int tirtc_demo_cloud_get_physical_client_id(char *output, size_t output_size)
{
    char mac[18];
    int written;

    if (output == NULL || output_size < 13U || cloud_get_mac(mac, sizeof(mac)) != 0) {
        return -1;
    }
    written = snprintf(output, output_size, "%c%c%c%c%c%c%c%c%c%c%c%c",
                       mac[0], mac[1], mac[3], mac[4], mac[6], mac[7],
                       mac[9], mac[10], mac[12], mac[13], mac[15], mac[16]);
    return written == 12 ? 0 : -1;
}

static int cloud_random_nonce(char *output, size_t output_size)
{
    mbedtls_entropy_context entropy;
    unsigned char random[8];
    int result;

    if (output == NULL || output_size < 17U) {
        return -1;
    }
    mbedtls_entropy_init(&entropy);
    result = mbedtls_entropy_func(&entropy, random, sizeof(random));
    mbedtls_entropy_free(&entropy);
    if (result != 0) {
        return result;
    }
    for (size_t i = 0; i < sizeof(random); ++i) {
        snprintf(output + (i * 2U), output_size - (i * 2U), "%02x", random[i]);
    }
    output[16] = '\0';
    memset(random, 0, sizeof(random));
    return 0;
}

static int cloud_hmac_signature(const char *device_id, const char *device_secret,
                                const char *timestamp, const char *nonce,
                                char *output, size_t output_size)
{
    const mbedtls_md_info_t *info;
    unsigned char digest[32];
    char source[TIRTC_DEMO_CLOUD_DEVICE_ID_MAX + 64];
    size_t encoded = 0U;
    int written;
    int result;

    written = snprintf(source, sizeof(source), "%s%s%s", device_id, timestamp, nonce);
    if (written <= 0 || (size_t)written >= sizeof(source)) {
        return -1;
    }
    info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL) {
        return -1;
    }
    result = mbedtls_md_hmac(info,
                             (const unsigned char *)device_secret,
                             strlen(device_secret),
                             (const unsigned char *)source,
                             strlen(source), digest);
    memset(source, 0, sizeof(source));
    if (result != 0) {
        return result;
    }
    result = mbedtls_base64_encode((unsigned char *)output, output_size - 1U,
                                   &encoded, digest, sizeof(digest));
    memset(digest, 0, sizeof(digest));
    if (result != 0 || encoded >= output_size) {
        return result != 0 ? result : -1;
    }
    output[encoded] = '\0';
    return 0;
}

static int cloud_signed_headers(const char *device_id, const char *device_secret,
                                const char *mac, char *headers, size_t headers_size)
{
    char timestamp[32];
    char nonce[17];
    char signature[64];
    int written;

    if (!cloud_time_ready()) {
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    snprintf(timestamp, sizeof(timestamp), "%ld", (long)time(NULL));
    if (cloud_random_nonce(nonce, sizeof(nonce)) != 0 ||
        cloud_hmac_signature(device_id, device_secret, timestamp, nonce,
                             signature, sizeof(signature)) != 0) {
        return -1;
    }
    written = snprintf(headers, headers_size,
                       "X-Device-Id: %s\r\n"
                       "X-Timestamp: %s\r\n"
                       "X-Nonce: %s\r\n"
                       "X-Signature: %s\r\n"
                       "X-MAC: %s\r\n",
                       device_id, timestamp, nonce, signature,
                       mac != NULL ? mac : "");
    memset(signature, 0, sizeof(signature));
    return written > 0 && (size_t)written < headers_size ? 0 : -1;
}

static int cloud_socket_send_all(int socket_fd, const char *data, size_t length)
{
    size_t sent = 0U;

    while (sent < length) {
        int result = send(socket_fd, data + sent, length - sent, 0);
        if (result <= 0) {
            return -1;
        }
        sent += (size_t)result;
    }
    return 0;
}

static int cloud_decode_chunked(const char *input, size_t input_length,
                                char *output, size_t output_size)
{
    const char *cursor = input;
    const char *end = input + input_length;
    size_t total = 0U;

    while (cursor < end) {
        char *chunk_end = NULL;
        unsigned long chunk_size = strtoul(cursor, &chunk_end, 16);
        if (chunk_end == cursor || chunk_end + 2 > end ||
            chunk_end[0] != '\r' || chunk_end[1] != '\n') {
            return -1;
        }
        cursor = chunk_end + 2;
        if (chunk_size == 0UL) {
            break;
        }
        if ((size_t)(end - cursor) < chunk_size + 2U ||
            total + chunk_size >= output_size) {
            return -1;
        }
        memcpy(output + total, cursor, chunk_size);
        total += chunk_size;
        cursor += chunk_size;
        if (cursor[0] != '\r' || cursor[1] != '\n') {
            return -1;
        }
        cursor += 2;
    }
    output[total] = '\0';
    return (int)total;
}

static int cloud_http_json(const char *method, const char *path, const char *body,
                           const char *extra_headers, char *response,
                           size_t response_size, int *http_status)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct timeval timeout;
    char port[8];
    char *request = NULL;
    char *raw = NULL;
    char *body_start;
    char dns_server[IPADDR_STRLEN_MAX] = "unknown";
    size_t body_length = body != NULL ? strlen(body) : 0U;
    size_t extra_length = extra_headers != NULL ? strlen(extra_headers) : 0U;
    size_t request_size = body_length + extra_length + 768U;
    size_t raw_size = response_size + 2048U;
    size_t received = 0U;
    int socket_fd = -1;
    int result = TIRTC_DEMO_CLOUD_ERR_HTTP_RESPONSE;
    int dns_result;
    int written;

    if (method == NULL || path == NULL || response == NULL || response_size < 2U) {
        return -1;
    }
    response[0] = '\0';
    if (http_status != NULL) {
        *http_status = 0;
    }
    (void)ipaddr_ntoa_r(dns_getserver(0), dns_server, sizeof(dns_server));
    printf("[tirtc_cloud] http begin method=%s host=%s port=%u path=%s "
           "body_len=%u dns=%s\n",
           method, TIRTC_DEMO_CLOUD_API_HOST,
           (unsigned int)TIRTC_DEMO_CLOUD_API_PORT, path,
           (unsigned int)body_length, dns_server);

    request = malloc(request_size);
    raw = malloc(raw_size);
    if (request == NULL || raw == NULL) {
        printf("[tirtc_cloud] http allocation failed request=%u raw=%u\n",
               (unsigned int)request_size, (unsigned int)raw_size);
        goto cleanup;
    }
    written = snprintf(request, request_size,
                       "%s %s HTTP/1.1\r\n"
                       "Host: %s\r\n"
                       "Accept: application/json\r\n"
                       "Content-Type: application/json\r\n"
                       "Connection: close\r\n"
                       "Content-Length: %u\r\n"
                       "%s\r\n%s",
                       method, path, TIRTC_DEMO_CLOUD_API_HOST,
                       (unsigned int)body_length,
                       extra_headers != NULL ? extra_headers : "",
                       body != NULL ? body : "");
    if (written <= 0 || (size_t)written >= request_size) {
        goto cleanup;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof(port), "%u", (unsigned int)TIRTC_DEMO_CLOUD_API_PORT);
    dns_result = getaddrinfo(TIRTC_DEMO_CLOUD_API_HOST, port, &hints,
                             &addresses);
    if (dns_result != 0 || addresses == NULL) {
        result = TIRTC_DEMO_CLOUD_ERR_DNS;
        printf("[tirtc_cloud] http dns failed host=%s result=%d dns=%s\n",
               TIRTC_DEMO_CLOUD_API_HOST, dns_result, dns_server);
        goto cleanup;
    }
    socket_fd = socket(addresses->ai_family, addresses->ai_socktype,
                       addresses->ai_protocol);
    if (socket_fd < 0) {
        result = TIRTC_DEMO_CLOUD_ERR_HTTP_CONNECT;
        printf("[tirtc_cloud] http socket create failed\n");
        goto cleanup;
    }
    timeout.tv_sec = TIRTC_DEMO_CLOUD_HTTP_TIMEOUT_MS / 1000U;
    timeout.tv_usec = (TIRTC_DEMO_CLOUD_HTTP_TIMEOUT_MS % 1000U) * 1000U;
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (connect(socket_fd, addresses->ai_addr, addresses->ai_addrlen) != 0) {
        result = TIRTC_DEMO_CLOUD_ERR_HTTP_CONNECT;
        printf("[tirtc_cloud] http connect failed\n");
        goto cleanup;
    }
    if (cloud_socket_send_all(socket_fd, request, (size_t)written) != 0) {
        result = TIRTC_DEMO_CLOUD_ERR_HTTP_IO;
        printf("[tirtc_cloud] http send failed\n");
        goto cleanup;
    }

    while (received < raw_size - 1U) {
        int count = recv(socket_fd, raw + received, raw_size - 1U - received, 0);
        if (count == 0) {
            break;
        }
        if (count < 0) {
            if (received == 0U) {
                result = TIRTC_DEMO_CLOUD_ERR_HTTP_IO;
                printf("[tirtc_cloud] http receive failed\n");
                goto cleanup;
            }
            break;
        }
        received += (size_t)count;
    }
    if (received == raw_size - 1U) {
        result = TIRTC_DEMO_CLOUD_ERR_HTTP_RESPONSE;
        printf("[tirtc_cloud] http response overflow bytes=%u\n",
               (unsigned int)received);
        goto cleanup;
    }
    raw[received] = '\0';
    if (sscanf(raw, "HTTP/%*u.%*u %d", &result) != 1) {
        result = TIRTC_DEMO_CLOUD_ERR_HTTP_RESPONSE;
        printf("[tirtc_cloud] http status parse failed bytes=%u\n",
               (unsigned int)received);
        goto cleanup;
    }
    if (http_status != NULL) {
        *http_status = result;
    }
    body_start = strstr(raw, "\r\n\r\n");
    if (body_start == NULL) {
        result = TIRTC_DEMO_CLOUD_ERR_HTTP_RESPONSE;
        printf("[tirtc_cloud] http header terminator missing status=%d\n",
               http_status != NULL ? *http_status : 0);
        goto cleanup;
    }
    body_start += 4;
    if (strstr(raw, "Transfer-Encoding: chunked") != NULL ||
        strstr(raw, "transfer-encoding: chunked") != NULL) {
        result = cloud_decode_chunked(body_start,
                                      received - (size_t)(body_start - raw),
                                      response, response_size);
        if (result < 0) {
            result = TIRTC_DEMO_CLOUD_ERR_HTTP_RESPONSE;
            printf("[tirtc_cloud] http chunk decode failed status=%d\n",
                   http_status != NULL ? *http_status : 0);
            goto cleanup;
        }
    } else {
        size_t copy_length = received - (size_t)(body_start - raw);
        if (copy_length >= response_size) {
            result = TIRTC_DEMO_CLOUD_ERR_HTTP_RESPONSE;
            printf("[tirtc_cloud] http body overflow bytes=%u limit=%u\n",
                   (unsigned int)copy_length, (unsigned int)response_size);
            goto cleanup;
        }
        memcpy(response, body_start, copy_length);
        response[copy_length] = '\0';
    }
    printf("[tirtc_cloud] http done status=%d body_len=%u\n",
           http_status != NULL ? *http_status : 0,
           (unsigned int)strlen(response));
    result = 0;

cleanup:
    if (socket_fd >= 0) {
        close(socket_fd);
    }
    if (addresses != NULL) {
        freeaddrinfo(addresses);
    }
    if (request != NULL) {
        memset(request, 0, request_size);
        free(request);
    }
    if (raw != NULL) {
        memset(raw, 0, raw_size);
        free(raw);
    }
    return result;
}

static int cloud_parse_business(const char *response, cJSON **root_out,
                                cJSON **data_out, int *business_code)
{
    cJSON *root;
    cJSON *code;
    int code_value;

    if (root_out != NULL) {
        *root_out = NULL;
    }
    if (data_out != NULL) {
        *data_out = NULL;
    }
    if (business_code != NULL) {
        *business_code = -1;
    }
    root = cJSON_Parse(response);
    if (root == NULL) {
        return -1;
    }
    code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsNumber(code)) {
        cJSON_Delete(root);
        return -1;
    }
    code_value = code->valueint;
    if (business_code != NULL) {
        *business_code = code_value;
    }
    if (data_out != NULL) {
        *data_out = cJSON_GetObjectItemCaseSensitive(root, "data");
    }
    if (root_out != NULL) {
        *root_out = root;
    } else {
        cJSON_Delete(root);
    }
    return code_value == 200 || code_value == 0 ? 0 : -1;
}

static const char *cloud_json_string(const cJSON *object, const char *name)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(value) && value->valuestring != NULL ?
               value->valuestring : "";
}

static const char *cloud_json_string_alias(const cJSON *object,
                                           const char *first,
                                           const char *second,
                                           const char *third)
{
    const char *value = cloud_json_string(object, first);

    if (value[0] == '\0' && second != NULL) {
        value = cloud_json_string(object, second);
    }
    if (value[0] == '\0' && third != NULL) {
        value = cloud_json_string(object, third);
    }
    return value;
}

static const char *cloud_json_string_alias4(const cJSON *object,
                                            const char *first,
                                            const char *second,
                                            const char *third,
                                            const char *fourth)
{
    const char *value = cloud_json_string_alias(object, first, second, third);

    if (value[0] == '\0' && fourth != NULL) {
        value = cloud_json_string(object, fourth);
    }
    return value;
}

static const char *cloud_json_string_alias5(const cJSON *object,
                                            const char *first,
                                            const char *second,
                                            const char *third,
                                            const char *fourth,
                                            const char *fifth)
{
    const char *value = cloud_json_string_alias4(
        object, first, second, third, fourth);

    if (value[0] == '\0' && fifth != NULL) {
        value = cloud_json_string(object, fifth);
    }
    return value;
}

static void cloud_query_value(const char *uri, const char *key,
                              char *output, size_t output_size)
{
    const char *query;
    size_t key_length;

    if (uri == NULL || key == NULL || output == NULL || output_size == 0U) {
        return;
    }
    output[0] = '\0';
    key_length = strlen(key);
    query = strchr(uri, '?');
    if (query == NULL || key_length == 0U) {
        return;
    }
    ++query;
    while (*query != '\0') {
        const char *value;
        const char *end;
        size_t length;

        if (strncmp(query, key, key_length) == 0 && query[key_length] == '=') {
            value = query + key_length + 1U;
            end = strchr(value, '&');
            if (end == NULL) {
                end = value + strlen(value);
            }
            length = (size_t)(end - value);
            if (length >= output_size) {
                return;
            }
            memcpy(output, value, length);
            output[length] = '\0';
            return;
        }
        query = strchr(query, '&');
        if (query == NULL) {
            return;
        }
        ++query;
    }
}

static bool cloud_wechat_join_payload(const cJSON *payload)
{
    const char *peer_id;

    if (!cJSON_IsObject(payload)) {
        return false;
    }
    if (cloud_json_string_alias(
            payload, "wx_room_id", "wxa_room_id", NULL)[0] != '\0' ||
        cloud_json_string_alias(
            payload, "wx_model_id", "wxa_model_id", NULL)[0] != '\0' ||
        cloud_json_string_alias4(payload, "wx_user_openid", "wxa_user_openid",
                                 "wx_open_id", "wxa_open_id")[0] != '\0') {
        return true;
    }
    peer_id = cloud_json_string(payload, "peer_id");
    return strstr(peer_id, "x_wx_") != NULL ||
           strstr(peer_id, "x_wxa_") != NULL;
}

static void cloud_emit_session(const tirtc_demo_cloud_session_t *session)
{
    tirtc_demo_cloud_session_ready_fn callback;
    void *context;
    unsigned long flags;

    if (session == NULL) {
        return;
    }
    cloud_lock(&flags);
    callback = g_cloud.callbacks.session_ready;
    context = g_cloud.callbacks.context;
    cloud_unlock(flags);
    if (callback != NULL) {
        callback(session, context);
    }
}

static int cloud_bearer_headers(char *headers, size_t headers_size)
{
    char token[TIRTC_DEMO_CLOUD_TOKEN_MAX];
    unsigned long flags;
    int written;

    cloud_lock(&flags);
    cloud_copy(token, sizeof(token), g_cloud.mqtt_token);
    cloud_unlock(flags);
    if (token[0] == '\0') {
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    written = snprintf(headers, headers_size, "Authorization: Bearer %s\r\n", token);
    memset(token, 0, sizeof(token));
    return written > 0 && (size_t)written < headers_size ? 0 : -1;
}

static int cloud_exchange_token_now(void)
{
    char device_id[TIRTC_DEMO_CLOUD_DEVICE_ID_MAX];
    char device_secret[TIRTC_DEMO_CLOUD_DEVICE_SECRET_MAX];
    char mac[18];
    char headers[768];
    char *response = NULL;
    cJSON *root = NULL;
    cJSON *data = NULL;
    const char *token;
    unsigned long flags;
    int http_status = 0;
    int business_code = -1;
    int result;

    cloud_lock(&flags);
    cloud_copy(device_id, sizeof(device_id), g_cloud.device_id);
    cloud_copy(device_secret, sizeof(device_secret), g_cloud.device_secret);
    cloud_unlock(flags);
    if (!cloud_identity_valid(device_id, device_secret)) {
        result = TIRTC_DEMO_CLOUD_ERR_NOT_READY;
        goto cleanup;
    }
    if (!cloud_network_ready() || !cloud_time_ready() ||
        cloud_get_mac(mac, sizeof(mac)) != 0) {
        result = TIRTC_DEMO_CLOUD_ERR_NOT_READY;
        goto cleanup;
    }
    result = cloud_signed_headers(device_id, device_secret, mac,
                                  headers, sizeof(headers));
    if (result != 0) {
        goto cleanup;
    }
    response = malloc(TIRTC_DEMO_CLOUD_HTTP_BODY_MAX);
    if (response == NULL) {
        result = -1;
        goto cleanup;
    }
    cloud_set_status(0, 0, -1, "设备认证", "正在获取设备业务令牌。" );
    result = cloud_http_json("POST", "/v1/device/token", NULL, headers,
                             response, TIRTC_DEMO_CLOUD_HTTP_BODY_MAX,
                             &http_status);
    if (result == 0) {
        result = cloud_parse_business(response, &root, &data, &business_code);
    }
    if (result != 0 || http_status < 200 || http_status >= 300) {
        if (http_status == 401) {
            cloud_lock(&flags);
            g_cloud.token_auth_failure_wall = time(NULL);
            cloud_unlock(flags);
        }
        result = business_code == 6006 ? -6006 : -1;
        cloud_set_status(result, http_status, business_code, "设备认证",
                         business_code == 6006 ?
                             "设备已解绑，正在进入重新绑定流程。" :
                             "设备业务令牌获取失败，将自动重试。" );
        goto cleanup;
    }
    token = cloud_json_string(data, "mqtt_token");
    if (token[0] == '\0' || strlen(token) >= TIRTC_DEMO_CLOUD_TOKEN_MAX) {
        result = -1;
        cloud_set_status(result, http_status, business_code, "设备认证",
                         "服务端令牌响应格式不正确。" );
        goto cleanup;
    }

    cloud_lock(&flags);
    cloud_copy(g_cloud.mqtt_token, sizeof(g_cloud.mqtt_token), token);
    g_cloud.snapshot.token_ready = true;
    g_cloud.next_retry_ms = 0U;
    g_cloud.token_auth_failure_wall = 0;
    cloud_set_status_locked(0, http_status, business_code, "设备认证",
                            "设备业务令牌已就绪，正在连接消息服务。" );
    cloud_unlock(flags);
    result = 0;

cleanup:
    if (root != NULL) {
        cJSON_Delete(root);
    }
    if (response != NULL) {
        memset(response, 0, TIRTC_DEMO_CLOUD_HTTP_BODY_MAX);
        free(response);
    }
    memset(device_secret, 0, sizeof(device_secret));
    memset(headers, 0, sizeof(headers));
    return result;
}

static void cloud_mqtt_request_callback(void *argument, err_t error)
{
    const char *operation = argument != NULL ?
                                (const char *)argument : "unknown";

    printf("[tirtc_cloud] mqtt request operation=%s result=%d\n",
           operation, (int)error);
    if (error != ERR_OK) {
        cloud_set_status((int)error, 0, -1, "消息服务",
                         "MQTT 请求发送失败，将由连接状态机恢复。" );
    }
}

static int cloud_mqtt_publish(const char *topic, const char *payload, uint8_t qos)
{
    mqtt_client_t *client;
    bool connected;
    unsigned long flags;
    err_t error;

    cloud_lock(&flags);
    client = g_cloud.mqtt_client;
    connected = g_cloud.mqtt_connected;
    cloud_unlock(flags);
    if (client == NULL || !connected || topic == NULL || payload == NULL ||
        strlen(payload) > UINT16_MAX) {
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    LOCK_TCPIP_CORE();
    error = mqtt_publish(client, topic, payload, (u16_t)strlen(payload), qos, 0,
                         cloud_mqtt_request_callback, (void *)"publish");
    UNLOCK_TCPIP_CORE();
    return error == ERR_OK ? 0 : (int)error;
}

static void cloud_auth_ack_callback(void *argument, err_t error)
{
    thread_ptr_t worker = NULL;
    bool completed = false;
    unsigned long flags;

    (void)argument;
    cloud_lock(&flags);
    g_cloud.auth_ack_inflight = false;
    if (error == ERR_OK && g_cloud.auth_grant_pending) {
        g_cloud.auth_grant_pending = false;
        g_cloud.work |= TIRTC_DEMO_CLOUD_WORK_AUTH_COMPLETE;
        worker = g_cloud.worker;
        completed = true;
        cloud_set_status_locked(0, 0, 200, "设备绑定",
                                "绑定确认已送达，正在切换正式服务。" );
    } else if (error != ERR_OK && g_cloud.auth_grant_pending) {
        g_cloud.next_retry_ms = cloud_now_ms() + TIRTC_DEMO_CLOUD_RETRY_MS;
        cloud_set_status_locked((int)error, 0, -1, "设备绑定",
                                "绑定凭据已保存，确认消息将自动重试。" );
    }
    cloud_unlock(flags);
    printf("[tirtc_cloud] binding ack puback result=%d complete=%d\n",
           (int)error, completed ? 1 : 0);
    if (worker != NULL) {
        thread_wakeup(worker);
    }
}

static int cloud_publish_auth_ack(void)
{
    mqtt_client_t *client;
    char topic[TIRTC_DEMO_CLOUD_MQTT_TOPIC_MAX];
    bool connected;
    tirtc_demo_cloud_mqtt_mode_t mode;
    unsigned long flags;
    err_t error;
    static const char payload[] = "{\"ack\":true}";

    cloud_lock(&flags);
    if (!g_cloud.auth_grant_pending) {
        cloud_unlock(flags);
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    if (g_cloud.auth_ack_inflight) {
        cloud_unlock(flags);
        return TIRTC_DEMO_CLOUD_ERR_BUSY;
    }
    client = g_cloud.mqtt_client;
    connected = g_cloud.mqtt_connected;
    mode = g_cloud.mqtt_mode;
    cloud_copy(topic, sizeof(topic), g_cloud.mqtt_ack_topic);
    if (client == NULL || !connected || mode != TIRTC_DEMO_CLOUD_MQTT_TEMP ||
        topic[0] == '\0') {
        cloud_unlock(flags);
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    g_cloud.auth_ack_inflight = true;
    cloud_unlock(flags);

    LOCK_TCPIP_CORE();
    error = mqtt_publish(client, topic, payload, (u16_t)(sizeof(payload) - 1U),
                         1, 0, cloud_auth_ack_callback, NULL);
    UNLOCK_TCPIP_CORE();
    if (error != ERR_OK) {
        cloud_lock(&flags);
        g_cloud.auth_ack_inflight = false;
        g_cloud.next_retry_ms = cloud_now_ms() + TIRTC_DEMO_CLOUD_RETRY_MS;
        cloud_unlock(flags);
        return (int)error;
    }
    printf("[tirtc_cloud] binding ack submitted qos=1\n");
    return 0;
}

static void cloud_mqtt_incoming_publish(void *argument, const char *topic,
                                        u32_t total_length)
{
    unsigned long flags;

    (void)argument;
    cloud_lock(&flags);
    g_cloud.mqtt_rx_length = 0U;
    g_cloud.mqtt_rx_expected = (size_t)total_length;
    g_cloud.mqtt_rx_drop =
                           !g_cloud.mqtt_connected ||
                           g_cloud.mqtt_rx_count >=
                               TIRTC_DEMO_CLOUD_MQTT_QUEUE_DEPTH ||
                           total_length > TIRTC_DEMO_CLOUD_MQTT_RX_MAX;
    cloud_copy(g_cloud.mqtt_rx_topic, sizeof(g_cloud.mqtt_rx_topic), topic);
    cloud_unlock(flags);
}

static void cloud_mqtt_incoming_data(void *argument, const u8_t *data,
                                     u16_t length, u8_t flags_value)
{
    thread_ptr_t worker = NULL;
    unsigned long flags;

    (void)argument;
    cloud_lock(&flags);
    if (!g_cloud.mqtt_rx_drop && data != NULL &&
        g_cloud.mqtt_rx_length + length <= TIRTC_DEMO_CLOUD_MQTT_RX_MAX) {
        memcpy(g_cloud.mqtt_rx_payload + g_cloud.mqtt_rx_length, data, length);
        g_cloud.mqtt_rx_length += length;
    } else if (length > 0U) {
        g_cloud.mqtt_rx_drop = true;
    }
    if ((flags_value & MQTT_DATA_FLAG_LAST) != 0U) {
        if (!g_cloud.mqtt_rx_drop &&
            g_cloud.mqtt_rx_length == g_cloud.mqtt_rx_expected) {
            tirtc_demo_cloud_mqtt_message_t *message =
                &g_cloud.mqtt_rx_queue[g_cloud.mqtt_rx_write];

            g_cloud.mqtt_rx_payload[g_cloud.mqtt_rx_length] = '\0';
            cloud_copy(message->topic, sizeof(message->topic),
                       g_cloud.mqtt_rx_topic);
            memcpy(message->payload, g_cloud.mqtt_rx_payload,
                   g_cloud.mqtt_rx_length + 1U);
            message->length = g_cloud.mqtt_rx_length;
            g_cloud.mqtt_rx_write = (uint8_t)(
                (g_cloud.mqtt_rx_write + 1U) %
                TIRTC_DEMO_CLOUD_MQTT_QUEUE_DEPTH);
            ++g_cloud.mqtt_rx_count;
            g_cloud.work |= TIRTC_DEMO_CLOUD_WORK_MQTT_RX;
            worker = g_cloud.worker;
        }
        g_cloud.mqtt_rx_drop = false;
    }
    cloud_unlock(flags);
    if (worker != NULL) {
        thread_wakeup(worker);
    }
}

static const char *cloud_mqtt_status_name(mqtt_connection_status_t status)
{
    switch (status) {
    case MQTT_CONNECT_ACCEPTED:
        return "accepted";
    case MQTT_CONNECT_REFUSED_PROTOCOL_VERSION:
        return "refused-protocol";
    case MQTT_CONNECT_REFUSED_IDENTIFIER:
        return "refused-client-id";
    case MQTT_CONNECT_REFUSED_SERVER:
        return "refused-server";
    case MQTT_CONNECT_REFUSED_USERNAME_PASS:
        return "refused-credentials";
    case MQTT_CONNECT_REFUSED_NOT_AUTHORIZED_:
        return "refused-not-authorized";
    case MQTT_CONNECT_DISCONNECTED:
        return "transport-disconnected";
    case MQTT_CONNECT_TIMEOUT:
        return "timeout";
    default:
        return "unknown";
    }
}

static void cloud_mqtt_connection_callback(mqtt_client_t *client, void *argument,
                                           mqtt_connection_status_t status)
{
    tirtc_demo_cloud_mqtt_mode_t mode;
    char cmd_topic[TIRTC_DEMO_CLOUD_MQTT_TOPIC_MAX];
    char notify_topic[TIRTC_DEMO_CLOUD_MQTT_TOPIC_MAX];
    unsigned long flags;

    (void)argument;
    cloud_lock(&flags);
    mode = g_cloud.mqtt_mode;
    cloud_copy(cmd_topic, sizeof(cmd_topic), g_cloud.mqtt_cmd_topic);
    cloud_copy(notify_topic, sizeof(notify_topic), g_cloud.mqtt_notify_topic);
    g_cloud.mqtt_connecting = false;
    g_cloud.mqtt_connected = status == MQTT_CONNECT_ACCEPTED;
    g_cloud.snapshot.mqtt_connected = g_cloud.mqtt_connected;
    if (g_cloud.mqtt_connected) {
        g_cloud.next_retry_ms = 0U;
        g_cloud.next_heartbeat_ms = cloud_now_ms() + TIRTC_DEMO_CLOUD_HEARTBEAT_MS;
        cloud_set_status_locked(0, 0, 200,
                                mode == TIRTC_DEMO_CLOUD_MQTT_TEMP ?
                                    "设备绑定" : "设备在线",
                                mode == TIRTC_DEMO_CLOUD_MQTT_TEMP ?
                                    "验证码已生成，正在等待用户完成绑定。" :
                                    "设备消息服务已上线。" );
    } else {
        if (mode == TIRTC_DEMO_CLOUD_MQTT_TEMP) {
            g_cloud.auth_ack_inflight = false;
        }
        g_cloud.next_retry_ms = cloud_now_ms() + TIRTC_DEMO_CLOUD_RETRY_MS;
        cloud_set_status_locked((int)status, 0, -1, "消息服务",
                                "MQTT 连接断开，将自动重连。" );
    }
    cloud_unlock(flags);

    printf("[tirtc_cloud] mqtt connection mode=%d status=%d name=%s "
           "connected=%d\n", (int)mode, (int)status,
           cloud_mqtt_status_name(status),
           status == MQTT_CONNECT_ACCEPTED ? 1 : 0);

    if (status == MQTT_CONNECT_ACCEPTED) {
        if (mode == TIRTC_DEMO_CLOUD_MQTT_TEMP) {
            printf("[tirtc_cloud] binding phase=temp-mqtt-online "
                   "next=subscribe-cmd\n");
        }
        err_t subscribe_result = mqtt_subscribe(client, cmd_topic, 1,
                                                cloud_mqtt_request_callback,
                                                (void *)"subscribe-cmd");

        printf("[tirtc_cloud] mqtt subscribe mode=%d topic=cmd result=%d\n",
               (int)mode, (int)subscribe_result);
        if (mode == TIRTC_DEMO_CLOUD_MQTT_FORMAL) {
            subscribe_result = mqtt_subscribe(client, notify_topic, 1,
                                              cloud_mqtt_request_callback,
                                              (void *)"subscribe-notify");
            printf("[tirtc_cloud] mqtt subscribe mode=%d topic=notify result=%d\n",
                   (int)mode, (int)subscribe_result);
            cloud_schedule(TIRTC_DEMO_CLOUD_WORK_CONTACTS |
                           TIRTC_DEMO_CLOUD_WORK_WECHAT_PROFILE |
                           TIRTC_DEMO_CLOUD_WORK_WECHAT_CONTACTS);
        }
    }
}

static int cloud_resolve_mqtt(ip_addr_t *address)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct sockaddr_in *ipv4;
    int error;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    error = getaddrinfo(TIRTC_DEMO_CLOUD_MQTT_HOST, NULL, &hints, &result);
    if (error != 0 || result == NULL) {
        char dns_server[IPADDR_STRLEN_MAX] = "unknown";

        (void)ipaddr_ntoa_r(dns_getserver(0), dns_server,
                            sizeof(dns_server));
        printf("[tirtc_cloud] mqtt dns failed host=%s result=%d dns=%s\n",
               TIRTC_DEMO_CLOUD_MQTT_HOST, error, dns_server);
        return -1;
    }
    ipv4 = (struct sockaddr_in *)result->ai_addr;
    ip_addr_set_ip4_u32(address, ipv4->sin_addr.s_addr);
    freeaddrinfo(result);
    printf("[tirtc_cloud] mqtt dns resolved host=%s\n",
           TIRTC_DEMO_CLOUD_MQTT_HOST);
    return 0;
}

static void cloud_mqtt_disconnect(void)
{
    mqtt_client_t *client;
    bool connected;
    unsigned long flags;

    cloud_lock(&flags);
    client = g_cloud.mqtt_client;
    connected = g_cloud.mqtt_connected || g_cloud.mqtt_connecting;
    g_cloud.mqtt_connected = false;
    g_cloud.mqtt_connecting = false;
    g_cloud.snapshot.mqtt_connected = false;
    g_cloud.mqtt_mode = TIRTC_DEMO_CLOUD_MQTT_NONE;
    g_cloud.mqtt_rx_drop = false;
    g_cloud.mqtt_rx_expected = 0U;
    g_cloud.mqtt_rx_length = 0U;
    g_cloud.mqtt_rx_read = 0U;
    g_cloud.mqtt_rx_write = 0U;
    g_cloud.mqtt_rx_count = 0U;
    memset(g_cloud.mqtt_rx_queue, 0, sizeof(g_cloud.mqtt_rx_queue));
    g_cloud.work &= ~TIRTC_DEMO_CLOUD_WORK_MQTT_RX;
    cloud_unlock(flags);
    if (client != NULL && connected) {
        LOCK_TCPIP_CORE();
        mqtt_disconnect(client);
        UNLOCK_TCPIP_CORE();
    }
}

static int cloud_mqtt_connect(tirtc_demo_cloud_mqtt_mode_t mode)
{
    ip_addr_t address;
    mqtt_client_t *client;
    struct altcp_tls_config *tls;
    char user[TIRTC_DEMO_CLOUD_DEVICE_ID_MAX];
    char password[TIRTC_DEMO_CLOUD_TOKEN_MAX];
    char client_id[160];
    char cmd_topic[TIRTC_DEMO_CLOUD_MQTT_TOPIC_MAX];
    char notify_topic[TIRTC_DEMO_CLOUD_MQTT_TOPIC_MAX];
    char ack_topic[TIRTC_DEMO_CLOUD_MQTT_TOPIC_MAX];
    char up_topic[TIRTC_DEMO_CLOUD_MQTT_TOPIC_MAX];
    unsigned long flags;
    err_t error;

    if (!cloud_network_ready()) {
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    if (cloud_resolve_mqtt(&address) != 0) {
        cloud_lock(&flags);
        g_cloud.next_retry_ms = cloud_now_ms() + TIRTC_DEMO_CLOUD_RETRY_MS;
        cloud_set_status_locked(TIRTC_DEMO_CLOUD_ERR_DNS, 0, -1,
                                "消息服务",
                                "MQTT 域名解析失败，5 秒后自动重试。");
        cloud_unlock(flags);
        return TIRTC_DEMO_CLOUD_ERR_DNS;
    }

    cloud_lock(&flags);
    if (g_cloud.mqtt_connected || g_cloud.mqtt_connecting) {
        cloud_unlock(flags);
        return TIRTC_DEMO_CLOUD_ERR_BUSY;
    }
    if (mode == TIRTC_DEMO_CLOUD_MQTT_TEMP) {
        cloud_copy(user, sizeof(user), g_cloud.temp_client_id);
        cloud_copy(password, sizeof(password), g_cloud.temp_token);
        cloud_copy(client_id, sizeof(client_id), g_cloud.temp_client_id);
        snprintf(cmd_topic, sizeof(cmd_topic), "device/%s/cmd", user);
        notify_topic[0] = '\0';
        snprintf(ack_topic, sizeof(ack_topic), "device/%s/ack", user);
        up_topic[0] = '\0';
    } else {
        cloud_copy(user, sizeof(user), g_cloud.device_id);
        cloud_copy(password, sizeof(password), g_cloud.mqtt_token);
        snprintf(client_id, sizeof(client_id), "sn_%s", user);
        snprintf(cmd_topic, sizeof(cmd_topic), "device/sn_%s/cmd", user);
        snprintf(notify_topic, sizeof(notify_topic), "device/sn_%s/notify", user);
        snprintf(ack_topic, sizeof(ack_topic), "device/sn_%s/ack", user);
        snprintf(up_topic, sizeof(up_topic), "device/sn_%s/up", user);
    }
    tls = g_cloud.mqtt_tls;
    client = g_cloud.mqtt_client;
    cloud_unlock(flags);

    if (user[0] == '\0' || password[0] == '\0') {
        memset(password, 0, sizeof(password));
        printf("[tirtc_cloud] mqtt connect blocked mode=%d credentials=missing\n",
               (int)mode);
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    if (tls == NULL) {
        LOCK_TCPIP_CORE();
        tls = altcp_tls_create_config_client(g_tirtc_demo_root_yr,
                                             sizeof(g_tirtc_demo_root_yr));
        UNLOCK_TCPIP_CORE();
        if (tls == NULL) {
            memset(password, 0, sizeof(password));
            printf("[tirtc_cloud] mqtt tls config allocation failed\n");
            return -1;
        }
        {
            struct altcp_tls_config *unused_tls = NULL;

            cloud_lock(&flags);
            if (g_cloud.mqtt_tls == NULL) {
                g_cloud.mqtt_tls = tls;
            } else {
                unused_tls = tls;
                tls = g_cloud.mqtt_tls;
            }
            cloud_unlock(flags);
            if (unused_tls != NULL) {
                LOCK_TCPIP_CORE();
                altcp_tls_free_config(unused_tls);
                UNLOCK_TCPIP_CORE();
            }
        }
    }
    if (client == NULL) {
        mqtt_client_t *new_client;

        LOCK_TCPIP_CORE();
        new_client = mqtt_client_new();
        UNLOCK_TCPIP_CORE();
        if (new_client == NULL) {
            memset(password, 0, sizeof(password));
            printf("[tirtc_cloud] mqtt client allocation failed\n");
            return -1;
        }
        cloud_lock(&flags);
        if (g_cloud.mqtt_client == NULL) {
            g_cloud.mqtt_client = new_client;
        }
        client = g_cloud.mqtt_client;
        cloud_unlock(flags);
        if (client != new_client) {
            LOCK_TCPIP_CORE();
            mqtt_client_free(new_client);
            UNLOCK_TCPIP_CORE();
        }
    }

    cloud_lock(&flags);
    cloud_copy(g_cloud.mqtt_client_id, sizeof(g_cloud.mqtt_client_id), client_id);
    cloud_copy(g_cloud.mqtt_user, sizeof(g_cloud.mqtt_user), user);
    cloud_copy(g_cloud.mqtt_password, sizeof(g_cloud.mqtt_password), password);
    cloud_copy(g_cloud.mqtt_cmd_topic, sizeof(g_cloud.mqtt_cmd_topic), cmd_topic);
    cloud_copy(g_cloud.mqtt_notify_topic, sizeof(g_cloud.mqtt_notify_topic), notify_topic);
    cloud_copy(g_cloud.mqtt_ack_topic, sizeof(g_cloud.mqtt_ack_topic), ack_topic);
    cloud_copy(g_cloud.mqtt_up_topic, sizeof(g_cloud.mqtt_up_topic), up_topic);
    memset(&g_cloud.mqtt_info, 0, sizeof(g_cloud.mqtt_info));
    g_cloud.mqtt_info.client_id = g_cloud.mqtt_client_id;
    g_cloud.mqtt_info.client_user = g_cloud.mqtt_user;
    g_cloud.mqtt_info.client_pass = g_cloud.mqtt_password;
    g_cloud.mqtt_info.keep_alive = 60;
    g_cloud.mqtt_info.tls_config = tls;
    g_cloud.mqtt_mode = mode;
    g_cloud.mqtt_wanted_mode = mode;
    g_cloud.mqtt_connecting = true;
    g_cloud.mqtt_connected = false;
    g_cloud.snapshot.mqtt_connected = false;
    cloud_unlock(flags);

    printf("[tirtc_cloud] mqtt tls profile verify=required "
           "key_exchange=ecdhe-rsa curves=secp256r1 x25519=disabled "
           "ecdh_context=legacy ca=root-yr\n");
    printf("[tirtc_cloud] mqtt connect begin mode=%d port=%u client_len=%u "
           "user_len=%u wall=%lld\n",
           (int)mode, (unsigned int)TIRTC_DEMO_CLOUD_MQTT_PORT,
           (unsigned int)strlen(client_id), (unsigned int)strlen(user),
           (long long)time(NULL));
    LOCK_TCPIP_CORE();
    mqtt_set_inpub_callback(client, cloud_mqtt_incoming_publish,
                            cloud_mqtt_incoming_data, NULL);
    error = mqtt_client_connect(client, &address, TIRTC_DEMO_CLOUD_MQTT_PORT,
                                cloud_mqtt_connection_callback, NULL,
                                &g_cloud.mqtt_info);
    UNLOCK_TCPIP_CORE();
    memset(password, 0, sizeof(password));
    if (error != ERR_OK) {
        cloud_lock(&flags);
        g_cloud.mqtt_connecting = false;
        g_cloud.next_retry_ms = cloud_now_ms() + TIRTC_DEMO_CLOUD_RETRY_MS;
        cloud_unlock(flags);
        printf("[tirtc_cloud] mqtt connect submit failed mode=%d result=%d\n",
               (int)mode, (int)error);
        return (int)error;
    }
    printf("[tirtc_cloud] mqtt connect submitted mode=%d\n", (int)mode);
    return 0;
}

static int cloud_apply_bound_identity(const char *device_id,
                                      const char *device_secret,
                                      const char *source)
{
    tirtc_demo_cloud_callbacks_t callbacks;
    unsigned long flags;
    int save_result;

    if (!cloud_identity_valid(device_id, device_secret)) {
        return TIRTC_DEMO_CLOUD_ERR_HTTP_RESPONSE;
    }

    cloud_lock(&flags);
    callbacks = g_cloud.callbacks;
    cloud_unlock(flags);
    if (callbacks.save_identity == NULL) {
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    save_result = callbacks.save_identity(device_id, device_secret,
                                          callbacks.context);
    if (save_result != 0) {
        printf("[tirtc_cloud] binding identity save failed source=%s result=%d\n",
               source != NULL ? source : "unknown", save_result);
        return save_result;
    }

    cloud_mqtt_disconnect();
    cloud_lock(&flags);
    cloud_copy(g_cloud.device_id, sizeof(g_cloud.device_id), device_id);
    cloud_copy(g_cloud.device_secret, sizeof(g_cloud.device_secret),
               device_secret);
    memset(g_cloud.temp_token, 0, sizeof(g_cloud.temp_token));
    g_cloud.temp_client_id[0] = '\0';
    g_cloud.snapshot.identity_ready = true;
    g_cloud.snapshot.binding_waiting = false;
    g_cloud.snapshot.binding_code[0] = '\0';
    g_cloud.binding_requesting = false;
    g_cloud.binding_retry_count = 0U;
    g_cloud.next_binding_retry_ms = 0U;
    g_cloud.next_binding_poll_ms = 0U;
    g_cloud.binding_retry_started_ms = 0U;
    g_cloud.binding_deadline_ms = 0U;
    g_cloud.mqtt_wanted_mode = TIRTC_DEMO_CLOUD_MQTT_FORMAL;
    cloud_set_status_locked(0, 200, 200, "设备绑定",
                            "设备绑定成功，正在切换正式服务。");
    cloud_unlock(flags);

    printf("[tirtc_cloud] binding identity activated source=%s id_len=%u\n",
           source != NULL ? source : "unknown",
           (unsigned int)strlen(device_id));
    if (callbacks.identity_activated != NULL) {
        callbacks.identity_activated(callbacks.context);
    }
    cloud_schedule(TIRTC_DEMO_CLOUD_WORK_START);
    return 0;
}

static int cloud_report_binding_now(void)
{
    char device_id[TIRTC_DEMO_CLOUD_DEVICE_ID_MAX];
    char device_secret[TIRTC_DEMO_CLOUD_DEVICE_SECRET_MAX];
    char mac[18];
    char headers[768] = {0};
    char body[256];
    char *response = NULL;
    cJSON *root = NULL;
    cJSON *data = NULL;
    const char *code;
    const char *temp_token;
    const char *temp_client_id;
    const char *bound_device_id;
    const char *bound_device_secret;
    unsigned long flags;
    int http_status = 0;
    int business_code = -1;
    int result;

    if (!cloud_network_ready() || cloud_get_mac(mac, sizeof(mac)) != 0) {
        printf("[tirtc_cloud] binding report blocked network_ready=%d\n",
               cloud_network_ready() ? 1 : 0);
        cloud_set_status(TIRTC_DEMO_CLOUD_ERR_NOT_READY, 0, -1, "设备绑定",
                         "请先通过君正系统设置连接 WiFi。" );
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    cloud_lock(&flags);
    cloud_copy(device_id, sizeof(device_id), g_cloud.device_id);
    cloud_copy(device_secret, sizeof(device_secret), g_cloud.device_secret);
    cloud_unlock(flags);
    printf("[tirtc_cloud] binding report begin signed=%d mac=%s\n",
           cloud_identity_valid(device_id, device_secret) ? 1 : 0, mac);
    if (cloud_identity_valid(device_id, device_secret)) {
        result = cloud_signed_headers(device_id, device_secret, mac,
                                      headers, sizeof(headers));
        if (result != 0) {
            cloud_set_status(result, 0, -1, "设备绑定",
                             "系统时间尚未同步，暂时无法签名重绑请求。" );
            goto cleanup;
        }
    }
    snprintf(body, sizeof(body),
             "{\"mac\":\"%s\",\"chip_uid\":\"\",\"device_rand\":\"\"}", mac);
    response = malloc(TIRTC_DEMO_CLOUD_HTTP_BODY_MAX);
    if (response == NULL) {
        result = TIRTC_DEMO_CLOUD_ERR_HTTP_RESPONSE;
        printf("[tirtc_cloud] binding response allocation failed bytes=%u\n",
               (unsigned int)TIRTC_DEMO_CLOUD_HTTP_BODY_MAX);
        goto cleanup;
    }
    cloud_set_status(0, 0, -1, "设备绑定", "正在向业务服务申请绑定验证码。" );
    result = cloud_http_json("POST", "/v1/device/report", body, headers,
                             response, TIRTC_DEMO_CLOUD_HTTP_BODY_MAX,
                             &http_status);
    if (result == 0) {
        result = cloud_parse_business(response, &root, &data, &business_code);
    }
    if (result != 0 || http_status < 200 || http_status >= 300 ||
        !cJSON_IsObject(data)) {
        int failure = result != 0 ? result :
                      TIRTC_DEMO_CLOUD_ERR_HTTP_RESPONSE;

        cloud_set_status(failure, http_status, business_code, "设备绑定",
                         business_code == 40901 ?
                             "上一验证码仍有效，请稍后重试。" :
                             "绑定验证码申请失败，请稍后重试。" );
        result = failure;
        printf("[tirtc_cloud] binding report failed result=%d http=%d "
               "business=%d\n", result, http_status, business_code);
        goto cleanup;
    }
    bound_device_id = cloud_json_string(data, "device_id");
    bound_device_secret = cloud_json_string(data, "device_key");
    if (cloud_identity_valid(bound_device_id, bound_device_secret)) {
        result = cloud_apply_bound_identity(bound_device_id,
                                            bound_device_secret,
                                            "http-report");
        goto cleanup;
    }
    code = cloud_json_string(data, "code");
    temp_token = cloud_json_string(data, "temp_token");
    temp_client_id = cloud_json_string(data, "temp_client_id");
    if (code[0] == '\0' || temp_token[0] == '\0' || temp_client_id[0] == '\0' ||
        strlen(code) >= sizeof(g_cloud.snapshot.binding_code) ||
        strlen(temp_token) >= sizeof(g_cloud.temp_token) ||
        strlen(temp_client_id) >= sizeof(g_cloud.temp_client_id)) {
        result = TIRTC_DEMO_CLOUD_ERR_HTTP_RESPONSE;
        cloud_set_status(result, http_status, business_code, "设备绑定",
                          "绑定服务响应格式不正确。" );
        printf("[tirtc_cloud] binding response invalid http=%d business=%d "
               "code_len=%u token_len=%u client_len=%u\n",
               http_status, business_code, (unsigned int)strlen(code),
               (unsigned int)strlen(temp_token),
               (unsigned int)strlen(temp_client_id));
        goto cleanup;
    }

    cloud_mqtt_disconnect();
    cloud_lock(&flags);
    g_cloud.auth_grant_pending = false;
    g_cloud.auth_ack_inflight = false;
    cloud_copy(g_cloud.snapshot.binding_code,
               sizeof(g_cloud.snapshot.binding_code), code);
    cloud_copy(g_cloud.temp_token, sizeof(g_cloud.temp_token), temp_token);
    cloud_copy(g_cloud.temp_client_id, sizeof(g_cloud.temp_client_id), temp_client_id);
    g_cloud.snapshot.binding_waiting = true;
    g_cloud.binding_requesting = false;
    g_cloud.binding_retry_count = 0U;
    g_cloud.next_binding_retry_ms = 0U;
    g_cloud.next_binding_poll_ms = cloud_now_ms() +
                                   TIRTC_DEMO_CLOUD_BINDING_POLL_MS;
    g_cloud.binding_retry_started_ms = 0U;
    g_cloud.binding_deadline_ms = cloud_now_ms() + TIRTC_DEMO_CLOUD_BINDING_TIMEOUT_MS;
    g_cloud.mqtt_wanted_mode = TIRTC_DEMO_CLOUD_MQTT_TEMP;
    cloud_set_status_locked(0, http_status, business_code, "设备绑定",
                            "验证码已生成，请在管理端完成绑定。" );
    cloud_unlock(flags);
    printf("[tirtc_cloud] binding code ready code_len=%u client_len=%u\n",
           (unsigned int)strlen(code), (unsigned int)strlen(temp_client_id));
    printf("[tirtc_cloud] binding phase=code-issued next=temp-mqtt\n");
    result = cloud_mqtt_connect(TIRTC_DEMO_CLOUD_MQTT_TEMP);
    if (result != 0) {
        printf("[tirtc_cloud] binding mqtt connect queued result=%d\n", result);
        cloud_set_status(result, http_status, business_code, "设备绑定",
                         "验证码已生成，临时消息连接正在重试。" );
    }

cleanup:
    if (root != NULL) {
        cJSON_Delete(root);
    }
    if (response != NULL) {
        memset(response, 0, TIRTC_DEMO_CLOUD_HTTP_BODY_MAX);
        free(response);
    }
    memset(device_secret, 0, sizeof(device_secret));
    memset(headers, 0, sizeof(headers));
    return result;
}

static int cloud_poll_binding_now(void)
{
    char mac[18];
    char body[256];
    char *response = NULL;
    cJSON *root = NULL;
    cJSON *data = NULL;
    const char *code;
    const char *bound_device_id;
    const char *bound_device_secret;
    unsigned long flags;
    bool still_waiting = false;
    int http_status = 0;
    int business_code = -1;
    int result;

    if (!cloud_network_ready() || cloud_get_mac(mac, sizeof(mac)) != 0) {
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    snprintf(body, sizeof(body),
             "{\"mac\":\"%s\",\"chip_uid\":\"\",\"device_rand\":\"\"}", mac);
    response = malloc(TIRTC_DEMO_CLOUD_HTTP_BODY_MAX);
    if (response == NULL) {
        return TIRTC_DEMO_CLOUD_ERR_HTTP_RESPONSE;
    }

    printf("[tirtc_cloud] binding HTTP poll begin mac=%s\n", mac);
    result = cloud_http_json("POST", "/v1/device/report", body, "",
                             response, TIRTC_DEMO_CLOUD_HTTP_BODY_MAX,
                             &http_status);
    if (result == 0) {
        result = cloud_parse_business(response, &root, &data, &business_code);
    }
    if (result != 0 && business_code == 40901 &&
        http_status >= 200 && http_status < 300) {
        printf("[tirtc_cloud] binding HTTP poll pending http=%d business=%d\n",
               http_status, business_code);
        result = 0;
        goto cleanup;
    }
    if (result != 0 || http_status < 200 || http_status >= 300 ||
        !cJSON_IsObject(data)) {
        result = result != 0 ? result : TIRTC_DEMO_CLOUD_ERR_HTTP_RESPONSE;
        printf("[tirtc_cloud] binding HTTP poll failed result=%d http=%d "
               "business=%d\n", result, http_status, business_code);
        goto cleanup;
    }

    bound_device_id = cloud_json_string(data, "device_id");
    bound_device_secret = cloud_json_string(data, "device_key");
    if (cloud_identity_valid(bound_device_id, bound_device_secret)) {
        printf("[tirtc_cloud] binding HTTP poll received credentials id_len=%u\n",
               (unsigned int)strlen(bound_device_id));
        result = cloud_apply_bound_identity(bound_device_id,
                                            bound_device_secret,
                                            "http-poll");
        goto cleanup;
    }

    code = cloud_json_string(data, "code");
    if (code[0] == '\0' || strlen(code) >= sizeof(g_cloud.snapshot.binding_code)) {
        result = TIRTC_DEMO_CLOUD_ERR_HTTP_RESPONSE;
        printf("[tirtc_cloud] binding HTTP poll invalid response code_len=%u\n",
               (unsigned int)strlen(code));
        goto cleanup;
    }
    cloud_lock(&flags);
    if (g_cloud.snapshot.binding_waiting && !g_cloud.auth_grant_pending) {
        cloud_copy(g_cloud.snapshot.binding_code,
                   sizeof(g_cloud.snapshot.binding_code), code);
        still_waiting = true;
    }
    cloud_unlock(flags);
    printf("[tirtc_cloud] binding HTTP poll still waiting code_len=%u active=%d\n",
           (unsigned int)strlen(code), still_waiting ? 1 : 0);
    result = 0;

cleanup:
    if (http_status == 429) {
        cloud_lock(&flags);
        if (g_cloud.snapshot.binding_waiting) {
            g_cloud.next_binding_poll_ms = cloud_now_ms() +
                                           TIRTC_DEMO_CLOUD_RATE_LIMIT_RETRY_MS;
        }
        cloud_unlock(flags);
        printf("[tirtc_cloud] binding HTTP poll rate limited "
               "backoff_ms=%u\n",
               (unsigned int)TIRTC_DEMO_CLOUD_RATE_LIMIT_RETRY_MS);
    }
    if (root != NULL) {
        cJSON_Delete(root);
    }
    if (response != NULL) {
        memset(response, 0, TIRTC_DEMO_CLOUD_HTTP_BODY_MAX);
        free(response);
    }
    return result;
}

static void cloud_handle_binding_result(int result)
{
    uint64_t now;
    uint64_t elapsed;
    uint32_t retry_ms;
    int http_status;
    uint8_t retry_count;
    unsigned long flags;

    if (result == 0) {
        return;
    }

    now = cloud_now_ms();
    cloud_lock(&flags);
    if (g_cloud.snapshot.binding_waiting) {
        g_cloud.binding_requesting = false;
        g_cloud.next_binding_retry_ms = 0U;
        cloud_unlock(flags);
        return;
    }
    if (g_cloud.binding_retry_started_ms == 0U) {
        g_cloud.binding_retry_started_ms = now;
    }
    elapsed = now - g_cloud.binding_retry_started_ms;
    if (elapsed < TIRTC_DEMO_CLOUD_BIND_RETRY_MAX_MS) {
        g_cloud.binding_requesting = true;
        ++g_cloud.binding_retry_count;
        if (g_cloud.binding_retry_count == 0U) {
            g_cloud.binding_retry_count = 1U;
        }
        retry_count = g_cloud.binding_retry_count;
        http_status = g_cloud.snapshot.http_status;
        retry_ms = http_status == 429 ?
                       TIRTC_DEMO_CLOUD_RATE_LIMIT_RETRY_MS :
                       TIRTC_DEMO_CLOUD_RETRY_MS;
        g_cloud.next_binding_retry_ms = now + retry_ms;
        cloud_set_status_locked(result, g_cloud.snapshot.http_status,
                                g_cloud.snapshot.business_code, "设备绑定",
                                http_status == 429 ?
                                    "绑定服务请求过快，30 秒后自动重试。" :
                                    "绑定申请暂时失败，5 秒后自动重试。");
        cloud_unlock(flags);
        printf("[tirtc_cloud] binding retry scheduled attempt=%u "
               "wait_ms=%u elapsed_ms=%llu result=%d http=%d\n",
               (unsigned int)retry_count,
               (unsigned int)retry_ms,
               (unsigned long long)elapsed, result, http_status);
        return;
    }

    retry_count = g_cloud.binding_retry_count;
    g_cloud.binding_requesting = false;
    g_cloud.next_binding_retry_ms = 0U;
    cloud_set_status_locked(result, g_cloud.snapshot.http_status,
                            g_cloud.snapshot.business_code, "设备绑定",
                            "绑定申请失败，请点击刷新重新申请。");
    cloud_unlock(flags);
    printf("[tirtc_cloud] binding retry exhausted attempts=%u elapsed_ms=%llu "
           "result=%d\n", (unsigned int)retry_count,
           (unsigned long long)elapsed, result);
}

static int cloud_refresh_contacts_now(void)
{
    char headers[TIRTC_DEMO_CLOUD_TOKEN_MAX + 64];
    char *response = NULL;
    cJSON *root = NULL;
    cJSON *data = NULL;
    cJSON *items;
    tirtc_demo_cloud_contact_t contacts[TIRTC_DEMO_CLOUD_CONTACT_MAX];
    size_t count = 0U;
    unsigned long flags;
    int http_status = 0;
    int business_code = -1;
    int result;

    memset(contacts, 0, sizeof(contacts));
    result = cloud_bearer_headers(headers, sizeof(headers));
    if (result != 0) {
        return result;
    }
    response = malloc(TIRTC_DEMO_CLOUD_HTTP_BODY_MAX);
    if (response == NULL) {
        result = -1;
        goto cleanup;
    }
    result = cloud_http_json("GET", "/v1/call/device/contacts", NULL, headers,
                             response, TIRTC_DEMO_CLOUD_HTTP_BODY_MAX,
                             &http_status);
    if (result == 0) {
        result = cloud_parse_business(response, &root, &data, &business_code);
    }
    if (result != 0 || !cJSON_IsObject(data)) {
        cloud_set_status(-1, http_status, business_code, "联系人",
                         "联系人同步失败，请检查设备在线状态。" );
        result = -1;
        goto cleanup;
    }
    items = cJSON_GetObjectItemCaseSensitive(data, "contacts");
    if (cJSON_IsArray(items)) {
        cJSON *item;
        cJSON_ArrayForEach(item, items) {
            const char *device_id = cloud_json_string(item, "device_id");
            const char *type = cloud_json_string(item, "type");
            const char *remark = cloud_json_string(item, "remark");
            const cJSON *online = cJSON_GetObjectItemCaseSensitive(item, "online");
            bool duplicate = false;

            if ((type[0] != '\0' && strcmp(type, "device") != 0) ||
                !cloud_target_valid(device_id)) {
                continue;
            }
            for (size_t i = 0; i < count; ++i) {
                if (strcmp(contacts[i].device_id, device_id) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate || count >= TIRTC_DEMO_CLOUD_CONTACT_MAX) {
                continue;
            }
            cloud_copy(contacts[count].device_id,
                       sizeof(contacts[count].device_id), device_id);
            cloud_copy(contacts[count].remark,
                       sizeof(contacts[count].remark), remark);
            contacts[count].online = cJSON_IsTrue(online);
            ++count;
        }
    }

    cloud_lock(&flags);
    memset(g_cloud.snapshot.contacts, 0, sizeof(g_cloud.snapshot.contacts));
    memcpy(g_cloud.snapshot.contacts, contacts, count * sizeof(contacts[0]));
    g_cloud.snapshot.contact_count = count;
    g_cloud.snapshot.contacts_ready = true;
    cloud_set_status_locked(0, http_status, business_code, "联系人",
                            count > 0U ? "联系人已从业务服务同步。" :
                                         "联系人同步完成，当前列表为空。" );
    cloud_unlock(flags);
    result = 0;

cleanup:
    if (root != NULL) {
        cJSON_Delete(root);
    }
    if (response != NULL) {
        memset(response, 0, TIRTC_DEMO_CLOUD_HTTP_BODY_MAX);
        free(response);
    }
    memset(headers, 0, sizeof(headers));
    return result;
}

static int cloud_request_contact_now(const char *target_device_id)
{
    char headers[TIRTC_DEMO_CLOUD_TOKEN_MAX + 64];
    char body[224];
    char *response = NULL;
    cJSON *root = NULL;
    cJSON *data = NULL;
    const char *status;
    int http_status = 0;
    int business_code = -1;
    int result;

    if (!cloud_target_valid(target_device_id)) {
        return -1;
    }
    result = cloud_bearer_headers(headers, sizeof(headers));
    if (result != 0) {
        return result;
    }
    snprintf(body, sizeof(body), "{\"target_device_id\":\"%s\"}", target_device_id);
    response = malloc(TIRTC_DEMO_CLOUD_HTTP_BODY_MAX);
    if (response == NULL) {
        result = -1;
        goto cleanup;
    }
    result = cloud_http_json("POST", "/v1/call/device/contacts/request", body,
                             headers, response, TIRTC_DEMO_CLOUD_HTTP_BODY_MAX,
                             &http_status);
    if (result == 0) {
        result = cloud_parse_business(response, &root, &data, &business_code);
    }
    status = cloud_json_string(data, "status");
    if (result != 0 || status[0] == '\0') {
        cloud_set_status(-1, http_status, business_code, "联系人",
                         "联系人申请发送失败。" );
        result = -1;
        goto cleanup;
    }
    cloud_set_status(0, http_status, business_code, "联系人",
                     strcmp(status, "accepted") == 0 ?
                         "联系人关系已建立，正在刷新列表。" :
                         "联系人申请已发送，等待对方处理。" );
    (void)cloud_refresh_contacts_now();
    result = 0;

cleanup:
    if (root != NULL) {
        cJSON_Delete(root);
    }
    if (response != NULL) {
        memset(response, 0, TIRTC_DEMO_CLOUD_HTTP_BODY_MAX);
        free(response);
    }
    memset(headers, 0, sizeof(headers));
    return result;
}

static int cloud_post_room_action_now(tirtc_demo_cloud_room_action_t action,
                                      const char *room_id, const char *reason)
{
    const char *path;
    const char *wire_reason = "";
    char headers[TIRTC_DEMO_CLOUD_TOKEN_MAX + 64];
    char body[256];
    char response[1024];
    cJSON *root = NULL;
    cJSON *data = NULL;
    int http_status = 0;
    int business_code = -1;
    int result;

    if (room_id == NULL || room_id[0] == '\0') {
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    if (action == TIRTC_DEMO_CLOUD_ROOM_CANCEL) {
        path = "/v1/call/cancel";
        snprintf(body, sizeof(body), "{\"room_id\":\"%s\"}", room_id);
    } else if (action == TIRTC_DEMO_CLOUD_ROOM_REJECT) {
        path = "/v1/call/reject";
        wire_reason = reason != NULL && strcmp(reason, "busy") == 0 ?
                          "busy" : "decline";
        snprintf(body, sizeof(body),
                 "{\"room_id\":\"%s\",\"reason\":\"%s\"}",
                 room_id, wire_reason);
    } else {
        path = "/v1/call/hangup";
        wire_reason = reason != NULL &&
                              (strcmp(reason, "hangup") == 0 ||
                               strcmp(reason, "local_hangup") == 0) ?
                          "hangup" : "p2p_error";
        snprintf(body, sizeof(body),
                 "{\"room_id\":\"%s\",\"reason\":\"%s\"}",
                 room_id, wire_reason);
    }
    result = cloud_bearer_headers(headers, sizeof(headers));
    if (result != 0) {
        return result;
    }
    result = cloud_http_json("POST", path, body, headers, response,
                             sizeof(response), &http_status);
    if (result == 0) {
        result = cloud_parse_business(response, &root, &data, &business_code);
    }
    if (root != NULL) {
        cJSON_Delete(root);
    }
    printf("[tirtc_cloud] room action path=%s reason=%s result=%d "
           "http=%d code=%d\n",
           path, wire_reason[0] != '\0' ? wire_reason : "none",
           result, http_status, business_code);
    memset(headers, 0, sizeof(headers));
    return result == 0 && http_status >= 200 && http_status < 300 ? 0 : -1;
}

static int cloud_request_call_now(const char *target_device_id)
{
    char headers[TIRTC_DEMO_CLOUD_TOKEN_MAX + 64];
    char body[256];
    char *response = NULL;
    cJSON *root = NULL;
    cJSON *data = NULL;
    const char *room_id;
    unsigned long flags;
    int http_status = 0;
    int business_code = -1;
    int result;
    bool request_active;

    if (!cloud_target_valid(target_device_id)) {
        return -1;
    }
    result = cloud_bearer_headers(headers, sizeof(headers));
    if (result != 0) {
        return result;
    }
    snprintf(body, sizeof(body),
             "{\"targets\":[\"%s\"],\"call_type\":\"audio\"}",
             target_device_id);
    response = malloc(TIRTC_DEMO_CLOUD_HTTP_BODY_MAX);
    if (response == NULL) {
        result = -1;
        goto cleanup;
    }
    result = cloud_http_json("POST", "/v1/call/request", body, headers,
                             response, TIRTC_DEMO_CLOUD_HTTP_BODY_MAX,
                             &http_status);
    if (result == 0) {
        result = cloud_parse_business(response, &root, &data, &business_code);
    }
    room_id = cloud_json_string(data, "room_id");
    printf("[tirtc_cloud] device call request result=%d http=%d code=%d "
           "target_len=%u room_len=%u\n",
           result, http_status, business_code,
           (unsigned)strlen(target_device_id), (unsigned)strlen(room_id));
    if (result != 0 || room_id[0] == '\0' ||
        strlen(room_id) >= TIRTC_DEMO_CLOUD_ROOM_ID_MAX) {
        result = -1;
        cloud_lock(&flags);
        request_active = g_cloud.snapshot.session_type ==
                             TIRTC_DEMO_CLOUD_SESSION_DEVICE &&
                         g_cloud.snapshot.call_state ==
                             TIRTC_DEMO_CLOUD_CALL_OUTGOING &&
                         strcmp(g_cloud.snapshot.peer_id,
                                target_device_id) == 0;
        if (request_active) {
            cloud_clear_call_locked("设备呼叫请求失败。");
            cloud_set_status_locked(result, http_status, business_code,
                                    "设备呼叫",
                                    "呼叫请求失败，请稍后重试。" );
        }
        cloud_unlock(flags);
        goto cleanup;
    }
    cloud_lock(&flags);
    request_active = g_cloud.snapshot.session_type ==
                         TIRTC_DEMO_CLOUD_SESSION_DEVICE &&
                     g_cloud.snapshot.call_state ==
                         TIRTC_DEMO_CLOUD_CALL_OUTGOING &&
                     strcmp(g_cloud.snapshot.peer_id, target_device_id) == 0;
    if (request_active) {
        cloud_copy(g_cloud.snapshot.room_id,
                   sizeof(g_cloud.snapshot.room_id), room_id);
        g_cloud.snapshot.call_state = TIRTC_DEMO_CLOUD_CALL_OUTGOING;
        g_cloud.snapshot.incoming_call = false;
        g_cloud.snapshot.call_active = false;
        g_cloud.call_is_caller = true;
        g_cloud.call_deadline_ms = cloud_now_ms() +
                                   TIRTC_DEMO_CLOUD_CALL_TIMEOUT_MS;
        cloud_set_status_locked(0, http_status, business_code, "设备呼叫",
                                "呼叫已发出，正在等待对方接听。" );
    }
    cloud_unlock(flags);
    if (request_active) {
        result = 0;
    } else {
        printf("[tirtc_cloud] stale device call response cancelled "
               "target_len=%u room_len=%u\n",
               (unsigned)strlen(target_device_id),
               (unsigned)strlen(room_id));
        (void)cloud_post_room_action_now(TIRTC_DEMO_CLOUD_ROOM_CANCEL,
                                         room_id, "local_hangup");
        result = TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }

cleanup:
    if (root != NULL) {
        cJSON_Delete(root);
    }
    if (response != NULL) {
        memset(response, 0, TIRTC_DEMO_CLOUD_HTTP_BODY_MAX);
        free(response);
    }
    memset(headers, 0, sizeof(headers));
    return result;
}

static int cloud_fetch_device_info_now(void)
{
    char headers[TIRTC_DEMO_CLOUD_TOKEN_MAX + 64U];
    char peer_id[TIRTC_DEMO_CLOUD_DEVICE_ID_MAX];
    char room_id[TIRTC_DEMO_CLOUD_ROOM_ID_MAX];
    char body[384];
    char *response = NULL;
    cJSON *root = NULL;
    cJSON *data = NULL;
    const char *token;
    const char *remote_id;
    tirtc_demo_cloud_session_t session = {0};
    bool incoming;
    bool current_call;
    unsigned long flags;
    int http_status = 0;
    int business_code = -1;
    int result;

    cloud_lock(&flags);
    cloud_copy(peer_id, sizeof(peer_id), g_cloud.snapshot.peer_id);
    cloud_copy(room_id, sizeof(room_id), g_cloud.snapshot.room_id);
    incoming = !g_cloud.call_is_caller;
    cloud_unlock(flags);
    if (!cloud_target_valid(peer_id) || room_id[0] == '\0') {
        result = TIRTC_DEMO_CLOUD_ERR_NOT_READY;
        goto done;
    }
    result = cloud_bearer_headers(headers, sizeof(headers));
    if (result != 0) {
        goto done;
    }
    if (snprintf(body, sizeof(body),
                 "{\"device_id\":\"%s\",\"room_id\":\"%s\",\"purpose\":\"call\"}",
                 peer_id, room_id) >= (int)sizeof(body)) {
        result = -1;
        goto done;
    }
    response = malloc(TIRTC_DEMO_CLOUD_HTTP_BODY_MAX);
    if (response == NULL) {
        result = -1;
        goto done;
    }
    cloud_set_status(0, 0, -1, "设备通话", "正在获取 TiRTC 连接凭据。");
    result = cloud_http_json("POST", "/v1/call/device/info", body, headers,
                             response, TIRTC_DEMO_CLOUD_HTTP_BODY_MAX,
                             &http_status);
    if (result == 0) {
        result = cloud_parse_business(response, &root, &data, &business_code);
    }
    token = cloud_json_string(data, "token");
    remote_id = cloud_json_string(data, "device_id");
    printf("[tirtc_cloud] device info result=%d http=%d code=%d "
           "incoming=%d room_len=%u peer_len=%u token_len=%u/%u\n",
           result, http_status, business_code, incoming ? 1 : 0,
           (unsigned)strlen(room_id),
           (unsigned)strlen(remote_id[0] != '\0' ? remote_id : peer_id),
           (unsigned)strlen(token), (unsigned)sizeof(session.token));
    if (result != 0 || http_status < 200 || http_status >= 300 ||
        token[0] == '\0' || strlen(token) >= sizeof(session.token)) {
        result = -1;
        goto done;
    }
    session.type = TIRTC_DEMO_CLOUD_SESSION_DEVICE;
    session.incoming = incoming;
    cloud_copy(session.peer_id, sizeof(session.peer_id),
               cloud_target_valid(remote_id) ? remote_id : peer_id);
    cloud_copy(session.token, sizeof(session.token), token);
    cloud_copy(session.room_id, sizeof(session.room_id), room_id);
    cloud_lock(&flags);
    current_call = g_cloud.device_info_pending &&
                   g_cloud.snapshot.session_type ==
                       TIRTC_DEMO_CLOUD_SESSION_DEVICE &&
                   strcmp(g_cloud.snapshot.room_id, room_id) == 0 &&
                   g_cloud.snapshot.call_state != TIRTC_DEMO_CLOUD_CALL_IDLE;
    if (!current_call) {
        cloud_unlock(flags);
        result = TIRTC_DEMO_CLOUD_ERR_NOT_READY;
        goto done;
    }
    g_cloud.snapshot.session_type = TIRTC_DEMO_CLOUD_SESSION_DEVICE;
    g_cloud.snapshot.call_state = TIRTC_DEMO_CLOUD_CALL_CONNECTING;
    g_cloud.snapshot.incoming_call = false;
    cloud_set_status_locked(0, http_status, business_code, "设备通话",
                            "连接凭据已就绪，正在建立 TiRTC 会话。");
    cloud_unlock(flags);
    cloud_emit_session(&session);
    result = 0;

done:
    cloud_lock(&flags);
    current_call = room_id[0] != '\0' &&
                   strcmp(g_cloud.snapshot.room_id, room_id) == 0 &&
                   g_cloud.snapshot.call_state != TIRTC_DEMO_CLOUD_CALL_IDLE;
    cloud_unlock(flags);
    if (result != 0 && current_call) {
        (void)cloud_post_room_action_now(
            incoming ? TIRTC_DEMO_CLOUD_ROOM_HANGUP :
                       TIRTC_DEMO_CLOUD_ROOM_CANCEL,
            room_id, "media_info_failed");
    }
    cloud_lock(&flags);
    if (strcmp(g_cloud.snapshot.room_id, room_id) == 0) {
        g_cloud.device_info_pending = false;
    }
    if (result != 0 && current_call &&
        strcmp(g_cloud.snapshot.room_id, room_id) == 0) {
        cloud_clear_call_locked("媒体连接参数获取失败。");
        cloud_set_status_locked(-1, http_status, business_code, "设备通话",
                                "获取 TiRTC 连接凭据失败。");
    }
    cloud_unlock(flags);
    if (root != NULL) {
        cJSON_Delete(root);
    }
    if (response != NULL) {
        memset(response, 0, TIRTC_DEMO_CLOUD_HTTP_BODY_MAX);
        free(response);
    }
    memset(headers, 0, sizeof(headers));
    memset(&session, 0, sizeof(session));
    return result;
}

static int cloud_refresh_wechat_profile_now(void)
{
    static const char body[] =
        "{\"screen_width\":1,\"screen_height\":1,"
        "\"audio_rate\":8000,\"audio_channels\":1,"
        "\"video_mt\":\"\",\"no_video\":true,"
        "\"calling_timeout_sec\":30}";
    char headers[TIRTC_DEMO_CLOUD_TOKEN_MAX + 64U];
    char response[1024];
    cJSON *root = NULL;
    cJSON *data = NULL;
    unsigned long flags;
    int http_status = 0;
    int business_code = -1;
    int result = cloud_bearer_headers(headers, sizeof(headers));

    if (result == 0) {
        result = cloud_http_json("POST", "/v1/voip/device/profile", body,
                                 headers, response, sizeof(response),
                                 &http_status);
    }
    if (result == 0) {
        result = cloud_parse_business(response, &root, &data, &business_code);
    }
    cloud_lock(&flags);
    g_cloud.snapshot.wechat_ready = result == 0 && http_status >= 200 &&
                                    http_status < 300;
    cloud_set_status_locked(result == 0 ? 0 : -1, http_status, business_code,
                            "微信通话",
                            result == 0 ? "微信设备能力已上报。" :
                                          "微信设备能力上报失败。");
    cloud_unlock(flags);
    if (root != NULL) {
        cJSON_Delete(root);
    }
    memset(headers, 0, sizeof(headers));
    return result;
}

static int cloud_refresh_wechat_contacts_now(void)
{
    char headers[TIRTC_DEMO_CLOUD_TOKEN_MAX + 64U];
    char *response = NULL;
    cJSON *root = NULL;
    cJSON *data = NULL;
    cJSON *list;
    tirtc_demo_cloud_wechat_contact_t
        contacts[TIRTC_DEMO_CLOUD_CONTACT_MAX] = {0};
    size_t count = 0U;
    unsigned long flags;
    int http_status = 0;
    int business_code = -1;
    int result = cloud_bearer_headers(headers, sizeof(headers));
    if (result != 0) {
        goto done;
    }
    response = malloc(TIRTC_DEMO_CLOUD_HTTP_BODY_MAX);
    if (response == NULL) {
        result = -1;
        goto done;
    }
    result = cloud_http_json("GET", "/v1/voip/device/callers", NULL,
                             headers, response, TIRTC_DEMO_CLOUD_HTTP_BODY_MAX,
                             &http_status);
    if (result == 0) {
        result = cloud_parse_business(response, &root, &data, &business_code);
    }
    list = cJSON_IsObject(data) ?
               cJSON_GetObjectItemCaseSensitive(data, "list") : NULL;
    if (result != 0 || !cJSON_IsArray(list)) {
        result = -1;
        goto done;
    }
    for (cJSON *item = list->child;
         item != NULL && count < TIRTC_DEMO_CLOUD_CONTACT_MAX;
         item = item->next) {
        const char *open_id = cloud_json_string_alias(
            item, "wx_open_id", "wxa_open_id", "wx_user_openid");
        const char *model_id = cloud_json_string_alias(
            item, "wx_model_id", "wxa_model_id", NULL);
        const char *app_id = cloud_json_string_alias(
            item, "wx_app_id", "wxa_app_id", NULL);
        const char *name = cloud_json_string_alias(
            item, "remark", "nickname", "name");

        if (open_id[0] == '\0' || strlen(open_id) >=
                                     sizeof(contacts[count].open_id)) {
            continue;
        }
        cloud_copy(contacts[count].open_id,
                   sizeof(contacts[count].open_id), open_id);
        cloud_copy(contacts[count].model_id,
                   sizeof(contacts[count].model_id), model_id);
        cloud_copy(contacts[count].app_id,
                   sizeof(contacts[count].app_id), app_id);
        if (name[0] != '\0') {
            cloud_copy(contacts[count].name,
                       sizeof(contacts[count].name), name);
        } else {
            snprintf(contacts[count].name, sizeof(contacts[count].name),
                     "微信联系人 %u", (unsigned int)(count + 1U));
        }
        ++count;
    }

done:
    cloud_lock(&flags);
    if (result == 0) {
        memcpy(g_cloud.snapshot.wechat_contacts, contacts, sizeof(contacts));
        g_cloud.snapshot.wechat_contact_count = count;
        g_cloud.snapshot.wechat_contacts_ready = true;
    } else {
        g_cloud.snapshot.wechat_contacts_ready = false;
    }
    cloud_set_status_locked(result == 0 ? 0 : -1, http_status, business_code,
                            "微信联系人",
                            result == 0 ? "微信联系人已同步。" :
                                          "微信联系人同步失败。");
    cloud_unlock(flags);
    if (root != NULL) {
        cJSON_Delete(root);
    }
    if (response != NULL) {
        memset(response, 0, TIRTC_DEMO_CLOUD_HTTP_BODY_MAX);
        free(response);
    }
    memset(headers, 0, sizeof(headers));
    return result;
}

static int cloud_request_wechat_call_now(void)
{
    char headers[TIRTC_DEMO_CLOUD_TOKEN_MAX + 64U];
    char device_id[TIRTC_DEMO_CLOUD_DEVICE_ID_MAX];
    char open_id[TIRTC_DEMO_CLOUD_WX_OPENID_MAX];
    char app_id[TIRTC_DEMO_CLOUD_WX_APP_ID_MAX];
    char model_id[TIRTC_DEMO_CLOUD_WX_MODEL_ID_MAX];
    char body[768];
    char response[1024];
    cJSON *root = NULL;
    cJSON *data = NULL;
    unsigned long flags;
    int http_status = 0;
    int business_code = -1;
    int result;
    int written;
    bool request_active = false;

    cloud_lock(&flags);
    cloud_copy(device_id, sizeof(device_id), g_cloud.device_id);
    cloud_copy(open_id, sizeof(open_id), g_cloud.pending_wechat_open_id);
    cloud_copy(app_id, sizeof(app_id), g_cloud.pending_wechat_app_id);
    cloud_copy(model_id, sizeof(model_id), g_cloud.pending_wechat_model_id);
    request_active = g_cloud.wechat_call_pending &&
                     g_cloud.snapshot.session_type ==
                         TIRTC_DEMO_CLOUD_SESSION_WECHAT &&
                     g_cloud.snapshot.call_state ==
                         TIRTC_DEMO_CLOUD_CALL_OUTGOING;
    cloud_unlock(flags);
    if (!request_active || device_id[0] == '\0' || open_id[0] == '\0' ||
        model_id[0] == '\0') {
        result = TIRTC_DEMO_CLOUD_ERR_NOT_READY;
        goto done;
    }
    result = cloud_bearer_headers(headers, sizeof(headers));
    if (result != 0) {
        goto done;
    }
    if (app_id[0] != '\0') {
        written = snprintf(
            body, sizeof(body),
            "{\"device_id\":\"%s\",\"wx_app_id\":\"%s\","
            "\"wx_user_openid\":\"%s\",\"wx_model_id\":\"%s\","
            "\"wx_room_type\":\"voice\",\"wx_version_type\":0,"
            "\"calling_timeout_sec\":30,\"wx_caller_camera_status\":0,"
            "\"wx_listener_camera_status\":0}",
            device_id, app_id, open_id, model_id);
    } else {
        written = snprintf(
            body, sizeof(body),
            "{\"device_id\":\"%s\",\"wx_user_openid\":\"%s\","
            "\"wx_model_id\":\"%s\",\"wx_room_type\":\"voice\","
            "\"wx_version_type\":0,\"calling_timeout_sec\":30,"
            "\"wx_caller_camera_status\":0,\"wx_listener_camera_status\":0}",
            device_id, open_id, model_id);
    }
    if (written <= 0 || written >= (int)sizeof(body)) {
        result = -1;
        goto done;
    }
    result = cloud_http_json("POST", "/v1/voip/device/call", body, headers,
                             response, sizeof(response), &http_status);
    if (result == 0) {
        result = cloud_parse_business(response, &root, &data, &business_code);
    }
    if (result == 0 && http_status >= 200 && http_status < 300) {
        cloud_lock(&flags);
        request_active = g_cloud.wechat_call_pending &&
                         g_cloud.snapshot.session_type ==
                             TIRTC_DEMO_CLOUD_SESSION_WECHAT &&
                         g_cloud.snapshot.call_state ==
                             TIRTC_DEMO_CLOUD_CALL_OUTGOING;
        if (request_active) {
            g_cloud.wechat_outgoing_waiting = true;
            g_cloud.snapshot.incoming_call = false;
            g_cloud.call_deadline_ms = cloud_now_ms() +
                                       TIRTC_DEMO_CLOUD_CALL_TIMEOUT_MS;
            cloud_set_status_locked(0, http_status, business_code,
                                    "微信通话",
                                    "微信呼叫已发起，等待入会参数。");
        }
        cloud_unlock(flags);
        if (!request_active) {
            printf("[tirtc_cloud] stale wechat call response ignored\n");
            result = TIRTC_DEMO_CLOUD_ERR_NOT_READY;
        }
    } else {
        result = -1;
    }

done:
    cloud_lock(&flags);
    request_active = request_active && g_cloud.wechat_call_pending &&
                     g_cloud.snapshot.session_type ==
                         TIRTC_DEMO_CLOUD_SESSION_WECHAT &&
                     g_cloud.snapshot.call_state !=
                         TIRTC_DEMO_CLOUD_CALL_IDLE;
    if (request_active) {
        g_cloud.wechat_call_pending = false;
    }
    if (result != 0 && request_active) {
        cloud_clear_call_locked("微信呼叫发起失败。");
        cloud_set_status_locked(result, http_status, business_code,
                                "微信通话", "微信呼叫发起失败。");
    }
    cloud_unlock(flags);
    if (root != NULL) {
        cJSON_Delete(root);
    }
    memset(headers, 0, sizeof(headers));
    return result;
}

static int cloud_wechat_contact_op_now(bool add)
{
    char device_id[TIRTC_DEMO_CLOUD_DEVICE_ID_MAX];
    char open_id[TIRTC_DEMO_CLOUD_WX_OPENID_MAX];
    char body[384];
    char response[1024];
    cJSON *root = NULL;
    cJSON *data = NULL;
    unsigned long flags;
    int http_status = 0;
    int business_code = -1;
    int result;

    cloud_lock(&flags);
    cloud_copy(device_id, sizeof(device_id), g_cloud.device_id);
    cloud_copy(open_id, sizeof(open_id), g_cloud.pending_wechat_open_id);
    cloud_unlock(flags);
    if (device_id[0] == '\0' || open_id[0] == '\0' ||
        snprintf(body, sizeof(body),
                 "{\"device_id\":\"%s\",\"wx_open_id\":\"%s\"}",
                 device_id, open_id) >= (int)sizeof(body)) {
        result = -1;
        goto done;
    }
    result = cloud_http_json("POST",
                             add ? "/v1/voip/user/report-auth" :
                                   "/v1/voip/user/delete-auth",
                             body, NULL, response, sizeof(response),
                             &http_status);
    if (result == 0) {
        result = cloud_parse_business(response, &root, &data, &business_code);
    }
    cloud_set_status(result == 0 ? 0 : -1, http_status, business_code,
                     "微信联系人",
                     result == 0 ?
                         (add ? "微信联系人添加请求已提交。" :
                                "微信联系人已删除。") :
                         (add ? "微信联系人添加失败。" :
                                "微信联系人删除失败。"));
    if (result == 0) {
        (void)cloud_refresh_wechat_contacts_now();
    }

done:
    cloud_lock(&flags);
    g_cloud.wechat_contact_op_pending = false;
    cloud_unlock(flags);
    if (root != NULL) {
        cJSON_Delete(root);
    }
    return result;
}

static int cloud_request_ai_token_now(void)
{
    char headers[TIRTC_DEMO_CLOUD_TOKEN_MAX + 64U];
    char *response = NULL;
    cJSON *root = NULL;
    cJSON *data = NULL;
    const char *peer_id;
    const char *token;
    const char *role_id;
    tirtc_demo_cloud_session_t session = {0};
    unsigned long flags;
    int http_status = 0;
    int business_code = -1;
    int result;
    bool request_active;

    cloud_lock(&flags);
    request_active = g_cloud.ai_token_pending &&
                     g_cloud.snapshot.session_type ==
                         TIRTC_DEMO_CLOUD_SESSION_AI &&
                     g_cloud.snapshot.call_state ==
                         TIRTC_DEMO_CLOUD_CALL_CONNECTING;
    cloud_unlock(flags);
    if (!request_active) {
        result = TIRTC_DEMO_CLOUD_ERR_NOT_READY;
        goto done;
    }
    result = cloud_bearer_headers(headers, sizeof(headers));
    if (result != 0) {
        goto done;
    }
    response = malloc(TIRTC_DEMO_CLOUD_HTTP_BODY_MAX);
    if (response == NULL) {
        result = -1;
        goto done;
    }
    result = cloud_http_json("GET", "/v1/ai/token", NULL, headers,
                             response, TIRTC_DEMO_CLOUD_HTTP_BODY_MAX,
                             &http_status);
    if (result == 0) {
        result = cloud_parse_business(response, &root, &data, &business_code);
    }
    if (!cJSON_IsObject(data)) {
        data = root;
    }
    peer_id = cloud_json_string(data, "peer_id");
    token = cloud_json_string(data, "token");
    role_id = cloud_json_string(data, "role_id");
    printf("[tirtc_cloud] ai token response result=%d http=%d code=%d "
           "peer_len=%u/%u token_len=%u/%u role_len=%u/%u\n",
           result, http_status, business_code,
           (unsigned)strlen(peer_id), (unsigned)sizeof(session.peer_id),
           (unsigned)strlen(token), (unsigned)sizeof(session.token),
           (unsigned)strlen(role_id), (unsigned)sizeof(session.role_id));
    if (result != 0 || peer_id[0] == '\0' || token[0] == '\0' ||
        strlen(peer_id) >= sizeof(session.peer_id) ||
        strlen(token) >= sizeof(session.token) ||
        strlen(role_id) >= sizeof(session.role_id)) {
        result = -1;
        goto done;
    }
    session.type = TIRTC_DEMO_CLOUD_SESSION_AI;
    cloud_copy(session.peer_id, sizeof(session.peer_id), peer_id);
    cloud_copy(session.token, sizeof(session.token), token);
    cloud_copy(session.role_id, sizeof(session.role_id), role_id);
    cloud_lock(&flags);
    request_active = g_cloud.ai_token_pending &&
                     g_cloud.snapshot.session_type ==
                         TIRTC_DEMO_CLOUD_SESSION_AI &&
                     g_cloud.snapshot.call_state ==
                         TIRTC_DEMO_CLOUD_CALL_CONNECTING;
    if (request_active) {
        g_cloud.snapshot.ai_token_ready = true;
        cloud_set_status_locked(0, http_status, business_code, "AI 对讲",
                                "AI 会话凭据已就绪，正在建立连接。");
    }
    cloud_unlock(flags);
    if (request_active) {
        cloud_emit_session(&session);
        result = 0;
    } else {
        printf("[tirtc_cloud] stale ai token response ignored\n");
        result = TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }

done:
    cloud_lock(&flags);
    request_active = request_active && g_cloud.ai_token_pending &&
                     g_cloud.snapshot.session_type ==
                         TIRTC_DEMO_CLOUD_SESSION_AI &&
                     g_cloud.snapshot.call_state !=
                         TIRTC_DEMO_CLOUD_CALL_IDLE;
    if (request_active) {
        g_cloud.ai_token_pending = false;
    }
    if (result != 0 && request_active) {
        cloud_clear_call_locked("AI 会话凭据获取失败。");
        cloud_set_status_locked(result, http_status, business_code,
                                "AI 对讲", "AI 会话凭据获取失败。");
    }
    cloud_unlock(flags);
    if (root != NULL) {
        cJSON_Delete(root);
    }
    if (response != NULL) {
        memset(response, 0, TIRTC_DEMO_CLOUD_HTTP_BODY_MAX);
        free(response);
    }
    memset(headers, 0, sizeof(headers));
    memset(&session, 0, sizeof(session));
    return result;
}

static void cloud_clear_call_locked(const char *message)
{
    g_cloud.snapshot.call_state = TIRTC_DEMO_CLOUD_CALL_IDLE;
    g_cloud.snapshot.incoming_call = false;
    g_cloud.snapshot.call_active = false;
    g_cloud.snapshot.wechat_incoming = false;
    g_cloud.snapshot.ai_token_ready = false;
    g_cloud.snapshot.session_type = TIRTC_DEMO_CLOUD_SESSION_NONE;
    g_cloud.snapshot.room_id[0] = '\0';
    g_cloud.snapshot.peer_id[0] = '\0';
    g_cloud.call_is_caller = false;
    g_cloud.device_info_pending = false;
    g_cloud.wechat_outgoing_waiting = false;
    g_cloud.call_deadline_ms = 0U;
    cloud_set_status_locked(0, 0, 200, "会话", message);
}

static void cloud_publish_ack(tirtc_demo_cloud_mqtt_mode_t mode)
{
    char topic[TIRTC_DEMO_CLOUD_MQTT_TOPIC_MAX];
    unsigned long flags;

    cloud_lock(&flags);
    cloud_copy(topic, sizeof(topic), g_cloud.mqtt_ack_topic);
    cloud_unlock(flags);
    if (mode != TIRTC_DEMO_CLOUD_MQTT_NONE && topic[0] != '\0') {
        (void)cloud_mqtt_publish(topic, "{\"ack\":true}", 1);
    }
}

static void cloud_handle_auth_grant(const cJSON *payload)
{
    const char *device_id = cloud_json_string(payload, "device_id");
    const char *device_secret = cloud_json_string(payload, "device_key");
    tirtc_demo_cloud_callbacks_t callbacks;
    char saved_id[TIRTC_DEMO_CLOUD_DEVICE_ID_MAX];
    char saved_secret[TIRTC_DEMO_CLOUD_DEVICE_SECRET_MAX];
    bool received_identity;
    unsigned long flags;
    int save_result = 0;
    int ack_result;

    cloud_lock(&flags);
    callbacks = g_cloud.callbacks;
    cloud_copy(saved_id, sizeof(saved_id), g_cloud.device_id);
    cloud_copy(saved_secret, sizeof(saved_secret), g_cloud.device_secret);
    cloud_unlock(flags);

    received_identity = cloud_identity_valid(device_id, device_secret);
    if (received_identity) {
        cloud_copy(saved_id, sizeof(saved_id), device_id);
        cloud_copy(saved_secret, sizeof(saved_secret), device_secret);
        if (callbacks.save_identity == NULL) {
            save_result = -1;
        } else {
            save_result = callbacks.save_identity(saved_id, saved_secret,
                                                  callbacks.context);
        }
    }
    if (!received_identity && !cloud_identity_valid(saved_id, saved_secret)) {
        cloud_publish_ack(TIRTC_DEMO_CLOUD_MQTT_TEMP);
        printf("[tirtc_cloud] binding auth_grant without credentials "
               "reconciling by HTTP\n");
        cloud_lock(&flags);
        g_cloud.binding_requesting = true;
        g_cloud.binding_retry_count = 0U;
        g_cloud.next_binding_retry_ms = 0U;
        g_cloud.next_binding_poll_ms = 0U;
        g_cloud.binding_retry_started_ms = cloud_now_ms();
        g_cloud.binding_deadline_ms = 0U;
        g_cloud.snapshot.binding_waiting = false;
        g_cloud.snapshot.binding_code[0] = '\0';
        cloud_set_status_locked(0, 0, 200, "设备绑定",
                                "已收到绑定授权，正在确认设备凭据。");
        cloud_unlock(flags);
        cloud_schedule(TIRTC_DEMO_CLOUD_WORK_BIND);
        memset(saved_secret, 0, sizeof(saved_secret));
        return;
    }
    if (!cloud_identity_valid(saved_id, saved_secret) || save_result != 0) {
        cloud_set_status(save_result != 0 ? save_result : -1, 0, -1,
                         "设备绑定", "绑定凭据保存失败。" );
        memset(saved_secret, 0, sizeof(saved_secret));
        return;
    }

    cloud_lock(&flags);
    cloud_copy(g_cloud.device_id, sizeof(g_cloud.device_id), saved_id);
    cloud_copy(g_cloud.device_secret, sizeof(g_cloud.device_secret), saved_secret);
    g_cloud.snapshot.identity_ready = true;
    g_cloud.auth_grant_pending = true;
    g_cloud.auth_ack_inflight = false;
    g_cloud.next_retry_ms = 0U;
    g_cloud.next_binding_poll_ms = 0U;
    g_cloud.binding_deadline_ms = cloud_now_ms() +
                                  TIRTC_DEMO_CLOUD_AUTH_ACK_TIMEOUT_MS;
    cloud_set_status_locked(0, 0, 200, "设备绑定",
                            "绑定凭据已保存，正在确认接收。" );
    cloud_unlock(flags);
    memset(saved_secret, 0, sizeof(saved_secret));
    ack_result = cloud_publish_auth_ack();
    if (ack_result != 0) {
        cloud_set_status(ack_result, 0, -1, "设备绑定",
                         "绑定凭据已保存，确认消息将自动重试。" );
    }
}

static void cloud_complete_auth_grant(void)
{
    tirtc_demo_cloud_callbacks_t callbacks;
    unsigned long flags;

    cloud_mqtt_disconnect();
    cloud_lock(&flags);
    callbacks = g_cloud.callbacks;
    memset(g_cloud.mqtt_token, 0, sizeof(g_cloud.mqtt_token));
    memset(g_cloud.temp_token, 0, sizeof(g_cloud.temp_token));
    memset(g_cloud.mqtt_password, 0, sizeof(g_cloud.mqtt_password));
    memset(g_cloud.mqtt_user, 0, sizeof(g_cloud.mqtt_user));
    memset(g_cloud.mqtt_client_id, 0, sizeof(g_cloud.mqtt_client_id));
    g_cloud.temp_client_id[0] = '\0';
    g_cloud.snapshot.identity_ready = true;
    g_cloud.snapshot.token_ready = false;
    g_cloud.token_auth_failure_wall = 0;
    g_cloud.snapshot.binding_waiting = false;
    g_cloud.snapshot.binding_code[0] = '\0';
    g_cloud.auth_grant_pending = false;
    g_cloud.auth_ack_inflight = false;
    g_cloud.binding_deadline_ms = 0U;
    g_cloud.next_binding_poll_ms = 0U;
    g_cloud.mqtt_wanted_mode = TIRTC_DEMO_CLOUD_MQTT_FORMAL;
    cloud_set_status_locked(0, 0, 200, "设备绑定",
                            "设备绑定成功，正在切换正式服务。" );
    cloud_unlock(flags);
    if (callbacks.identity_activated != NULL) {
        callbacks.identity_activated(callbacks.context);
    }
    cloud_schedule(TIRTC_DEMO_CLOUD_WORK_START);
}

static bool cloud_room_matches_locked(const char *room_id)
{
    return room_id != NULL && room_id[0] != '\0' &&
           strcmp(g_cloud.snapshot.room_id, room_id) == 0;
}

static void cloud_process_mqtt_message(void)
{
    char topic[TIRTC_DEMO_CLOUD_MQTT_TOPIC_MAX];
    char payload[TIRTC_DEMO_CLOUD_MQTT_RX_MAX + 1U];
    tirtc_demo_cloud_mqtt_mode_t mode;
    size_t payload_length;
    cJSON *root = NULL;
    cJSON *event_payload;
    const char *type;
    const char *channel;
    bool wechat_join;
    bool wechat_cancel;
    unsigned long flags;

    cloud_lock(&flags);
    if (g_cloud.mqtt_rx_count == 0U) {
        cloud_unlock(flags);
        return;
    }
    cloud_copy(topic, sizeof(topic),
               g_cloud.mqtt_rx_queue[g_cloud.mqtt_rx_read].topic);
    payload_length = g_cloud.mqtt_rx_queue[g_cloud.mqtt_rx_read].length;
    memcpy(payload, g_cloud.mqtt_rx_queue[g_cloud.mqtt_rx_read].payload,
           g_cloud.mqtt_rx_queue[g_cloud.mqtt_rx_read].length + 1U);
    mode = g_cloud.mqtt_mode;
    memset(&g_cloud.mqtt_rx_queue[g_cloud.mqtt_rx_read], 0,
           sizeof(g_cloud.mqtt_rx_queue[g_cloud.mqtt_rx_read]));
    g_cloud.mqtt_rx_read = (uint8_t)((g_cloud.mqtt_rx_read + 1U) %
                                     TIRTC_DEMO_CLOUD_MQTT_QUEUE_DEPTH);
    --g_cloud.mqtt_rx_count;
    if (g_cloud.mqtt_rx_count > 0U) {
        g_cloud.work |= TIRTC_DEMO_CLOUD_WORK_MQTT_RX;
    }
    cloud_unlock(flags);

    printf("[tirtc_cloud] mqtt rx message mode=%d topic=%s bytes=%u\n",
           (int)mode, topic, (unsigned int)payload_length);
    root = cJSON_Parse(payload);
    memset(payload, 0, sizeof(payload));
    if (root == NULL) {
        cloud_set_status(-1, 0, -1, "消息服务", "收到无法解析的业务消息。" );
        return;
    }
    type = cloud_json_string(root, "type");
    channel = cloud_json_string(root, "channel");
    event_payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (!cJSON_IsObject(event_payload)) {
        event_payload = cJSON_GetObjectItemCaseSensitive(root, "msg");
    }
    wechat_join = strcmp(type, "wx_join_voip_room") == 0 ||
                  strcmp(type, "wxa_join_voip_room") == 0 ||
                  (strcmp(type, "call_incoming") == 0 &&
                   (strcmp(channel, "wx") == 0 ||
                    cloud_wechat_join_payload(event_payload)));
    wechat_cancel = strcmp(type, "wx_user_cancel") == 0 ||
                    strcmp(type, "wxa_user_cancel") == 0 ||
                    strcmp(type, "call_cancel") == 0;
    if (strstr(topic, "/cmd") != NULL &&
        !(mode == TIRTC_DEMO_CLOUD_MQTT_TEMP &&
          strcmp(type, "auth_grant") == 0)) {
        cloud_publish_ack(mode);
    }

    if (mode == TIRTC_DEMO_CLOUD_MQTT_TEMP &&
        strcmp(type, "auth_grant") == 0) {
        printf("[tirtc_cloud] binding phase=auth-grant next=ack-puback\n");
        printf("[tirtc_cloud] binding auth_grant received payload=%s\n",
               cJSON_IsObject(event_payload) ? "object" : "bare");
        cloud_handle_auth_grant(cJSON_IsObject(event_payload) ?
                                    event_payload : root);
        cJSON_Delete(root);
        return;
    }
    if (strcmp(type, "unbind") == 0) {
        cloud_mqtt_disconnect();
        cloud_lock(&flags);
        memset(g_cloud.mqtt_token, 0, sizeof(g_cloud.mqtt_token));
        g_cloud.auth_grant_pending = false;
        g_cloud.auth_ack_inflight = false;
        g_cloud.snapshot.token_ready = false;
        cloud_set_status_locked(-6006, 0, 6006, "设备绑定",
                                "设备已解绑，正在重新申请绑定验证码。" );
        cloud_unlock(flags);
        cloud_schedule(TIRTC_DEMO_CLOUD_WORK_BIND);
    } else if (wechat_join && cJSON_IsObject(event_payload)) {
        tirtc_demo_cloud_session_t *session = calloc(1U, sizeof(*session));
        const char *peer_id = cloud_json_string(event_payload, "peer_id");
        const char *token = cloud_json_string(event_payload, "token");
        const char *room_id = cloud_json_string_alias(
            event_payload, "wx_room_id", "wxa_room_id", "room_id");
        const char *app_id = cloud_json_string_alias(
            event_payload, "wx_app_id", "wxa_app_id", NULL);
        const char *model_id = cloud_json_string_alias(
            event_payload, "wx_model_id", "wxa_model_id", NULL);
        const char *open_id = cloud_json_string_alias4(
            event_payload, "wx_user_openid", "wxa_user_openid",
            "wx_open_id", "wxa_open_id");
        const char *session_token = cloud_json_string_alias5(
            event_payload, "wx_session_token", "wxa_session_token",
            "wx_server_token", "wxa_server_token", "wx_session_key");
        const char *wx_payload = cloud_json_string_alias(
            event_payload, "wx_payload", "wxa_payload", "payload");
        bool outgoing;
        bool busy;

        if (session == NULL) {
            cloud_set_status(-1, 0, -1, "微信通话",
                             "微信入会参数内存不足。");
            cJSON_Delete(root);
            return;
        }
        if (peer_id[0] == '\0' || token[0] == '\0' ||
            strlen(peer_id) >= sizeof(session->peer_id) ||
            strlen(token) >= sizeof(session->token) ||
            strlen(room_id) >= sizeof(session->room_id) ||
            strlen(app_id) >= sizeof(session->wx_app_id) ||
            strlen(model_id) >= sizeof(session->wx_model_id) ||
            strlen(open_id) >= sizeof(session->wx_open_id) ||
            strlen(session_token) >= sizeof(session->wx_session_token) ||
            strlen(wx_payload) >= sizeof(session->wx_payload)) {
            printf("[tirtc_cloud] wechat join rejected invalid lengths "
                   "peer=%u/%u token=%u/%u room=%u session=%u/%u payload=%u/%u\n",
                   (unsigned)strlen(peer_id), (unsigned)sizeof(session->peer_id),
                   (unsigned)strlen(token), (unsigned)sizeof(session->token),
                   (unsigned)strlen(room_id),
                   (unsigned)strlen(session_token),
                   (unsigned)sizeof(session->wx_session_token),
                   (unsigned)strlen(wx_payload),
                   (unsigned)sizeof(session->wx_payload));
            cloud_set_status(-1, 0, -1, "微信通话",
                             "微信入会凭据缺失或长度超限。");
            memset(session, 0, sizeof(*session));
            free(session);
            cJSON_Delete(root);
            return;
        }
        session->type = TIRTC_DEMO_CLOUD_SESSION_WECHAT;
        cloud_copy(session->peer_id, sizeof(session->peer_id), peer_id);
        cloud_copy(session->token, sizeof(session->token), token);
        cloud_copy(session->room_id, sizeof(session->room_id), room_id);
        cloud_copy(session->wx_app_id, sizeof(session->wx_app_id), app_id);
        cloud_copy(session->wx_model_id, sizeof(session->wx_model_id), model_id);
        cloud_copy(session->wx_open_id, sizeof(session->wx_open_id), open_id);
        cloud_copy(session->wx_session_token,
                   sizeof(session->wx_session_token), session_token);
        cloud_copy(session->wx_payload, sizeof(session->wx_payload), wx_payload);
        if (session->wx_app_id[0] == '\0') {
            cloud_query_value(peer_id, "x_wx_app_id", session->wx_app_id,
                              sizeof(session->wx_app_id));
            if (session->wx_app_id[0] == '\0') {
                cloud_query_value(peer_id, "x_wxa_app_id", session->wx_app_id,
                                  sizeof(session->wx_app_id));
            }
        }
        if (session->wx_model_id[0] == '\0') {
            cloud_query_value(peer_id, "x_wx_model_id", session->wx_model_id,
                              sizeof(session->wx_model_id));
            if (session->wx_model_id[0] == '\0') {
                cloud_query_value(peer_id, "x_wxa_model_id",
                                  session->wx_model_id,
                                  sizeof(session->wx_model_id));
            }
        }
        cloud_lock(&flags);
        outgoing = g_cloud.wechat_outgoing_waiting;
        busy = !outgoing &&
               g_cloud.snapshot.call_state != TIRTC_DEMO_CLOUD_CALL_IDLE;
        if (outgoing) {
            g_cloud.wechat_outgoing_waiting = false;
        }
        session->incoming = !outgoing;
        if (!busy && session->peer_id[0] != '\0' &&
            session->token[0] != '\0') {
            g_cloud.snapshot.session_type = TIRTC_DEMO_CLOUD_SESSION_WECHAT;
            g_cloud.snapshot.incoming_call = session->incoming;
            g_cloud.snapshot.wechat_incoming = session->incoming;
            g_cloud.snapshot.call_active = false;
            g_cloud.snapshot.call_state = session->incoming ?
                TIRTC_DEMO_CLOUD_CALL_INCOMING :
                TIRTC_DEMO_CLOUD_CALL_CONNECTING;
            cloud_copy(g_cloud.snapshot.peer_id,
                       sizeof(g_cloud.snapshot.peer_id), session->peer_id);
            cloud_copy(g_cloud.snapshot.room_id,
                       sizeof(g_cloud.snapshot.room_id), session->room_id);
            cloud_set_status_locked(0, 0, 0, "微信通话",
                                    session->incoming ?
                                        "收到微信来电，请选择接听或拒绝。" :
                                        "微信入会参数已就绪，正在建立会话。");
        }
        cloud_unlock(flags);
        printf("[tirtc_cloud] wechat join ready incoming=%d busy=%d "
               "peer_len=%u token_len=%u room_len=%u app_len=%u model_len=%u "
               "session_token_len=%u\n",
               session->incoming ? 1 : 0, busy ? 1 : 0,
               (unsigned)strlen(session->peer_id),
               (unsigned)strlen(session->token),
               (unsigned)strlen(session->room_id),
               (unsigned)strlen(session->wx_app_id),
               (unsigned)strlen(session->wx_model_id),
               (unsigned)strlen(session->wx_session_token));
        if (session->peer_id[0] != '\0' && session->token[0] != '\0') {
            cloud_emit_session(session);
        }
        memset(session, 0, sizeof(*session));
        free(session);
    } else if (wechat_cancel) {
        const char *room_id = cloud_json_string_alias(
            event_payload, "wx_room_id", "wxa_room_id", "room_id");
        bool matched;

        if (room_id[0] == '\0') {
            room_id = cloud_json_string_alias(root, "wx_room_id",
                                              "wxa_room_id", "room_id");
        }
        cloud_lock(&flags);
        matched = g_cloud.snapshot.session_type ==
                      TIRTC_DEMO_CLOUD_SESSION_WECHAT &&
                  (room_id[0] == '\0' ||
                   g_cloud.snapshot.room_id[0] == '\0' ||
                   strcmp(g_cloud.snapshot.room_id, room_id) == 0);
        if (matched) {
            cloud_clear_call_locked("微信用户已取消通话。");
        }
        cloud_unlock(flags);
        printf("[tirtc_cloud] wechat cancel room_len=%u matched=%d\n",
               (unsigned)strlen(room_id), matched ? 1 : 0);
    } else if ((channel[0] == '\0' || strcmp(channel, "device") == 0) &&
               strcmp(type, "call_incoming") == 0 &&
               cJSON_IsObject(event_payload)) {
        const char *room_id = cloud_json_string(event_payload, "room_id");
        const char *caller_id = cloud_json_string(event_payload, "caller_id");
        bool busy = false;
        if (room_id[0] != '\0' && caller_id[0] != '\0' &&
            strlen(room_id) < TIRTC_DEMO_CLOUD_ROOM_ID_MAX &&
            cloud_target_valid(caller_id)) {
            cloud_lock(&flags);
            busy = g_cloud.snapshot.call_state != TIRTC_DEMO_CLOUD_CALL_IDLE;
            if (!busy) {
                cloud_copy(g_cloud.snapshot.room_id,
                           sizeof(g_cloud.snapshot.room_id), room_id);
                cloud_copy(g_cloud.snapshot.peer_id,
                           sizeof(g_cloud.snapshot.peer_id), caller_id);
                g_cloud.snapshot.session_type =
                    TIRTC_DEMO_CLOUD_SESSION_DEVICE;
                g_cloud.snapshot.call_state = TIRTC_DEMO_CLOUD_CALL_INCOMING;
                g_cloud.snapshot.incoming_call = true;
                g_cloud.snapshot.call_active = false;
                g_cloud.call_is_caller = false;
                cloud_set_status_locked(0, 0, 200, "设备来电",
                                        "收到设备来电，请选择接听或拒绝。" );
            }
            cloud_unlock(flags);
            printf("[tirtc_cloud] device incoming room_len=%u caller_len=%u "
                   "busy=%d\n",
                   (unsigned)strlen(room_id), (unsigned)strlen(caller_id),
                   busy ? 1 : 0);
            if (busy) {
                (void)cloud_post_room_action_now(
                    TIRTC_DEMO_CLOUD_ROOM_REJECT, room_id, "busy");
            }
        }
    } else if (strcmp(channel, "device") == 0 &&
               strcmp(type, "room_cancel") == 0 && cJSON_IsObject(event_payload)) {
        const char *room_id = cloud_json_string(event_payload, "room_id");
        bool matched;

        cloud_lock(&flags);
        matched = cloud_room_matches_locked(room_id);
        if (matched) {
            cloud_clear_call_locked("对方已取消呼叫。" );
        }
        cloud_unlock(flags);
        printf("[tirtc_cloud] device room_cancel room_len=%u matched=%d\n",
               (unsigned)strlen(room_id), matched ? 1 : 0);
    } else if (strcmp(channel, "device") == 0 &&
               strcmp(type, "call_reject") == 0 && cJSON_IsObject(event_payload)) {
        const char *room_id = cloud_json_string(event_payload, "room_id");
        bool matched;

        cloud_lock(&flags);
        matched = cloud_room_matches_locked(room_id);
        if (matched) {
            cloud_clear_call_locked("对方已拒绝呼叫。" );
        }
        cloud_unlock(flags);
        printf("[tirtc_cloud] device call_reject room_len=%u matched=%d\n",
               (unsigned)strlen(room_id), matched ? 1 : 0);
    } else if (strcmp(channel, "device") == 0 &&
               strcmp(type, "callee_answered") == 0 && cJSON_IsObject(event_payload)) {
        const char *room_id = cloud_json_string(event_payload, "room_id");
        const char *callee_id = cloud_json_string(event_payload, "callee_id");
        bool fetch_info = false;
        cloud_lock(&flags);
        if (cloud_room_matches_locked(room_id) && g_cloud.call_is_caller) {
            if (cloud_target_valid(callee_id)) {
                cloud_copy(g_cloud.snapshot.peer_id,
                           sizeof(g_cloud.snapshot.peer_id), callee_id);
            }
            g_cloud.snapshot.call_state = TIRTC_DEMO_CLOUD_CALL_CONNECTING;
            if (!g_cloud.device_info_pending) {
                g_cloud.device_info_pending = true;
                fetch_info = true;
            }
            cloud_set_status_locked(0, 0, 200, "设备呼叫",
                                    "对方已接听，正在建立 TiRTC 连接。" );
        }
        cloud_unlock(flags);
        printf("[tirtc_cloud] device callee_answered room_len=%u "
               "callee_len=%u fetch_info=%d\n",
               (unsigned)strlen(room_id), (unsigned)strlen(callee_id),
               fetch_info ? 1 : 0);
        if (fetch_info) {
            cloud_schedule(TIRTC_DEMO_CLOUD_WORK_DEVICE_INFO);
        }
    } else if (strcmp(channel, "device") == 0 &&
               strcmp(type, "callers_update") == 0) {
        cloud_schedule(TIRTC_DEMO_CLOUD_WORK_CONTACTS);
    } else if (strcmp(channel, "wx") == 0 &&
               strcmp(type, "callers_update") == 0) {
        cloud_schedule(TIRTC_DEMO_CLOUD_WORK_WECHAT_CONTACTS);
    }
    cJSON_Delete(root);
}

static void cloud_process_hangup(bool timed_out)
{
    char room_id[TIRTC_DEMO_CLOUD_ROOM_ID_MAX];
    bool caller;
    tirtc_demo_cloud_call_state_t state;
    unsigned long flags;

    cloud_lock(&flags);
    cloud_copy(room_id, sizeof(room_id), g_cloud.snapshot.room_id);
    caller = g_cloud.call_is_caller;
    state = g_cloud.snapshot.call_state;
    cloud_unlock(flags);
    if (room_id[0] != '\0') {
        tirtc_demo_cloud_room_action_t action = caller &&
            state != TIRTC_DEMO_CLOUD_CALL_ACTIVE ?
            TIRTC_DEMO_CLOUD_ROOM_CANCEL : TIRTC_DEMO_CLOUD_ROOM_HANGUP;
        (void)cloud_post_room_action_now(action, room_id,
                                         timed_out ? "timeout" : "local_hangup");
    }
    cloud_lock(&flags);
    cloud_clear_call_locked(timed_out ? "呼叫等待超时，已自动取消。" :
                                       "通话已结束。" );
    cloud_unlock(flags);
}

static void cloud_process_reject(void)
{
    char room_id[TIRTC_DEMO_CLOUD_ROOM_ID_MAX];
    unsigned long flags;

    cloud_lock(&flags);
    cloud_copy(room_id, sizeof(room_id), g_cloud.snapshot.room_id);
    cloud_unlock(flags);
    if (room_id[0] != '\0') {
        (void)cloud_post_room_action_now(TIRTC_DEMO_CLOUD_ROOM_REJECT,
                                         room_id, "decline");
    }
    cloud_lock(&flags);
    cloud_clear_call_locked("来电已拒绝。" );
    cloud_unlock(flags);
}

static void cloud_process_start(void)
{
    bool identity_ready;
    bool token_ready;
    bool connected;
    bool binding_pending;
    unsigned long flags;
    int result;

    if (!cloud_network_ready()) {
        cloud_set_status(TIRTC_DEMO_CLOUD_ERR_NOT_READY, 0, -1, "等待网络",
                         "请先通过君正系统设置连接 WiFi。" );
        return;
    }
    cloud_lock(&flags);
    identity_ready = g_cloud.snapshot.identity_ready;
    token_ready = g_cloud.snapshot.token_ready;
    connected = g_cloud.mqtt_connected || g_cloud.mqtt_connecting;
    binding_pending = g_cloud.binding_requesting ||
                      g_cloud.snapshot.binding_waiting;
    g_cloud.snapshot.time_ready = cloud_time_ready();
    cloud_unlock(flags);
    if (!identity_ready) {
        if (binding_pending) {
            return;
        }
        cloud_set_status(TIRTC_DEMO_CLOUD_ERR_NOT_READY, 0, -1, "设备绑定",
                         "设备尚未绑定，可点击设备绑定生成验证码。" );
        return;
    }
    if (!cloud_time_ready()) {
        cloud_set_status(TIRTC_DEMO_CLOUD_ERR_NOT_READY, 0, -1, "等待时间",
                         "正在等待君正系统时间服务完成同步。" );
        return;
    }
    if (!token_ready) {
        result = cloud_exchange_token_now();
        if (result == -6006) {
            cloud_schedule(TIRTC_DEMO_CLOUD_WORK_BIND);
            return;
        }
        if (result != 0) {
            cloud_lock(&flags);
            g_cloud.next_retry_ms = cloud_now_ms() + TIRTC_DEMO_CLOUD_RETRY_MS;
            cloud_unlock(flags);
            return;
        }
    }
    if (!connected) {
        result = cloud_mqtt_connect(TIRTC_DEMO_CLOUD_MQTT_FORMAL);
        if (result != 0) {
            cloud_set_status(result, 0, -1, "消息服务",
                             "MQTT 连接建立失败，将自动重试。" );
        }
    }
}

static void cloud_publish_heartbeat(void)
{
    char topic[TIRTC_DEMO_CLOUD_MQTT_TOPIC_MAX];
    char payload[160];
    uint32_t sequence;
    unsigned long flags;

    cloud_lock(&flags);
    cloud_copy(topic, sizeof(topic), g_cloud.mqtt_up_topic);
    sequence = ++g_cloud.heartbeat_seq;
    cloud_unlock(flags);
    snprintf(payload, sizeof(payload),
             "{\"type\":\"heartbeat\",\"seq\":%u,\"ts\":%ld}",
             (unsigned int)sequence, (long)time(NULL));
    (void)cloud_mqtt_publish(topic, payload, 0);
}

static void cloud_housekeeping(void)
{
    uint64_t now = cloud_now_ms();
    bool start_requested;
    bool connected;
    bool connecting;
    bool token_ready;
    bool binding_waiting;
    bool binding_requesting;
    bool auth_grant_pending;
    bool auth_ack_inflight;
    bool call_expired;
    tirtc_demo_cloud_mqtt_mode_t wanted_mode;
    uint64_t next_retry;
    uint64_t next_binding_retry;
    uint64_t next_binding_poll;
    uint64_t next_heartbeat;
    uint64_t binding_deadline;
    time_t token_auth_failure_wall;
    unsigned long flags;

    cloud_lock(&flags);
    g_cloud.snapshot.time_ready = cloud_time_ready();
    start_requested = g_cloud.start_requested;
    connected = g_cloud.mqtt_connected;
    connecting = g_cloud.mqtt_connecting;
    token_ready = g_cloud.snapshot.token_ready;
    binding_waiting = g_cloud.snapshot.binding_waiting;
    binding_requesting = g_cloud.binding_requesting;
    auth_grant_pending = g_cloud.auth_grant_pending;
    auth_ack_inflight = g_cloud.auth_ack_inflight;
    wanted_mode = g_cloud.mqtt_wanted_mode;
    next_retry = g_cloud.next_retry_ms;
    next_binding_retry = g_cloud.next_binding_retry_ms;
    next_binding_poll = g_cloud.next_binding_poll_ms;
    next_heartbeat = g_cloud.next_heartbeat_ms;
    binding_deadline = g_cloud.binding_deadline_ms;
    token_auth_failure_wall = g_cloud.token_auth_failure_wall;
    call_expired = g_cloud.call_deadline_ms != 0U &&
                   now >= g_cloud.call_deadline_ms &&
                   g_cloud.snapshot.call_state != TIRTC_DEMO_CLOUD_CALL_ACTIVE;
    cloud_unlock(flags);

    if (!token_ready && next_retry != 0U && token_auth_failure_wall != 0) {
        int64_t wall_delta = (int64_t)time(NULL) -
                             (int64_t)token_auth_failure_wall;

        if (wall_delta > 60 || wall_delta < -60) {
            bool accelerate = false;

            cloud_lock(&flags);
            if (!g_cloud.snapshot.token_ready &&
                g_cloud.token_auth_failure_wall == token_auth_failure_wall) {
                g_cloud.token_auth_failure_wall = 0;
                g_cloud.next_retry_ms = 0U;
                accelerate = true;
            }
            cloud_unlock(flags);
            if (accelerate) {
                printf("[tirtc_cloud] token retry accelerated after clock correction delta=%llds\n",
                       (long long)wall_delta);
                cloud_process_start();
                return;
            }
        }
    }

    if (call_expired) {
        cloud_process_hangup(true);
    }
    if (binding_waiting && binding_deadline != 0U && now >= binding_deadline) {
        cloud_mqtt_disconnect();
        cloud_lock(&flags);
        g_cloud.snapshot.binding_waiting = false;
        g_cloud.snapshot.binding_code[0] = '\0';
        g_cloud.auth_grant_pending = false;
        g_cloud.auth_ack_inflight = false;
        g_cloud.binding_deadline_ms = 0U;
        g_cloud.next_binding_poll_ms = 0U;
        cloud_set_status_locked(-1, 0, -1, "设备绑定",
                                "绑定验证码已过期，请重新申请。" );
        cloud_unlock(flags);
        return;
    }
    if (binding_requesting && next_binding_retry != 0U &&
        now >= next_binding_retry && cloud_network_ready()) {
        cloud_lock(&flags);
        if (g_cloud.binding_requesting &&
            g_cloud.next_binding_retry_ms == next_binding_retry) {
            g_cloud.next_binding_retry_ms = 0U;
            g_cloud.work |= TIRTC_DEMO_CLOUD_WORK_BIND;
        }
        cloud_unlock(flags);
        printf("[tirtc_cloud] binding retry dispatched\n");
        return;
    }
    if (binding_waiting && !auth_grant_pending &&
        next_binding_poll != 0U && now >= next_binding_poll &&
        cloud_network_ready()) {
        cloud_lock(&flags);
        if (g_cloud.snapshot.binding_waiting &&
            !g_cloud.auth_grant_pending &&
            g_cloud.next_binding_poll_ms == next_binding_poll) {
            g_cloud.next_binding_poll_ms = now +
                                           TIRTC_DEMO_CLOUD_BINDING_POLL_MS;
            g_cloud.work |= TIRTC_DEMO_CLOUD_WORK_BIND_POLL;
        }
        cloud_unlock(flags);
        printf("[tirtc_cloud] binding HTTP poll dispatched\n");
        return;
    }
    if (connected && wanted_mode == TIRTC_DEMO_CLOUD_MQTT_FORMAL &&
        now >= next_heartbeat) {
        cloud_publish_heartbeat();
        cloud_lock(&flags);
        g_cloud.next_heartbeat_ms = now + TIRTC_DEMO_CLOUD_HEARTBEAT_MS;
        cloud_unlock(flags);
    }
    if (connected && wanted_mode == TIRTC_DEMO_CLOUD_MQTT_TEMP &&
        auth_grant_pending && !auth_ack_inflight &&
        (next_retry == 0U || now >= next_retry)) {
        (void)cloud_publish_auth_ack();
    }
    if (!connected && !connecting && cloud_network_ready() &&
        (next_retry == 0U || now >= next_retry)) {
        if (binding_waiting && wanted_mode == TIRTC_DEMO_CLOUD_MQTT_TEMP) {
            (void)cloud_mqtt_connect(TIRTC_DEMO_CLOUD_MQTT_TEMP);
        } else if (start_requested) {
            cloud_process_start();
        }
    }
}

static void cloud_worker(void *data)
{
    (void)data;
    for (;;) {
        uint32_t work = cloud_take_work();

        if ((work & TIRTC_DEMO_CLOUD_WORK_RECONFIGURE) != 0U) {
            cloud_mqtt_disconnect();
        }
        if ((work & TIRTC_DEMO_CLOUD_WORK_BIND) != 0U) {
            int binding_result;

            printf("[tirtc_cloud] binding work begin\n");
            binding_result = cloud_report_binding_now();
            printf("[tirtc_cloud] binding work done result=%d\n",
                   binding_result);
            cloud_handle_binding_result(binding_result);
        }
        if ((work & TIRTC_DEMO_CLOUD_WORK_BIND_POLL) != 0U) {
            int poll_result = cloud_poll_binding_now();

            printf("[tirtc_cloud] binding HTTP poll done result=%d\n",
                   poll_result);
        }
        if ((work & TIRTC_DEMO_CLOUD_WORK_START) != 0U) {
            cloud_process_start();
        }
        if ((work & TIRTC_DEMO_CLOUD_WORK_MQTT_RX) != 0U) {
            cloud_process_mqtt_message();
        }
        if ((work & TIRTC_DEMO_CLOUD_WORK_AUTH_COMPLETE) != 0U) {
            cloud_complete_auth_grant();
        }
        if ((work & TIRTC_DEMO_CLOUD_WORK_CONTACTS) != 0U) {
            (void)cloud_refresh_contacts_now();
        }
        if ((work & TIRTC_DEMO_CLOUD_WORK_CONTACT_REQUEST) != 0U) {
            char target[TIRTC_DEMO_CLOUD_DEVICE_ID_MAX];
            unsigned long flags;

            cloud_lock(&flags);
            cloud_copy(target, sizeof(target), g_cloud.pending_contact_id);
            g_cloud.contact_request_pending = false;
            cloud_unlock(flags);
            (void)cloud_request_contact_now(target);
        }
        if ((work & TIRTC_DEMO_CLOUD_WORK_CALL) != 0U) {
            char target[TIRTC_DEMO_CLOUD_DEVICE_ID_MAX];
            bool pending;
            unsigned long flags;

            cloud_lock(&flags);
            pending = g_cloud.call_request_pending;
            if (pending) {
                cloud_copy(target, sizeof(target), g_cloud.pending_call_id);
                g_cloud.call_request_pending = false;
            }
            cloud_unlock(flags);
            if (pending) {
                (void)cloud_request_call_now(target);
            }
        }
        if ((work & TIRTC_DEMO_CLOUD_WORK_DEVICE_INFO) != 0U) {
            bool pending;
            unsigned long flags;

            cloud_lock(&flags);
            pending = g_cloud.device_info_pending;
            cloud_unlock(flags);
            if (pending) {
                (void)cloud_fetch_device_info_now();
            }
        }
        if ((work & TIRTC_DEMO_CLOUD_WORK_WECHAT_PROFILE) != 0U) {
            (void)cloud_refresh_wechat_profile_now();
        }
        if ((work & TIRTC_DEMO_CLOUD_WORK_WECHAT_CONTACTS) != 0U) {
            (void)cloud_refresh_wechat_contacts_now();
        }
        if ((work & TIRTC_DEMO_CLOUD_WORK_WECHAT_CALL) != 0U) {
            bool pending;
            unsigned long flags;

            cloud_lock(&flags);
            pending = g_cloud.wechat_call_pending;
            cloud_unlock(flags);
            if (pending) {
                (void)cloud_request_wechat_call_now();
            }
        }
        if ((work & TIRTC_DEMO_CLOUD_WORK_WECHAT_ADD) != 0U) {
            (void)cloud_wechat_contact_op_now(true);
        }
        if ((work & TIRTC_DEMO_CLOUD_WORK_WECHAT_DELETE) != 0U) {
            (void)cloud_wechat_contact_op_now(false);
        }
        if ((work & TIRTC_DEMO_CLOUD_WORK_AI_TOKEN) != 0U) {
            bool pending;
            unsigned long flags;

            cloud_lock(&flags);
            pending = g_cloud.ai_token_pending;
            cloud_unlock(flags);
            if (pending) {
                (void)cloud_request_ai_token_now();
            }
        }
        if ((work & TIRTC_DEMO_CLOUD_WORK_REJECT) != 0U) {
            cloud_process_reject();
        }
        if ((work & TIRTC_DEMO_CLOUD_WORK_HANGUP) != 0U) {
            cloud_process_hangup(false);
        }
        cloud_housekeeping();
        (void)thread_wait_timeout(TIRTC_DEMO_CLOUD_POLL_MS);
    }
}

static int cloud_ensure_worker(void)
{
    thread_ptr_t worker;
    unsigned long flags;

    cloud_lock(&flags);
    worker = g_cloud.worker;
    cloud_unlock(flags);
    if (worker != NULL) {
        return 0;
    }
    worker = thread_create("tirtc_cloud", TIRTC_DEMO_CLOUD_WORKER_STACK,
                           cloud_worker, NULL);
    if (worker == NULL) {
        printf("[tirtc_cloud] worker create failed stack=%u\n",
               (unsigned int)TIRTC_DEMO_CLOUD_WORKER_STACK);
        return -1;
    }
    cloud_lock(&flags);
    if (g_cloud.worker == NULL) {
        g_cloud.worker = worker;
        worker = NULL;
    }
    cloud_unlock(flags);
    if (worker != NULL) {
        thread_delete(worker);
    } else {
        printf("[tirtc_cloud] worker started stack=%u\n",
               (unsigned int)TIRTC_DEMO_CLOUD_WORKER_STACK);
    }
    return 0;
}

int tirtc_demo_cloud_init(const tirtc_demo_cloud_callbacks_t *callbacks)
{
    unsigned long flags;

    cloud_lock(&flags);
    if (callbacks != NULL) {
        g_cloud.callbacks = *callbacks;
    } else {
        memset(&g_cloud.callbacks, 0, sizeof(g_cloud.callbacks));
    }
    g_cloud.initialized = true;
    cloud_unlock(flags);
    return 0;
}

int tirtc_demo_cloud_start(void)
{
    unsigned long flags;
    int result;

    if (!TIRTC_DEMO_CLOUD_ENABLE) {
        return TIRTC_DEMO_CLOUD_ERR_UNSUPPORTED;
    }
    result = cloud_ensure_worker();
    if (result != 0) {
        cloud_set_status(result, 0, -1, "业务线程", "设备业务线程创建失败。" );
        return result;
    }
    cloud_lock(&flags);
    g_cloud.start_requested = true;
    cloud_unlock(flags);
    cloud_schedule(TIRTC_DEMO_CLOUD_WORK_START);
    return 0;
}

int tirtc_demo_cloud_apply_identity(const char *device_id,
                                    const char *device_secret)
{
    bool empty = (device_id == NULL || device_id[0] == '\0') &&
                 (device_secret == NULL || device_secret[0] == '\0');
    unsigned long flags;

    if (!empty && !cloud_identity_valid(device_id, device_secret)) {
        return -1;
    }
    cloud_lock(&flags);
    memset(g_cloud.device_id, 0, sizeof(g_cloud.device_id));
    memset(g_cloud.device_secret, 0, sizeof(g_cloud.device_secret));
    cloud_copy(g_cloud.device_id, sizeof(g_cloud.device_id),
               empty ? "" : device_id);
    cloud_copy(g_cloud.device_secret, sizeof(g_cloud.device_secret),
               empty ? "" : device_secret);
    memset(g_cloud.mqtt_token, 0, sizeof(g_cloud.mqtt_token));
    memset(g_cloud.temp_token, 0, sizeof(g_cloud.temp_token));
    memset(g_cloud.pending_contact_id, 0, sizeof(g_cloud.pending_contact_id));
    memset(g_cloud.pending_call_id, 0, sizeof(g_cloud.pending_call_id));
    memset(g_cloud.pending_wechat_open_id, 0,
           sizeof(g_cloud.pending_wechat_open_id));
    memset(g_cloud.pending_wechat_app_id, 0,
           sizeof(g_cloud.pending_wechat_app_id));
    memset(g_cloud.pending_wechat_model_id, 0,
           sizeof(g_cloud.pending_wechat_model_id));
    g_cloud.auth_grant_pending = false;
    g_cloud.auth_ack_inflight = false;
    g_cloud.binding_requesting = false;
    g_cloud.binding_retry_count = 0U;
    g_cloud.next_binding_retry_ms = 0U;
    g_cloud.next_binding_poll_ms = 0U;
    g_cloud.binding_retry_started_ms = 0U;
    g_cloud.binding_deadline_ms = 0U;
    g_cloud.token_auth_failure_wall = 0;
    g_cloud.contact_request_pending = false;
    g_cloud.call_request_pending = false;
    g_cloud.wechat_call_pending = false;
    g_cloud.wechat_contact_op_pending = false;
    g_cloud.ai_token_pending = false;
    g_cloud.snapshot.contacts_ready = false;
    g_cloud.snapshot.contact_count = 0U;
    memset(g_cloud.snapshot.contacts, 0, sizeof(g_cloud.snapshot.contacts));
    g_cloud.snapshot.wechat_ready = false;
    g_cloud.snapshot.wechat_contacts_ready = false;
    g_cloud.snapshot.wechat_contact_count = 0U;
    memset(g_cloud.snapshot.wechat_contacts, 0,
           sizeof(g_cloud.snapshot.wechat_contacts));
    cloud_clear_call_locked("设备身份已更新，正在重新连接业务服务。");
    g_cloud.snapshot.identity_ready = !empty;
    g_cloud.snapshot.token_ready = false;
    g_cloud.snapshot.mqtt_connected = false;
    g_cloud.snapshot.binding_waiting = false;
    g_cloud.snapshot.binding_code[0] = '\0';
    g_cloud.temp_client_id[0] = '\0';
    g_cloud.mqtt_wanted_mode = TIRTC_DEMO_CLOUD_MQTT_FORMAL;
    g_cloud.next_retry_ms = 0U;
    cloud_unlock(flags);
    cloud_schedule(TIRTC_DEMO_CLOUD_WORK_RECONFIGURE |
                   TIRTC_DEMO_CLOUD_WORK_START);
    return 0;
}

int tirtc_demo_cloud_request_binding(void)
{
    uint64_t now;
    bool auth_pending = false;
    unsigned long flags;

    if (cloud_ensure_worker() != 0) {
        printf("[tirtc_cloud] binding request rejected worker unavailable\n");
        return -1;
    }
    now = cloud_now_ms();
    cloud_lock(&flags);
    if (g_cloud.snapshot.binding_waiting) {
        auth_pending = g_cloud.auth_grant_pending;
        if (!auth_pending) {
            g_cloud.next_binding_poll_ms = now +
                                           TIRTC_DEMO_CLOUD_BINDING_POLL_MS;
        }
        cloud_unlock(flags);
        printf("[tirtc_cloud] binding refresh existing session auth_pending=%d\n",
               auth_pending ? 1 : 0);
        if (!auth_pending) {
            cloud_schedule(TIRTC_DEMO_CLOUD_WORK_BIND_POLL);
        }
        return 0;
    }
    g_cloud.binding_requesting = true;
    g_cloud.binding_retry_count = 0U;
    g_cloud.next_binding_retry_ms = 0U;
    g_cloud.next_binding_poll_ms = 0U;
    g_cloud.binding_retry_started_ms = now;
    g_cloud.binding_deadline_ms = 0U;
    g_cloud.snapshot.binding_waiting = false;
    g_cloud.snapshot.binding_code[0] = '\0';
    g_cloud.auth_grant_pending = false;
    g_cloud.auth_ack_inflight = false;
    cloud_set_status_locked(0, 0, -1, "设备绑定",
                            "绑定申请已排队，正在连接业务服务。");
    cloud_unlock(flags);
    printf("[tirtc_cloud] binding request queued\n");
    cloud_schedule(TIRTC_DEMO_CLOUD_WORK_RECONFIGURE |
                   TIRTC_DEMO_CLOUD_WORK_BIND);
    return 0;
}

int tirtc_demo_cloud_refresh_contacts(void)
{
    tirtc_demo_cloud_snapshot_t snapshot;

    tirtc_demo_cloud_get_snapshot(&snapshot);
    if (!snapshot.token_ready) {
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    cloud_schedule(TIRTC_DEMO_CLOUD_WORK_CONTACTS);
    return 0;
}

int tirtc_demo_cloud_request_contact(const char *target_device_id)
{
    unsigned long flags;

    if (!cloud_target_valid(target_device_id)) {
        return -1;
    }
    cloud_lock(&flags);
    if (!g_cloud.snapshot.token_ready || g_cloud.contact_request_pending) {
        cloud_unlock(flags);
        return g_cloud.contact_request_pending ? TIRTC_DEMO_CLOUD_ERR_BUSY :
                                                 TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    cloud_copy(g_cloud.pending_contact_id,
               sizeof(g_cloud.pending_contact_id), target_device_id);
    g_cloud.contact_request_pending = true;
    cloud_set_status_locked(0, 0, -1, "联系人", "正在发送联系人申请。" );
    cloud_unlock(flags);
    cloud_schedule(TIRTC_DEMO_CLOUD_WORK_CONTACT_REQUEST);
    return 0;
}

int tirtc_demo_cloud_call(const char *target_device_id)
{
    unsigned long flags;

    if (!cloud_target_valid(target_device_id)) {
        return -1;
    }
    cloud_lock(&flags);
    if (!g_cloud.snapshot.mqtt_connected) {
        cloud_unlock(flags);
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    if (g_cloud.call_request_pending ||
        g_cloud.snapshot.call_state != TIRTC_DEMO_CLOUD_CALL_IDLE) {
        cloud_unlock(flags);
        return TIRTC_DEMO_CLOUD_ERR_BUSY;
    }
    cloud_copy(g_cloud.pending_call_id, sizeof(g_cloud.pending_call_id),
               target_device_id);
    cloud_copy(g_cloud.snapshot.peer_id, sizeof(g_cloud.snapshot.peer_id),
               target_device_id);
    g_cloud.call_request_pending = true;
    g_cloud.snapshot.session_type = TIRTC_DEMO_CLOUD_SESSION_DEVICE;
    g_cloud.snapshot.call_state = TIRTC_DEMO_CLOUD_CALL_OUTGOING;
    g_cloud.snapshot.incoming_call = false;
    g_cloud.snapshot.call_active = false;
    cloud_set_status_locked(0, 0, -1, "设备呼叫", "正在创建呼叫房间。" );
    cloud_unlock(flags);
    cloud_schedule(TIRTC_DEMO_CLOUD_WORK_CALL);
    return 0;
}

int tirtc_demo_cloud_accept(void)
{
    unsigned long flags;

    cloud_lock(&flags);
    if (!g_cloud.snapshot.incoming_call) {
        cloud_unlock(flags);
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    if (g_cloud.device_info_pending) {
        cloud_unlock(flags);
        return TIRTC_DEMO_CLOUD_ERR_BUSY;
    }
    g_cloud.device_info_pending = true;
    g_cloud.snapshot.call_state = TIRTC_DEMO_CLOUD_CALL_CONNECTING;
    cloud_set_status_locked(0, 0, -1, "设备来电",
                            "已接听，正在获取 TiRTC 连接凭据。");
    cloud_unlock(flags);
    cloud_schedule(TIRTC_DEMO_CLOUD_WORK_DEVICE_INFO);
    return 0;
}

int tirtc_demo_cloud_reject(void)
{
    unsigned long flags;

    cloud_lock(&flags);
    if (!g_cloud.snapshot.incoming_call || g_cloud.snapshot.room_id[0] == '\0') {
        cloud_unlock(flags);
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    cloud_unlock(flags);
    cloud_schedule(TIRTC_DEMO_CLOUD_WORK_REJECT);
    return 0;
}

int tirtc_demo_cloud_hangup(void)
{
    unsigned long flags;
    tirtc_demo_cloud_session_type_t type;
    bool device_hangup;

    cloud_lock(&flags);
    if (g_cloud.snapshot.call_state == TIRTC_DEMO_CLOUD_CALL_IDLE) {
        cloud_unlock(flags);
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    type = g_cloud.snapshot.session_type;
    device_hangup = type == TIRTC_DEMO_CLOUD_SESSION_DEVICE;
    g_cloud.call_request_pending = false;
    g_cloud.device_info_pending = false;
    g_cloud.wechat_call_pending = false;
    g_cloud.ai_token_pending = false;
    g_cloud.work &= ~(TIRTC_DEMO_CLOUD_WORK_CALL |
                      TIRTC_DEMO_CLOUD_WORK_DEVICE_INFO |
                      TIRTC_DEMO_CLOUD_WORK_WECHAT_CALL |
                      TIRTC_DEMO_CLOUD_WORK_AI_TOKEN);
    if (!device_hangup) {
        cloud_clear_call_locked("会话已结束。");
    }
    cloud_unlock(flags);
    printf("[tirtc_cloud] lifecycle hangup type=%d device_action=%d\n",
           (int)type, device_hangup ? 1 : 0);
    if (device_hangup) {
        cloud_schedule(TIRTC_DEMO_CLOUD_WORK_HANGUP);
    }
    return 0;
}

int tirtc_demo_cloud_refresh_wechat_profile(void)
{
    tirtc_demo_cloud_snapshot_t snapshot;

    tirtc_demo_cloud_get_snapshot(&snapshot);
    if (!snapshot.token_ready) {
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    cloud_schedule(TIRTC_DEMO_CLOUD_WORK_WECHAT_PROFILE);
    return 0;
}

int tirtc_demo_cloud_refresh_wechat_contacts(void)
{
    tirtc_demo_cloud_snapshot_t snapshot;

    tirtc_demo_cloud_get_snapshot(&snapshot);
    if (!snapshot.token_ready) {
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    cloud_schedule(TIRTC_DEMO_CLOUD_WORK_WECHAT_CONTACTS);
    return 0;
}

int tirtc_demo_cloud_call_wechat(const char *open_id,
                                 const char *app_id,
                                 const char *model_id)
{
    unsigned long flags;

    if (!cloud_json_value_valid(open_id, TIRTC_DEMO_CLOUD_WX_OPENID_MAX,
                                false) ||
        !cloud_json_value_valid(app_id, TIRTC_DEMO_CLOUD_WX_APP_ID_MAX,
                                true) ||
        !cloud_json_value_valid(model_id, TIRTC_DEMO_CLOUD_WX_MODEL_ID_MAX,
                                false)) {
        return -1;
    }
    cloud_lock(&flags);
    if (!g_cloud.snapshot.token_ready || !g_cloud.snapshot.mqtt_connected) {
        cloud_unlock(flags);
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    if (g_cloud.wechat_call_pending || g_cloud.wechat_outgoing_waiting ||
        g_cloud.snapshot.call_state != TIRTC_DEMO_CLOUD_CALL_IDLE) {
        cloud_unlock(flags);
        return TIRTC_DEMO_CLOUD_ERR_BUSY;
    }
    cloud_copy(g_cloud.pending_wechat_open_id,
               sizeof(g_cloud.pending_wechat_open_id), open_id);
    cloud_copy(g_cloud.pending_wechat_app_id,
               sizeof(g_cloud.pending_wechat_app_id), app_id);
    cloud_copy(g_cloud.pending_wechat_model_id,
               sizeof(g_cloud.pending_wechat_model_id), model_id);
    g_cloud.wechat_call_pending = true;
    g_cloud.snapshot.session_type = TIRTC_DEMO_CLOUD_SESSION_WECHAT;
    g_cloud.snapshot.call_state = TIRTC_DEMO_CLOUD_CALL_OUTGOING;
    cloud_set_status_locked(0, 0, -1, "微信通话", "正在发起微信呼叫。");
    cloud_unlock(flags);
    cloud_schedule(TIRTC_DEMO_CLOUD_WORK_WECHAT_CALL);
    return 0;
}

int tirtc_demo_cloud_add_wechat_contact(const char *open_id)
{
    unsigned long flags;

    if (!cloud_json_value_valid(open_id, TIRTC_DEMO_CLOUD_WX_OPENID_MAX,
                                false)) {
        return -1;
    }
    cloud_lock(&flags);
    if (!g_cloud.snapshot.token_ready) {
        cloud_unlock(flags);
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    if (g_cloud.wechat_contact_op_pending) {
        cloud_unlock(flags);
        return TIRTC_DEMO_CLOUD_ERR_BUSY;
    }
    cloud_copy(g_cloud.pending_wechat_open_id,
               sizeof(g_cloud.pending_wechat_open_id), open_id);
    g_cloud.wechat_contact_op_pending = true;
    cloud_unlock(flags);
    cloud_schedule(TIRTC_DEMO_CLOUD_WORK_WECHAT_ADD);
    return 0;
}

int tirtc_demo_cloud_delete_wechat_contact(const char *open_id)
{
    unsigned long flags;

    if (!cloud_json_value_valid(open_id, TIRTC_DEMO_CLOUD_WX_OPENID_MAX,
                                false)) {
        return -1;
    }
    cloud_lock(&flags);
    if (!g_cloud.snapshot.token_ready) {
        cloud_unlock(flags);
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    if (g_cloud.wechat_contact_op_pending) {
        cloud_unlock(flags);
        return TIRTC_DEMO_CLOUD_ERR_BUSY;
    }
    cloud_copy(g_cloud.pending_wechat_open_id,
               sizeof(g_cloud.pending_wechat_open_id), open_id);
    g_cloud.wechat_contact_op_pending = true;
    cloud_unlock(flags);
    cloud_schedule(TIRTC_DEMO_CLOUD_WORK_WECHAT_DELETE);
    return 0;
}

int tirtc_demo_cloud_request_ai_session(void)
{
    unsigned long flags;

    cloud_lock(&flags);
    if (!g_cloud.snapshot.token_ready) {
        cloud_unlock(flags);
        return TIRTC_DEMO_CLOUD_ERR_NOT_READY;
    }
    if (g_cloud.ai_token_pending || g_cloud.call_request_pending ||
        g_cloud.device_info_pending || g_cloud.wechat_call_pending ||
        g_cloud.wechat_outgoing_waiting ||
        g_cloud.snapshot.call_state != TIRTC_DEMO_CLOUD_CALL_IDLE ||
        g_cloud.snapshot.session_type != TIRTC_DEMO_CLOUD_SESSION_NONE) {
        cloud_unlock(flags);
        return TIRTC_DEMO_CLOUD_ERR_BUSY;
    }
    g_cloud.ai_token_pending = true;
    g_cloud.snapshot.ai_token_ready = false;
    g_cloud.snapshot.session_type = TIRTC_DEMO_CLOUD_SESSION_AI;
    g_cloud.snapshot.call_state = TIRTC_DEMO_CLOUD_CALL_CONNECTING;
    cloud_set_status_locked(0, 0, -1, "AI 对讲", "正在获取 AI 会话凭据。");
    cloud_unlock(flags);
    cloud_schedule(TIRTC_DEMO_CLOUD_WORK_AI_TOKEN);
    return 0;
}

void tirtc_demo_cloud_notify_rtc_connected(void)
{
    unsigned long flags;

    cloud_lock(&flags);
    if (g_cloud.snapshot.call_state == TIRTC_DEMO_CLOUD_CALL_OUTGOING ||
        g_cloud.snapshot.call_state == TIRTC_DEMO_CLOUD_CALL_CONNECTING) {
        g_cloud.snapshot.call_state = TIRTC_DEMO_CLOUD_CALL_CONNECTING;
        cloud_set_status_locked(0, 0, 200, "设备呼叫",
                                "TiRTC 对端已连接，等待房间确认。" );
    }
    cloud_unlock(flags);
}

void tirtc_demo_cloud_notify_rtc_disconnected(void)
{
    unsigned long flags;
    bool active;
    tirtc_demo_cloud_session_type_t type;

    cloud_lock(&flags);
    active = g_cloud.snapshot.call_state != TIRTC_DEMO_CLOUD_CALL_IDLE;
    type = g_cloud.snapshot.session_type;
    if (active && type != TIRTC_DEMO_CLOUD_SESSION_DEVICE) {
        cloud_clear_call_locked("会话已结束。");
    }
    cloud_unlock(flags);
    if (active && type == TIRTC_DEMO_CLOUD_SESSION_DEVICE) {
        cloud_schedule(TIRTC_DEMO_CLOUD_WORK_HANGUP);
    }
}

void tirtc_demo_cloud_notify_room_confirmed(const char *room_id)
{
    unsigned long flags;

    cloud_lock(&flags);
    if (cloud_room_matches_locked(room_id) && g_cloud.call_is_caller) {
        g_cloud.snapshot.call_state = TIRTC_DEMO_CLOUD_CALL_ACTIVE;
        g_cloud.snapshot.call_active = true;
        g_cloud.snapshot.incoming_call = false;
        g_cloud.call_deadline_ms = 0U;
        cloud_set_status_locked(0, 0, 200, "设备通话", "设备通话已建立。" );
    } else {
        cloud_set_status_locked(-1, 0, -1, "设备通话",
                                "收到的房间确认与当前呼叫不匹配。" );
    }
    cloud_unlock(flags);
}

void tirtc_demo_cloud_notify_session_connecting(
    tirtc_demo_cloud_session_type_t type)
{
    unsigned long flags;

    cloud_lock(&flags);
    if (type != TIRTC_DEMO_CLOUD_SESSION_NONE &&
        g_cloud.snapshot.session_type == type) {
        g_cloud.snapshot.call_state = TIRTC_DEMO_CLOUD_CALL_CONNECTING;
        g_cloud.snapshot.incoming_call = false;
        g_cloud.snapshot.wechat_incoming = false;
        cloud_set_status_locked(0, 0, 0,
                                type == TIRTC_DEMO_CLOUD_SESSION_WECHAT ?
                                    "微信通话" : "会话",
                                "已接听，正在建立媒体连接。");
    }
    cloud_unlock(flags);
}

void tirtc_demo_cloud_notify_session_active(tirtc_demo_cloud_session_type_t type)
{
    unsigned long flags;

    cloud_lock(&flags);
    if (type != TIRTC_DEMO_CLOUD_SESSION_NONE &&
        g_cloud.snapshot.session_type == type) {
        g_cloud.snapshot.call_state = TIRTC_DEMO_CLOUD_CALL_ACTIVE;
        g_cloud.snapshot.call_active = true;
        g_cloud.snapshot.incoming_call = false;
        g_cloud.snapshot.wechat_incoming = false;
        g_cloud.call_deadline_ms = 0U;
        cloud_set_status_locked(0, 0, type == TIRTC_DEMO_CLOUD_SESSION_WECHAT ? 0 : 200,
                                type == TIRTC_DEMO_CLOUD_SESSION_AI ?
                                    "AI 对讲" : "通话",
                                type == TIRTC_DEMO_CLOUD_SESSION_AI ?
                                    "AI 会话已建立。" : "通话已建立。");
    }
    cloud_unlock(flags);
}

void tirtc_demo_cloud_notify_session_ended(tirtc_demo_cloud_session_type_t type,
                                            const char *reason)
{
    unsigned long flags;

    cloud_lock(&flags);
    if (type == TIRTC_DEMO_CLOUD_SESSION_NONE ||
        g_cloud.snapshot.session_type == type) {
        cloud_clear_call_locked(reason != NULL && reason[0] != '\0' ?
                                    reason : "会话已结束。");
    }
    cloud_unlock(flags);
}

void tirtc_demo_cloud_get_snapshot(tirtc_demo_cloud_snapshot_t *snapshot)
{
    unsigned long flags;

    if (snapshot == NULL) {
        return;
    }
    cloud_lock(&flags);
    *snapshot = g_cloud.snapshot;
    cloud_unlock(flags);
}

void tirtc_demo_cloud_get_runtime_state(
    tirtc_demo_cloud_runtime_state_t *state)
{
    unsigned long flags;

    if (state == NULL) {
        return;
    }
    cloud_lock(&flags);
    state->mqtt_connected = g_cloud.snapshot.mqtt_connected;
    state->binding_waiting = g_cloud.snapshot.binding_waiting;
    state->session_type = g_cloud.snapshot.session_type;
    state->call_state = g_cloud.snapshot.call_state;
    cloud_unlock(flags);
}
