#include "binding_mqtt_client.h"

#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "binding_mqtt";

#define BINDING_MQTT_TOPIC_MAX_LEN 96
#define BINDING_MQTT_CLIENT_ID_MAX_LEN 64
#define BINDING_MQTT_PAYLOAD_MAX_LEN 512
#define BINDING_MQTT_DEFAULT_READY_TIMEOUT_MS 25000U
#define BINDING_MQTT_NETWORK_TIMEOUT_MS 20000U
#define BINDING_MQTT_GRANT_BIT BIT0
#define BINDING_MQTT_ERROR_BIT BIT1
#define BINDING_MQTT_READY_BIT BIT2

typedef struct {
    EventGroupHandle_t events;
    esp_mqtt_client_handle_t client;
    char cmd_topic[BINDING_MQTT_TOPIC_MAX_LEN];
    char ack_topic[BINDING_MQTT_TOPIC_MAX_LEN];
    char payload[BINDING_MQTT_PAYLOAD_MAX_LEN];
    size_t payload_len;
    binding_mqtt_auth_grant_t grant;
    esp_err_t last_error;
    int subscribe_msg_id;
    bool ready;
} binding_mqtt_runtime_t;

static bool binding_mqtt_topic_equals(const esp_mqtt_event_t *event, const char *topic)
{
    return event != NULL && topic != NULL &&
           event->topic != NULL &&
           event->topic_len == (int)strlen(topic) &&
           strncmp(event->topic, topic, (size_t)event->topic_len) == 0;
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

static void binding_mqtt_publish_ack(binding_mqtt_runtime_t *runtime)
{
    static const char ack_payload[] = "{\"ack\":true}";

    if (runtime == NULL || runtime->client == NULL || runtime->ack_topic[0] == '\0') {
        return;
    }

    int msg_id = esp_mqtt_client_publish(runtime->client,
                                         runtime->ack_topic,
                                         ack_payload,
                                         0,
                                         1,
                                         0);
    ESP_LOGD(TAG, "binding ack published: msg_id=%d", msg_id);
}

static void binding_mqtt_handle_data(binding_mqtt_runtime_t *runtime, const esp_mqtt_event_t *event)
{
    esp_err_t ret = ESP_OK;

    if (runtime == NULL || event == NULL || event->data == NULL || event->data_len <= 0) {
        return;
    }
    if (!binding_mqtt_topic_equals(event, runtime->cmd_topic)) {
        return;
    }
    if (event->total_data_len >= (int)sizeof(runtime->payload)) {
        runtime->last_error = ESP_ERR_INVALID_SIZE;
        xEventGroupSetBits(runtime->events, BINDING_MQTT_ERROR_BIT);
        return;
    }
    if (event->current_data_offset == 0) {
        runtime->payload_len = 0;
        runtime->payload[0] = '\0';
    }
    if (runtime->payload_len + (size_t)event->data_len >= sizeof(runtime->payload)) {
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

    ESP_LOGI(TAG,
             "binding mqtt command received: topic=%s bytes=%u",
             runtime->cmd_topic,
             (unsigned)runtime->payload_len);
    ret = binding_mqtt_parse_auth_grant(runtime, runtime->payload);
    if (ret == ESP_OK) {
        binding_mqtt_publish_ack(runtime);
        xEventGroupSetBits(runtime->events, BINDING_MQTT_GRANT_BIT);
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
        runtime->subscribe_msg_id = msg_id;
        ESP_LOGI(TAG, "binding mqtt connected: topic=%s msg_id=%d", runtime->cmd_topic, msg_id);
        if (msg_id < 0) {
            runtime->last_error = ESP_FAIL;
            xEventGroupSetBits(runtime->events, BINDING_MQTT_ERROR_BIT);
        }
        break;
    }
    case MQTT_EVENT_SUBSCRIBED:
        if (runtime->subscribe_msg_id <= 0 || event->msg_id == runtime->subscribe_msg_id) {
            runtime->ready = true;
            xEventGroupSetBits(runtime->events, BINDING_MQTT_READY_BIT);
            ESP_LOGI(TAG, "binding mqtt subscribed: topic=%s msg_id=%d", runtime->cmd_topic, event->msg_id);
        }
        break;
    case MQTT_EVENT_DATA:
        binding_mqtt_handle_data(runtime, event);
        break;
    case MQTT_EVENT_ERROR:
        runtime->last_error = ESP_FAIL;
        if (event->error_handle != NULL) {
            ESP_LOGW(TAG,
                     "binding mqtt error: type=%d tls_esp=%s tls_stack=0x%x sock_errno=%d connect_rc=%d",
                     event->error_handle->error_type,
                     esp_err_to_name(event->error_handle->esp_tls_last_esp_err),
                     event->error_handle->esp_tls_stack_err,
                     event->error_handle->esp_transport_sock_errno,
                     event->error_handle->connect_return_code);
        } else {
            ESP_LOGW(TAG, "binding mqtt error");
        }
        xEventGroupSetBits(runtime->events, BINDING_MQTT_ERROR_BIT);
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (runtime->ready) {
            runtime->last_error = ESP_ERR_INVALID_STATE;
            xEventGroupSetBits(runtime->events, BINDING_MQTT_ERROR_BIT);
        }
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
    bool ready_notified = false;
    uint32_t elapsed_ms = 0;
    uint32_t ready_timeout_ms = 0;

    if (config == NULL || grant == NULL ||
        config->broker_uri == NULL || config->broker_uri[0] == '\0' ||
        config->mac == NULL || config->mac[0] == '\0' ||
        config->temp_client_id == NULL || config->temp_client_id[0] == '\0' ||
        config->temp_token == NULL || config->temp_token[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    memset(grant, 0, sizeof(*grant));
    if (strlen(config->temp_client_id) >= sizeof(client_id)) {
        return ESP_ERR_INVALID_SIZE;
    }
    ready_timeout_ms = config->ready_timeout_ms != 0U ?
                       config->ready_timeout_ms :
                       BINDING_MQTT_DEFAULT_READY_TIMEOUT_MS;
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
        .network.timeout_ms = BINDING_MQTT_NETWORK_TIMEOUT_MS,
        .network.disable_auto_reconnect = true,
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

    while (true) {
        uint32_t current_timeout_ms = ready_notified ?
                                      config->wait_timeout_ms :
                                      ready_timeout_ms;
        uint32_t remaining_ms = elapsed_ms < config->wait_timeout_ms ?
                                config->wait_timeout_ms - elapsed_ms :
                                0U;
        if (current_timeout_ms < config->wait_timeout_ms) {
            remaining_ms = elapsed_ms < current_timeout_ms ?
                           current_timeout_ms - elapsed_ms :
                           0U;
        }
        int64_t wait_start_us = esp_timer_get_time();
        if (remaining_ms == 0U) {
            ret = ESP_ERR_TIMEOUT;
            break;
        }

        bits = xEventGroupWaitBits(runtime.events,
                                   BINDING_MQTT_GRANT_BIT |
                                   BINDING_MQTT_ERROR_BIT |
                                   BINDING_MQTT_READY_BIT,
                                   pdTRUE,
                                   pdFALSE,
                                   pdMS_TO_TICKS(remaining_ms));
        elapsed_ms += (uint32_t)((esp_timer_get_time() - wait_start_us) / 1000);

        if ((bits & BINDING_MQTT_GRANT_BIT) != 0) {
            *grant = runtime.grant;
            ret = ESP_OK;
            break;
        }
        if ((bits & BINDING_MQTT_ERROR_BIT) != 0) {
            ret = runtime.last_error != ESP_OK ? runtime.last_error : ESP_FAIL;
            break;
        }
        if ((bits & BINDING_MQTT_READY_BIT) != 0 && !ready_notified) {
            ready_notified = true;
            if (config->ready_cb != NULL) {
                config->ready_cb(config->ready_ctx);
            }
            continue;
        }
        if (bits == 0) {
            ret = ESP_ERR_TIMEOUT;
            break;
        }
    }
    ESP_LOGI(TAG,
             "binding mqtt wait done: ret=%s ready=%d grant=%d elapsed_ms=%u",
             esp_err_to_name(ret),
             ready_notified ? 1 : 0,
             grant->has_credentials ? 1 : 0,
             (unsigned)elapsed_ms);

    (void)esp_mqtt_client_stop(runtime.client);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_mqtt_client_destroy(runtime.client);
    vEventGroupDelete(runtime.events);
    return ret;
}
