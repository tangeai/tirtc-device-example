#ifndef APP_USER_LOG_H
#define APP_USER_LOG_H

#include <stdbool.h>
#include <stddef.h>

#include "app_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_USER_LOG_CATEGORY_SIZE 16
#define APP_USER_LOG_SUBJECT_SIZE 16
#define APP_USER_LOG_TEXT_SIZE 192

typedef enum {
  APP_USER_LOG_LEVEL_NONE = 0,
  APP_USER_LOG_LEVEL_INFO,
  APP_USER_LOG_LEVEL_WARNING,
  APP_USER_LOG_LEVEL_ERROR,
} app_user_log_level_t;

typedef struct {
  app_user_log_level_t level;
  char category[APP_USER_LOG_CATEGORY_SIZE];
  char subject[APP_USER_LOG_SUBJECT_SIZE];
  char text[APP_USER_LOG_TEXT_SIZE];
} app_user_log_message_t;

void app_user_log_init(void);

/*
 * Projects an internal event onto the concise Chinese user log. The message is
 * cleared first and populated only when the event is actually displayed.
 */
bool app_user_log_event(const app_event_t *event,
                        app_user_log_message_t *message);

#ifdef __cplusplus
}
#endif

#endif
