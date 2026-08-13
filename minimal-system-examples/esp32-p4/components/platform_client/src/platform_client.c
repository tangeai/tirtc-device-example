#include "platform_client.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mqtt_client.h"

#define PLATFORM_HTTP_BODY_MAX 8192U
#define PLATFORM_REQUEST_QUEUE_DEPTH 4U
#define PLATFORM_MQTT_STATE_QUEUE_DEPTH 8U
#define PLATFORM_REQUEST_PATH_MAX 128U
#define PLATFORM_REQUEST_BODY_MAX 2048U
#define PLATFORM_METHOD_MAX 8U
#define PLATFORM_MQTT_PAYLOAD_MAX 4096U
#define PLATFORM_MQTT_TOPIC_MAX 160U
#define PLATFORM_MQTT_TOKEN_MAX 1536U
#define PLATFORM_DEFAULT_DISCOVERY "https://ep-open.tangeopen.com/services"
#define PLATFORM_DEFAULT_PROVISION_TIMEOUT_SECONDS 190U
#define PLATFORM_PROVISION_ACK_TIMEOUT_MS 10000U
#define PLATFORM_PROVISION_REPORT_DEFAULT_RETRY_SECONDS 10U
#define PLATFORM_PROVISION_REPORT_RETRY_MAX_SECONDS 300U
#define PLATFORM_PROVISION_NETWORK_RETRY_SECONDS 5U
#define PLATFORM_PROVISION_NETWORK_RETRY_MAX_SECONDS 60U
#define PLATFORM_WORKER_STACK (24U * 1024U)
#define PLATFORM_WORKER_PRIORITY 5U
#define PLATFORM_WORKER_STOP_TIMEOUT_MS 20000U
#define PLATFORM_REQUEST_SUBMISSION_DRAIN_TIMEOUT_MS 1000U
#define PLATFORM_WORKER_POLL_MS 250U
#define PLATFORM_TOKEN_REFRESH_INITIAL_MS 5000U
#define PLATFORM_TOKEN_REFRESH_MAX_MS 60000U
#define PLATFORM_TOKEN_REFRESH_SETTLE_MS 100U
#define PLATFORM_MQTT_ACK_PENDING_MAX 8U
#define PLATFORM_MQTT_DEDUP_MAX 16U
#define PLATFORM_TIME_VALID_UNIX_SECONDS 1704067200LL
#define PLATFORM_TIME_SYNC_WAIT_MS 1500U
#define PLATFORM_TIME_SYNC_WAIT_COUNT 8U
#define PLATFORM_TIME_SERVER_COUNT 4U
#define PLATFORM_TIME_SERVER_0 "ntp.aliyun.com"
#define PLATFORM_TIME_SERVER_1 "ntp.tencent.com"
#define PLATFORM_TIME_SERVER_2 "ntp.huaweicloud.com"
#define PLATFORM_TIME_SERVER_3 "cn.pool.ntp.org"

#define PROVISION_READY_BIT BIT0
#define PROVISION_GRANT_BIT BIT1
#define PROVISION_ERROR_BIT BIT2

typedef struct {
  char device[256];
  char ai[256];
  char call[256];
  char mqtt[256];
  char tirtc[256];
} platform_services_t;

typedef struct {
  char *data;
  size_t capacity;
  size_t length;
  bool overflow;
  bool retry_after_present;
  unsigned retry_after_seconds;
} http_output_t;

typedef struct {
  platform_service_t service;
  char method[PLATFORM_METHOD_MAX];
  char path[PLATFORM_REQUEST_PATH_MAX];
  bool has_body;
  char body[PLATFORM_REQUEST_BODY_MAX];
  platform_response_callback_t callback;
  void *user_data;
  unsigned auth_retry_count;
  uint32_t client_epoch;
  uint32_t auth_generation;
} platform_request_t;

typedef enum {
  MQTT_MESSAGE_NONE = 0,
  MQTT_MESSAGE_COMMAND,
  MQTT_MESSAGE_NOTIFY,
} mqtt_message_kind_t;

typedef struct {
  bool active;
  int message_id;
  size_t total_size;
  size_t received_size;
  mqtt_message_kind_t kind;
  bool duplicate_delivery;
  char topic[PLATFORM_MQTT_TOPIC_MAX];
  char payload[PLATFORM_MQTT_PAYLOAD_MAX + 1U];
} mqtt_reassembly_t;

typedef struct {
  bool valid;
  int incoming_message_id;
  uint32_t payload_hash;
  size_t payload_size;
  mqtt_message_kind_t kind;
  bool ack_confirmed;
} mqtt_dedup_entry_t;

typedef struct {
  bool active;
  int publish_message_id;
  int incoming_message_id;
  uint32_t payload_hash;
  size_t payload_size;
} mqtt_pending_ack_t;

typedef struct {
  char code[17];
  char temp_token[1024];
  char temp_client_id[65];
} provision_report_t;

typedef struct {
  EventGroupHandle_t events;
  esp_mqtt_client_handle_t mqtt;
  char temp_client_id[65];
  char device_id[65];
  char device_secret[257];
  mqtt_reassembly_t incoming;
  QueueHandle_t published_ids;
  int subscription_message_id;
  int grant_ack_message_id;
  char verification_code[17];
  bool grant_ready;
} provision_mqtt_t;

typedef enum {
  PLATFORM_REQUEST_COMPLETE = 0,
  PLATFORM_REQUEST_AUTH_RETRY,
  PLATFORM_REQUEST_REBIND,
} platform_request_result_t;

typedef enum {
  TOKEN_REFRESH_IDLE = 0,
  TOKEN_REFRESH_PENDING,
  TOKEN_REFRESH_READY,
  TOKEN_REFRESH_REBIND,
} token_refresh_result_t;

typedef struct {
  esp_mqtt_client_handle_t client;
  bool connected;
  unsigned reason_code;
  uint32_t client_epoch;
  uint32_t auth_generation;
} mqtt_state_event_t;

static const char *TAG = "platform_client";
static platform_services_t s_services;
static char s_device_id[65];
static char s_device_secret[257];
static char s_client_id[129];
static char s_mac_address[24];
static char s_mqtt_token[PLATFORM_MQTT_TOKEN_MAX];
static QueueHandle_t s_request_queue;
static QueueHandle_t s_mqtt_state_queue;
static TaskHandle_t s_request_task;
static StaticSemaphore_t s_worker_stopped_storage;
static SemaphoreHandle_t s_worker_stopped;
static esp_mqtt_client_handle_t s_mqtt;
static volatile bool s_ready;
static volatile bool s_mqtt_connected;
static volatile bool s_provisioning;
static bool s_services_ready;
static bool s_client_active;
static bool s_worker_stop_requested;
static bool s_mqtt_stopping;
static bool s_observer_mqtt_connected;
static bool s_mqtt_state_queue_overflow;
static bool s_request_submission_closed;
static bool s_token_refresh_requested;
static unsigned s_token_refresh_reason;
static uint32_t s_token_refresh_backoff_ms;
static int64_t s_token_refresh_due_ms;
static char s_verification_code[17];
static platform_signal_callback_t s_signal_callback;
static void *s_signal_user_data;
static platform_client_observer_t s_observer;
static void *s_observer_user_data;
static mqtt_reassembly_t s_mqtt_incoming;
static int s_command_subscription_id = -1;
static int s_notify_subscription_id = -1;
static bool s_command_subscription_ready;
static bool s_notify_subscription_ready;
static bool s_unbind_pending;
static bool s_rebind_event_pending;
static bool s_rebind_emit_mqtt_disconnected;
static platform_client_event_source_t s_rebind_source;
static int s_rebind_status_code;
static unsigned s_rebind_reason_code;
static uint32_t s_client_epoch;
static uint32_t s_token_refresh_generation;
static uint32_t s_mqtt_epoch;
static uint32_t s_mqtt_auth_generation;
static uint32_t s_worker_generation;
static uint32_t s_worker_completed_generation;
static uint32_t s_response_callbacks_inflight;
static uint32_t s_request_submissions;
static mqtt_dedup_entry_t s_mqtt_dedup[PLATFORM_MQTT_DEDUP_MAX];
static size_t s_mqtt_dedup_cursor;
static mqtt_pending_ack_t s_mqtt_pending_acks[PLATFORM_MQTT_ACK_PENDING_MAX];
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_api_lifecycle_mutex_storage;
static StaticSemaphore_t s_mqtt_op_mutex_storage;
static SemaphoreHandle_t s_api_lifecycle_mutex;
static SemaphoreHandle_t s_mqtt_op_mutex;

static bool client_requests_enabled(void);

static bool ensure_client_mutexes(void) {
  taskENTER_CRITICAL(&s_state_mux);
  if (s_api_lifecycle_mutex == NULL) {
    s_api_lifecycle_mutex =
        xSemaphoreCreateRecursiveMutexStatic(&s_api_lifecycle_mutex_storage);
  }
  if (s_mqtt_op_mutex == NULL) {
    s_mqtt_op_mutex = xSemaphoreCreateMutexStatic(&s_mqtt_op_mutex_storage);
  }
  bool ready =
      s_api_lifecycle_mutex != NULL && s_mqtt_op_mutex != NULL;
  taskEXIT_CRITICAL(&s_state_mux);
  return ready;
}

static uint32_t next_generation(uint32_t generation) {
  ++generation;
  return generation == 0U ? 1U : generation;
}

static void secure_zero(void *data, size_t length) {
  volatile uint8_t *bytes = (volatile uint8_t *)data;

  while (length-- > 0U) {
    *bytes++ = 0U;
  }
}

static void json_secure_clear(cJSON *item) {
  if (item == NULL) {
    return;
  }
  for (cJSON *child = item->child; child != NULL; child = child->next) {
    json_secure_clear(child);
  }
  if (item->valuestring != NULL) {
    secure_zero(item->valuestring, strlen(item->valuestring));
  }
}

static void json_secure_delete(cJSON *item) {
  json_secure_clear(item);
  cJSON_Delete(item);
}

static int64_t monotonic_ms(void) { return esp_timer_get_time() / 1000; }

static void platform_emit_event(const platform_client_event_t *event) {
  platform_client_observer_t observer = NULL;
  void *observer_ctx = NULL;

  if (event == NULL) {
    return;
  }
  taskENTER_CRITICAL(&s_state_mux);
  observer = s_observer;
  observer_ctx = s_observer_user_data;
  taskEXIT_CRITICAL(&s_state_mux);

  if (observer != NULL) {
    observer(event, observer_ctx);
  }
}

static void platform_emit_simple(platform_client_event_type_t type,
                                 platform_client_event_source_t source) {
  const platform_client_event_t event = {
      .type = type,
      .source = source,
      .error = ESP_OK,
  };
  platform_emit_event(&event);
}

static void platform_emit_error(platform_client_event_source_t source,
                                esp_err_t error, int status_code,
                                unsigned reason_code) {
  const platform_client_event_t event = {
      .type = PLATFORM_CLIENT_EVENT_ERROR,
      .source = source,
      .error = error,
      .status_code = status_code,
      .reason_code = reason_code,
  };
  platform_emit_event(&event);
}

static void platform_emit_rebind_required(
    platform_client_event_source_t source, int status_code,
    unsigned reason_code) {
  const platform_client_event_t event = {
      .type = PLATFORM_CLIENT_EVENT_REBIND_REQUIRED,
      .source = source,
      .error = ESP_ERR_NOT_FOUND,
      .status_code = status_code,
      .reason_code = reason_code,
  };
  platform_emit_event(&event);
}

static void platform_emit_mqtt_state(bool connected, unsigned reason_code) {
  const platform_client_event_t event = {
      .type = connected ? PLATFORM_CLIENT_EVENT_MQTT_CONNECTED
                        : PLATFORM_CLIENT_EVENT_MQTT_DISCONNECTED,
      .source = PLATFORM_CLIENT_EVENT_SOURCE_MQTT,
      .error = ESP_OK,
      .reason_code = reason_code,
  };
  platform_emit_event(&event);
}

static void emit_observer_mqtt_state(bool connected, unsigned reason_code) {
  bool changed = false;

  taskENTER_CRITICAL(&s_state_mux);
  if (s_observer_mqtt_connected != connected) {
    s_observer_mqtt_connected = connected;
    changed = true;
  }
  taskEXIT_CRITICAL(&s_state_mux);
  if (changed) {
    platform_emit_mqtt_state(connected, reason_code);
  }
}

static void platform_emit_provision_progress(
    platform_provision_progress_t progress) {
  const platform_client_event_t event = {
      .type = PLATFORM_CLIENT_EVENT_PROVISION_PROGRESS,
      .source = PLATFORM_CLIENT_EVENT_SOURCE_PROVISION,
      .error = ESP_OK,
      .reason_code = (unsigned)progress,
  };
  platform_emit_event(&event);
}

static bool platform_is_http_url(const char *url) {
  return url != NULL &&
         (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

static bool platform_is_https_url(const char *url) {
  return url != NULL && strncmp(url, "https://", 8) == 0;
}

static bool platform_is_mqtt_url(const char *url) {
  return url != NULL &&
         (strncmp(url, "mqtt://", 7) == 0 || strncmp(url, "mqtts://", 8) == 0);
}

static bool platform_method_valid(const char *method,
                                  esp_http_client_method_t *esp_method) {
  if (method == NULL || esp_method == NULL) {
    return false;
  }
  if (strcmp(method, PLATFORM_HTTP_METHOD_GET) == 0) {
    *esp_method = HTTP_METHOD_GET;
    return true;
  }
  if (strcmp(method, PLATFORM_HTTP_METHOD_POST) == 0) {
    *esp_method = HTTP_METHOD_POST;
    return true;
  }
  if (strcmp(method, PLATFORM_HTTP_METHOD_PUT) == 0) {
    *esp_method = HTTP_METHOD_PUT;
    return true;
  }
  if (strcmp(method, PLATFORM_HTTP_METHOD_DELETE) == 0) {
    *esp_method = HTTP_METHOD_DELETE;
    return true;
  }
  return false;
}

static esp_err_t http_event(esp_http_client_event_t *event) {
  if (event == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  http_output_t *output = event->user_data;

  if (event->event_id == HTTP_EVENT_ON_HEADER && output != NULL &&
      event->header_key != NULL &&
      event->header_value != NULL &&
      strcasecmp(event->header_key, "Retry-After") == 0) {
    char *end = NULL;
    unsigned long seconds = strtoul(event->header_value, &end, 10);
    if (end != event->header_value && end != NULL && *end == '\0' &&
        seconds <= UINT32_MAX) {
      output->retry_after_present = true;
      output->retry_after_seconds = (unsigned)seconds;
    }
    return ESP_OK;
  }

  if (event->event_id != HTTP_EVENT_ON_DATA || event->data == NULL ||
      event->data_len <= 0) {
    return ESP_OK;
  }

  if (output == NULL || output->overflow ||
      output->length + (size_t)event->data_len >= output->capacity) {
    if (output != NULL) {
      output->overflow = true;
    }
    return ESP_OK;
  }

  memcpy(output->data + output->length, event->data, (size_t)event->data_len);
  output->length += (size_t)event->data_len;
  output->data[output->length] = '\0';
  return ESP_OK;
}

static esp_err_t http_request(const char *url, const char *method,
                              const char *json_body, const char *bearer,
                              const char *const header_names[],
                              const char *const header_values[],
                              size_t header_count, char *response,
                              size_t response_size, int *status,
                              unsigned *retry_after_seconds) {
  esp_http_client_method_t esp_method = HTTP_METHOD_GET;

  if (!platform_is_https_url(url) ||
      !platform_method_valid(method, &esp_method) || response == NULL ||
      response_size == 0U ||
      (header_count > 0U && (header_names == NULL || header_values == NULL))) {
    return ESP_ERR_INVALID_ARG;
  }

  http_output_t output = {
      .data = response,
      .capacity = response_size,
  };
  response[0] = '\0';
  if (status != NULL) {
    *status = 0;
  }
  if (retry_after_seconds != NULL) {
    *retry_after_seconds = 0;
  }

  const esp_http_client_config_t config = {
      .url = url,
      .event_handler = http_event,
      .user_data = &output,
      .timeout_ms = 15000,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .disable_auto_redirect = true,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    return ESP_ERR_NO_MEM;
  }

  esp_err_t err = esp_http_client_set_method(client, esp_method);
  if (err != ESP_OK) {
    esp_http_client_cleanup(client);
    return err;
  }
  if (json_body != NULL) {
    err =
        esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err == ESP_OK) {
      err =
          esp_http_client_set_post_field(client, json_body, strlen(json_body));
    }
    if (err != ESP_OK) {
      esp_http_client_cleanup(client);
      return err;
    }
  }

  char authorization[PLATFORM_MQTT_TOKEN_MAX + 16U] = {0};
  if (bearer != NULL && bearer[0] != '\0') {
    int count =
        snprintf(authorization, sizeof(authorization), "Bearer %s", bearer);
    if (count <= 0 || (size_t)count >= sizeof(authorization)) {
      esp_http_client_cleanup(client);
      secure_zero(authorization, sizeof(authorization));
      return ESP_ERR_INVALID_SIZE;
    }
    err = esp_http_client_set_header(client, "Authorization", authorization);
    if (err != ESP_OK) {
      esp_http_client_cleanup(client);
      secure_zero(authorization, sizeof(authorization));
      return err;
    }
  }

  for (size_t index = 0; index < header_count; ++index) {
    if (header_names[index] == NULL || header_values[index] == NULL) {
      esp_http_client_cleanup(client);
      secure_zero(authorization, sizeof(authorization));
      return ESP_ERR_INVALID_ARG;
    }
    err = esp_http_client_set_header(client, header_names[index],
                                     header_values[index]);
    if (err != ESP_OK) {
      esp_http_client_cleanup(client);
      secure_zero(authorization, sizeof(authorization));
      return err;
    }
  }

  err = esp_http_client_perform(client);
  if (err != ESP_OK) {
    int socket_errno = esp_http_client_get_errno(client);
    int tls_error = 0;
    int tls_flags = 0;
    esp_err_t tls_status = esp_http_client_get_and_clear_last_tls_error(
        client, &tls_error, &tls_flags);
    ESP_LOGW(TAG,
             "HTTP transport failed err=%s(%d) errno=%d tls_status=%d "
             "tls_error=%d tls_flags=%d internal_free=%u internal_largest=%u",
             esp_err_to_name(err), (int)err, socket_errno, (int)tls_status,
             tls_error, tls_flags,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                               MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                        MALLOC_CAP_8BIT));
  }
  if (status != NULL) {
    *status = esp_http_client_get_status_code(client);
  }
  if (retry_after_seconds != NULL && output.retry_after_present) {
    *retry_after_seconds = output.retry_after_seconds;
  }
  esp_http_client_cleanup(client);
  secure_zero(authorization, sizeof(authorization));

  if (err == ESP_OK && output.overflow) {
    return ESP_ERR_INVALID_SIZE;
  }
  return err;
}

static bool json_copy_string(const cJSON *object, const char *name,
                             char *destination, size_t destination_size) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);

  if (!cJSON_IsString(item) || item->valuestring == NULL ||
      item->valuestring[0] == '\0' ||
      strlen(item->valuestring) >= destination_size) {
    return false;
  }
  strlcpy(destination, item->valuestring, destination_size);
  return true;
}

static bool json_copy_optional_string(const cJSON *object, const char *name,
                                      char *destination,
                                      size_t destination_size) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);

  if (item == NULL || cJSON_IsNull(item) ||
      (cJSON_IsString(item) && item->valuestring != NULL &&
       item->valuestring[0] == '\0')) {
    destination[0] = '\0';
    return true;
  }
  return json_copy_string(object, name, destination, destination_size);
}

static bool
platform_service_response_valid(const platform_services_t *services) {
  return services != NULL && platform_is_https_url(services->device) &&
         platform_is_https_url(services->ai) &&
         platform_is_https_url(services->call) &&
         platform_is_mqtt_url(services->mqtt) &&
         /* TiRTC currently accepts both HTTP and HTTPS service endpoints. */
         (services->tirtc[0] == '\0' ||
          platform_is_http_url(services->tirtc));
}

static esp_err_t discover_services(const char *url) {
  if (!platform_is_https_url(url)) {
    platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_DISCOVERY,
                        ESP_ERR_INVALID_ARG, 0, 0);
    return ESP_ERR_INVALID_ARG;
  }

  char *response = heap_caps_calloc(1, PLATFORM_HTTP_BODY_MAX,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (response == NULL) {
    response = calloc(1, PLATFORM_HTTP_BODY_MAX);
  }
  if (response == NULL) {
    platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_DISCOVERY, ESP_ERR_NO_MEM,
                        0, 0);
    return ESP_ERR_NO_MEM;
  }

  int status = 0;
  esp_err_t err =
      http_request(url, PLATFORM_HTTP_METHOD_GET, NULL, NULL, NULL, NULL, 0,
                   response, PLATFORM_HTTP_BODY_MAX, &status, NULL);
  if (err != ESP_OK || status != 200) {
    ESP_LOGE(TAG, "service discovery failed: transport=%s status=%d",
             esp_err_to_name(err), status);
    platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_DISCOVERY,
                        err == ESP_OK ? ESP_FAIL : err, status, 0);
    free(response);
    return err == ESP_OK ? ESP_FAIL : err;
  }

  platform_services_t candidate = {0};
  cJSON *root = cJSON_Parse(response);
  bool ok =
      cJSON_IsObject(root) &&
      json_copy_string(root, "device-srv", candidate.device,
                       sizeof(candidate.device)) &&
      json_copy_string(root, "ai-srv", candidate.ai, sizeof(candidate.ai)) &&
      json_copy_string(root, "call-srv", candidate.call,
                       sizeof(candidate.call)) &&
      json_copy_string(root, "mqtt-srv", candidate.mqtt,
                       sizeof(candidate.mqtt)) &&
      json_copy_optional_string(root, "tirtc-srv", candidate.tirtc,
                                sizeof(candidate.tirtc)) &&
      platform_service_response_valid(&candidate);
  cJSON_Delete(root);
  free(response);

  if (!ok) {
    ESP_LOGE(TAG, "service discovery response is incomplete or invalid");
    platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_DISCOVERY,
                        ESP_ERR_INVALID_RESPONSE, status, 0);
    return ESP_ERR_INVALID_RESPONSE;
  }

  s_services = candidate;
  s_services_ready = true;
  ESP_LOGI(TAG, "service discovery complete");
  platform_emit_simple(PLATFORM_CLIENT_EVENT_DISCOVERY_READY,
                       PLATFORM_CLIENT_EVENT_SOURCE_DISCOVERY);
  return ESP_OK;
}

static esp_err_t hmac_signature_with_key(const char *key, const char *text,
                                         char *base64, size_t base64_size) {
  unsigned char digest[32] = {0};
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

  if (key == NULL || key[0] == '\0' || text == NULL || base64 == NULL ||
      base64_size == 0U || info == NULL ||
      mbedtls_md_hmac(info, (const unsigned char *)key, strlen(key),
                      (const unsigned char *)text, strlen(text), digest) != 0) {
    secure_zero(digest, sizeof(digest));
    return ESP_FAIL;
  }

  size_t encoded = 0;
  int result = mbedtls_base64_encode((unsigned char *)base64, base64_size,
                                     &encoded, digest, sizeof(digest));
  secure_zero(digest, sizeof(digest));
  if (result != 0 || encoded >= base64_size) {
    return ESP_ERR_INVALID_SIZE;
  }
  base64[encoded] = '\0';
  return ESP_OK;
}

static esp_err_t hmac_signature(const char *text, char *base64,
                                size_t base64_size) {
  return hmac_signature_with_key(s_device_secret, text, base64, base64_size);
}

static bool platform_service_code_success(int code) {
  return code == 0 || code == 200;
}

static esp_err_t obtain_mqtt_token(char *token_out, size_t token_out_size,
                                   int *http_status, int *service_code) {
  if (token_out == NULL || token_out_size < PLATFORM_MQTT_TOKEN_MAX) {
    return ESP_ERR_INVALID_ARG;
  }
  token_out[0] = '\0';
  if (http_status != NULL) {
    *http_status = 0;
  }
  if (service_code != NULL) {
    *service_code = -1;
  }

  char timestamp[24] = {0};
  snprintf(timestamp, sizeof(timestamp), "%lld", (long long)time(NULL));
  char nonce[17] = {0};
  snprintf(nonce, sizeof(nonce), "%08lx%08lx", (unsigned long)esp_random(),
           (unsigned long)esp_random());
  char signed_text[384] = {0};
  int text_length = snprintf(signed_text, sizeof(signed_text), "%s%s%s",
                             s_device_id, timestamp, nonce);
  char signature[64] = {0};
  if (text_length <= 0 || (size_t)text_length >= sizeof(signed_text) ||
      hmac_signature(signed_text, signature, sizeof(signature)) != ESP_OK) {
    secure_zero(signed_text, sizeof(signed_text));
    secure_zero(signature, sizeof(signature));
    return ESP_FAIL;
  }

  char url[384] = {0};
  int url_length =
      snprintf(url, sizeof(url), "%s/v1/device/token", s_services.device);
  if (url_length <= 0 || (size_t)url_length >= sizeof(url)) {
    secure_zero(signed_text, sizeof(signed_text));
    secure_zero(signature, sizeof(signature));
    return ESP_ERR_INVALID_SIZE;
  }

  const char *names[] = {
      "X-Device-Id", "X-Timestamp", "X-Nonce", "X-Mac", "X-Signature",
  };
  const char *values[] = {
      s_device_id, timestamp, nonce, s_mac_address, signature,
  };

  char *response = heap_caps_calloc(1, PLATFORM_HTTP_BODY_MAX,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (response == NULL) {
    response = calloc(1, PLATFORM_HTTP_BODY_MAX);
  }
  if (response == NULL) {
    secure_zero(signed_text, sizeof(signed_text));
    secure_zero(signature, sizeof(signature));
    return ESP_ERR_NO_MEM;
  }

  int status = 0;
  esp_err_t err = http_request(url, PLATFORM_HTTP_METHOD_POST, "", NULL, names,
                               values, sizeof(names) / sizeof(names[0]),
                               response, PLATFORM_HTTP_BODY_MAX, &status, NULL);
  secure_zero(signed_text, sizeof(signed_text));
  secure_zero(signature, sizeof(signature));
  if (http_status != NULL) {
    *http_status = status;
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "device token request failed: %s", esp_err_to_name(err));
    secure_zero(response, PLATFORM_HTTP_BODY_MAX);
    free(response);
    return err;
  }

  cJSON *root = cJSON_Parse(response);
  const cJSON *code_item =
      root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "code");
  const cJSON *data =
      root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "data");
  int code = cJSON_IsNumber(code_item) ? code_item->valueint : -1;
  if (service_code != NULL) {
    *service_code = code;
  }

  char token[PLATFORM_MQTT_TOKEN_MAX] = {0};
  bool ok = status == 200 && platform_service_code_success(code) &&
            cJSON_IsObject(data) &&
            json_copy_string(data, "mqtt_token", token, sizeof(token));
  json_secure_delete(root);
  secure_zero(response, PLATFORM_HTTP_BODY_MAX);
  free(response);

  if (!ok) {
    secure_zero(token, sizeof(token));
    if (code == 6006) {
      ESP_LOGW(TAG, "device authorization requires retained rebind");
      return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGE(TAG, "device token response rejected: status=%d code=%d", status,
             code);
    return ESP_ERR_INVALID_RESPONSE;
  }

  strlcpy(token_out, token, token_out_size);
  secure_zero(token, sizeof(token));
  ESP_LOGI(TAG, "device authorization ready");
  return ESP_OK;
}

static const char *service_base(platform_service_t service) {
  switch (service) {
  case PLATFORM_SERVICE_DEVICE:
    return s_services.device;
  case PLATFORM_SERVICE_AI:
    return s_services.ai;
  case PLATFORM_SERVICE_CALL:
    return s_services.call;
  default:
    return NULL;
  }
}

static int response_service_code(const char *response) {
  if (response == NULL || response[0] == '\0') {
    return -1;
  }
  cJSON *root = cJSON_Parse(response);
  const cJSON *code =
      root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "code");
  int result = cJSON_IsNumber(code) ? code->valueint : -1;
  json_secure_delete(root);
  return result;
}

static void finish_request(platform_request_t *request, const char *response) {
  platform_response_callback_t callback = NULL;
  void *callback_ctx = NULL;
  bool deliver_response = false;

  if (request == NULL) {
    return;
  }
  callback = request->callback;
  callback_ctx = request->user_data;
  request->callback = NULL;
  request->user_data = NULL;
  if (callback == NULL) {
    return;
  }

  taskENTER_CRITICAL(&s_state_mux);
  deliver_response =
      response != NULL && !s_worker_stop_requested && !s_unbind_pending &&
      s_client_active && request->client_epoch == s_client_epoch &&
      request->auth_generation == s_token_refresh_generation &&
      !s_token_refresh_requested;
  ++s_response_callbacks_inflight;
  taskEXIT_CRITICAL(&s_state_mux);

  callback(deliver_response ? response : NULL, callback_ctx);

  taskENTER_CRITICAL(&s_state_mux);
  if (s_response_callbacks_inflight > 0U) {
    --s_response_callbacks_inflight;
  }
  taskEXIT_CRITICAL(&s_state_mux);
}

static bool copy_mqtt_token(char *token, size_t token_size) {
  if (token == NULL || token_size < sizeof(s_mqtt_token) ||
      !ensure_client_mutexes() ||
      xSemaphoreTake(s_mqtt_op_mutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }
  strlcpy(token, s_mqtt_token, token_size);
  xSemaphoreGive(s_mqtt_op_mutex);
  return token[0] != '\0';
}

static platform_request_result_t
process_request(platform_request_t *request) {
  const char *base = request == NULL ? NULL : service_base(request->service);
  if (request == NULL || base == NULL) {
    finish_request(request, NULL);
    return PLATFORM_REQUEST_COMPLETE;
  }

  char url[512] = {0};
  int url_length = snprintf(url, sizeof(url), "%s%s", base, request->path);
  char *response = heap_caps_calloc(1, PLATFORM_HTTP_BODY_MAX,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (response == NULL) {
    response = calloc(1, PLATFORM_HTTP_BODY_MAX);
  }
  if (response == NULL) {
    platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_REQUEST, ESP_ERR_NO_MEM, 0,
                        0);
    finish_request(request, NULL);
    return PLATFORM_REQUEST_COMPLETE;
  }

  int status = 0;
  char auth_token[PLATFORM_MQTT_TOKEN_MAX] = {0};
  bool have_auth_token = copy_mqtt_token(auth_token, sizeof(auth_token));
  esp_err_t err =
      url_length <= 0 || (size_t)url_length >= sizeof(url)
          ? ESP_ERR_INVALID_SIZE
          : !have_auth_token
                ? ESP_ERR_INVALID_STATE
                : http_request(url, request->method,
                               request->has_body ? request->body : NULL,
                               auth_token, NULL, NULL, 0, response,
                               PLATFORM_HTTP_BODY_MAX, &status, NULL);
  secure_zero(auth_token, sizeof(auth_token));
  if (err != ESP_OK) {
    ESP_LOGW(TAG,
             "platform request transport failed: method=%s service=%d error=%s",
             request->method, (int)request->service, esp_err_to_name(err));
    platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_REQUEST, err, status, 0);
    finish_request(request, NULL);
    secure_zero(response, PLATFORM_HTTP_BODY_MAX);
    free(response);
    return PLATFORM_REQUEST_COMPLETE;
  }

  int service_code = response_service_code(response);
  bool auth_rejected = status == 401 || service_code == 401;
  if (auth_rejected && request->auth_retry_count == 0U) {
    secure_zero(response, PLATFORM_HTTP_BODY_MAX);
    free(response);
    return PLATFORM_REQUEST_AUTH_RETRY;
  }
  if (auth_rejected) {
    platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_AUTH, ESP_FAIL, status,
                        401U);
  }

  if (!auth_rejected && (status < 200 || status >= 300)) {
    ESP_LOGW(TAG, "platform request rejected: method=%s service=%d status=%d",
             request->method, (int)request->service, status);
    platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_REQUEST, ESP_FAIL, status,
                        (unsigned)(service_code > 0 ? service_code : 0));
  }
  bool rebind = service_code == 6006;
  finish_request(request, rebind ? NULL : response);
  secure_zero(response, PLATFORM_HTTP_BODY_MAX);
  free(response);
  return rebind ? PLATFORM_REQUEST_REBIND : PLATFORM_REQUEST_COMPLETE;
}

static void publish_heartbeat(unsigned sequence) {
  esp_mqtt_client_handle_t mqtt = NULL;
  bool connected = false;

  if (!ensure_client_mutexes() ||
      xSemaphoreTake(s_mqtt_op_mutex, portMAX_DELAY) != pdTRUE) {
    return;
  }
  taskENTER_CRITICAL(&s_state_mux);
  mqtt = s_mqtt;
  connected = s_mqtt_connected;
  taskEXIT_CRITICAL(&s_state_mux);
  if (!connected || mqtt == NULL) {
    xSemaphoreGive(s_mqtt_op_mutex);
    return;
  }

  char topic[160] = {0};
  char body[128] = {0};
  int topic_length =
      snprintf(topic, sizeof(topic), "device/sn_%s/up", s_device_id);
  int body_length = snprintf(body, sizeof(body),
                             "{\"type\":\"heartbeat\",\"seq\":%u,\"ts\":%lld}",
                             sequence, (long long)time(NULL));
  if (topic_length > 0 && (size_t)topic_length < sizeof(topic) &&
      body_length > 0 && (size_t)body_length < sizeof(body)) {
    (void)esp_mqtt_client_publish(mqtt, topic, body, body_length, 0, 0);
  }
  xSemaphoreGive(s_mqtt_op_mutex);
}

static esp_err_t dispatch_mqtt_signal(const char *json, size_t length) {
  platform_signal_callback_t callback = NULL;
  void *callback_ctx = NULL;

  taskENTER_CRITICAL(&s_state_mux);
  callback = s_signal_callback;
  callback_ctx = s_signal_user_data;
  taskEXIT_CRITICAL(&s_state_mux);
  if (callback == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  return callback(json, length, callback_ctx);
}

static void mqtt_reassembly_reset(mqtt_reassembly_t *incoming) {
  if (incoming != NULL) {
    memset(incoming, 0, sizeof(*incoming));
    incoming->message_id = -1;
  }
}

static mqtt_message_kind_t mqtt_topic_kind(const char *topic) {
  char command_topic[PLATFORM_MQTT_TOPIC_MAX] = {0};
  char notify_topic[PLATFORM_MQTT_TOPIC_MAX] = {0};

  snprintf(command_topic, sizeof(command_topic), "device/sn_%s/cmd",
           s_device_id);
  snprintf(notify_topic, sizeof(notify_topic), "device/sn_%s/notify",
           s_device_id);
  if (strcmp(topic, command_topic) == 0) {
    return MQTT_MESSAGE_COMMAND;
  }
  if (strcmp(topic, notify_topic) == 0) {
    return MQTT_MESSAGE_NOTIFY;
  }
  return MQTT_MESSAGE_NONE;
}

static bool mqtt_reassembly_begin(mqtt_reassembly_t *incoming,
                                  const esp_mqtt_event_t *event,
                                  mqtt_message_kind_t forced_kind) {
  if (incoming == NULL || event == NULL || event->current_data_offset != 0 ||
      event->topic == NULL || event->topic_len <= 0 ||
      (size_t)event->topic_len >= sizeof(incoming->topic) ||
      event->total_data_len <= 0 ||
      (size_t)event->total_data_len > PLATFORM_MQTT_PAYLOAD_MAX) {
    return false;
  }

  mqtt_reassembly_reset(incoming);
  memcpy(incoming->topic, event->topic, (size_t)event->topic_len);
  incoming->topic[event->topic_len] = '\0';
  incoming->kind = forced_kind != MQTT_MESSAGE_NONE
                       ? forced_kind
                       : mqtt_topic_kind(incoming->topic);
  if (incoming->kind == MQTT_MESSAGE_NONE) {
    mqtt_reassembly_reset(incoming);
    return false;
  }
  incoming->active = true;
  incoming->message_id = event->msg_id;
  incoming->duplicate_delivery = event->dup;
  incoming->total_size = (size_t)event->total_data_len;
  return true;
}

static bool mqtt_reassembly_append(mqtt_reassembly_t *incoming,
                                   const esp_mqtt_event_t *event) {
  if (incoming == NULL || event == NULL || !incoming->active ||
      event->msg_id != incoming->message_id || event->data_len < 0 ||
      event->current_data_offset < 0 || event->total_data_len < 0 ||
      (size_t)event->current_data_offset != incoming->received_size ||
      (size_t)event->total_data_len != incoming->total_size ||
      incoming->received_size + (size_t)event->data_len >
          incoming->total_size ||
      incoming->received_size + (size_t)event->data_len >
          PLATFORM_MQTT_PAYLOAD_MAX) {
    mqtt_reassembly_reset(incoming);
    return false;
  }

  if (event->data_len > 0) {
    memcpy(incoming->payload + incoming->received_size, event->data,
           (size_t)event->data_len);
    incoming->received_size += (size_t)event->data_len;
  }
  if (incoming->received_size == incoming->total_size) {
    incoming->payload[incoming->received_size] = '\0';
    return true;
  }
  return false;
}

static bool mqtt_payload_is_json_object(const char *payload, size_t length) {
  const char *parse_end = NULL;
  cJSON *root =
      cJSON_ParseWithLengthOpts(payload, length + 1U, &parse_end, true);
  bool valid = cJSON_IsObject(root) && parse_end == payload + length;
  json_secure_delete(root);
  return valid;
}

static bool mqtt_payload_type_is(const char *payload, size_t length,
                                 const char *expected_type) {
  if (payload == NULL || expected_type == NULL) {
    return false;
  }
  const char *parse_end = NULL;
  cJSON *root =
      cJSON_ParseWithLengthOpts(payload, length + 1U, &parse_end, true);
  const cJSON *type = cJSON_IsObject(root)
                          ? cJSON_GetObjectItemCaseSensitive(root, "type")
                          : NULL;
  bool matches = parse_end == payload + length && cJSON_IsString(type) &&
                 strcmp(type->valuestring, expected_type) == 0;
  json_secure_delete(root);
  return matches;
}

static uint32_t mqtt_payload_hash(const char *payload, size_t length) {
  uint32_t hash = 2166136261U;

  for (size_t index = 0; index < length; ++index) {
    hash ^= (uint8_t)payload[index];
    hash *= 16777619U;
  }
  return hash;
}

static void mqtt_subscription_state_reset(void) {
  memset(s_mqtt_pending_acks, 0, sizeof(s_mqtt_pending_acks));
  s_command_subscription_id = -1;
  s_notify_subscription_id = -1;
  s_command_subscription_ready = false;
  s_notify_subscription_ready = false;
}

static void mqtt_delivery_state_reset(void) {
  mqtt_subscription_state_reset();
  memset(s_mqtt_dedup, 0, sizeof(s_mqtt_dedup));
  s_mqtt_dedup_cursor = 0U;
}

static mqtt_dedup_entry_t *
mqtt_find_delivery(const mqtt_reassembly_t *incoming, uint32_t payload_hash) {
  if (incoming == NULL || incoming->message_id <= 0 ||
      !incoming->duplicate_delivery) {
    return NULL;
  }
  for (size_t index = 0; index < PLATFORM_MQTT_DEDUP_MAX; ++index) {
    mqtt_dedup_entry_t *entry = &s_mqtt_dedup[index];
    if (entry->valid &&
        entry->incoming_message_id == incoming->message_id &&
        entry->payload_hash == payload_hash &&
        entry->payload_size == incoming->received_size &&
        entry->kind == incoming->kind) {
      return entry;
    }
  }
  return NULL;
}

static mqtt_dedup_entry_t *
mqtt_remember_delivery(const mqtt_reassembly_t *incoming,
                       uint32_t payload_hash) {
  mqtt_dedup_entry_t *entry =
      &s_mqtt_dedup[s_mqtt_dedup_cursor % PLATFORM_MQTT_DEDUP_MAX];
  s_mqtt_dedup_cursor =
      (s_mqtt_dedup_cursor + 1U) % PLATFORM_MQTT_DEDUP_MAX;
  *entry = (mqtt_dedup_entry_t){
      .valid = true,
      .incoming_message_id = incoming->message_id,
      .payload_hash = payload_hash,
      .payload_size = incoming->received_size,
      .kind = incoming->kind,
  };
  return entry;
}

static bool mqtt_ack_pending_for(const mqtt_dedup_entry_t *delivery) {
  if (delivery == NULL) {
    return false;
  }
  for (size_t index = 0; index < PLATFORM_MQTT_ACK_PENDING_MAX; ++index) {
    const mqtt_pending_ack_t *pending = &s_mqtt_pending_acks[index];
    if (pending->active &&
        pending->incoming_message_id == delivery->incoming_message_id &&
        pending->payload_hash == delivery->payload_hash &&
        pending->payload_size == delivery->payload_size) {
      return true;
    }
  }
  return false;
}

static esp_err_t mqtt_publish_command_ack(
    esp_mqtt_client_handle_t mqtt, const mqtt_dedup_entry_t *delivery) {
  if (mqtt == NULL || delivery == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (mqtt_ack_pending_for(delivery)) {
    return ESP_OK;
  }

  mqtt_pending_ack_t *pending = NULL;
  for (size_t index = 0; index < PLATFORM_MQTT_ACK_PENDING_MAX; ++index) {
    if (!s_mqtt_pending_acks[index].active) {
      pending = &s_mqtt_pending_acks[index];
      break;
    }
  }
  if (pending == NULL) {
    return ESP_ERR_NO_MEM;
  }

  *pending = (mqtt_pending_ack_t){
      .active = true,
      .publish_message_id = -1,
      .incoming_message_id = delivery->incoming_message_id,
      .payload_hash = delivery->payload_hash,
      .payload_size = delivery->payload_size,
  };
  char ack_topic[PLATFORM_MQTT_TOPIC_MAX] = {0};
  snprintf(ack_topic, sizeof(ack_topic), "device/sn_%s/ack", s_device_id);
  int ack_id =
      esp_mqtt_client_publish(mqtt, ack_topic, "{\"ack\":true}", 12, 1, 0);
  if (ack_id < 0) {
    memset(pending, 0, sizeof(*pending));
    return ESP_FAIL;
  }
  pending->publish_message_id = ack_id;
  return ESP_OK;
}

static void mqtt_confirm_command_ack(int publish_message_id) {
  for (size_t index = 0; index < PLATFORM_MQTT_ACK_PENDING_MAX; ++index) {
    mqtt_pending_ack_t *pending = &s_mqtt_pending_acks[index];
    if (!pending->active ||
        pending->publish_message_id != publish_message_id) {
      continue;
    }
    for (size_t delivery_index = 0;
         delivery_index < PLATFORM_MQTT_DEDUP_MAX; ++delivery_index) {
      mqtt_dedup_entry_t *delivery = &s_mqtt_dedup[delivery_index];
      if (delivery->valid &&
          delivery->incoming_message_id == pending->incoming_message_id &&
          delivery->payload_hash == pending->payload_hash &&
          delivery->payload_size == pending->payload_size) {
        delivery->ack_confirmed = true;
        break;
      }
    }
    memset(pending, 0, sizeof(*pending));
    return;
  }
}

static bool latch_rebind_required(uint32_t expected_epoch,
                                  platform_client_event_source_t source,
                                  int status_code, unsigned reason_code,
                                  bool *was_connected) {
  bool latched = false;
  taskENTER_CRITICAL(&s_state_mux);
  if (!s_unbind_pending && !s_worker_stop_requested && s_client_active &&
      (expected_epoch == 0U || expected_epoch == s_client_epoch)) {
    s_unbind_pending = true;
    s_ready = false;
    s_client_active = false;
    s_client_epoch = next_generation(s_client_epoch);
    if (was_connected != NULL) {
      *was_connected = s_mqtt_connected;
    }
    s_rebind_event_pending = true;
    s_rebind_emit_mqtt_disconnected = s_mqtt_connected;
    s_rebind_source = source;
    s_rebind_status_code = status_code;
    s_rebind_reason_code = reason_code;
    s_mqtt_connected = false;
    s_token_refresh_requested = false;
    s_token_refresh_reason = 0;
    s_token_refresh_due_ms = 0;
    s_token_refresh_backoff_ms = PLATFORM_TOKEN_REFRESH_INITIAL_MS;
    latched = true;
  }
  taskEXIT_CRITICAL(&s_state_mux);
  return latched;
}

static void mqtt_enter_unbind_required(void) {
  uint32_t client_epoch = 0U;
  taskENTER_CRITICAL(&s_state_mux);
  client_epoch = s_client_epoch;
  taskEXIT_CRITICAL(&s_state_mux);
  if (latch_rebind_required(client_epoch, PLATFORM_CLIENT_EVENT_SOURCE_MQTT, 0,
                            6006U, NULL)) {
    mqtt_subscription_state_reset();
  }
}

static void emit_pending_rebind_event(void) {
  bool pending = false;
  bool emit_mqtt_disconnected = false;
  platform_client_event_source_t source =
      PLATFORM_CLIENT_EVENT_SOURCE_INTERNAL;
  int status_code = 0;
  unsigned reason_code = 0U;

  taskENTER_CRITICAL(&s_state_mux);
  if (s_rebind_event_pending) {
    pending = true;
    emit_mqtt_disconnected = s_rebind_emit_mqtt_disconnected;
    source = s_rebind_source;
    status_code = s_rebind_status_code;
    reason_code = s_rebind_reason_code;
    s_rebind_event_pending = false;
    s_rebind_emit_mqtt_disconnected = false;
    s_rebind_source = PLATFORM_CLIENT_EVENT_SOURCE_INTERNAL;
    s_rebind_status_code = 0;
    s_rebind_reason_code = 0U;
  }
  taskEXIT_CRITICAL(&s_state_mux);

  if (!pending) {
    return;
  }
  (void)emit_mqtt_disconnected;
  emit_observer_mqtt_state(false, reason_code);
  platform_emit_rebind_required(source, status_code, reason_code);
}

static void mqtt_complete_message(esp_mqtt_client_handle_t mqtt,
                                  mqtt_reassembly_t *incoming) {
  if (mqtt == NULL || incoming == NULL || !incoming->active) {
    return;
  }

  if (!mqtt_payload_is_json_object(incoming->payload,
                                   incoming->received_size)) {
    ESP_LOGW(TAG, "dropping MQTT message with invalid JSON");
    platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_MQTT,
                        ESP_ERR_INVALID_RESPONSE, 0, 0);
    mqtt_reassembly_reset(incoming);
    return;
  }

  uint32_t payload_hash =
      mqtt_payload_hash(incoming->payload, incoming->received_size);
  mqtt_dedup_entry_t *delivery =
      mqtt_find_delivery(incoming, payload_hash);
  if (delivery != NULL) {
    if (incoming->kind == MQTT_MESSAGE_COMMAND &&
        mqtt_publish_command_ack(mqtt, delivery) != ESP_OK) {
      platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_MQTT, ESP_FAIL, 0, 0);
    }
    mqtt_reassembly_reset(incoming);
    return;
  }

  bool is_unbind =
      mqtt_payload_type_is(incoming->payload, incoming->received_size,
                           "unbind");
  if (is_unbind && incoming->kind != MQTT_MESSAGE_COMMAND) {
    ESP_LOGW(TAG, "dropping unbind outside the command topic");
    mqtt_reassembly_reset(incoming);
    return;
  }
  esp_err_t dispatch_error =
      is_unbind
          ? ESP_OK
          : dispatch_mqtt_signal(incoming->payload, incoming->received_size);
  if (dispatch_error != ESP_OK) {
    ESP_LOGW(TAG, "MQTT message was not accepted by the business queue: %s",
             esp_err_to_name(dispatch_error));
    platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_MQTT, dispatch_error, 0,
                        0);
    mqtt_reassembly_reset(incoming);
    return;
  }

  delivery = mqtt_remember_delivery(incoming, payload_hash);
  if (incoming->kind == MQTT_MESSAGE_COMMAND) {
    esp_err_t ack_error = mqtt_publish_command_ack(mqtt, delivery);
    if (ack_error != ESP_OK) {
      ESP_LOGW(TAG, "MQTT command ACK enqueue failed: %s",
               esp_err_to_name(ack_error));
      platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_MQTT, ack_error, 0, 0);
    }
  }
  if (is_unbind) {
    /*
     * The server publishes unbind best-effort and can kick the MQTT client
     * immediately. Gate new work as soon as the validated command is accepted;
     * the application ACK above is still attempted but is not a lifecycle
     * prerequisite.
     */
    mqtt_enter_unbind_required();
  }
  mqtt_reassembly_reset(incoming);
}

static unsigned mqtt_event_reason(const esp_mqtt_event_t *event) {
  if (event == NULL || event->error_handle == NULL) {
    return 0U;
  }
#ifdef CONFIG_MQTT_PROTOCOL_5
  if (event->error_handle->disconnect_return_code != 0) {
    return (unsigned)event->error_handle->disconnect_return_code;
  }
#endif
  if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
    return (unsigned)event->error_handle->connect_return_code;
  }
  return 0U;
}

static bool mqtt_reason_invalidates_token(unsigned reason) {
  return reason == 0x98U || reason == 0x99U ||
         reason == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
         reason == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED;
}

static void schedule_token_refresh(unsigned reason) {
  taskENTER_CRITICAL(&s_state_mux);
  if (!s_worker_stop_requested && !s_unbind_pending && s_client_active) {
    if (!s_token_refresh_requested) {
      s_token_refresh_due_ms =
          monotonic_ms() + PLATFORM_TOKEN_REFRESH_SETTLE_MS;
      s_token_refresh_backoff_ms = PLATFORM_TOKEN_REFRESH_INITIAL_MS;
    }
    s_ready = false;
    s_token_refresh_generation =
        next_generation(s_token_refresh_generation);
    s_token_refresh_requested = true;
    s_token_refresh_reason = reason;
  }
  taskEXIT_CRITICAL(&s_state_mux);
}

static bool mqtt_subscription_ack_ok(const esp_mqtt_event_t *event) {
  if (event == NULL || event->data == NULL || event->data_len <= 0) {
    return false;
  }
  if (event->error_handle != NULL &&
      event->error_handle->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) {
    return false;
  }
  for (int index = 0; index < event->data_len; ++index) {
    if ((uint8_t)event->data[index] >= 0x80U) {
      return false;
    }
  }
  return true;
}

static bool mqtt_event_current_locked(esp_mqtt_client_handle_t client) {
  return client != NULL && client == s_mqtt && !s_mqtt_stopping &&
         !s_worker_stop_requested && !s_unbind_pending && s_client_active &&
         s_mqtt_epoch == s_client_epoch;
}

static bool mqtt_capture_event_generation(esp_mqtt_client_handle_t client,
                                          uint32_t *client_epoch,
                                          uint32_t *auth_generation) {
  bool current = false;

  taskENTER_CRITICAL(&s_state_mux);
  current = mqtt_event_current_locked(client);
  if (current) {
    if (client_epoch != NULL) {
      *client_epoch = s_mqtt_epoch;
    }
    if (auth_generation != NULL) {
      *auth_generation = s_mqtt_auth_generation;
    }
  }
  taskEXIT_CRITICAL(&s_state_mux);
  return current;
}

static void queue_mqtt_state_event(esp_mqtt_client_handle_t client,
                                   bool connected, unsigned reason_code,
                                   uint32_t client_epoch,
                                   uint32_t auth_generation) {
  QueueHandle_t queue = NULL;
  const mqtt_state_event_t state_event = {
      .client = client,
      .connected = connected,
      .reason_code = reason_code,
      .client_epoch = client_epoch,
      .auth_generation = auth_generation,
  };

  taskENTER_CRITICAL(&s_state_mux);
  queue = s_mqtt_state_queue;
  taskEXIT_CRITICAL(&s_state_mux);
  if (queue == NULL || xQueueSend(queue, &state_event, 0) != pdTRUE) {
    taskENTER_CRITICAL(&s_state_mux);
    s_mqtt_state_queue_overflow = true;
    taskEXIT_CRITICAL(&s_state_mux);
  }
}

static void process_pending_mqtt_state_events(void) {
  QueueHandle_t queue = NULL;
  bool overflow = false;

  taskENTER_CRITICAL(&s_state_mux);
  queue = s_mqtt_state_queue;
  overflow = s_mqtt_state_queue_overflow;
  s_mqtt_state_queue_overflow = false;
  taskEXIT_CRITICAL(&s_state_mux);
  if (overflow) {
    platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_MQTT, ESP_ERR_NO_MEM, 0,
                        0);
  }
  if (queue == NULL) {
    return;
  }

  mqtt_state_event_t state_event = {0};
  while (xQueueReceive(queue, &state_event, 0) == pdTRUE) {
    bool current = false;
    bool changed = false;
    taskENTER_CRITICAL(&s_state_mux);
    current = state_event.client_epoch == s_client_epoch;
    if (state_event.connected) {
      current = current && mqtt_event_current_locked(state_event.client) &&
                state_event.auth_generation == s_mqtt_auth_generation &&
                !s_token_refresh_requested && s_mqtt_connected;
    }
    if (current &&
        s_observer_mqtt_connected != state_event.connected) {
      s_observer_mqtt_connected = state_event.connected;
      changed = true;
    }
    taskEXIT_CRITICAL(&s_state_mux);
    if (changed) {
      platform_emit_mqtt_state(state_event.connected, state_event.reason_code);
    }
    memset(&state_event, 0, sizeof(state_event));
  }
}

static void mqtt_fail_subscriptions(esp_mqtt_client_handle_t client) {
  bool current = false;
  taskENTER_CRITICAL(&s_state_mux);
  current = mqtt_event_current_locked(client);
  if (current) {
    s_mqtt_connected = false;
  }
  taskEXIT_CRITICAL(&s_state_mux);
  if (!current) {
    return;
  }
  mqtt_subscription_state_reset();
  platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_MQTT,
                      ESP_ERR_INVALID_RESPONSE, 0, 0);
  if (client != NULL) {
    (void)esp_mqtt_client_disconnect(client);
  }
}

static void mqtt_event(void *handler_args, esp_event_base_t base,
                       int32_t event_id, void *event_data) {
  (void)handler_args;
  (void)base;
  esp_mqtt_event_handle_t event = event_data;
  uint32_t event_client_epoch = 0U;
  uint32_t event_auth_generation = 0U;

  if (event == NULL ||
      !mqtt_capture_event_generation(event->client, &event_client_epoch,
                                     &event_auth_generation)) {
    return;
  }

  if (event_id == MQTT_EVENT_CONNECTED) {
    mqtt_subscription_state_reset();
    mqtt_reassembly_reset(&s_mqtt_incoming);
    char command_topic[PLATFORM_MQTT_TOPIC_MAX] = {0};
    char notify_topic[PLATFORM_MQTT_TOPIC_MAX] = {0};
    snprintf(command_topic, sizeof(command_topic), "device/sn_%s/cmd",
             s_device_id);
    snprintf(notify_topic, sizeof(notify_topic), "device/sn_%s/notify",
             s_device_id);
    s_command_subscription_id =
        esp_mqtt_client_subscribe(event->client, command_topic, 1);
    s_notify_subscription_id =
        esp_mqtt_client_subscribe(event->client, notify_topic, 1);
    if (s_command_subscription_id < 0 || s_notify_subscription_id < 0) {
      ESP_LOGE(TAG, "MQTT topic subscription failed");
      mqtt_fail_subscriptions(event->client);
      return;
    }
    ESP_LOGI(TAG, "MQTT connected; waiting for device topic SUBACKs");
    return;
  }

  if (event_id == MQTT_EVENT_SUBSCRIBED) {
    bool command = event->msg_id == s_command_subscription_id;
    bool notify = event->msg_id == s_notify_subscription_id;
    bool became_connected = false;
    if (!command && !notify) {
      return;
    }
    if (!mqtt_subscription_ack_ok(event)) {
      ESP_LOGE(TAG, "MQTT broker rejected a device topic subscription");
      mqtt_fail_subscriptions(event->client);
      return;
    }
    taskENTER_CRITICAL(&s_state_mux);
    if (!mqtt_event_current_locked(event->client) ||
        s_token_refresh_requested ||
        event_client_epoch != s_mqtt_epoch ||
        event_auth_generation != s_mqtt_auth_generation ||
        s_mqtt_auth_generation != s_token_refresh_generation) {
      taskEXIT_CRITICAL(&s_state_mux);
      return;
    }
    if (command) {
      s_command_subscription_ready = true;
    }
    if (notify) {
      s_notify_subscription_ready = true;
    }
    if (s_command_subscription_ready && s_notify_subscription_ready &&
        !s_mqtt_connected) {
      s_mqtt_connected = true;
      became_connected = true;
    }
    taskEXIT_CRITICAL(&s_state_mux);
    if (became_connected) {
      ESP_LOGI(TAG, "MQTT device topic subscriptions confirmed");
      queue_mqtt_state_event(event->client, true, 0, event_client_epoch,
                             event_auth_generation);
    }
    return;
  }

  if (event_id == MQTT_EVENT_DISCONNECTED) {
    unsigned reason = mqtt_event_reason(event);
    bool current = false;
    taskENTER_CRITICAL(&s_state_mux);
    current = mqtt_event_current_locked(event->client) &&
              event_client_epoch == s_mqtt_epoch &&
              event_auth_generation == s_mqtt_auth_generation;
    if (current) {
      s_mqtt_connected = false;
    }
    taskEXIT_CRITICAL(&s_state_mux);
    if (!current) {
      return;
    }
    mqtt_subscription_state_reset();
    mqtt_reassembly_reset(&s_mqtt_incoming);
    ESP_LOGW(TAG, "MQTT disconnected: reason=%u", reason);
    queue_mqtt_state_event(event->client, false, reason, event_client_epoch,
                           event_auth_generation);
    if (mqtt_reason_invalidates_token(reason)) {
      schedule_token_refresh(reason);
    }
    return;
  }

  if (event_id == MQTT_EVENT_PUBLISHED) {
    mqtt_confirm_command_ack(event->msg_id);
    return;
  }

  if (event_id == MQTT_EVENT_DATA) {
    if (event->current_data_offset == 0 &&
        !mqtt_reassembly_begin(&s_mqtt_incoming, event, MQTT_MESSAGE_NONE)) {
      ESP_LOGW(TAG, "dropping MQTT message with invalid topic or size");
      return;
    }
    if (mqtt_reassembly_append(&s_mqtt_incoming, event)) {
      mqtt_complete_message(event->client, &s_mqtt_incoming);
    } else if (!s_mqtt_incoming.active) {
      ESP_LOGW(TAG, "dropping out-of-order MQTT fragment");
    }
    return;
  }

  if (event_id == MQTT_EVENT_ERROR) {
    if (event->error_handle != NULL &&
        event->error_handle->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) {
      ESP_LOGE(TAG, "MQTT broker rejected a device topic subscription");
      mqtt_fail_subscriptions(event->client);
      return;
    }
    unsigned reason = mqtt_event_reason(event);
    ESP_LOGW(TAG, "MQTT transport/authentication error: reason=%u", reason);
    if (mqtt_reason_invalidates_token(reason)) {
      schedule_token_refresh(reason);
    } else {
      platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_MQTT, ESP_FAIL, 0,
                          reason);
    }
  }
}

static esp_err_t client_operation_status_locked(uint32_t expected_epoch,
                                                uint32_t auth_generation) {
  if (s_unbind_pending) {
    return ESP_ERR_NOT_FOUND;
  }
  if (s_worker_stop_requested || !s_client_active ||
      expected_epoch != s_client_epoch) {
    return ESP_ERR_INVALID_STATE;
  }
  if (s_token_refresh_requested ||
      auth_generation != s_token_refresh_generation) {
    return ESP_ERR_NOT_FINISHED;
  }
  return ESP_OK;
}

static esp_err_t start_mqtt(const char *token, uint32_t expected_epoch,
                            uint32_t auth_generation) {
  if (token == NULL || token[0] == '\0' ||
      strlen(token) >= sizeof(s_mqtt_token) ||
      !ensure_client_mutexes() ||
      xSemaphoreTake(s_mqtt_op_mutex, portMAX_DELAY) != pdTRUE) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t lifecycle_status = ESP_ERR_INVALID_STATE;
  taskENTER_CRITICAL(&s_state_mux);
  lifecycle_status =
      client_operation_status_locked(expected_epoch, auth_generation);
  if (lifecycle_status == ESP_OK && s_mqtt != NULL) {
    lifecycle_status = ESP_ERR_INVALID_STATE;
  }
  taskEXIT_CRITICAL(&s_state_mux);
  if (lifecycle_status != ESP_OK) {
    xSemaphoreGive(s_mqtt_op_mutex);
    return lifecycle_status;
  }

  secure_zero(s_mqtt_token, sizeof(s_mqtt_token));
  strlcpy(s_mqtt_token, token, sizeof(s_mqtt_token));
  mqtt_delivery_state_reset();
  esp_mqtt_client_config_t config = {
      .broker.address.uri = s_services.mqtt,
      .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
      .credentials.username = s_device_id,
      .credentials.client_id = s_client_id,
      .credentials.authentication.password = s_mqtt_token,
      .session.keepalive = 60,
#ifdef CONFIG_MQTT_PROTOCOL_5
      .session.protocol_ver = MQTT_PROTOCOL_V_5,
#else
      .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
#endif
      .network.timeout_ms = 10000,
      .network.reconnect_timeout_ms = 3000,
      .task.stack_size = 6144,
      .buffer.size = 2048,
      .buffer.out_size = 2048,
  };
  esp_mqtt_client_handle_t client = esp_mqtt_client_init(&config);
  if (client == NULL) {
    secure_zero(s_mqtt_token, sizeof(s_mqtt_token));
    xSemaphoreGive(s_mqtt_op_mutex);
    return ESP_ERR_NO_MEM;
  }

  esp_err_t err = esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID,
                                                 mqtt_event, NULL);
  if (err != ESP_OK) {
    esp_mqtt_client_destroy(client);
    secure_zero(s_mqtt_token, sizeof(s_mqtt_token));
    xSemaphoreGive(s_mqtt_op_mutex);
    return err;
  }

  err = esp_mqtt_client_start(client);
  if (err != ESP_OK) {
    esp_mqtt_client_destroy(client);
    secure_zero(s_mqtt_token, sizeof(s_mqtt_token));
    xSemaphoreGive(s_mqtt_op_mutex);
    return err;
  }

  taskENTER_CRITICAL(&s_state_mux);
  lifecycle_status =
      client_operation_status_locked(expected_epoch, auth_generation);
  if (lifecycle_status == ESP_OK && s_mqtt == NULL) {
    s_mqtt = client;
    s_mqtt_stopping = false;
    s_mqtt_epoch = expected_epoch;
    s_mqtt_auth_generation = auth_generation;
  } else if (lifecycle_status == ESP_OK) {
    lifecycle_status = ESP_ERR_INVALID_STATE;
  }
  taskEXIT_CRITICAL(&s_state_mux);
  if (lifecycle_status != ESP_OK) {
    taskENTER_CRITICAL(&s_state_mux);
    s_mqtt_stopping = true;
    taskEXIT_CRITICAL(&s_state_mux);
    (void)esp_mqtt_client_stop(client);
    (void)esp_mqtt_client_destroy(client);
    secure_zero(s_mqtt_token, sizeof(s_mqtt_token));
    taskENTER_CRITICAL(&s_state_mux);
    s_mqtt_stopping = false;
    taskEXIT_CRITICAL(&s_state_mux);
    xSemaphoreGive(s_mqtt_op_mutex);
    return lifecycle_status;
  }
  xSemaphoreGive(s_mqtt_op_mutex);
  return ESP_OK;
}

static void stop_mqtt(void) {
  esp_mqtt_client_handle_t client = NULL;

  if (!ensure_client_mutexes() ||
      xSemaphoreTake(s_mqtt_op_mutex, portMAX_DELAY) != pdTRUE) {
    return;
  }
  taskENTER_CRITICAL(&s_state_mux);
  s_mqtt_stopping = true;
  client = s_mqtt;
  s_mqtt = NULL;
  s_mqtt_connected = false;
  s_mqtt_epoch = 0U;
  s_mqtt_auth_generation = 0U;
  taskEXIT_CRITICAL(&s_state_mux);

  if (client != NULL) {
    (void)esp_mqtt_client_stop(client);
    (void)esp_mqtt_client_destroy(client);
  }
  mqtt_reassembly_reset(&s_mqtt_incoming);
  mqtt_delivery_state_reset();

  taskENTER_CRITICAL(&s_state_mux);
  s_mqtt_stopping = false;
  taskEXIT_CRITICAL(&s_state_mux);
  xSemaphoreGive(s_mqtt_op_mutex);
}

static void clear_mqtt_token(void) {
  if (!ensure_client_mutexes() ||
      xSemaphoreTake(s_mqtt_op_mutex, portMAX_DELAY) != pdTRUE) {
    return;
  }
  secure_zero(s_mqtt_token, sizeof(s_mqtt_token));
  xSemaphoreGive(s_mqtt_op_mutex);
}

static bool worker_stop_requested(void) {
  bool stop = false;

  taskENTER_CRITICAL(&s_state_mux);
  stop = s_worker_stop_requested;
  taskEXIT_CRITICAL(&s_state_mux);
  return stop;
}

static bool worker_generation_completed(uint32_t generation) {
  bool completed = false;

  taskENTER_CRITICAL(&s_state_mux);
  completed = generation != 0U &&
              s_worker_completed_generation == generation &&
              s_request_task == NULL;
  taskEXIT_CRITICAL(&s_state_mux);
  return completed;
}

static bool wait_for_worker_completion(uint32_t generation,
                                       uint32_t timeout_ms) {
  const int64_t deadline = monotonic_ms() + timeout_ms;

  while (!worker_generation_completed(generation)) {
    int64_t remaining_ms = deadline - monotonic_ms();
    if (remaining_ms <= 0 || s_worker_stopped == NULL) {
      return false;
    }
    if (remaining_ms > PLATFORM_WORKER_POLL_MS) {
      remaining_ms = PLATFORM_WORKER_POLL_MS;
    }
    TickType_t wait_ticks = pdMS_TO_TICKS((uint32_t)remaining_ms);
    if (wait_ticks == 0) {
      wait_ticks = 1;
    }
    (void)xSemaphoreTake(s_worker_stopped, wait_ticks);
  }
  return true;
}

static bool client_requests_enabled(void) {
  bool enabled = false;

  taskENTER_CRITICAL(&s_state_mux);
  enabled = !s_worker_stop_requested && !s_unbind_pending && s_client_active;
  taskEXIT_CRITICAL(&s_state_mux);
  return enabled;
}

static esp_err_t commit_client_ready(uint32_t expected_epoch,
                                     uint32_t auth_generation) {
  esp_err_t result = ESP_ERR_INVALID_STATE;

  taskENTER_CRITICAL(&s_state_mux);
  result = client_operation_status_locked(expected_epoch, auth_generation);
  if (result == ESP_OK) {
    s_ready = true;
  }
  taskEXIT_CRITICAL(&s_state_mux);
  return result;
}

static bool reserve_request_submission(QueueHandle_t *queue,
                                       uint32_t *client_epoch,
                                       uint32_t *auth_generation) {
  bool reserved = false;

  taskENTER_CRITICAL(&s_state_mux);
  if (queue != NULL && client_epoch != NULL && auth_generation != NULL &&
      s_client_active && !s_unbind_pending &&
      !s_worker_stop_requested && !s_request_submission_closed &&
      s_request_task != NULL &&
      s_request_queue != NULL) {
    ++s_request_submissions;
    *queue = s_request_queue;
    *client_epoch = s_client_epoch;
    *auth_generation = s_token_refresh_generation;
    reserved = true;
  }
  taskEXIT_CRITICAL(&s_state_mux);
  return reserved;
}

static void release_request_submission(void) {
  taskENTER_CRITICAL(&s_state_mux);
  if (s_request_submissions > 0U) {
    --s_request_submissions;
  }
  taskEXIT_CRITICAL(&s_state_mux);
}

static uint32_t request_submissions(void) {
  uint32_t submissions = 0U;

  taskENTER_CRITICAL(&s_state_mux);
  submissions = s_request_submissions;
  taskEXIT_CRITICAL(&s_state_mux);
  return submissions;
}

static bool token_refresh_requested(void) {
  bool requested = false;

  taskENTER_CRITICAL(&s_state_mux);
  requested = s_token_refresh_requested;
  taskEXIT_CRITICAL(&s_state_mux);
  return requested;
}

static esp_err_t client_operation_status(uint32_t expected_epoch,
                                         uint32_t auth_generation) {
  esp_err_t result = ESP_ERR_INVALID_STATE;
  taskENTER_CRITICAL(&s_state_mux);
  result =
      client_operation_status_locked(expected_epoch, auth_generation);
  taskEXIT_CRITICAL(&s_state_mux);
  return result;
}

static bool enter_rebind_required(uint32_t expected_epoch, int status_code,
                                  unsigned reason_code) {
  if (!latch_rebind_required(expected_epoch, PLATFORM_CLIENT_EVENT_SOURCE_AUTH,
                             status_code, reason_code, NULL)) {
    return false;
  }
  stop_mqtt();
  clear_mqtt_token();
  return true;
}

static token_refresh_result_t handle_token_refresh(void) {
  unsigned reason = 0;
  uint32_t backoff_ms = 0;
  uint32_t client_epoch = 0U;
  uint32_t refresh_generation = 0U;
  int64_t due_ms = 0;
  int64_t now_ms = monotonic_ms();
  bool requested = false;
  bool enabled = false;

  taskENTER_CRITICAL(&s_state_mux);
  requested = s_token_refresh_requested;
  enabled = !s_worker_stop_requested && !s_unbind_pending && s_client_active;
  reason = s_token_refresh_reason;
  due_ms = s_token_refresh_due_ms;
  backoff_ms = s_token_refresh_backoff_ms;
  client_epoch = s_client_epoch;
  refresh_generation = s_token_refresh_generation;
  if (enabled && requested && now_ms >= due_ms) {
    s_token_refresh_requested = false;
    s_token_refresh_reason = 0;
    s_token_refresh_due_ms = 0;
    s_ready = false;
  }
  taskEXIT_CRITICAL(&s_state_mux);
  if (!enabled || !requested || now_ms < due_ms) {
    return requested ? TOKEN_REFRESH_PENDING : TOKEN_REFRESH_IDLE;
  }

  stop_mqtt();
  clear_mqtt_token();

  char token[PLATFORM_MQTT_TOKEN_MAX] = {0};
  int status = 0;
  int service_code = -1;
  esp_err_t err =
      obtain_mqtt_token(token, sizeof(token), &status, &service_code);
  esp_err_t lifecycle_status =
      client_operation_status(client_epoch, refresh_generation);
  if (lifecycle_status != ESP_OK) {
    secure_zero(token, sizeof(token));
    return lifecycle_status == ESP_ERR_NOT_FINISHED
               ? TOKEN_REFRESH_PENDING
               : TOKEN_REFRESH_IDLE;
  }
  if (err == ESP_OK) {
    platform_emit_simple(PLATFORM_CLIENT_EVENT_AUTH_READY,
                         PLATFORM_CLIENT_EVENT_SOURCE_AUTH);
    err = start_mqtt(token, client_epoch, refresh_generation);
    secure_zero(token, sizeof(token));
    if (err == ESP_OK) {
      esp_err_t commit =
          commit_client_ready(client_epoch, refresh_generation);
      if (commit == ESP_OK) {
        ESP_LOGI(TAG, "MQTT authorization refreshed");
        return TOKEN_REFRESH_READY;
      }
      stop_mqtt();
      clear_mqtt_token();
      return commit == ESP_ERR_NOT_FINISHED ? TOKEN_REFRESH_PENDING
                                            : TOKEN_REFRESH_IDLE;
    }
    if (err == ESP_ERR_NOT_FINISHED) {
      return TOKEN_REFRESH_PENDING;
    }
  } else {
    secure_zero(token, sizeof(token));
  }

  if (err == ESP_ERR_NOT_FOUND || service_code == 6006) {
    if (enter_rebind_required(client_epoch, status, 6006U)) {
      return TOKEN_REFRESH_REBIND;
    }
    return token_refresh_requested() ? TOKEN_REFRESH_PENDING
                                     : TOKEN_REFRESH_IDLE;
  }

  lifecycle_status =
      client_operation_status(client_epoch, refresh_generation);
  if (lifecycle_status != ESP_OK) {
    return lifecycle_status == ESP_ERR_NOT_FINISHED
               ? TOKEN_REFRESH_PENDING
               : TOKEN_REFRESH_IDLE;
  }
  platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_AUTH, err, status, reason);
  if (backoff_ms == 0U) {
    backoff_ms = PLATFORM_TOKEN_REFRESH_INITIAL_MS;
  }
  taskENTER_CRITICAL(&s_state_mux);
  if (!s_worker_stop_requested && !s_unbind_pending && s_client_active &&
      s_client_epoch == client_epoch &&
      s_token_refresh_generation == refresh_generation &&
      !s_token_refresh_requested) {
    s_ready = false;
    s_token_refresh_requested = true;
    s_token_refresh_reason = reason;
    s_token_refresh_due_ms = monotonic_ms() + backoff_ms;
    s_token_refresh_backoff_ms =
        backoff_ms >= PLATFORM_TOKEN_REFRESH_MAX_MS / 2U
            ? PLATFORM_TOKEN_REFRESH_MAX_MS
            : backoff_ms * 2U;
  }
  taskEXIT_CRITICAL(&s_state_mux);
  return TOKEN_REFRESH_PENDING;
}

static void reject_queued_requests(void) {
  if (s_request_queue == NULL) {
    return;
  }
  platform_request_t request = {0};
  while (xQueueReceive(s_request_queue, &request, 0) == pdTRUE) {
    finish_request(&request, NULL);
    secure_zero(&request, sizeof(request));
  }
}

static void request_task(void *argument) {
  const uint32_t worker_generation = (uint32_t)(uintptr_t)argument;
  unsigned heartbeat_sequence = 0;
  int64_t next_heartbeat_ms = monotonic_ms() + 30000;
  platform_request_t held_request = {0};
  bool held_request_valid = false;

  for (;;) {
    process_pending_mqtt_state_events();
    if (!client_requests_enabled()) {
      if (held_request_valid) {
        finish_request(&held_request, NULL);
      }
      secure_zero(&held_request, sizeof(held_request));
      held_request_valid = false;
      reject_queued_requests();
      emit_pending_rebind_event();
      if (worker_stop_requested()) {
        break;
      }
      (void)ulTaskNotifyTake(pdTRUE,
                            pdMS_TO_TICKS(PLATFORM_WORKER_POLL_MS));
      continue;
    }
    token_refresh_result_t refresh = handle_token_refresh();
    if (worker_stop_requested()) {
      break;
    }
    if (!client_requests_enabled()) {
      if (held_request_valid) {
        finish_request(&held_request, NULL);
      }
      secure_zero(&held_request, sizeof(held_request));
      held_request_valid = false;
      reject_queued_requests();
      emit_pending_rebind_event();
      if (worker_stop_requested()) {
        break;
      }
      (void)ulTaskNotifyTake(pdTRUE,
                            pdMS_TO_TICKS(PLATFORM_WORKER_POLL_MS));
      continue;
    }
    if (refresh == TOKEN_REFRESH_REBIND) {
      if (held_request_valid) {
        finish_request(&held_request, NULL);
      }
      secure_zero(&held_request, sizeof(held_request));
      held_request_valid = false;
      reject_queued_requests();
    }
    if (refresh == TOKEN_REFRESH_PENDING || token_refresh_requested()) {
      (void)ulTaskNotifyTake(pdTRUE,
                            pdMS_TO_TICKS(PLATFORM_WORKER_POLL_MS));
      continue;
    }

    platform_request_t request = {0};
    bool have_request = false;
    if (held_request_valid) {
      request = held_request;
      secure_zero(&held_request, sizeof(held_request));
      held_request_valid = false;
      have_request = true;
    } else if (s_request_queue != NULL &&
               xQueueReceive(s_request_queue, &request, 0) == pdTRUE) {
      have_request = true;
    }
    if (have_request) {
      platform_request_result_t result = process_request(&request);
      if (result == PLATFORM_REQUEST_AUTH_RETRY) {
        if (client_requests_enabled()) {
          ++request.auth_retry_count;
          schedule_token_refresh(401U);
          taskENTER_CRITICAL(&s_state_mux);
          request.auth_generation = s_token_refresh_generation;
          taskEXIT_CRITICAL(&s_state_mux);
          held_request = request;
          held_request_valid = true;
        } else {
          finish_request(&request, NULL);
        }
      } else if (result == PLATFORM_REQUEST_REBIND) {
        (void)enter_rebind_required(request.client_epoch, 410, 6006U);
        reject_queued_requests();
      }
      secure_zero(&request, sizeof(request));
    }

    int64_t current_ms = monotonic_ms();
    if (current_ms >= next_heartbeat_ms && client_requests_enabled()) {
      publish_heartbeat(++heartbeat_sequence);
      next_heartbeat_ms = current_ms + 30000;
    }
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PLATFORM_WORKER_POLL_MS));
  }

  if (held_request_valid) {
    finish_request(&held_request, NULL);
  }
  secure_zero(&held_request, sizeof(held_request));
  reject_queued_requests();
  process_pending_mqtt_state_events();
  if (s_worker_stopped != NULL) {
    xSemaphoreGive(s_worker_stopped);
  }
  taskENTER_CRITICAL(&s_state_mux);
  if (s_worker_generation == worker_generation) {
    s_worker_completed_generation = worker_generation;
    s_request_task = NULL;
  }
  taskEXIT_CRITICAL(&s_state_mux);
  vTaskDeleteWithCaps(NULL);
}

static esp_err_t ensure_worker(void) {
  if (s_request_task != NULL) {
    return ESP_OK;
  }
  if (s_request_queue == NULL) {
    s_request_queue =
        xQueueCreate(PLATFORM_REQUEST_QUEUE_DEPTH, sizeof(platform_request_t));
    if (s_request_queue == NULL) {
      return ESP_ERR_NO_MEM;
    }
  }
  if (s_mqtt_state_queue == NULL) {
    s_mqtt_state_queue = xQueueCreate(PLATFORM_MQTT_STATE_QUEUE_DEPTH,
                                     sizeof(mqtt_state_event_t));
    if (s_mqtt_state_queue == NULL) {
      vQueueDelete(s_request_queue);
      s_request_queue = NULL;
      return ESP_ERR_NO_MEM;
    }
  }
  if (s_worker_stopped == NULL) {
    s_worker_stopped =
        xSemaphoreCreateBinaryStatic(&s_worker_stopped_storage);
    if (s_worker_stopped == NULL) {
      vQueueDelete(s_mqtt_state_queue);
      s_mqtt_state_queue = NULL;
      vQueueDelete(s_request_queue);
      s_request_queue = NULL;
      return ESP_ERR_NO_MEM;
    }
  }
  (void)xSemaphoreTake(s_worker_stopped, 0);

  taskENTER_CRITICAL(&s_state_mux);
  s_worker_stop_requested = false;
  uint32_t worker_generation = next_generation(s_worker_generation);
  taskEXIT_CRITICAL(&s_state_mux);
  TaskHandle_t worker = NULL;
  BaseType_t task_result =
      xTaskCreateWithCaps(request_task, "platform_worker",
                          PLATFORM_WORKER_STACK,
                          (void *)(uintptr_t)worker_generation,
                          PLATFORM_WORKER_PRIORITY, &worker,
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (task_result != pdPASS) {
    vQueueDelete(s_mqtt_state_queue);
    s_mqtt_state_queue = NULL;
    vQueueDelete(s_request_queue);
    s_request_queue = NULL;
    return ESP_ERR_NO_MEM;
  }
  taskENTER_CRITICAL(&s_state_mux);
  s_worker_generation = worker_generation;
  s_request_task = worker;
  taskEXIT_CRITICAL(&s_state_mux);
  return ESP_OK;
}

static esp_err_t sync_clock(void) {
  time_t now = 0;
  time(&now);
  if (now >= (time_t)PLATFORM_TIME_VALID_UNIX_SECONDS) {
    return ESP_OK;
  }

  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
      PLATFORM_TIME_SERVER_COUNT,
      ESP_SNTP_SERVER_LIST(PLATFORM_TIME_SERVER_0, PLATFORM_TIME_SERVER_1,
                           PLATFORM_TIME_SERVER_2, PLATFORM_TIME_SERVER_3));
  const int64_t started_ms = monotonic_ms();
  esp_netif_sntp_deinit();
  esp_err_t err = esp_netif_sntp_init(&config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "network clock initialization failed: %s",
             esp_err_to_name(err));
    return err;
  }

  for (unsigned attempt = 0; attempt < PLATFORM_TIME_SYNC_WAIT_COUNT;
       ++attempt) {
    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(PLATFORM_TIME_SYNC_WAIT_MS));
    time(&now);
    if (err != ESP_ERR_TIMEOUT ||
        now >= (time_t)PLATFORM_TIME_VALID_UNIX_SECONDS) {
      break;
    }
  }
  esp_netif_sntp_deinit();

  if (now >= (time_t)PLATFORM_TIME_VALID_UNIX_SECONDS) {
    ESP_LOGI(TAG, "network clock synchronized in %lld ms",
             (long long)(monotonic_ms() - started_ms));
    return ESP_OK;
  }
  ESP_LOGE(TAG, "network clock synchronization failed after %lld ms: %s",
           (long long)(monotonic_ms() - started_ms), esp_err_to_name(err));
  return err == ESP_OK ? ESP_FAIL : err;
}

static esp_err_t report_for_provision(const platform_provision_config_t *config,
                                       provision_report_t *report) {
  cJSON *body_root = cJSON_CreateObject();
  if (body_root == NULL ||
      !cJSON_AddStringToObject(body_root, "mac", config->mac_address)) {
    cJSON_Delete(body_root);
    return ESP_ERR_NO_MEM;
  }
  char *body = cJSON_PrintUnformatted(body_root);
  cJSON_Delete(body_root);
  if (body == NULL) {
    return ESP_ERR_NO_MEM;
  }

  bool signed_report = config->existing_device_id != NULL &&
                       config->existing_device_id[0] != '\0' &&
                       config->existing_device_secret != NULL &&
                       config->existing_device_secret[0] != '\0';
  char url[384] = {0};
  int url_length = snprintf(url, sizeof(url), "%s/v1/device/report",
                            s_services.device);
  if (url_length <= 0 || (size_t)url_length >= sizeof(url)) {
    free(body);
    return ESP_ERR_INVALID_SIZE;
  }

  char *response = heap_caps_calloc(1, PLATFORM_HTTP_BODY_MAX,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (response == NULL) {
    response = calloc(1, PLATFORM_HTTP_BODY_MAX);
  }
  if (response == NULL) {
    free(body);
    return ESP_ERR_NO_MEM;
  }

  esp_err_t result = ESP_ERR_INVALID_RESPONSE;
  unsigned report_waited_seconds = 0U;
  unsigned network_waited_seconds = 0U;
  for (;;) {
    const char *header_names[4] = {0};
    const char *header_values[4] = {0};
    size_t header_count = 0;
    char timestamp[24] = {0};
    char nonce[17] = {0};
    char signature[64] = {0};

    if (signed_report) {
      snprintf(timestamp, sizeof(timestamp), "%lld", (long long)time(NULL));
      snprintf(nonce, sizeof(nonce), "%08lx%08lx",
               (unsigned long)esp_random(), (unsigned long)esp_random());
      char signed_text[384] = {0};
      int signed_length =
          snprintf(signed_text, sizeof(signed_text), "%s%s%s",
                   config->existing_device_id, timestamp, nonce);
      if (signed_length <= 0 ||
          (size_t)signed_length >= sizeof(signed_text) ||
          hmac_signature_with_key(config->existing_device_secret, signed_text,
                                  signature, sizeof(signature)) != ESP_OK) {
        secure_zero(signed_text, sizeof(signed_text));
        secure_zero(signature, sizeof(signature));
        result = ESP_FAIL;
        break;
      }
      secure_zero(signed_text, sizeof(signed_text));
      header_names[0] = "X-Device-Id";
      header_values[0] = config->existing_device_id;
      header_names[1] = "X-Timestamp";
      header_values[1] = timestamp;
      header_names[2] = "X-Nonce";
      header_values[2] = nonce;
      header_names[3] = "X-Signature";
      header_values[3] = signature;
      header_count = 4;
    }

    memset(response, 0, PLATFORM_HTTP_BODY_MAX);
    int status = 0;
    unsigned retry_after_seconds = 0;
    esp_err_t err =
        http_request(url, PLATFORM_HTTP_METHOD_POST, body, NULL, header_names,
                     header_values, header_count, response,
                     PLATFORM_HTTP_BODY_MAX, &status, &retry_after_seconds);
    secure_zero(signature, sizeof(signature));
    if (err != ESP_OK) {
      const bool retryable =
          err != ESP_ERR_INVALID_ARG && err != ESP_ERR_NO_MEM &&
          err != ESP_ERR_INVALID_SIZE;
      if (!retryable ||
          network_waited_seconds + PLATFORM_PROVISION_NETWORK_RETRY_SECONDS >
              PLATFORM_PROVISION_NETWORK_RETRY_MAX_SECONDS) {
        ESP_LOGE(TAG, "device report transport failed: %s",
                 esp_err_to_name(err));
        result = err;
        break;
      }
      ESP_LOGW(TAG,
               "device report network retry: err=%s wait=%us elapsed=%us",
               esp_err_to_name(err), PLATFORM_PROVISION_NETWORK_RETRY_SECONDS,
               network_waited_seconds);
      vTaskDelay(pdMS_TO_TICKS(PLATFORM_PROVISION_NETWORK_RETRY_SECONDS *
                               1000U));
      network_waited_seconds += PLATFORM_PROVISION_NETWORK_RETRY_SECONDS;
      continue;
    }
    network_waited_seconds = 0U;

    cJSON *root = cJSON_Parse(response);
    const cJSON *response_code =
        root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "code");
    const cJSON *data =
        root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "data");
    int code = cJSON_IsNumber(response_code) ? response_code->valueint : -1;
    bool ok =
        status == 200 && platform_service_code_success(code) &&
        cJSON_IsObject(data) &&
        json_copy_string(data, "code", report->code, sizeof(report->code)) &&
        json_copy_string(data, "temp_token", report->temp_token,
                         sizeof(report->temp_token)) &&
        json_copy_string(data, "temp_client_id", report->temp_client_id,
                         sizeof(report->temp_client_id));
    bool retryable = code == 40901 || status == 429 || code == 429;
    json_secure_delete(root);
    secure_zero(response, PLATFORM_HTTP_BODY_MAX);

    if (ok) {
      result = ESP_OK;
      break;
    }
    secure_zero(report, sizeof(*report));
    if (!retryable) {
      ESP_LOGW(TAG, "device report rejected: status=%d code=%d", status, code);
      result = ESP_ERR_INVALID_RESPONSE;
      break;
    }

    unsigned delay_seconds =
        retry_after_seconds == 0U
            ? PLATFORM_PROVISION_REPORT_DEFAULT_RETRY_SECONDS
            : retry_after_seconds;
    if (delay_seconds == 0U ||
        delay_seconds > PLATFORM_PROVISION_REPORT_RETRY_MAX_SECONDS ||
        report_waited_seconds + delay_seconds >
            PLATFORM_PROVISION_REPORT_RETRY_MAX_SECONDS) {
      ESP_LOGW(TAG,
               "device report cooldown exhausted: status=%d code=%d "
               "retry_after=%us elapsed=%us",
               status, code, delay_seconds, report_waited_seconds);
      result = ESP_ERR_TIMEOUT;
      break;
    }
    ESP_LOGW(TAG,
             "device report transiently rejected: status=%d code=%d; "
             "retrying in %u seconds",
             status, code, delay_seconds);
    vTaskDelay(pdMS_TO_TICKS(delay_seconds * 1000U));
    report_waited_seconds += delay_seconds;
  }

  secure_zero(response, PLATFORM_HTTP_BODY_MAX);
  free(response);
  free(body);
  if (result == ESP_OK) {
    ESP_LOGI(TAG, "device report accepted; temporary authorization ready");
  }
  return result;
}

static bool pending_provision_valid(
    const platform_pending_provision_t *pending,
    const platform_provision_config_t *config, time_t now) {
  return pending != NULL && config != NULL && config->mac_address != NULL &&
         pending->expires_at_unix > (int64_t)now &&
         pending->mac_address[sizeof(pending->mac_address) - 1U] == '\0' &&
         pending->code[sizeof(pending->code) - 1U] == '\0' &&
         pending->temp_token[sizeof(pending->temp_token) - 1U] == '\0' &&
         pending->temp_client_id[sizeof(pending->temp_client_id) - 1U] ==
             '\0' &&
         strcmp(pending->mac_address, config->mac_address) == 0 &&
         pending->code[0] != '\0' && pending->temp_token[0] != '\0' &&
         pending->temp_client_id[0] != '\0';
}

static void report_from_pending(const platform_pending_provision_t *pending,
                                provision_report_t *report) {
  memset(report, 0, sizeof(*report));
  strlcpy(report->code, pending->code, sizeof(report->code));
  strlcpy(report->temp_token, pending->temp_token,
          sizeof(report->temp_token));
  strlcpy(report->temp_client_id, pending->temp_client_id,
          sizeof(report->temp_client_id));
}

static void pending_from_report(const platform_provision_config_t *config,
                                const provision_report_t *report,
                                time_t expires_at,
                                platform_pending_provision_t *pending) {
  memset(pending, 0, sizeof(*pending));
  pending->expires_at_unix = (int64_t)expires_at;
  strlcpy(pending->mac_address, config->mac_address,
          sizeof(pending->mac_address));
  strlcpy(pending->code, report->code, sizeof(pending->code));
  strlcpy(pending->temp_token, report->temp_token,
          sizeof(pending->temp_token));
  strlcpy(pending->temp_client_id, report->temp_client_id,
          sizeof(pending->temp_client_id));
}

static void provision_finish_with_error(provision_mqtt_t *context) {
  if (context != NULL && context->events != NULL) {
    xEventGroupSetBits(context->events, PROVISION_ERROR_BIT);
  }
}

static void provision_publish_code_ready(provision_mqtt_t *context) {
  if (context == NULL || context->verification_code[0] == '\0') {
    provision_finish_with_error(context);
    return;
  }

  strlcpy(s_verification_code, context->verification_code,
          sizeof(s_verification_code));
  platform_client_event_t event = {
      .type = PLATFORM_CLIENT_EVENT_PROVISION_CODE,
      .source = PLATFORM_CLIENT_EVENT_SOURCE_PROVISION,
      .error = ESP_OK,
  };
  strlcpy(event.provision_code, context->verification_code,
          sizeof(event.provision_code));
  platform_emit_event(&event);
  xEventGroupSetBits(context->events, PROVISION_READY_BIT);
  ESP_LOGI(TAG,
           "temporary MQTT subscription confirmed; verification code is ready");
}

static const cJSON *provision_payload_object(const cJSON *root,
                                             cJSON **owned_payload) {
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
  if (owned_payload != NULL && cJSON_IsString(payload) &&
      payload->valuestring != NULL && payload->valuestring[0] == '{') {
    *owned_payload = cJSON_Parse(payload->valuestring);
    if (cJSON_IsObject(*owned_payload)) {
      return *owned_payload;
    }
    json_secure_delete(*owned_payload);
    *owned_payload = NULL;
  }

  data = cJSON_GetObjectItemCaseSensitive(root, "data");
  return cJSON_IsObject(data) ? data : root;
}

static const cJSON *json_string_alias(const cJSON *object, const char *first,
                                      const char *second) {
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, first);
  if (cJSON_IsString(value) && value->valuestring != NULL &&
      value->valuestring[0] != '\0') {
    return value;
  }
  value = cJSON_GetObjectItemCaseSensitive(object, second);
  return cJSON_IsString(value) && value->valuestring != NULL &&
                 value->valuestring[0] != '\0'
             ? value
             : NULL;
}

static void provision_handle_message(provision_mqtt_t *context,
                                     const char *json, size_t length) {
  if (context == NULL || context->grant_ready) {
    return;
  }

  const char *parse_end = NULL;
  cJSON *root = cJSON_ParseWithLengthOpts(json, length + 1U, &parse_end, true);
  const cJSON *type =
      root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "type");
  if (!cJSON_IsObject(root) || parse_end != json + length ||
      !cJSON_IsString(type) || strcmp(type->valuestring, "auth_grant") != 0) {
    ESP_LOGW(TAG, "temporary MQTT message ignored: bytes=%u type=%s",
             (unsigned)length,
             cJSON_IsString(type) ? type->valuestring : "-");
    json_secure_delete(root);
    return;
  }

  cJSON *owned_payload = NULL;
  const cJSON *payload = provision_payload_object(root, &owned_payload);
  const cJSON *device_id =
      payload == NULL ? NULL
                      : json_string_alias(payload, "device_id", "deviceId");
  const cJSON *device_key =
      payload == NULL
          ? NULL
          : json_string_alias(payload, "device_key", "device_secret");
  bool has_id = cJSON_IsString(device_id);
  bool has_key = cJSON_IsString(device_key);
  if (has_id != has_key ||
      (has_id &&
       (strlen(device_id->valuestring) >= sizeof(context->device_id) ||
        strlen(device_key->valuestring) >= sizeof(context->device_secret)))) {
    ESP_LOGE(TAG, "auth_grant contains invalid device credentials");
    json_secure_delete(owned_payload);
    json_secure_delete(root);
    provision_finish_with_error(context);
    return;
  }

  if (has_id) {
    strlcpy(context->device_id, device_id->valuestring,
            sizeof(context->device_id));
    strlcpy(context->device_secret, device_key->valuestring,
            sizeof(context->device_secret));
  }
  json_secure_delete(owned_payload);
  json_secure_delete(root);

  if (context->device_id[0] == '\0' || context->device_secret[0] == '\0') {
    ESP_LOGE(TAG, "auth_grant did not provide or retain credentials");
    provision_finish_with_error(context);
    return;
  }

  char ack_topic[PLATFORM_MQTT_TOPIC_MAX] = {0};
  int ack_topic_length = snprintf(ack_topic, sizeof(ack_topic),
                                  "device/%s/ack", context->temp_client_id);
  if (ack_topic_length <= 0 ||
      (size_t)ack_topic_length >= sizeof(ack_topic)) {
    ESP_LOGE(TAG, "auth_grant ACK topic is too long");
    provision_finish_with_error(context);
    return;
  }
  context->grant_ack_message_id = esp_mqtt_client_publish(
      context->mqtt, ack_topic, "{\"ack\":true}", 12, 1, 0);
  if (context->grant_ack_message_id < 0) {
    ESP_LOGE(TAG, "auth_grant ACK enqueue failed");
    provision_finish_with_error(context);
    return;
  }

  context->grant_ready = true;
  platform_emit_provision_progress(
      PLATFORM_PROVISION_PROGRESS_GRANT_VALIDATED);
  xEventGroupSetBits(context->events, PROVISION_GRANT_BIT);
  ESP_LOGI(TAG, "auth_grant validated; ACK queued before credential persistence");
}

static void provision_mqtt_event(void *handler_args, esp_event_base_t base,
                                 int32_t event_id, void *event_data) {
  (void)base;
  provision_mqtt_t *context = handler_args;
  esp_mqtt_event_handle_t event = event_data;
  if (context == NULL || event == NULL) {
    return;
  }

  if (event_id == MQTT_EVENT_CONNECTED) {
    platform_emit_provision_progress(
        PLATFORM_PROVISION_PROGRESS_MQTT_CONNECTED);
    char topic[PLATFORM_MQTT_TOPIC_MAX] = {0};
    snprintf(topic, sizeof(topic), "device/%s/cmd", context->temp_client_id);
    context->subscription_message_id =
        esp_mqtt_client_subscribe(event->client, topic, 1);
    if (context->subscription_message_id < 0) {
      ESP_LOGE(TAG, "temporary MQTT subscription failed");
      provision_finish_with_error(context);
    } else {
      ESP_LOGI(TAG, "temporary MQTT connected; waiting for SUBACK");
    }
    return;
  }

  if (event_id == MQTT_EVENT_SUBSCRIBED &&
      event->msg_id == context->subscription_message_id) {
    if (!mqtt_subscription_ack_ok(event)) {
      ESP_LOGE(TAG, "temporary MQTT subscription was rejected");
      provision_finish_with_error(context);
    } else {
      ESP_LOGI(TAG, "temporary MQTT subscription confirmed");
      platform_emit_provision_progress(
          PLATFORM_PROVISION_PROGRESS_SUBSCRIBED);
      provision_publish_code_ready(context);
    }
    return;
  }

  if (event_id == MQTT_EVENT_DATA) {
    if (event->current_data_offset == 0 &&
        !mqtt_reassembly_begin(&context->incoming, event,
                               MQTT_MESSAGE_COMMAND)) {
      ESP_LOGW(TAG, "dropping invalid temporary MQTT message");
      return;
    }
    if (mqtt_reassembly_append(&context->incoming, event)) {
      ESP_LOGI(TAG, "temporary MQTT message received: bytes=%u",
               (unsigned)context->incoming.received_size);
      platform_emit_provision_progress(
          PLATFORM_PROVISION_PROGRESS_MESSAGE_RECEIVED);
      if (mqtt_payload_is_json_object(context->incoming.payload,
                                      context->incoming.received_size)) {
        provision_handle_message(context, context->incoming.payload,
                                 context->incoming.received_size);
      }
      mqtt_reassembly_reset(&context->incoming);
    } else if (!context->incoming.active) {
      ESP_LOGW(TAG, "dropping temporary MQTT fragment");
    }
    return;
  }

  if (event_id == MQTT_EVENT_PUBLISHED) {
    int published_id = event->msg_id;
    if (context->published_ids == NULL ||
        xQueueSend(context->published_ids, &published_id, 0) != pdTRUE) {
      provision_finish_with_error(context);
    }
    return;
  }

  if (event_id == MQTT_EVENT_DISCONNECTED) {
    ESP_LOGW(TAG, "temporary MQTT disconnected; waiting for reconnect");
    return;
  }

  if (event_id == MQTT_EVENT_ERROR) {
    ESP_LOGE(TAG, "temporary MQTT transport/authentication error");
    provision_finish_with_error(context);
  }
}

static TickType_t remaining_ticks(TickType_t start, TickType_t total) {
  TickType_t elapsed = xTaskGetTickCount() - start;
  return elapsed >= total ? 0 : total - elapsed;
}

static esp_err_t provision_wait_for_puback(provision_mqtt_t *context,
                                           int expected_message_id,
                                           TickType_t timeout_ticks) {
  TickType_t started = xTaskGetTickCount();

  while (remaining_ticks(started, timeout_ticks) > 0) {
    int published_message_id = -1;
    while (xQueueReceive(context->published_ids, &published_message_id, 0) ==
           pdTRUE) {
      if (published_message_id == expected_message_id) {
        return ESP_OK;
      }
    }
    EventBits_t bits = xEventGroupGetBits(context->events);
    if ((bits & PROVISION_ERROR_BIT) != 0U) {
      return ESP_FAIL;
    }
    TickType_t remaining = remaining_ticks(started, timeout_ticks);
    TickType_t slice = pdMS_TO_TICKS(100);
    if (slice == 0 || slice > remaining) {
      slice = remaining;
    }
    published_message_id = -1;
    if (xQueueReceive(context->published_ids, &published_message_id, slice) ==
            pdTRUE &&
        published_message_id == expected_message_id) {
      return ESP_OK;
    }
  }
  int published_message_id = -1;
  while (xQueueReceive(context->published_ids, &published_message_id, 0) ==
         pdTRUE) {
    if (published_message_id == expected_message_id) {
      return ESP_OK;
    }
  }
  return ESP_ERR_TIMEOUT;
}

static esp_err_t wait_for_auth_grant(const provision_report_t *report,
                                     const platform_provision_config_t *config,
                                     platform_provision_result_t *result) {
  provision_mqtt_t context = {
      .subscription_message_id = -1,
      .grant_ack_message_id = -1,
  };
  mqtt_reassembly_reset(&context.incoming);
  strlcpy(context.temp_client_id, report->temp_client_id,
          sizeof(context.temp_client_id));
  strlcpy(context.verification_code, report->code,
          sizeof(context.verification_code));
  if (config->existing_device_id != NULL &&
      config->existing_device_secret != NULL) {
    strlcpy(context.device_id, config->existing_device_id,
            sizeof(context.device_id));
    strlcpy(context.device_secret, config->existing_device_secret,
            sizeof(context.device_secret));
  }

  context.events = xEventGroupCreate();
  if (context.events == NULL) {
    secure_zero(&context, sizeof(context));
    return ESP_ERR_NO_MEM;
  }
  context.published_ids = xQueueCreate(4, sizeof(int));
  if (context.published_ids == NULL) {
    vEventGroupDelete(context.events);
    secure_zero(&context, sizeof(context));
    return ESP_ERR_NO_MEM;
  }

  const esp_mqtt_client_config_t mqtt_config = {
      .broker.address.uri = s_services.mqtt,
      .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
      .credentials.username = report->temp_client_id,
      .credentials.client_id = report->temp_client_id,
      .credentials.authentication.password = report->temp_token,
      .session.keepalive = 60,
      .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
      .network.timeout_ms = 10000,
      .network.reconnect_timeout_ms = 3000,
      .task.stack_size = 6144,
      .buffer.size = 1024,
      .buffer.out_size = 1024,
  };
  context.mqtt = esp_mqtt_client_init(&mqtt_config);
  if (context.mqtt == NULL) {
    vQueueDelete(context.published_ids);
    vEventGroupDelete(context.events);
    secure_zero(&context, sizeof(context));
    return ESP_ERR_NO_MEM;
  }

  esp_err_t err = esp_mqtt_client_register_event(
      context.mqtt, ESP_EVENT_ANY_ID, provision_mqtt_event, &context);
  if (err == ESP_OK) {
    err = esp_mqtt_client_start(context.mqtt);
  }
  if (err != ESP_OK) {
    esp_mqtt_client_destroy(context.mqtt);
    vQueueDelete(context.published_ids);
    vEventGroupDelete(context.events);
    secure_zero(&context, sizeof(context));
    return err;
  }

  unsigned timeout_seconds = config->timeout_seconds == 0U
                                 ? PLATFORM_DEFAULT_PROVISION_TIMEOUT_SECONDS
                                 : config->timeout_seconds;
  TickType_t total_ticks = pdMS_TO_TICKS(timeout_seconds * 1000U);
  TickType_t started = xTaskGetTickCount();
  EventBits_t bits = xEventGroupWaitBits(
      context.events, PROVISION_READY_BIT | PROVISION_ERROR_BIT, pdFALSE,
      pdFALSE, total_ticks);
  if ((bits & PROVISION_READY_BIT) == 0U) {
    err = (bits & PROVISION_ERROR_BIT) != 0U ? ESP_FAIL : ESP_ERR_TIMEOUT;
    goto cleanup;
  }

  TickType_t grant_ticks = remaining_ticks(started, total_ticks);
  bits = xEventGroupWaitBits(
      context.events, PROVISION_GRANT_BIT | PROVISION_ERROR_BIT, pdFALSE,
      pdFALSE, grant_ticks);
  if ((bits & PROVISION_GRANT_BIT) == 0U) {
    err = (bits & PROVISION_ERROR_BIT) != 0U ? ESP_FAIL : ESP_ERR_TIMEOUT;
    goto cleanup;
  }

  TickType_t ack_ticks = pdMS_TO_TICKS(PLATFORM_PROVISION_ACK_TIMEOUT_MS);
  TickType_t overall_remaining = remaining_ticks(started, total_ticks);
  if (overall_remaining < ack_ticks) {
    ack_ticks = overall_remaining;
  }
  err = provision_wait_for_puback(&context, context.grant_ack_message_id,
                                  ack_ticks);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "auth_grant ACK PUBACK was not confirmed");
    goto cleanup;
  }
  platform_emit_provision_progress(
      PLATFORM_PROVISION_PROGRESS_ACK_CONFIRMED);

  platform_provision_result_t candidate = {0};
  strlcpy(candidate.device_id, context.device_id, sizeof(candidate.device_id));
  strlcpy(candidate.device_secret, context.device_secret,
          sizeof(candidate.device_secret));
  err = config->persist_credentials(&candidate, config->persist_user_data);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "credential persistence rejected auth_grant: %s",
             esp_err_to_name(err));
    platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_PROVISION, err, 0, 0);
    secure_zero(&candidate, sizeof(candidate));
    goto cleanup;
  }
  platform_emit_provision_progress(
      PLATFORM_PROVISION_PROGRESS_CREDENTIALS_PERSISTED);

  *result = candidate;
  secure_zero(&candidate, sizeof(candidate));
  ESP_LOGI(TAG, "auth_grant persisted and ACK confirmed");
  err = ESP_OK;

cleanup:
  (void)esp_mqtt_client_stop(context.mqtt);
  (void)esp_mqtt_client_destroy(context.mqtt);
  vQueueDelete(context.published_ids);
  vEventGroupDelete(context.events);
  secure_zero(context.device_secret, sizeof(context.device_secret));
  secure_zero(context.incoming.payload, sizeof(context.incoming.payload));
  return err;
}

static bool provisioning_begin(void) {
  bool allowed = false;

  taskENTER_CRITICAL(&s_state_mux);
  if (!s_provisioning && !s_client_active && s_request_task == NULL &&
      s_request_queue == NULL && s_mqtt_state_queue == NULL &&
      s_mqtt == NULL && !s_worker_stop_requested &&
      !s_request_submission_closed) {
    s_provisioning = true;
    allowed = true;
  }
  taskEXIT_CRITICAL(&s_state_mux);
  return allowed;
}

static void provisioning_end(void) {
  taskENTER_CRITICAL(&s_state_mux);
  s_provisioning = false;
  taskEXIT_CRITICAL(&s_state_mux);
}

esp_err_t platform_client_provision(const platform_provision_config_t *config,
                                    platform_provision_result_t *result) {
  const bool has_any_pending_callback =
      config != NULL && (config->load_pending != NULL ||
                         config->save_pending != NULL ||
                         config->clear_pending != NULL);
  const bool has_all_pending_callbacks =
      config != NULL && config->load_pending != NULL &&
      config->save_pending != NULL && config->clear_pending != NULL;
  if (config == NULL || result == NULL || config->persist_credentials == NULL ||
      config->mac_address == NULL || config->mac_address[0] == '\0' ||
      (config->discovery_url != NULL && config->discovery_url[0] != '\0' &&
       !platform_is_https_url(config->discovery_url)) ||
      strlen(config->mac_address) >= sizeof(s_mac_address) ||
      config->timeout_seconds > 3600U ||
      ((config->existing_device_id == NULL) !=
       (config->existing_device_secret == NULL)) ||
       (config->existing_device_id != NULL &&
        (config->existing_device_id[0] == '\0' ||
         config->existing_device_secret[0] == '\0' ||
         strlen(config->existing_device_id) >= sizeof(result->device_id) ||
         strlen(config->existing_device_secret) >=
             sizeof(result->device_secret))) ||
      (has_any_pending_callback && !has_all_pending_callbacks)) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!ensure_client_mutexes() ||
      xSemaphoreTakeRecursive(s_api_lifecycle_mutex, portMAX_DELAY) !=
          pdTRUE) {
    return ESP_ERR_NO_MEM;
  }
  if (!provisioning_begin()) {
    xSemaphoreGiveRecursive(s_api_lifecycle_mutex);
    return ESP_ERR_INVALID_STATE;
  }

  memset(result, 0, sizeof(*result));
  esp_err_t err = sync_clock();
  if (err != ESP_OK) {
    platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_PROVISION, err, 0, 0);
    goto done;
  }

  const char *discovery =
      config->discovery_url != NULL && config->discovery_url[0] != '\0'
          ? config->discovery_url
          : PLATFORM_DEFAULT_DISCOVERY;
  if (!s_services_ready) {
    err = discover_services(discovery);
    if (err != ESP_OK) {
      platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_PROVISION, err, 0, 0);
      goto done;
    }
  }

  const unsigned requested_timeout =
      config->timeout_seconds == 0U ? PLATFORM_DEFAULT_PROVISION_TIMEOUT_SECONDS
                                    : config->timeout_seconds;
  platform_provision_config_t effective_config = *config;
  provision_report_t report = {0};
  platform_pending_provision_t pending = {0};
  time_t now = time(NULL);
  bool restored_pending = false;

  if (has_all_pending_callbacks) {
    esp_err_t load_err =
        config->load_pending(&pending, config->pending_user_data);
    if (load_err == ESP_OK && pending_provision_valid(&pending, config, now)) {
      report_from_pending(&pending, &report);
      const int64_t remaining = pending.expires_at_unix - (int64_t)now;
      effective_config.timeout_seconds =
          remaining > 0 && remaining < (int64_t)requested_timeout
              ? (unsigned)remaining
              : requested_timeout;
      restored_pending = true;
      ESP_LOGI(TAG, "pending binding session restored; ttl_left=%us",
               effective_config.timeout_seconds);
    } else if (load_err == ESP_OK || load_err == ESP_ERR_INVALID_SIZE) {
      (void)config->clear_pending(config->pending_user_data);
    } else if (load_err != ESP_ERR_NOT_FOUND) {
      ESP_LOGW(TAG, "pending binding session load failed: %s",
               esp_err_to_name(load_err));
    }
  }

  if (!restored_pending) {
    err = report_for_provision(config, &report);
    if (err != ESP_OK) {
      platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_PROVISION, err, 0, 0);
      secure_zero(report.temp_token, sizeof(report.temp_token));
      secure_zero(&pending, sizeof(pending));
      goto done;
    }
    effective_config.timeout_seconds = requested_timeout;
    if (has_all_pending_callbacks) {
      now = time(NULL);
      pending_from_report(config, &report, now + requested_timeout, &pending);
      esp_err_t save_err =
          config->save_pending(&pending, config->pending_user_data);
      if (save_err != ESP_OK) {
        ESP_LOGW(TAG, "pending binding session save failed: %s",
                 esp_err_to_name(save_err));
      } else {
        ESP_LOGI(TAG, "pending binding session saved; ttl=%us",
                 requested_timeout);
      }
    }
  }

  err = wait_for_auth_grant(&report, &effective_config, result);
  secure_zero(report.temp_token, sizeof(report.temp_token));
  if (err != ESP_OK) {
    platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_PROVISION, err, 0, 0);
  } else if (has_all_pending_callbacks) {
    esp_err_t clear_err =
        config->clear_pending(config->pending_user_data);
    if (clear_err != ESP_OK) {
      ESP_LOGW(TAG, "pending binding session clear failed: %s",
               esp_err_to_name(clear_err));
    }
  }
  if (err == ESP_ERR_TIMEOUT && has_all_pending_callbacks) {
    (void)config->clear_pending(config->pending_user_data);
  }
  secure_zero(&pending, sizeof(pending));

done:
  s_verification_code[0] = '\0';
  provisioning_end();
  xSemaphoreGiveRecursive(s_api_lifecycle_mutex);
  return err;
}

static esp_err_t
platform_client_start_locked(const platform_client_config_t *config) {
  if (s_ready) {
    return ESP_OK;
  }
  if (s_client_active) {
    return ESP_ERR_INVALID_STATE;
  }
  bool stopping = false;
  taskENTER_CRITICAL(&s_state_mux);
  stopping = s_worker_stop_requested || s_request_submission_closed;
  taskEXIT_CRITICAL(&s_state_mux);
  if (stopping) {
    return ESP_ERR_INVALID_STATE;
  }
  if (config == NULL || config->device_id == NULL ||
      config->device_secret == NULL || config->mac_address == NULL ||
      config->device_id[0] == '\0' || config->device_secret[0] == '\0' ||
      config->mac_address[0] == '\0' ||
      strlen(config->device_id) >= sizeof(s_device_id) ||
      strlen(config->device_secret) >= sizeof(s_device_secret) ||
      strlen(config->mac_address) >= sizeof(s_mac_address)) {
    return ESP_ERR_INVALID_ARG;
  }
  if (s_provisioning) {
    return ESP_ERR_INVALID_STATE;
  }

  strlcpy(s_device_id, config->device_id, sizeof(s_device_id));
  strlcpy(s_device_secret, config->device_secret, sizeof(s_device_secret));
  strlcpy(s_mac_address, config->mac_address, sizeof(s_mac_address));
  int client_length =
      snprintf(s_client_id, sizeof(s_client_id), "sn_%s", config->device_id);
  if (client_length <= 0 || (size_t)client_length >= sizeof(s_client_id)) {
    return ESP_ERR_INVALID_SIZE;
  }

  esp_err_t err = sync_clock();
  if (err != ESP_OK) {
    platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_AUTH, err, 0, 0);
    return err;
  }

  const char *discovery =
      config->discovery_url != NULL && config->discovery_url[0] != '\0'
          ? config->discovery_url
          : PLATFORM_DEFAULT_DISCOVERY;
  if (!s_services_ready) {
    err = discover_services(discovery);
    if (err != ESP_OK) {
      return err;
    }
  }

  int status = 0;
  int service_code = -1;
  char token[PLATFORM_MQTT_TOKEN_MAX] = {0};
  err = obtain_mqtt_token(token, sizeof(token), &status, &service_code);
  if (err == ESP_ERR_NOT_FOUND || service_code == 6006) {
    secure_zero(token, sizeof(token));
    platform_emit_rebind_required(PLATFORM_CLIENT_EVENT_SOURCE_AUTH, status,
                                  6006U);
    return ESP_ERR_NOT_FOUND;
  }
  if (err != ESP_OK) {
    secure_zero(token, sizeof(token));
    platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_AUTH, err, status,
                        (unsigned)(service_code > 0 ? service_code : 0));
    return err;
  }
  platform_emit_simple(PLATFORM_CLIENT_EVENT_AUTH_READY,
                       PLATFORM_CLIENT_EVENT_SOURCE_AUTH);

  err = ensure_worker();
  if (err != ESP_OK) {
    platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_INTERNAL, err, 0, 0);
    secure_zero(token, sizeof(token));
    return err;
  }
  uint32_t client_epoch = 0U;
  uint32_t auth_generation = 0U;
  taskENTER_CRITICAL(&s_state_mux);
  s_unbind_pending = false;
  s_rebind_event_pending = false;
  s_rebind_emit_mqtt_disconnected = false;
  s_rebind_source = PLATFORM_CLIENT_EVENT_SOURCE_INTERNAL;
  s_client_active = true;
  s_ready = false;
  s_client_epoch = next_generation(s_client_epoch);
  s_token_refresh_generation =
      next_generation(s_token_refresh_generation);
  s_token_refresh_requested = false;
  s_token_refresh_reason = 0U;
  s_token_refresh_due_ms = 0;
  client_epoch = s_client_epoch;
  auth_generation = s_token_refresh_generation;
  taskEXIT_CRITICAL(&s_state_mux);
  err = start_mqtt(token, client_epoch, auth_generation);
  secure_zero(token, sizeof(token));
  if (err != ESP_OK) {
    if (err == ESP_ERR_NOT_FINISHED) {
      return ESP_OK;
    }
    ESP_LOGE(TAG, "MQTT start failed: %s", esp_err_to_name(err));
    platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_MQTT, err, 0, 0);
    (void)platform_client_stop();
    return err;
  }

  err = commit_client_ready(client_epoch, auth_generation);
  if (err == ESP_ERR_NOT_FINISHED) {
    return ESP_OK;
  }
  if (err != ESP_OK) {
    stop_mqtt();
    clear_mqtt_token();
    return err;
  }
  return ESP_OK;
}

static esp_err_t platform_client_stop_locked(void) {
  if (s_provisioning) {
    return ESP_ERR_INVALID_STATE;
  }
  if (s_request_task != NULL && xTaskGetCurrentTaskHandle() == s_request_task) {
    return ESP_ERR_INVALID_STATE;
  }

  taskENTER_CRITICAL(&s_state_mux);
  s_request_submission_closed = true;
  taskEXIT_CRITICAL(&s_state_mux);

  int64_t submission_deadline =
      monotonic_ms() + PLATFORM_REQUEST_SUBMISSION_DRAIN_TIMEOUT_MS;
  while (request_submissions() != 0U &&
         monotonic_ms() < submission_deadline) {
    vTaskDelay(1);
  }
  if (request_submissions() != 0U) {
    taskENTER_CRITICAL(&s_state_mux);
    s_request_submission_closed = false;
    taskEXIT_CRITICAL(&s_state_mux);
    platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_INTERNAL,
                        ESP_ERR_TIMEOUT, 0, 0);
    ESP_LOGE(TAG, "request submission drain timed out; stop not started");
    return ESP_ERR_TIMEOUT;
  }

  bool wait_for_worker = false;
  uint32_t worker_generation = 0U;
  taskENTER_CRITICAL(&s_state_mux);
  s_ready = false;
  s_client_active = false;
  s_worker_stop_requested = true;
  s_token_refresh_requested = false;
  s_client_epoch = next_generation(s_client_epoch);
  s_token_refresh_generation =
      next_generation(s_token_refresh_generation);
  wait_for_worker = s_request_task != NULL;
  worker_generation = s_worker_generation;
  taskEXIT_CRITICAL(&s_state_mux);

  stop_mqtt();
  if (wait_for_worker) {
    if (!wait_for_worker_completion(worker_generation,
                                    PLATFORM_WORKER_STOP_TIMEOUT_MS)) {
      platform_emit_error(PLATFORM_CLIENT_EVENT_SOURCE_INTERNAL,
                          ESP_ERR_TIMEOUT, 0, 0);
      return ESP_ERR_TIMEOUT;
    }
  }
  reject_queued_requests();
  emit_observer_mqtt_state(false, 0);

  if (s_request_queue != NULL) {
    vQueueDelete(s_request_queue);
    s_request_queue = NULL;
  }
  if (s_mqtt_state_queue != NULL) {
    vQueueDelete(s_mqtt_state_queue);
    s_mqtt_state_queue = NULL;
  }
  clear_mqtt_token();
  secure_zero(s_device_secret, sizeof(s_device_secret));
  memset(s_device_id, 0, sizeof(s_device_id));
  memset(s_client_id, 0, sizeof(s_client_id));
  memset(s_mac_address, 0, sizeof(s_mac_address));
  memset(&s_services, 0, sizeof(s_services));
  s_services_ready = false;
  s_mqtt_connected = false;
  s_verification_code[0] = '\0';

  taskENTER_CRITICAL(&s_state_mux);
  s_rebind_event_pending = false;
  s_rebind_emit_mqtt_disconnected = false;
  s_rebind_source = PLATFORM_CLIENT_EVENT_SOURCE_INTERNAL;
  s_rebind_status_code = 0;
  s_rebind_reason_code = 0U;
  s_mqtt_state_queue_overflow = false;
  s_worker_stop_requested = false;
  s_request_submission_closed = false;
  s_token_refresh_reason = 0;
  s_token_refresh_due_ms = 0;
  s_token_refresh_backoff_ms = PLATFORM_TOKEN_REFRESH_INITIAL_MS;
  taskEXIT_CRITICAL(&s_state_mux);
  return ESP_OK;
}

esp_err_t platform_client_start(const platform_client_config_t *config) {
  if (!ensure_client_mutexes() ||
      xSemaphoreTakeRecursive(s_api_lifecycle_mutex, portMAX_DELAY) !=
          pdTRUE) {
    return ESP_ERR_NO_MEM;
  }
  esp_err_t result = platform_client_start_locked(config);
  xSemaphoreGiveRecursive(s_api_lifecycle_mutex);
  return result;
}

esp_err_t platform_client_stop(void) {
  if (!ensure_client_mutexes() ||
      xSemaphoreTakeRecursive(s_api_lifecycle_mutex, portMAX_DELAY) !=
          pdTRUE) {
    return ESP_ERR_NO_MEM;
  }
  esp_err_t result = platform_client_stop_locked();
  xSemaphoreGiveRecursive(s_api_lifecycle_mutex);
  return result;
}

bool platform_client_ready(void) { return s_ready; }

bool platform_client_mqtt_connected(void) { return s_mqtt_connected; }

bool platform_client_provisioning(void) { return s_provisioning; }

const char *platform_client_verification_code(void) {
  return s_verification_code;
}

const char *platform_client_device_id(void) { return s_device_id; }

const char *platform_client_tirtc_endpoint(void) { return s_services.tirtc; }

esp_err_t platform_client_request_ex(platform_service_t service,
                                     const char *method, const char *path,
                                     const char *body,
                                     platform_response_callback_t callback,
                                     void *user_data) {
  esp_http_client_method_t ignored_method = HTTP_METHOD_GET;
  if (!platform_method_valid(method, &ignored_method) || path == NULL ||
      path[0] != '/' || strlen(path) >= PLATFORM_REQUEST_PATH_MAX ||
      (body != NULL && strlen(body) >= PLATFORM_REQUEST_BODY_MAX) ||
      (strcmp(method, PLATFORM_HTTP_METHOD_GET) == 0 && body != NULL &&
       body[0] != '\0')) {
    return ESP_ERR_INVALID_ARG;
  }

  QueueHandle_t queue = NULL;
  uint32_t client_epoch = 0U;
  uint32_t auth_generation = 0U;
  if (!reserve_request_submission(&queue, &client_epoch, &auth_generation)) {
    return ESP_ERR_INVALID_STATE;
  }
  if (service_base(service) == NULL) {
    release_request_submission();
    return ESP_ERR_INVALID_ARG;
  }

  platform_request_t request = {
      .service = service,
      .has_body = body != NULL,
      .callback = callback,
      .user_data = user_data,
      .client_epoch = client_epoch,
      .auth_generation = auth_generation,
  };
  strlcpy(request.method, method, sizeof(request.method));
  strlcpy(request.path, path, sizeof(request.path));
  if (body != NULL) {
    strlcpy(request.body, body, sizeof(request.body));
  }
  BaseType_t queued = xQueueSend(queue, &request, 0);
  release_request_submission();
  return queued == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t platform_client_request(platform_service_t service, const char *path,
                                  const char *json_body,
                                  platform_response_callback_t callback,
                                  void *user_data) {
  return platform_client_request_ex(
      service,
      json_body == NULL ? PLATFORM_HTTP_METHOD_GET : PLATFORM_HTTP_METHOD_POST,
      path, json_body, callback, user_data);
}

void platform_client_set_signal_handler(platform_signal_callback_t callback,
                                        void *user_data) {
  taskENTER_CRITICAL(&s_state_mux);
  s_signal_callback = callback;
  s_signal_user_data = user_data;
  taskEXIT_CRITICAL(&s_state_mux);
}

void platform_client_set_observer(platform_client_observer_t observer,
                                  void *user_data) {
  taskENTER_CRITICAL(&s_state_mux);
  s_observer = observer;
  s_observer_user_data = user_data;
  taskEXIT_CRITICAL(&s_state_mux);
}
