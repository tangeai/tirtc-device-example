#ifndef APP_IDENTITY_H
#define APP_IDENTITY_H

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CLIENT_ID_SIZE 13
#define APP_MAC_TEXT_SIZE 18

/* Derives both values from the factory Wi-Fi STA MAC.
 * client_id is 12 uppercase hex digits and remains stable across binding. */
esp_err_t app_identity_read(char *client_id,
                            size_t client_id_size,
                            char *mac_text,
                            size_t mac_text_size);

#ifdef __cplusplus
}
#endif

#endif
