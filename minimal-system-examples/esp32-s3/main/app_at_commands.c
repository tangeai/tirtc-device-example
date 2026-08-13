#include "app_at_commands.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "app_at_facade.h"
#include "app_at_parser.h"
#include "app_controller.h"
#include "app_user_log.h"
#include "at_server.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "tirtc_adapter.h"

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))
#define APP_AT_TASK_STACK_SIZE 8192

static atomic_uint_fast32_t s_request_sequence;

static void secure_zero(void *memory, size_t size) {
  volatile unsigned char *cursor = memory;
  while (cursor != NULL && size-- > 0U) {
    *cursor++ = 0;
  }
}

static uint32_t next_request_id(void) {
  uint32_t id = (uint32_t)atomic_fetch_add_explicit(&s_request_sequence, 1,
                                                    memory_order_relaxed) +
                1U;
  if (id == 0) {
    id = (uint32_t)atomic_fetch_add_explicit(&s_request_sequence, 1,
                                             memory_order_relaxed) +
         1U;
  }
  return id;
}

static bool equals(const char *left, const char *right) {
  return left != NULL && right != NULL && strcasecmp(left, right) == 0;
}

static esp_err_t parse_set(const at_server_request_t *request,
                           app_at_fields_t *fields) {
  if (request == NULL || fields == NULL ||
      request->operation != AT_SERVER_OP_SET) {
    return ESP_ERR_INVALID_ARG;
  }
  return app_at_parse_fields(request->arguments, request->arguments_length,
                             fields);
}

static esp_err_t submit_with_id(app_intent_type_t type, const char *first,
                                const char *second, const char *third,
                                bool accepted, uint32_t *request_id) {
  app_intent_t intent = {
      .type = type,
      .request_id = next_request_id(),
      .accepted = accepted,
  };
  if (first != NULL) {
    (void)snprintf(intent.first, sizeof(intent.first), "%s", first);
  }
  if (second != NULL) {
    (void)snprintf(intent.second, sizeof(intent.second), "%s", second);
  }
  if (third != NULL) {
    (void)snprintf(intent.third, sizeof(intent.third), "%s", third);
  }
  esp_err_t err = app_controller_submit(&intent);
  if (err == ESP_OK && request_id != NULL) {
    *request_id = intent.request_id;
  }
  secure_zero(&intent, sizeof(intent));
  return err;
}

static esp_err_t submit(app_intent_type_t type, const char *first,
                        const char *second, const char *third, bool accepted) {
  return submit_with_id(type, first, second, third, accepted, NULL);
}

static esp_err_t help_command(const at_server_request_t *request,
                              void *context) {
  (void)context;
  if (request->operation != AT_SERVER_OP_EXECUTE &&
      request->operation != AT_SERVER_OP_TEST) {
    return ESP_ERR_INVALID_ARG;
  }
  return at_server_response("+HELP:\"内部回归指令\"");
}

static const char *adapter_state_name(int state) {
  static const char *const names[] = {
      "idle", "starting", "running", "stopping", "stopped", "error",
  };
  return state >= 0 && (size_t)state < ARRAY_SIZE(names) ? names[state]
                                                         : "unknown";
}

static const char *media_profile_name(int profile) {
  static const char *const names[] = {"none", "ai", "call", "view"};
  return profile >= 0 && (size_t)profile < ARRAY_SIZE(names) ? names[profile]
                                                             : "unknown";
}

static esp_err_t build_command(const at_server_request_t *request,
                               void *context) {
  (void)context;
  if (request->operation != AT_SERVER_OP_READ) {
    return ESP_ERR_INVALID_ARG;
  }

  const esp_app_desc_t *description = esp_app_get_description();
  const char *sdk_version = tirtc_adapter_version();
  const char *sdk_build = tirtc_adapter_build_info();
  if (description == NULL || sdk_version == NULL || sdk_build == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  static const char hex[] = "0123456789abcdef";
  char elf_sha256[65] = {0};
  char project[65] = {0};
  char version[65] = {0};
  char idf_version[65] = {0};
  char tirtc_version[65] = {0};
  char tirtc_build[APP_TEXT_MEDIUM * 2U + 1U] = {0};
  for (size_t index = 0; index < sizeof(description->app_elf_sha256); ++index) {
    elf_sha256[index * 2U] = hex[description->app_elf_sha256[index] >> 4U];
    elf_sha256[index * 2U + 1U] =
        hex[description->app_elf_sha256[index] & 0x0fU];
  }
  esp_err_t err = ESP_OK;
  if (err == ESP_OK) {
    err = app_at_escape(description->project_name, project, sizeof(project));
  }
  if (err == ESP_OK) {
    err = app_at_escape(description->version, version, sizeof(version));
  }
  if (err == ESP_OK) {
    err = app_at_escape(description->idf_ver, idf_version, sizeof(idf_version));
  }
  if (err == ESP_OK) {
    err = app_at_escape(sdk_version, tirtc_version, sizeof(tirtc_version));
  }
  if (err == ESP_OK) {
    err = app_at_escape(sdk_build, tirtc_build, sizeof(tirtc_build));
  }
  if (err != ESP_OK) {
    return err;
  }
  return at_server_response(
      "+BUILD:1,\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"", project, version,
      idf_version, elf_sha256, tirtc_version, tirtc_build);
}

static esp_err_t session_command(const at_server_request_t *request,
                                 void *context) {
  (void)context;
  if (request->operation != AT_SERVER_OP_READ) {
    return ESP_ERR_INVALID_ARG;
  }
  app_session_snapshot_t snapshot = {0};
  esp_err_t err = app_controller_session_snapshot(&snapshot);
  if (err != ESP_OK) {
    return err;
  }
  typedef struct {
    char room[APP_TEXT_MEDIUM * 2U + 1U];
    char peer[APP_TEXT_MEDIUM * 2U + 1U];
    char ai_session[APP_TEXT_MEDIUM * 2U + 1U];
    char ai_action[APP_TEXT_MEDIUM * 2U + 1U];
  } session_escaped_fields_t;
  session_escaped_fields_t *escaped = heap_caps_calloc(
      1, sizeof(*escaped), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (escaped == NULL) {
    return ESP_ERR_NO_MEM;
  }
  err = app_at_escape(snapshot.room_id, escaped->room, sizeof(escaped->room));
  if (err == ESP_OK) {
    err = app_at_escape(snapshot.peer_id, escaped->peer, sizeof(escaped->peer));
  }
  if (err == ESP_OK) {
    err = app_at_escape(snapshot.ai_session_id, escaped->ai_session,
                        sizeof(escaped->ai_session));
  }
  if (err == ESP_OK) {
    err = app_at_escape(snapshot.ai_action_id, escaped->ai_action,
                        sizeof(escaped->ai_action));
  }
  if (err != ESP_OK) {
    secure_zero(escaped, sizeof(*escaped));
    heap_caps_free(escaped);
    return err;
  }
  err = at_server_response(
      "+SESSION:3,%lu,%lu,%lu,%lu,%s,%s,%lld,%d,%d,%s,"
      "\"%s\",\"%s\",\"%s\",%d,%lld,%d,\"%s\",%lld,"
      "%d,\"%s\",%lld",
      (unsigned long)snapshot.app_generation, (unsigned long)snapshot.revision,
      (unsigned long)snapshot.session_generation,
      (unsigned long)snapshot.origin_request_id, snapshot.owner, snapshot.state,
      (long long)snapshot.deadline_remaining_ms, snapshot.pending_incoming_call,
      snapshot.caller,
      strcmp(snapshot.owner, "call") == 0
          ? (snapshot.call_type == 1 ? "video" : "audio")
          : "none",
      escaped->room, escaped->peer, escaped->ai_session,
      snapshot.ai_update_pending,
      (long long)snapshot.ai_update_deadline_remaining_ms,
      snapshot.ai_action_pending, escaped->ai_action,
      (long long)snapshot.ai_action_deadline_remaining_ms,
      snapshot.ai_call_handoff_pending, snapshot.ai_call_handoff_phase,
      (long long)snapshot.ai_call_handoff_deadline_remaining_ms);
  secure_zero(escaped, sizeof(*escaped));
  heap_caps_free(escaped);
  return err;
}

static esp_err_t media_command(const at_server_request_t *request,
                               void *context) {
  (void)context;
  if (request->operation != AT_SERVER_OP_READ) {
    return ESP_ERR_INVALID_ARG;
  }
  app_media_snapshot_t snapshot = {0};
  esp_err_t err = app_controller_media_snapshot(&snapshot);
  if (err != ESP_OK) {
    return err;
  }
  return at_server_response(
      "+MEDIA:1,%s,%d,%s,%s,%lu,%lu,%lu,"
      "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
      "%d,%d,%lu,%lu,%lu,%d",
      adapter_state_name(snapshot.adapter_state), snapshot.connected,
      media_profile_name(snapshot.active_profile),
      media_profile_name(snapshot.measured_profile),
      (unsigned long)snapshot.active_session_generation,
      (unsigned long)snapshot.measured_session_generation,
      (unsigned long)snapshot.connection_generation,
      (unsigned long)snapshot.tx_audio_frames,
      (unsigned long)snapshot.tx_audio_bytes,
      (unsigned long)snapshot.tx_video_frames,
      (unsigned long)snapshot.tx_video_bytes,
      (unsigned long)snapshot.rx_audio_frames,
      (unsigned long)snapshot.rx_audio_bytes,
      (unsigned long)snapshot.rx_video_frames,
      (unsigned long)snapshot.rx_video_bytes,
      (unsigned long)snapshot.send_errors, (unsigned long)snapshot.first_tx_ms,
      (unsigned long)snapshot.last_tx_ms, (unsigned long)snapshot.first_rx_ms,
      (unsigned long)snapshot.last_rx_ms, snapshot.connect_request_pending,
      snapshot.connect_callback_pending,
      (unsigned long)snapshot.accept_callbacks_pending,
      (unsigned long)snapshot.disconnects_pending,
      (unsigned long)snapshot.connection_users, snapshot.incoming_armed);
}

static esp_err_t status_command(const at_server_request_t *request,
                                void *context) {
  (void)context;
  if (request->operation != AT_SERVER_OP_READ) {
    return ESP_ERR_INVALID_ARG;
  }
  app_snapshot_t snapshot = {0};
  app_controller_snapshot(&snapshot);
  char ssid[67];
  char ip[97];
  char device_id[131];
  esp_err_t err = app_at_escape(snapshot.ssid, ssid, sizeof(ssid));
  if (err == ESP_OK)
    err = app_at_escape(snapshot.ip_address, ip, sizeof(ip));
  if (err == ESP_OK) {
    err = app_at_escape(snapshot.device_id, device_id, sizeof(device_id));
  }
  if (err != ESP_OK) {
    return err;
  }
  return at_server_response(
      "+STATUS:%lu,%s,%d,%d,%d,%d,%d,\"%s\",\"%s\",\"%s\",%s,%s,%d",
      (unsigned long)snapshot.generation, app_state_name(snapshot.state),
      snapshot.wifi_configured, snapshot.wifi_online, snapshot.platform_ready,
      snapshot.mqtt_online, snapshot.tirtc_ready, ssid, ip, device_id,
      snapshot.session_owner, snapshot.session_state, snapshot.last_error);
}

static esp_err_t wifi_command(const at_server_request_t *request,
                              void *context) {
  (void)context;
  if (request->operation == AT_SERVER_OP_READ) {
    app_snapshot_t snapshot = {0};
    app_controller_snapshot(&snapshot);
    char ssid[67];
    char ip[97];
    esp_err_t err = app_at_escape(snapshot.ssid, ssid, sizeof(ssid));
    if (err == ESP_OK) {
      err = app_at_escape(snapshot.ip_address, ip, sizeof(ip));
    }
    if (err != ESP_OK) {
      return err;
    }
    return at_server_response("+WIFI:%d,%d,\"%s\",\"%s\"",
                              snapshot.wifi_configured, snapshot.wifi_online,
                              ssid, ip);
  }

  app_at_fields_t fields;
  esp_err_t err = parse_set(request, &fields);
  if (err != ESP_OK) {
    secure_zero(&fields, sizeof(fields));
    return err;
  }
  if (fields.count == 1 && equals(fields.values[0], "CLEAR")) {
    err = submit(APP_INTENT_WIFI_CLEAR, NULL, NULL, NULL, false);
    secure_zero(&fields, sizeof(fields));
    return err;
  }
  if (fields.count != 2) {
    secure_zero(&fields, sizeof(fields));
    return ESP_ERR_INVALID_ARG;
  }
  err = submit(APP_INTENT_WIFI_SET, fields.values[0], fields.values[1], NULL,
               false);
  secure_zero(&fields, sizeof(fields));
  return err;
}

static esp_err_t bind_command(const at_server_request_t *request,
                              void *context) {
  (void)context;
  if (request->operation == AT_SERVER_OP_READ) {
    app_snapshot_t snapshot = {0};
    app_controller_snapshot(&snapshot);
    char code[35];
    esp_err_t err =
        app_at_escape(snapshot.verification_code, code, sizeof(code));
    if (err != ESP_OK) {
      return err;
    }
    return at_server_response("+BIND:%s,\"%s\"", app_state_name(snapshot.state),
                              code);
  }
  app_at_fields_t fields;
  esp_err_t err = parse_set(request, &fields);
  if (err != ESP_OK || fields.count != 1 ||
      !equals(fields.values[0], "START")) {
    return ESP_ERR_INVALID_ARG;
  }
  return submit(APP_INTENT_BIND_START, NULL, NULL, NULL, false);
}

static esp_err_t platform_command(const at_server_request_t *request,
                                  void *context) {
  (void)context;
  if (request->operation != AT_SERVER_OP_READ) {
    return ESP_ERR_INVALID_ARG;
  }
  app_snapshot_t snapshot = {0};
  app_controller_snapshot(&snapshot);
  char device_id[131];
  esp_err_t err =
      app_at_escape(snapshot.device_id, device_id, sizeof(device_id));
  if (err != ESP_OK) {
    return err;
  }
  return at_server_response("+PLATFORM:%d,%d,%d,\"%s\"",
                            snapshot.platform_ready, snapshot.mqtt_online,
                            snapshot.tirtc_ready, device_id);
}

static esp_err_t ai_command(const at_server_request_t *request, void *context) {
  (void)context;
  if (request->operation == AT_SERVER_OP_READ) {
    app_snapshot_t snapshot = {0};
    app_controller_snapshot(&snapshot);
    return at_server_response("+AI:%lu,%s,%s",
                              (unsigned long)snapshot.generation,
                              snapshot.session_owner, snapshot.session_state);
  }
  app_at_fields_t fields;
  esp_err_t err = parse_set(request, &fields);
  if (err != ESP_OK || fields.count == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (fields.count == 1 && equals(fields.values[0], "START")) {
    return submit(APP_INTENT_AI_START, NULL, NULL, NULL, false);
  }
  if (fields.count == 1 && equals(fields.values[0], "STOP")) {
    return submit(APP_INTENT_AI_STOP, NULL, NULL, NULL, false);
  }
  if (fields.count == 1 && equals(fields.values[0], "INTERRUPT")) {
    return submit(APP_INTENT_AI_INTERRUPT, NULL, NULL, NULL, false);
  }
  if (fields.count == 1 && equals(fields.values[0], "SUBMIT")) {
    return submit(APP_INTENT_AI_SUBMIT, NULL, NULL, NULL, false);
  }
  if (fields.count == 2 && equals(fields.values[0], "UPDATE") &&
      fields.values[1][0] != '\0') {
    return submit(APP_INTENT_AI_UPDATE, fields.values[1], NULL, NULL, false);
  }
  if (fields.count == 2 && equals(fields.values[0], "PROMPT") &&
      fields.values[1][0] != '\0') {
    return submit(APP_INTENT_AI_PROMPT, fields.values[1], NULL, NULL, false);
  }
  return ESP_ERR_INVALID_ARG;
}

static esp_err_t ai_action_command(const at_server_request_t *request,
                                   void *context) {
  (void)context;
  app_at_fields_t fields;
  esp_err_t err = parse_set(request, &fields);
  if (err != ESP_OK || fields.count < 2 || fields.count > 3 ||
      fields.values[0][0] == '\0') {
    return ESP_ERR_INVALID_ARG;
  }
  bool accepted;
  if (equals(fields.values[1], "OK")) {
    accepted = true;
  } else if (equals(fields.values[1], "ERROR")) {
    accepted = false;
  } else {
    return ESP_ERR_INVALID_ARG;
  }
  return submit(APP_INTENT_AI_ACTION_RESULT, fields.values[0],
                fields.count == 3 ? fields.values[2] : "", NULL, accepted);
}

static esp_err_t call_command(const at_server_request_t *request,
                              void *context) {
  (void)context;
  if (request->operation == AT_SERVER_OP_READ) {
    app_snapshot_t snapshot = {0};
    app_controller_snapshot(&snapshot);
    return at_server_response("+CALL:%lu,%s,%s",
                              (unsigned long)snapshot.generation,
                              snapshot.session_owner, snapshot.session_state);
  }
  app_at_fields_t fields;
  esp_err_t err = parse_set(request, &fields);
  if (err != ESP_OK || fields.count < 1 || fields.count > 2 ||
      fields.values[0][0] == '\0' ||
      (fields.count == 2 && !equals(fields.values[1], "AUDIO") &&
       !equals(fields.values[1], "VIDEO"))) {
    return ESP_ERR_INVALID_ARG;
  }
  return submit(APP_INTENT_CALL_START, fields.values[0],
                fields.count == 2 ? fields.values[1] : "AUDIO", NULL, false);
}

static esp_err_t simple_intent_command(const at_server_request_t *request,
                                       void *context) {
  if (request->operation != AT_SERVER_OP_EXECUTE || context == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  app_intent_type_t type = (app_intent_type_t)(uintptr_t)context;
  return submit(type, NULL, NULL, NULL, false);
}

static esp_err_t list_command(const at_server_request_t *request,
                              void *context) {
  if (request->operation != AT_SERVER_OP_READ || context == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  app_intent_type_t type = (app_intent_type_t)(uintptr_t)context;
  return submit(type, NULL, NULL, NULL, false);
}

static esp_err_t contact_command(const at_server_request_t *request,
                                 void *context) {
  (void)context;
  app_at_fields_t fields;
  esp_err_t err = parse_set(request, &fields);
  if (err != ESP_OK || fields.count < 2) {
    return ESP_ERR_INVALID_ARG;
  }
  if (fields.count == 2 && equals(fields.values[0], "REQUEST")) {
    return submit(APP_INTENT_CONTACT_REQUEST, fields.values[1], NULL, NULL,
                  false);
  }
  if (fields.count == 3 && equals(fields.values[0], "RESPOND") &&
      (equals(fields.values[2], "ACCEPT") ||
       equals(fields.values[2], "REJECT"))) {
    return submit(APP_INTENT_CONTACT_RESPOND, fields.values[1],
                  fields.values[2], NULL, false);
  }
  if (fields.count == 3 && equals(fields.values[0], "REMARK")) {
    return submit(APP_INTENT_CONTACT_REMARK, fields.values[1], fields.values[2],
                  NULL, false);
  }
  if (fields.count == 2 && equals(fields.values[0], "DELETE")) {
    return submit(APP_INTENT_CONTACT_DELETE, fields.values[1], NULL, NULL,
                  false);
  }
  return ESP_ERR_INVALID_ARG;
}

typedef struct {
  at_server_command_handler_t handler;
  void *context;
} raw_command_binding_t;

static esp_err_t raw_only_command(const at_server_request_t *request,
                                  void *context) {
  raw_command_binding_t *binding = context;
  if (binding == NULL || binding->handler == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!app_at_facade_raw_mode()) {
    (void)at_server_response("+失败,\"请发送 AT+帮助 查看可用指令\"");
    return ESP_ERR_NOT_SUPPORTED;
  }
  return binding->handler(request, binding->context);
}

#define RAW_BINDING(name, command_handler, command_context)                    \
  static raw_command_binding_t name = {.handler = command_handler,             \
                                       .context = command_context}

RAW_BINDING(s_raw_help, help_command, NULL);
RAW_BINDING(s_raw_tirtc, app_at_facade_legacy_command, NULL);
RAW_BINDING(s_raw_build, build_command, NULL);
RAW_BINDING(s_raw_status, status_command, NULL);
RAW_BINDING(s_raw_session, session_command, NULL);
RAW_BINDING(s_raw_media, media_command, NULL);
RAW_BINDING(s_raw_wifi, wifi_command, NULL);
RAW_BINDING(s_raw_bind, bind_command, NULL);
RAW_BINDING(s_raw_platform, platform_command, NULL);
RAW_BINDING(s_raw_ai, ai_command, NULL);
RAW_BINDING(s_raw_ai_action, ai_action_command, NULL);
RAW_BINDING(s_raw_call, call_command, NULL);
RAW_BINDING(s_raw_accept, simple_intent_command,
            (void *)(uintptr_t)APP_INTENT_CALL_ACCEPT);
RAW_BINDING(s_raw_reject, simple_intent_command,
            (void *)(uintptr_t)APP_INTENT_CALL_REJECT);
RAW_BINDING(s_raw_cancel, simple_intent_command,
            (void *)(uintptr_t)APP_INTENT_CALL_CANCEL);
RAW_BINDING(s_raw_hangup, simple_intent_command,
            (void *)(uintptr_t)APP_INTENT_CALL_HANGUP);
RAW_BINDING(s_raw_contacts, list_command,
            (void *)(uintptr_t)APP_INTENT_CONTACTS_LIST);
RAW_BINDING(s_raw_pending, list_command,
            (void *)(uintptr_t)APP_INTENT_PENDING_LIST);
RAW_BINDING(s_raw_contact, contact_command, NULL);
RAW_BINDING(s_raw_restart, simple_intent_command,
            (void *)(uintptr_t)APP_INTENT_RESTART);

#define RAW_COMMAND(command_name, binding)                                     \
  {.name = command_name, .handler = raw_only_command, .context = &binding}

#define PUBLIC_COMMAND(command_name, command_id)                               \
  {.name = command_name,                                                       \
   .handler = app_at_facade_command,                                           \
   .context = (void *)(uintptr_t)(command_id)}

static const at_server_command_t s_commands[] = {
    PUBLIC_COMMAND("帮助", APP_AT_PUBLIC_HELP),
    PUBLIC_COMMAND("状态", APP_AT_PUBLIC_STATUS),
    PUBLIC_COMMAND("配网", APP_AT_PUBLIC_WIFI),
    PUBLIC_COMMAND("绑定", APP_AT_PUBLIC_BIND),
    PUBLIC_COMMAND("设备ID", APP_AT_PUBLIC_DEVICE_ID),
    PUBLIC_COMMAND("讲故事", APP_AT_PUBLIC_STORY),
    PUBLIC_COMMAND("讲笑话", APP_AT_PUBLIC_JOKE),
    PUBLIC_COMMAND("查天气", APP_AT_PUBLIC_WEATHER),
    PUBLIC_COMMAND("结束对讲", APP_AT_PUBLIC_AI_STOP),
    PUBLIC_COMMAND("联系人", APP_AT_PUBLIC_CONTACTS),
    PUBLIC_COMMAND("好友申请", APP_AT_PUBLIC_PENDING),
    PUBLIC_COMMAND("加好友", APP_AT_PUBLIC_CONTACT_REQUEST),
    PUBLIC_COMMAND("同意好友", APP_AT_PUBLIC_CONTACT_ACCEPT),
    PUBLIC_COMMAND("备注", APP_AT_PUBLIC_CONTACT_REMARK),
    PUBLIC_COMMAND("呼叫", APP_AT_PUBLIC_CALL),
    PUBLIC_COMMAND("接听", APP_AT_PUBLIC_CALL_ACCEPT),
    PUBLIC_COMMAND("拒接", APP_AT_PUBLIC_CALL_REJECT),
    PUBLIC_COMMAND("取消呼叫", APP_AT_PUBLIC_CALL_CANCEL),
    PUBLIC_COMMAND("挂断", APP_AT_PUBLIC_CALL_HANGUP),
    PUBLIC_COMMAND("发消息", APP_AT_PUBLIC_CALL_MESSAGE),
    {.name = "PROTO", .handler = app_at_protocol_command},
    RAW_COMMAND("TIRTC", s_raw_tirtc),
    RAW_COMMAND("HELP", s_raw_help),
    RAW_COMMAND("BUILD", s_raw_build),
    RAW_COMMAND("STATUS", s_raw_status),
    RAW_COMMAND("SESSION", s_raw_session),
    RAW_COMMAND("MEDIA", s_raw_media),
    RAW_COMMAND("WIFI", s_raw_wifi),
    RAW_COMMAND("BIND", s_raw_bind),
    RAW_COMMAND("PLATFORM", s_raw_platform),
    RAW_COMMAND("AI", s_raw_ai),
    RAW_COMMAND("AIACTION", s_raw_ai_action),
    RAW_COMMAND("CALL", s_raw_call),
    RAW_COMMAND("ACCEPT", s_raw_accept),
    RAW_COMMAND("REJECT", s_raw_reject),
    RAW_COMMAND("CANCEL", s_raw_cancel),
    RAW_COMMAND("HANGUP", s_raw_hangup),
    RAW_COMMAND("CONTACTS", s_raw_contacts),
    RAW_COMMAND("PENDING", s_raw_pending),
    RAW_COMMAND("CONTACT", s_raw_contact),
    RAW_COMMAND("RESTART", s_raw_restart),
};

static void escape_or_placeholder(const char *input, char *output,
                                  size_t output_size) {
  if (app_at_escape(input == NULL ? "" : input, output, output_size) !=
      ESP_OK) {
    (void)snprintf(output, output_size, "%s", "[invalid-or-truncated]");
  }
}

static void raw_event(const app_event_t *event) {
  if (event == NULL) {
    return;
  }
  char first[APP_TEXT_MEDIUM * 2U + 1U];
  char second[APP_TEXT_MEDIUM * 2U + 1U];
  char payload[sizeof(event->payload) * 2U + 1U];
  escape_or_placeholder(event->first, first, sizeof(first));
  escape_or_placeholder(event->second, second, sizeof(second));
  escape_or_placeholder(event->payload, payload, sizeof(payload));

  switch (event->domain) {
  case APP_EVENT_SYSTEM:
    (void)at_server_urc("+SYSTEM:%s,%lu,%d,\"%s\"", event->name,
                        (unsigned long)event->generation, event->code, first);
    break;
  case APP_EVENT_WIFI:
    (void)at_server_urc("+WIFI:%s,%lu,\"%s\",\"%s\"", event->name,
                        (unsigned long)event->generation, first, second);
    break;
  case APP_EVENT_BIND:
    (void)at_server_urc("+BIND:%s,%lu,\"%s\"", event->name,
                        (unsigned long)event->generation, first);
    break;
  case APP_EVENT_PLATFORM:
    (void)at_server_urc("+PLATFORM:%s,%lu,%d,\"%s\"", event->name,
                        (unsigned long)event->generation, event->code, first);
    break;
  case APP_EVENT_TIRTC:
    (void)at_server_urc("+TIRTC:%s,%lu,%d", event->name,
                        (unsigned long)event->generation, event->code);
    break;
  case APP_EVENT_SESSION:
    if (strcmp(event->name, "AI:CAPTION") == 0) {
      (void)at_server_urc("+AI:CAPTION,%lu,%lu,%d,%d,%d,\"%s\",%d,\"%s\"",
                          (unsigned long)event->generation,
                          (unsigned long)event->request_id, event->code,
                          event->value1, event->value2, first, event->flag,
                          payload);
    } else if (strcmp(event->name, "CALL:INCOMING") == 0) {
      (void)at_server_urc("+CALL:INCOMING,%lu,%lu,\"%s\",\"%s\",\"%s\"",
                          (unsigned long)event->generation,
                          (unsigned long)event->request_id, first, second,
                          payload);
    } else if (strcmp(event->name, "AI:ACTION") == 0) {
      (void)at_server_urc("+AI:ACTION,%lu,%lu,\"%s\",\"%s\",\"%s\"",
                          (unsigned long)event->generation,
                          (unsigned long)event->request_id, first, second,
                          payload);
    } else if (strcmp(event->name, "CONTACT") == 0 ||
               strcmp(event->name, "PENDING") == 0) {
      (void)at_server_urc("+%s,%lu,%d,%d,\"%s\",\"%s\",\"%s\"", event->name,
                          (unsigned long)event->request_id, event->value1,
                          event->flag, first, second, payload);
    } else if (strcmp(event->name, "CONTACTS:DONE") == 0 ||
               strcmp(event->name, "PENDING:DONE") == 0) {
      (void)at_server_urc("+%s,%lu,%d,%d", event->name,
                          (unsigned long)event->request_id, event->value1,
                          event->code);
    } else {
      (void)at_server_urc("+%s,%lu,%lu,%d,\"%s\",\"%s\",\"%s\"", event->name,
                          (unsigned long)event->generation,
                          (unsigned long)event->request_id, event->code, first,
                          second, payload);
    }
    break;
  case APP_EVENT_ERROR:
  default:
    (void)at_server_urc(
        "+ERROR:%lu,%lu,%d,\"%s\",\"%s\"", (unsigned long)event->generation,
        (unsigned long)event->request_id, event->code, event->name, first);
    break;
  }
}

static void app_event(const app_event_t *event, void *user_data) {
  (void)user_data;
  if (event == NULL) {
    return;
  }
  app_user_log_message_t message;
  bool displayed = app_user_log_event(event, &message);
  if (at_server_running()) {
    app_at_facade_route_event(event, displayed ? &message : NULL, raw_event);
  }
  secure_zero(&message, sizeof(message));
}

esp_err_t app_at_commands_start(void) {
  esp_err_t err = app_at_facade_init(submit_with_id);
  if (err != ESP_OK) {
    return err;
  }
  at_server_config_t config =
      AT_SERVER_CONFIG_DEFAULT(s_commands, ARRAY_SIZE(s_commands));
  config.task_stack_size = APP_AT_TASK_STACK_SIZE;
  config.echo = false;
  err = at_server_init(&config);
  if (err == ESP_OK) {
    err = at_server_start();
  }
  if (err == ESP_OK) {
    err = app_controller_init(app_event, NULL);
  }
  if (err != ESP_OK) {
    (void)at_server_stop();
  }
  return err;
}
