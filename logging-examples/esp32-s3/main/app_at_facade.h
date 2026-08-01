#ifndef APP_AT_FACADE_H
#define APP_AT_FACADE_H

#include <stdbool.h>

#include "app_controller.h"
#include "app_user_log.h"
#include "at_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*app_at_facade_submit_fn)(app_intent_type_t type,
                                             const char *first,
                                             const char *second,
                                             const char *third, bool accepted,
                                             uint32_t *request_id);

typedef void (*app_at_facade_raw_event_fn)(const app_event_t *event);

esp_err_t app_at_facade_init(app_at_facade_submit_fn submit);

/*
 * Public, developer-facing facade:
 *   AT+TIRTC=?
 *   AT+TIRTC?
 *   AT+TIRTC=<Chinese action>[,...]
 */
esp_err_t app_at_facade_command(const at_server_request_t *request,
                                void *context);

/* Hidden compatibility switch used by the structured regression harness. */
esp_err_t app_at_protocol_command(const at_server_request_t *request,
                                  void *context);

/* Legacy structured commands are accepted only after AT+PROTO=RAW. */
bool app_at_facade_raw_mode(void);

/*
 * Route one controller event to either the concise Chinese facade or the
 * legacy structured protocol. Exactly one projection is emitted.
 */
void app_at_facade_route_event(const app_event_t *event,
                               const app_user_log_message_t *user_message,
                               app_at_facade_raw_event_fn raw_event);

#ifdef __cplusplus
}
#endif

#endif
