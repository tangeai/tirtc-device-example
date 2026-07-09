#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t platform_nvs_async_init(void);
esp_err_t platform_nvs_async_set_blob(const char *namespace_name,
                                      const char *key,
                                      const void *value,
                                      size_t value_len);
esp_err_t platform_nvs_async_set_str(const char *namespace_name,
                                     const char *key,
                                     const char *value);
esp_err_t platform_nvs_async_set_u8(const char *namespace_name,
                                    const char *key,
                                    uint8_t value);
esp_err_t platform_nvs_async_erase_key(const char *namespace_name,
                                       const char *key);
esp_err_t platform_nvs_async_get_blob(const char *namespace_name,
                                      const char *key,
                                      void *value,
                                      size_t *value_len);
