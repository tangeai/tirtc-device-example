#include "runtime_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nvs.h"

#define TIRTC_NVS_NAMESPACE "tirtc_cfg"
#define TIRTC_NVS_RECORD_A "record_a"
#define TIRTC_NVS_RECORD_B "record_b"
#define TIRTC_RECORD_MAGIC 0x54495243U

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t record_size;
  uint32_t generation;
  runtime_tirtc_config_t config;
  uint32_t checksum;
} runtime_tirtc_store_t;

static void set_error(char *error, size_t error_size, const char *message) {
  if (error != NULL && error_size > 0) {
    (void)snprintf(error, error_size, "%s", message);
  }
}

bool runtime_config_tirtc_valid(const runtime_tirtc_config_t *config,
                                char *error, size_t error_size) {
  if (config == NULL) {
    set_error(error, error_size, "config is null");
    return false;
  }
  size_t device_id_length =
      strnlen(config->device_id, sizeof(config->device_id));
  size_t secret_length =
      strnlen(config->device_secret, sizeof(config->device_secret));
  size_t client_id_length =
      strnlen(config->client_id, sizeof(config->client_id));
  size_t endpoint_length =
      strnlen(config->service_endpoint, sizeof(config->service_endpoint));
  if (device_id_length == 0 || device_id_length >= sizeof(config->device_id)) {
    set_error(error, error_size, "device_id length must be 1..64 bytes");
    return false;
  }
  if (secret_length == 0 || secret_length >= sizeof(config->device_secret)) {
    set_error(error, error_size, "device_secret length must be 1..256 bytes");
    return false;
  }
  if (client_id_length >= sizeof(config->client_id)) {
    set_error(error, error_size, "client_id is not terminated");
    return false;
  }
  if (endpoint_length >= sizeof(config->service_endpoint)) {
    set_error(error, error_size, "service_endpoint is not terminated");
    return false;
  }
  if (endpoint_length > 0 &&
      strncmp(config->service_endpoint, "http://", 7) != 0 &&
      strncmp(config->service_endpoint, "https://", 8) != 0) {
    set_error(error, error_size, "service_endpoint must use http or https");
    return false;
  }
  set_error(error, error_size, "");
  return true;
}

static uint32_t runtime_config_checksum(const void *data, size_t length) {
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t crc = UINT32_MAX;

  for (size_t index = 0; index < length; ++index) {
    crc ^= bytes[index];
    for (unsigned bit = 0; bit < 8U; ++bit) {
      uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
      crc = (crc >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

static bool runtime_config_store_valid(const runtime_tirtc_store_t *store) {
  if (store == NULL || store->magic != TIRTC_RECORD_MAGIC ||
      store->version != RUNTIME_CONFIG_RECORD_VERSION ||
      store->record_size != sizeof(*store) || store->generation == 0U) {
    return false;
  }
  uint32_t checksum =
      runtime_config_checksum(store, offsetof(runtime_tirtc_store_t, checksum));
  return checksum == store->checksum &&
         runtime_config_tirtc_valid(&store->config, NULL, 0);
}

static esp_err_t runtime_config_read_store(nvs_handle_t nvs, const char *key,
                                           runtime_tirtc_store_t *store) {
  size_t size = sizeof(*store);

  memset(store, 0, sizeof(*store));
  esp_err_t err = nvs_get_blob(nvs, key, store, &size);
  if (err == ESP_OK && size != sizeof(*store)) {
    memset(store, 0, sizeof(*store));
    return ESP_ERR_INVALID_SIZE;
  }
  return err;
}

static bool runtime_config_generation_newer(uint32_t left, uint32_t right) {
  return (int32_t)(left - right) > 0;
}

static const runtime_tirtc_store_t *
runtime_config_pick_store(const runtime_tirtc_store_t *a, bool a_valid,
                          const runtime_tirtc_store_t *b, bool b_valid) {
  if (a_valid && b_valid) {
    return runtime_config_generation_newer(a->generation, b->generation) ? a
                                                                         : b;
  }
  if (a_valid) {
    return a;
  }
  return b_valid ? b : NULL;
}

static esp_err_t runtime_config_get_optional_string(nvs_handle_t nvs,
                                                    const char *key,
                                                    char *value,
                                                    size_t value_size) {
  size_t size = value_size;
  esp_err_t err = nvs_get_str(nvs, key, value, &size);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    value[0] = '\0';
    return ESP_OK;
  }
  return err;
}

static esp_err_t runtime_config_load_legacy(nvs_handle_t nvs,
                                            runtime_tirtc_config_t *config) {
  size_t size = sizeof(config->device_id);
  esp_err_t err = nvs_get_str(nvs, "device_id", config->device_id, &size);
  if (err == ESP_OK) {
    size = sizeof(config->device_secret);
    err = nvs_get_str(nvs, "secret", config->device_secret, &size);
  }
  if (err == ESP_OK) {
    err = runtime_config_get_optional_string(
        nvs, "client_id", config->client_id, sizeof(config->client_id));
  }
  if (err == ESP_OK) {
    err = runtime_config_get_optional_string(nvs, "endpoint",
                                             config->service_endpoint,
                                             sizeof(config->service_endpoint));
  }
  if (err == ESP_OK && !runtime_config_tirtc_valid(config, NULL, 0)) {
    err = ESP_ERR_INVALID_CRC;
  }
  return err;
}

esp_err_t runtime_config_load_tirtc_ex(runtime_tirtc_config_t *config,
                                       runtime_config_record_info_t *info) {
  if (config == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  memset(config, 0, sizeof(*config));
  if (info != NULL) {
    memset(info, 0, sizeof(*info));
  }

  nvs_handle_t nvs = 0;
  esp_err_t err = nvs_open(TIRTC_NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK) {
    return err;
  }

  runtime_tirtc_store_t a = {0};
  runtime_tirtc_store_t b = {0};
  esp_err_t a_err = runtime_config_read_store(nvs, TIRTC_NVS_RECORD_A, &a);
  esp_err_t b_err = runtime_config_read_store(nvs, TIRTC_NVS_RECORD_B, &b);
  bool a_valid = a_err == ESP_OK && runtime_config_store_valid(&a);
  bool b_valid = b_err == ESP_OK && runtime_config_store_valid(&b);
  const runtime_tirtc_store_t *selected =
      runtime_config_pick_store(&a, a_valid, &b, b_valid);

  if (selected != NULL) {
    *config = selected->config;
    if (info != NULL) {
      info->version = selected->version;
      info->generation = selected->generation;
    }
    nvs_close(nvs);
    return ESP_OK;
  }

  bool record_present =
      a_err != ESP_ERR_NVS_NOT_FOUND || b_err != ESP_ERR_NVS_NOT_FOUND;
  err = runtime_config_load_legacy(nvs, config);
  nvs_close(nvs);
  if (err != ESP_OK) {
    memset(config, 0, sizeof(*config));
    return record_present && err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_INVALID_CRC
                                                          : err;
  }

  /* Migrate the former multi-key layout after it has been fully validated.
   * Loading still succeeds if the best-effort migration cannot be committed. */
  runtime_config_record_info_t migrated = {0};
  if (runtime_config_save_tirtc_ex(config, &migrated) == ESP_OK &&
      info != NULL) {
    *info = migrated;
  }
  return ESP_OK;
}

esp_err_t runtime_config_save_tirtc_ex(const runtime_tirtc_config_t *config,
                                       runtime_config_record_info_t *info) {
  if (!runtime_config_tirtc_valid(config, NULL, 0)) {
    return ESP_ERR_INVALID_ARG;
  }
  if (info != NULL) {
    memset(info, 0, sizeof(*info));
  }

  nvs_handle_t nvs = 0;
  esp_err_t err = nvs_open(TIRTC_NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    return err;
  }

  runtime_tirtc_store_t a = {0};
  runtime_tirtc_store_t b = {0};
  bool a_valid =
      runtime_config_read_store(nvs, TIRTC_NVS_RECORD_A, &a) == ESP_OK &&
      runtime_config_store_valid(&a);
  bool b_valid =
      runtime_config_read_store(nvs, TIRTC_NVS_RECORD_B, &b) == ESP_OK &&
      runtime_config_store_valid(&b);
  const runtime_tirtc_store_t *current =
      runtime_config_pick_store(&a, a_valid, &b, b_valid);

  runtime_tirtc_store_t next = {
      .magic = TIRTC_RECORD_MAGIC,
      .version = RUNTIME_CONFIG_RECORD_VERSION,
      .record_size = sizeof(runtime_tirtc_store_t),
      .generation = current == NULL ? 1U : current->generation + 1U,
  };
  strlcpy(next.config.device_id, config->device_id,
          sizeof(next.config.device_id));
  strlcpy(next.config.device_secret, config->device_secret,
          sizeof(next.config.device_secret));
  strlcpy(next.config.client_id, config->client_id,
          sizeof(next.config.client_id));
  strlcpy(next.config.service_endpoint, config->service_endpoint,
          sizeof(next.config.service_endpoint));
  if (next.generation == 0U) {
    next.generation = 1U;
  }
  next.checksum =
      runtime_config_checksum(&next, offsetof(runtime_tirtc_store_t, checksum));

  const char *target_key = NULL;
  if (!a_valid) {
    target_key = TIRTC_NVS_RECORD_A;
  } else if (!b_valid) {
    target_key = TIRTC_NVS_RECORD_B;
  } else {
    target_key = current == &a ? TIRTC_NVS_RECORD_B : TIRTC_NVS_RECORD_A;
  }

  err = nvs_set_blob(nvs, target_key, &next, sizeof(next));
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);
  if (err == ESP_OK && info != NULL) {
    info->version = next.version;
    info->generation = next.generation;
  }
  return err;
}

esp_err_t runtime_config_load_tirtc(runtime_tirtc_config_t *config) {
  return runtime_config_load_tirtc_ex(config, NULL);
}

esp_err_t runtime_config_save_tirtc(const runtime_tirtc_config_t *config) {
  return runtime_config_save_tirtc_ex(config, NULL);
}

esp_err_t runtime_config_clear_tirtc(void) {
  nvs_handle_t nvs = 0;
  esp_err_t err = nvs_open(TIRTC_NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err == ESP_OK)
    err = nvs_erase_all(nvs);
  if (err == ESP_OK)
    err = nvs_commit(nvs);
  if (nvs != 0)
    nvs_close(nvs);
  return err;
}
