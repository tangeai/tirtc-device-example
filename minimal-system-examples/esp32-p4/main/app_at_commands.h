#ifndef APP_AT_COMMANDS_H
#define APP_AT_COMMANDS_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the complete user-facing command table and starts UART AT I/O. */
esp_err_t app_at_commands_start(void);

#ifdef __cplusplus
}
#endif

#endif
