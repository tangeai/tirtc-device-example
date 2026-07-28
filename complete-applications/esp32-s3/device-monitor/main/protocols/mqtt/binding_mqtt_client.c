#include "binding_mqtt_client.h"

#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "binding_mqtt";

#define BINDING_MQTT_TOPIC_MAX_LEN 96
#define BINDING_MQTT_CLIENT_ID_MAX_LEN 64
#define BINDING_MQTT_PAYLOAD_MAX_LEN 512
#define BINDING_MQTT_GRANT_BIT BIT0
#define BINDING_MQTT_ERROR_BIT BIT1
#define BINDING_MQTT_WAIT_POLL_MS 500U
#define BINDING_MQTT_MIN_INTERNAL_FREE    (32U * 1024U)
#define BINDING_MQTT_MIN_INTERNAL_LARGEST (16U * 1024U)

typedef struct {
    EventGroupHandle_t events;
    esp_mqtt_client_handle_t client;
    char cmd_topic[BINDING_MQTT_TOPIC_MAX_LEN];
    char ack_topic[BINDING_MQTT_TOPIC_MAX_LEN];
    char payload[BINDING_MQTT_PAYLOAD_MAX_LEN];
    size_t payload_len;
    bool receiving_cmd;
    binding_mqtt_auth_grant_t grant;
    esp_err_t last_error;
    int ack_msg_id;
} binding_mqtt_runtime_t;

static bool binding_mqtt_has_start_heap(void)
{
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (internal_free >= BINDING_MQTT_MIN_INTERNAL_FREE &&
        internal_largest >= BINDING_MQTT_MIN_INTERNAL_LARGEST) {
        return true;
    }

    ESP_LOGW(TAG,
             "binding mqtt start rejected: internal_free=%u largest=%u need=%u/%u",
             (unsigned)internal_free,
             (unsigned)internal_largest,
             (unsigned)BINDING_MQTT_MIN_INTERNAL_FREE,
             (unsigned)BINDING_MQTT_MIN_INTERNAL_LARGEST);
    return false;
}

static const cJSON *binding_mqtt_get_string_any(const cJSON *object,
                                                const char * const *names,
                                                size_t name_count)
{
    if (!cJSON_IsObject(object) || names == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < name_count; ++i) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, names[i]);
        if (cJSON_IsString(item) && item->valuestring[0] != '\0') {
            return item;
        }
    }
    return NULL;
}

static const cJSON *binding_mqtt_pick_payload_object(const cJSON *root, cJSON **owned_payload)
{
    const cJSON *payload = NULL;
    const cJSON *data = NULL;

    if (owned_payload != NULL) {
        *owned_payload = NULL;
    }
    if (!cJSON_IsObject(root)) {
        return NULL;
    }

    payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (cJSON_IsObject(payload)) {
        return payload;
    }
    if (owned_payload != NULL && cJSON_IsString(payload) && payload->valuestring[0] == '{') {
        *owned_payload = cJSON_Parse(payload->valuestring);
        if (cJSON_IsObject(*owned_payload)) {
            return *owned_payload;
        }
        cJSON_Delete(*owned_payload);
        *owned_payload = NULL;
    }

    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsObject(data)) {
        return data;
    }
    return root;
}

static void binding_mqtt_collect_object_keys(const cJSON *object, char *buffer, size_t buffer_size)
{
    const cJSON *child = NULL;
    size_t used = 0;

    if (buffer == NULL || buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';
    if (!cJSON_IsObject(object)) {
        return;
    }

    cJSON_ArrayForEach(child, object) {
        const char *key = child->string;
        size_t key_len = key != NULL ? strlen(key) : 0;

        if (key_len == 0) {
            continue;
        }
        if (used != 0 && used + 1 < buffer_size) {
            buffer[used++] = ',';
            buffer[used] = '\0';
        }
        if (used + key_len >= buffer_size) {
            strlcpy(buffer + used, "...", buffer_size - used);
            return;
        }
        memcpy(buffer + used, key, key_len);
        used += key_len;
        buffer[used] = '\0';
    }
}

static bool binding_mqtt_topic_equals(const esp_mqtt_event_t *event, const char *topic)
{
    return event != NULL && topic != NULL &&
           event->topic != NULL &&
           event->topic_len == (int)strlen(topic) &&
           strncmp(event->topic, topic, (size_t)event->topic_len) == 0;
}

static esp_err_t binding_mqtt_parse_auth_grant(binding_mqtt_runtime_t *runtime,
                                               const char *payload)
{
    cJSON *root = NULL;
    cJSON *owned_payload = NULL;
    const cJSON *grant_payload = NULL;
    const cJSON *type = NULL;
    const cJSON *device_id = NULL;
    const cJSON *device_key = NULL;
    char root_keys[128] = {0};
    char payload_keys[128] = {0};
    esp_err_t ret = ESP_OK;
    static const char * const device_id_keys[] = {
        "device_id",
        "deviceId",
    };
    static const char * const device_key_keys[] = {
        "device_key",
        "device_secret_key",
        "deviceSecretKey",
        "device_secret",
        "secret_key",
    };

    if (runtime == NULL || payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(payload);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "auth_grant") != 0) {
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    grant_payload = binding_mqtt_pick_payload_object(root, &owned_payload);
    device_id = binding_mqtt_get_string_any(grant_payload,
                                            device_id_keys,
                                            sizeof(device_id_keys) / sizeof(device_id_keys[0]));
    device_key = binding_mqtt_get_string_any(grant_payload,
                                             device_key_keys,
                                             sizeof(device_key_keys) / sizeof(device_key_keys[0]));
    if (!cJSON_IsString(device_id) || !cJSON_IsString(device_key)) {
        runtime->grant.has_credentials = false;
        runtime->grant.device_id[0] = '\0';
        runtime->grant.device_key[0] = '\0';
        binding_mqtt_collect_object_keys(root, root_keys, sizeof(root_keys));
        binding_mqtt_collect_object_keys(grant_payload, payload_keys, sizeof(payload_keys));
        ESP_LOGI(TAG,
                 "binding auth grant received without inline credentials: root_keys=%s payload_keys=%s",
                 root_keys[0] != '\0' ? root_keys : "-",
                 payload_keys[0] != '\0' ? payload_keys : "-");
        ret = ESP_OK;
        goto cleanup;
    }

    runtime->grant.has_credentials = true;
    strlcpy(runtime->grant.device_id, device_id->valuestring, sizeof(runtime->grant.device_id));
    strlcpy(runtime->grant.device_key, device_key->valuestring, sizeof(runtime->grant.device_key));
    ESP_LOGI(TAG,
             "binding auth grant received: device_id_len=%u key_len=%u",
             (unsigned)strlen(runtime->grant.device_id),
             (unsigned)strlen(runtime->grant.device_key));

cleanup:
    cJSON_Delete(owned_payload);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t binding_mqtt_publish_ack(binding_mqtt_runtime_t *runtime)
{
    static const char ack_payload[] = "{\"ack\":true}";

    if (runtime == NULL || runtime->client == NULL || runtime->ack_topic[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    int msg_id = esp_mqtt_client_publish(runtime->client,
                                         runtime->ack_topic,
                                         ack_payload,
                                         0,
                                         1,
                                         0);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "binding ack publish failed: msg_id=%d", msg_id);
        return ESP_FAIL;
    }
    runtime->ack_msg_id = msg_id;
    ESP_LOGD(TAG, "binding ack queued: msg_id=%d", msg_id);
    return ESP_OK;
}

static void binding_mqtt_reset_payload(binding_mqtt_runtime_t *runtime)
{
    if (runtime == NULL) {
        return;
    }
    runtime->payload_len = 0;
    runtime->payload[0] = '\0';
    runtime->receiving_cmd = false;
}

static void binding_mqtt_handle_data(binding_mqtt_runtime_t *runtime, const esp_mqtt_event_t *event)
{
    esp_err_t ret = ESP_OK;

    if (runtime == NULL || event == NULL || event->data == NULL || event->data_len <= 0) {
        return;
    }
    if (event->total_data_len >= (int)sizeof(runtime->payload)) {
        binding_mqtt_reset_payload(runtime);
        runtime->last_error = ESP_ERR_INVALID_SIZE;
        xEventGroupSetBits(runtime->events, BINDING_MQTT_ERROR_BIT);
        return;
    }
    if (event->current_data_offset == 0) {
        binding_mqtt_reset_payload(runtime);
        runtime->receiving_cmd = binding_mqtt_topic_equals(event, runtime->cmd_topic);
    }
    if (!runtime->receiving_cmd) {
        return;
    }
    if (event->current_data_offset != (int)runtime->payload_len) {
        binding_mqtt_reset_payload(runtime);
        runtime->last_error = ESP_ERR_INVALID_RESPONSE;
        xEventGroupSetBits(runtime->events, BINDING_MQTT_ERROR_BIT);
        return;
    }
    if (runtime->payload_len + (size_t)event->data_len >= sizeof(runtime->payload)) {
        binding_mqtt_reset_payload(runtime);
        runtime->last_error = ESP_ERR_INVALID_SIZE;
        xEventGroupSetBits(runtime->events, BINDING_MQTT_ERROR_BIT);
        return;
    }

    memcpy(runtime->payload + runtime->payload_len, event->data, (size_t)event->data_len);
    runtime->payload_len += (size_t)event->data_len;
    runtime->payload[runtime->payload_len] = '\0';
    if (event->current_data_offset + event->data_len < event->total_data_len) {
        return;
    }
    runtime->receiving_cmd = false;

    ESP_LOGI(TAG,
             "binding mqtt command received: topic=%s bytes=%u",
             runtime->cmd_topic,
             (unsigned)runtime->payload_len);
    ret = binding_mqtt_parse_auth_grant(runtime, runtime->payload);
    if (ret == ESP_OK) {
        ret = binding_mqtt_publish_ack(runtime);
        if (ret != ESP_OK) {
            runtime->last_error = ret;
            xEventGroupSetBits(runtime->events, BINDING_MQTT_ERROR_BIT);
        }
    } else if (ret != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "binding command parse failed: %s", esp_err_to_name(ret));
    }
}

static void binding_mqtt_event_handler(void *handler_args,
                                       esp_event_base_t base,
                                       int32_t event_id,
                                       void *event_data)
{
    binding_mqtt_runtime_t *runtime = (binding_mqtt_runtime_t *)handler_args;
    esp_mqtt_event_t *event = (esp_mqtt_event_t *)event_data;

    (void)base;

    if (runtime == NULL || event == NULL) {
        return;
    }

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
    {
        int msg_id = esp_mqtt_client_subscribe(runtime->client, runtime->cmd_topic, 1);
        ESP_LOGI(TAG, "binding mqtt connected: topic=%s msg_id=%d", runtime->cmd_topic, msg_id);
        break;
    }
    case MQTT_EVENT_DATA:
        binding_mqtt_handle_data(runtime, event);
        break;
    case MQTT_EVENT_PUBLISHED:
        if (runtime->ack_msg_id >= 0 && event->msg_id == runtime->ack_msg_id) {
            ESP_LOGI(TAG, "binding ack confirmed: msg_id=%d", event->msg_id);
            runtime->ack_msg_id = -1;
            xEventGroupSetBits(runtime->events, BINDING_MQTT_GRANT_BIT);
        }
        break;
    case MQTT_EVENT_ERROR:
        runtime->last_error = ESP_FAIL;
        xEventGroupSetBits(runtime->events, BINDING_MQTT_ERROR_BIT);
        break;
    case MQTT_EVENT_DISCONNECTED:
        break;
    default:
        break;
    }
}

esp_err_t binding_mqtt_client_wait_auth_grant(const binding_mqtt_client_config_t *config,
                                              binding_mqtt_auth_grant_t *grant)
{
    binding_mqtt_runtime_t runtime = {0};
    char client_id[BINDING_MQTT_CLIENT_ID_MAX_LEN] = {0};
    EventBits_t bits = 0;
    esp_err_t ret = ESP_OK;

    if (config == NULL || grant == NULL ||
        config->broker_uri == NULL || config->broker_uri[0] == '\0' ||
        config->mac == NULL || config->mac[0] == '\0' ||
        config->temp_client_id == NULL || config->temp_client_id[0] == '\0' ||
        config->temp_token == NULL || config->temp_token[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    memset(grant, 0, sizeof(*grant));
    runtime.ack_msg_id = -1;
    if (strlen(config->temp_client_id) >= sizeof(client_id)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!binding_mqtt_has_start_heap()) {
        return ESP_ERR_NO_MEM;
    }
    runtime.events = xEventGroupCreate();
    if (runtime.events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    strlcpy(client_id, config->temp_client_id, sizeof(client_id));
    snprintf(runtime.cmd_topic, sizeof(runtime.cmd_topic), "device/%s/cmd", client_id);
    snprintf(runtime.ack_topic, sizeof(runtime.ack_topic), "device/%s/ack", client_id);

    esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = config->broker_uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = client_id,
        .credentials.client_id = client_id,
        .credentials.authentication.password = config->temp_token,
        .session.keepalive = 30,
        .network.timeout_ms = 10000,
        .network.reconnect_timeout_ms = 3000,
        .task.stack_size = 6144,
        .buffer.size = 1024,
        .buffer.out_size = 1024,
    };

    runtime.client = esp_mqtt_client_init(&mqtt_config);
    if (runtime.client == NULL) {
        vEventGroupDelete(runtime.events);
        return ESP_ERR_NO_MEM;
    }

    ret = esp_mqtt_client_register_event(runtime.client,
                                         MQTT_EVENT_ANY,
                                         binding_mqtt_event_handler,
                                         &runtime);
    if (ret == ESP_OK) {
        ret = esp_mqtt_client_start(runtime.client);
    }
    if (ret != ESP_OK) {
        esp_mqtt_client_destroy(runtime.client);
        vEventGroupDelete(runtime.events);
        return ret;
    }

    uint32_t waited_ms = 0;
    while (true) {
        uint32_t remaining_ms = config->wait_timeout_ms > waited_ms ?
                                config->wait_timeout_ms - waited_ms :
                                0U;
        uint32_t wait_ms = remaining_ms < BINDING_MQTT_WAIT_POLL_MS ?
                           remaining_ms :
                           BINDING_MQTT_WAIT_POLL_MS;
        if (config->should_cancel != NULL && config->should_cancel(config->cancel_ctx)) {
            ret = ESP_ERR_INVALID_STATE;
            break;
        }
        if (remaining_ms == 0U) {
            ret = ESP_ERR_TIMEOUT;
            break;
        }

        bits = xEventGroupWaitBits(runtime.events,
                                   BINDING_MQTT_GRANT_BIT | BINDING_MQTT_ERROR_BIT,
                                   pdTRUE,
                                   pdFALSE,
                                   pdMS_TO_TICKS(wait_ms));
        if ((bits & BINDING_MQTT_GRANT_BIT) != 0) {
            *grant = runtime.grant;
            ret = ESP_OK;
            break;
        }
        if ((bits & BINDING_MQTT_ERROR_BIT) != 0) {
            ret = runtime.last_error != ESP_OK ? runtime.last_error : ESP_FAIL;
            break;
        }
        waited_ms += wait_ms;
    }
    ESP_LOGI(TAG,
             "binding mqtt wait done: ret=%s grant=%d",
             esp_err_to_name(ret),
             grant->has_credentials ? 1 : 0);

    (void)esp_mqtt_client_stop(runtime.client);
    esp_mqtt_client_destroy(runtime.client);
    vEventGroupDelete(runtime.events);
    return ret;
}
