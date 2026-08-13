#ifndef RUNTIME_CONFIG_H
#define RUNTIME_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RUNTIME_CONFIG_RECORD_VERSION 1U

typedef struct {
  char device_id[65];
  char device_secret[257];
  char client_id[65];
  char service_endpoint[256];
} runtime_tirtc_config_t;

typedef struct {
  unsigned version;
  unsigned generation;
} runtime_config_record_info_t;

/* The simple API remains source-compatible. Internally it uses a validated,
 * versioned, dual-slot record so a failed write cannot destroy the last good
 * credential set. */
esp_err_t runtime_config_load_tirtc(runtime_tirtc_config_t *config);
esp_err_t runtime_config_save_tirtc(const runtime_tirtc_config_t *config);

/* Extended variants expose the record metadata without exposing its on-flash
 * representation. info may be NULL. */
esp_err_t runtime_config_load_tirtc_ex(runtime_tirtc_config_t *config,
                                       runtime_config_record_info_t *info);
esp_err_t runtime_config_save_tirtc_ex(const runtime_tirtc_config_t *config,
                                       runtime_config_record_info_t *info);

esp_err_t runtime_config_clear_tirtc(void);
bool runtime_config_tirtc_valid(const runtime_tirtc_config_t *config,
                                char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
