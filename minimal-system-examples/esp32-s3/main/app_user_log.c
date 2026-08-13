#include "app_user_log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "device/device_utf8.h"
#include "esp_log.h"

static const char *TAG = "设备日志";
#define USER_CAPTION_LOG_BYTES 160U
static bool s_ai_active;
static bool s_call_in_progress;
static bool s_call_connected;
static bool s_view_in_progress;
static bool s_platform_retry_reported;

static void clear_message(app_user_log_message_t *message) {
  if (message != NULL) {
    memset(message, 0, sizeof(*message));
  }
}

static bool display_message(app_user_log_message_t *message,
                            app_user_log_level_t level, const char *category,
                            const char *subject, const char *format, ...) {
  char text[APP_USER_LOG_TEXT_SIZE];
  va_list arguments;
  va_start(arguments, format);
  int length = vsnprintf(text, sizeof(text), format, arguments);
  va_end(arguments);
  if (length < 0 || (size_t)length >= sizeof(text)) {
    return false;
  }

  const char *safe_subject = subject == NULL ? "" : subject;
  if (message != NULL) {
    message->level = level;
    (void)snprintf(message->category, sizeof(message->category), "%s",
                   category);
    (void)snprintf(message->subject, sizeof(message->subject), "%s",
                   safe_subject);
    (void)snprintf(message->text, sizeof(message->text), "%s", text);
  }

  switch (level) {
  case APP_USER_LOG_LEVEL_INFO:
    if (safe_subject[0] == '\0') {
      ESP_LOGI(TAG, "[%s] %s", category, text);
    } else {
      ESP_LOGI(TAG, "[%s][%s] %s", category, safe_subject, text);
    }
    break;
  case APP_USER_LOG_LEVEL_WARNING:
    if (safe_subject[0] == '\0') {
      ESP_LOGW(TAG, "[%s] %s", category, text);
    } else {
      ESP_LOGW(TAG, "[%s][%s] %s", category, safe_subject, text);
    }
    break;
  case APP_USER_LOG_LEVEL_ERROR:
    if (safe_subject[0] == '\0') {
      ESP_LOGE(TAG, "[%s] %s", category, text);
    } else {
      ESP_LOGE(TAG, "[%s][%s] %s", category, safe_subject, text);
    }
    break;
  case APP_USER_LOG_LEVEL_NONE:
  default:
    clear_message(message);
    return false;
  }
  return true;
}

void app_user_log_init(void) {
  /*
   * UART0 is the human-facing surface. Raw component logs stay off in user
   * mode; structured state and terminal failures are projected in Chinese.
   */
  esp_log_level_set("*", ESP_LOG_NONE);
  esp_log_level_set("TiRTC", ESP_LOG_NONE);
  esp_log_level_set(TAG, ESP_LOG_INFO);
  esp_log_level_set("app_main", ESP_LOG_WARN);
  s_ai_active = false;
  s_call_in_progress = false;
  s_call_connected = false;
  s_view_in_progress = false;
  s_platform_retry_reported = false;
}

static void summarize_caption(const char *source, char *destination,
                              size_t destination_size) {
  (void)device_utf8_sanitize_line(source, destination, destination_size,
                                  USER_CAPTION_LOG_BYTES);
}

static bool log_session_event(const app_event_t *event,
                              app_user_log_message_t *message) {
  if (strcmp(event->name, "AI:CAPTION") == 0) {
    if (event->flag && event->payload[0] != '\0') {
      char caption[USER_CAPTION_LOG_BYTES + 4U];
      summarize_caption(event->payload, caption, sizeof(caption));
      return display_message(message, APP_USER_LOG_LEVEL_INFO, "字幕",
                             event->code == 0 ? "用户" : "AI", "%s", caption);
    }
    return false;
  }
  if (strcmp(event->name, "AI:STATE") == 0) {
    bool displayed = false;
    if (strcmp(event->first, "ai-active") == 0) {
      if (!s_ai_active) {
        displayed = display_message(message, APP_USER_LOG_LEVEL_INFO, "AI",
                                    NULL, "对讲开始");
      }
      s_ai_active = true;
    } else if (strcmp(event->first, "idle") == 0) {
      if (s_ai_active && event->code == 0) {
        displayed = display_message(message, APP_USER_LOG_LEVEL_INFO, "AI",
                                    NULL, "对讲结束");
      } else if (s_ai_active) {
        displayed =
            display_message(message, APP_USER_LOG_LEVEL_WARNING, "AI", NULL,
                            "对讲异常结束（代码=%d）", event->code);
      } else if (event->code != 0) {
        displayed =
            display_message(message, APP_USER_LOG_LEVEL_WARNING, "AI", NULL,
                            "对讲启动失败（代码=%d）", event->code);
      }
      s_ai_active = false;
    }
    return displayed;
  }
  if (strcmp(event->name, "AI:OP") == 0 &&
      strcmp(event->first, "ai-prompt") == 0) {
    if (event->code != 0) {
      return display_message(message, APP_USER_LOG_LEVEL_WARNING, "AI", NULL,
                             "语音请求失败（代码=%d）", event->code);
    }
    return false;
  }
  if (strcmp(event->name, "CALL:INCOMING") == 0) {
    s_call_in_progress = true;
    return display_message(message, APP_USER_LOG_LEVEL_INFO, "呼叫", NULL,
                           "收到来电");
  }
  if (strcmp(event->name, "CALL:STATE") == 0) {
    bool displayed = false;
    if (strcmp(event->first, "calling") == 0 ||
        strcmp(event->first, "ringing") == 0 ||
        strcmp(event->first, "call-connecting") == 0) {
      s_call_in_progress = true;
    } else if (strcmp(event->first, "in-call") == 0) {
      s_call_in_progress = true;
      if (!s_call_connected) {
        displayed = display_message(message, APP_USER_LOG_LEVEL_INFO, "呼叫",
                                    NULL, "通话接通");
      }
      s_call_connected = true;
    } else if (strcmp(event->first, "idle") == 0) {
      if (!s_call_in_progress && !s_call_connected) {
        return false;
      }
      bool target_resolution_failed =
          strncmp(event->payload, "call-target-", 12) == 0 ||
          strncmp(event->payload, "call-contacts-", 14) == 0;
      if (target_resolution_failed) {
        displayed = false;
      } else if (s_call_connected && event->code == 0) {
        displayed = display_message(message, APP_USER_LOG_LEVEL_INFO, "呼叫",
                                    NULL, "通话结束");
      } else if (event->code != 0) {
        displayed =
            display_message(message, APP_USER_LOG_LEVEL_WARNING, "呼叫", NULL,
                            "通话异常结束（代码=%d）", event->code);
      } else if (strcmp(event->payload, "call-local-reject") == 0) {
        displayed = display_message(message, APP_USER_LOG_LEVEL_INFO, "呼叫",
                                    NULL, "已拒绝来电");
      } else if (strcmp(event->payload, "call-local-cancel") == 0) {
        displayed = display_message(message, APP_USER_LOG_LEVEL_INFO, "呼叫",
                                    NULL, "已取消呼叫");
      } else {
        displayed = display_message(message, APP_USER_LOG_LEVEL_INFO, "呼叫",
                                    NULL, "呼叫未接通");
      }
      s_call_in_progress = false;
      s_call_connected = false;
    }
    return displayed;
  }
  if (strcmp(event->name, "CALL:EVENT") == 0 &&
      strcmp(event->first, "message-received") == 0) {
    char text[APP_TEXT_MEDIUM];
    summarize_caption(event->payload, text, sizeof(text));
    return display_message(message, APP_USER_LOG_LEVEL_INFO, "收到消息", NULL,
                           "%s", text);
  }
  if (strcmp(event->name, "CALL:EVENT") == 0 &&
      strcmp(event->first, "message-sent") == 0) {
    if (event->code == 0) {
      return display_message(message, APP_USER_LOG_LEVEL_INFO, "消息", NULL,
                             "已发送");
    }
    return display_message(message, APP_USER_LOG_LEVEL_WARNING, "消息", NULL,
                           "发送失败（代码=%d）", event->code);
  }
  if (strcmp(event->name, "VIEW:STATE") == 0) {
    if (strcmp(event->first, "viewing") == 0) {
      if (!s_view_in_progress) {
        s_view_in_progress = true;
        return display_message(message, APP_USER_LOG_LEVEL_INFO, "远程查看",
                               NULL, "已连接");
      }
      return false;
    }
    if (strcmp(event->first, "idle") == 0 && s_view_in_progress) {
      s_view_in_progress = false;
      if (event->code == 0) {
        return display_message(message, APP_USER_LOG_LEVEL_INFO, "远程查看",
                               NULL, "已结束");
      }
      return display_message(message, APP_USER_LOG_LEVEL_WARNING, "远程查看",
                             NULL, "异常结束（代码=%d）", event->code);
    }
    return false;
  }
  if (strcmp(event->name, "VIEW:EVENT") == 0 &&
      strcmp(event->first, "media-started") == 0) {
    return display_message(message, APP_USER_LOG_LEVEL_INFO, "远程查看", NULL,
                           "音视频已开始循环播放");
  }
  if (event->code != 0) {
    if (strcmp(event->name, "AI:OP") == 0) {
      return display_message(message, APP_USER_LOG_LEVEL_WARNING, "AI", NULL,
                             "操作失败（代码=%d）", event->code);
    } else if (strcmp(event->name, "CALL:OP") == 0) {
      return display_message(message, APP_USER_LOG_LEVEL_WARNING, "呼叫", NULL,
                             "操作失败（代码=%d）", event->code);
    } else if (strcmp(event->name, "CONTACT:OP") == 0 ||
               strcmp(event->name, "CONTACTS:DONE") == 0 ||
               strcmp(event->name, "PENDING:DONE") == 0) {
      return display_message(message, APP_USER_LOG_LEVEL_WARNING, "联系人",
                             NULL, "操作失败（代码=%d）", event->code);
    } else if (strcmp(event->name, "DIAG") == 0) {
      return display_message(message, APP_USER_LOG_LEVEL_WARNING, "会话", NULL,
                             "运行异常（代码=%d）", event->code);
    }
  }
  return false;
}

static const char *error_text(const app_event_t *event) {
  if (strcmp(event->name, "SESSION_EVENT_OVERFLOW") == 0) {
    return "会话事件拥堵";
  }
  if (strcmp(event->name, "PLATFORM") == 0 ||
      strcmp(event->name, "PLATFORM_BOOTSTRAP") == 0) {
    return "平台连接失败";
  }
  if (strcmp(event->name, "BIND") == 0) {
    return "设备绑定失败";
  }
  if (strcmp(event->name, "TIRTC_START") == 0) {
    return "实时通信启动失败";
  }
  if (strcmp(event->name, "TIRTC_START_TIMEOUT") == 0) {
    return "实时通信启动超时";
  }
  if (strcmp(event->name, "REQUEST_REJECTED") == 0) {
    return "指令未执行";
  }
  return "运行异常";
}

static const char *restart_text(const char *reason) {
  if (strcmp(reason, "wifi_config_changed") == 0 ||
      strcmp(reason, "wifi_config_cleared") == 0) {
    return "网络配置已更新，重启中";
  }
  if (strcmp(reason, "binding_completed") == 0) {
    return "绑定完成，重启中";
  }
  if (strcmp(reason, "at_request") == 0) {
    return "按指令重启中";
  }
  if (strcmp(reason, "cloud_reset") == 0) {
    return "云端已重置，设备重启中";
  }
  return "故障恢复，重启中";
}

bool app_user_log_event(const app_event_t *event,
                        app_user_log_message_t *message) {
  clear_message(message);
  if (event == NULL) {
    return false;
  }
  switch (event->domain) {
  case APP_EVENT_SYSTEM:
    if (strcmp(event->name, "BOOTING") == 0) {
      s_ai_active = false;
      s_call_in_progress = false;
      s_call_connected = false;
      s_view_in_progress = false;
      s_platform_retry_reported = false;
      return display_message(message, APP_USER_LOG_LEVEL_INFO, "系统", NULL,
                             "启动中");
    } else if (strcmp(event->name, "RESTARTING") == 0) {
      return display_message(message, APP_USER_LOG_LEVEL_WARNING, "系统", NULL,
                             "%s", restart_text(event->first));
    }
    break;
  case APP_EVENT_WIFI:
    if (strcmp(event->name, "ONLINE") == 0) {
      return display_message(message, APP_USER_LOG_LEVEL_INFO, "网络", NULL,
                             "Wi-Fi 已连接");
    } else if (strcmp(event->name, "DISCONNECTED") == 0) {
      return display_message(message, APP_USER_LOG_LEVEL_WARNING, "网络", NULL,
                             "Wi-Fi 已断开");
    }
    break;
  case APP_EVENT_BIND:
    if (strcmp(event->name, "REQUIRED") == 0) {
      return display_message(message, APP_USER_LOG_LEVEL_INFO, "绑定", NULL,
                             "等待平台绑定");
    } else if (strcmp(event->name, "BOUND") == 0) {
      return display_message(message, APP_USER_LOG_LEVEL_INFO, "绑定", NULL,
                             "设备绑定完成");
    }
    break;
  case APP_EVENT_PLATFORM:
    if (strcmp(event->name, "RETRY") == 0) {
      if (s_platform_retry_reported) {
        return false;
      }
      s_platform_retry_reported = true;
      return display_message(message, APP_USER_LOG_LEVEL_WARNING, "平台", NULL,
                             "连接较慢，正在重试");
    } else if (strcmp(event->name, "MQTT") == 0 &&
               strcmp(event->first, "ONLINE") == 0) {
      s_platform_retry_reported = false;
      return false;
    } else if (strcmp(event->name, "MQTT") == 0 &&
        strcmp(event->first, "OFFLINE") == 0) {
      return display_message(message, APP_USER_LOG_LEVEL_WARNING, "网络", NULL,
                             "平台连接已断开（代码=%d）", event->code);
    }
    break;
  case APP_EVENT_TIRTC:
    if (strcmp(event->name, "READY") == 0) {
      s_platform_retry_reported = false;
      return display_message(message, APP_USER_LOG_LEVEL_INFO, "系统", NULL,
                             "已就绪");
    } else if (strcmp(event->name, "ERROR") == 0) {
      return display_message(message, APP_USER_LOG_LEVEL_ERROR, "错误", NULL,
                             "实时通信运行异常（代码=%d）", event->code);
    }
    break;
  case APP_EVENT_SESSION:
    return log_session_event(event, message);
  case APP_EVENT_ERROR:
    if (strcmp(event->name, "SESSION_EVENT_OVERFLOW") == 0 &&
        event->first[0] != '\0') {
      return display_message(message, APP_USER_LOG_LEVEL_ERROR, "错误", NULL,
                             "会话事件拥堵，丢失 %s 条", event->first);
    } else {
      return display_message(message, APP_USER_LOG_LEVEL_ERROR, "错误", NULL,
                             "%s（代码=%d）", error_text(event), event->code);
    }
  default:
    return display_message(message, APP_USER_LOG_LEVEL_ERROR, "错误", NULL,
                           "收到未知事件");
  }
  return false;
}
