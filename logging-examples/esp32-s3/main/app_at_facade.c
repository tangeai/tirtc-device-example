#include "app_at_facade.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_at_parser.h"
#include "device/device_utf8.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

#define FACADE_PENDING_TIMEOUT_MS 90000U

typedef enum {
  APP_AT_MODE_USER = 0,
  APP_AT_MODE_RAW,
} app_at_mode_t;

typedef enum {
  FACADE_ACTION_NONE = 0,
  FACADE_ACTION_STORY,
  FACADE_ACTION_JOKE,
  FACADE_ACTION_WEATHER,
} facade_action_t;

typedef enum {
  FACADE_PHASE_NONE = 0,
  FACADE_PHASE_WAIT_AI_ACTIVE,
  FACADE_PHASE_WAIT_AI_UPDATE,
  FACADE_PHASE_WAIT_AI_CAPTION,
  FACADE_PHASE_WAIT_AI_ROUND_END,
} facade_phase_t;

typedef struct {
  facade_action_t action;
  facade_phase_t phase;
  uint32_t request_id;
  uint32_t session_generation;
  int64_t deadline_us;
  char update_json[APP_TEXT_MEDIUM];
} facade_pending_t;

static SemaphoreHandle_t s_lock;
static TimerHandle_t s_pending_timer;
static app_at_facade_submit_fn s_submit;
static app_at_mode_t s_mode = APP_AT_MODE_USER;
static facade_pending_t s_pending;
static uint32_t s_contact_request_id;

static void pending_timer_callback(TimerHandle_t timer);

static void secure_zero(void *memory, size_t size) {
  volatile unsigned char *cursor = memory;
  while (cursor != NULL && size-- > 0U) {
    *cursor++ = 0;
  }
}

static bool equals(const char *left, const char *right) {
  return left != NULL && right != NULL && strcmp(left, right) == 0;
}

static bool valid_single_line_text(const char *text) {
  if (!device_utf8_validate(text)) {
    return false;
  }
  char sanitized[APP_AT_FIELD_SIZE * 2U];
  (void)device_utf8_sanitize_line(text, sanitized, sizeof(sanitized),
                                  sizeof(sanitized) - 1U);
  return strcmp(text, sanitized) == 0;
}

static esp_err_t take_lock(void) {
  if (s_lock == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  return xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE ? ESP_OK
                                                         : ESP_ERR_TIMEOUT;
}

static void give_lock(void) { (void)xSemaphoreGive(s_lock); }

static void clear_pending_locked(void) {
  if (s_pending_timer != NULL) {
    (void)xTimerStop(s_pending_timer, 0);
  }
  memset(&s_pending, 0, sizeof(s_pending));
}

static void clear_contact_pending_locked(void) { s_contact_request_id = 0; }

static void clear_all_pending_locked(void) {
  clear_pending_locked();
  clear_contact_pending_locked();
}

static void clear_pending(void) {
  if (take_lock() == ESP_OK) {
    clear_pending_locked();
    give_lock();
  }
}

static void clear_all_pending(void) {
  if (take_lock() == ESP_OK) {
    clear_all_pending_locked();
    give_lock();
  }
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

static esp_err_t respond_failure(esp_err_t error, const char *message) {
  (void)at_server_response("+失败,\"%s\"",
                           message == NULL ? "请求失败" : message);
  return error;
}

static esp_err_t respond_accepted(esp_err_t result, const char *message) {
  if (result != ESP_OK) {
    if (result == ESP_ERR_INVALID_STATE) {
      return respond_failure(result, "当前状态不允许此操作");
    }
    if (result == ESP_ERR_NO_MEM || result == ESP_ERR_TIMEOUT) {
      return respond_failure(result, "请求队列繁忙，请稍后重试");
    }
    return respond_failure(result, "请求未受理，请稍后重试");
  }
  (void)at_server_response("+提示,\"%s\"", message);
  return ESP_OK;
}

static const char *system_state_text(const app_snapshot_t *snapshot) {
  if (snapshot->state == APP_STATE_ERROR) {
    return "运行异常";
  }
  switch (snapshot->state) {
  case APP_STATE_NEED_WIFI:
    return "需要配网";
  case APP_STATE_NETWORKING:
    return "正在连接网络";
  case APP_STATE_NEED_BINDING:
    return "需要绑定";
  case APP_STATE_BINDING:
    return "正在绑定";
  case APP_STATE_STARTING_PLATFORM:
    return "正在连接平台";
  case APP_STATE_READY:
    return "已就绪";
  case APP_STATE_RESTARTING:
    return "正在重启";
  case APP_STATE_BOOTING:
  default:
    return "正在启动";
  }
}

static const char *session_state_text(const app_snapshot_t *snapshot) {
  if (strcmp(snapshot->session_owner, "ai") == 0) {
    return strcmp(snapshot->session_state, "ai-active") == 0 ? "AI对讲中"
                                                             : "AI连接中";
  }
  if (strcmp(snapshot->session_owner, "call") == 0) {
    return strcmp(snapshot->session_state, "in-call") == 0 ? "通话中"
                                                           : "呼叫中";
  }
  return "空闲";
}

static esp_err_t respond_help(void) {
  static const char *const lines[] = {
      "+帮助,\"准备\",\"AT+配网=\\\"名称\\\",\\\"密码\\\"；AT+绑定；AT+"
      "设备ID；AT+状态\"",
      "+帮助,\"AI对讲\",\"AT+讲故事；AT+讲笑话；AT+查天气；AT+结束对讲\"",
      "+帮助,\"联系人\",\"AT+联系人；AT+好友申请；AT+加好友=\\\"设备ID\\\"；AT+"
      "同意好友=\\\"设备ID\\\"；AT+备注=\\\"设备ID\\\",\\\"小张\\\"\"",
      "+帮助,\"呼叫\",\"AT+呼叫=\\\"小张\\\" 或 AT+呼叫=\\\"设备ID\\\"；AT+"
      "接听；AT+拒接；AT+取消呼叫；AT+挂断\"",
      "+帮助,\"消息\",\"通话接通后：AT+发消息=\\\"文字\\\"\"",
  };
  for (size_t index = 0; index < sizeof(lines) / sizeof(lines[0]); ++index) {
    esp_err_t err = at_server_response("%s", lines[index]);
    if (err != ESP_OK) {
      return err;
    }
  }
  return ESP_OK;
}

static esp_err_t respond_status(void) {
  app_snapshot_t snapshot = {0};
  app_controller_snapshot(&snapshot);
  return at_server_response("+状态,\"%s\",\"%s\"", system_state_text(&snapshot),
                            session_state_text(&snapshot));
}

static esp_err_t respond_device(void) {
  app_snapshot_t snapshot = {0};
  app_controller_snapshot(&snapshot);
  char device_id[APP_TEXT_SMALL * 2U + 1U];
  esp_err_t err =
      app_at_escape(snapshot.device_id, device_id, sizeof(device_id));
  if (err != ESP_OK) {
    return err;
  }
  return at_server_response("+设备ID,\"%s\"",
                            device_id[0] == '\0' ? "未绑定" : device_id);
}

static esp_err_t handle_bind(void) {
  app_snapshot_t snapshot = {0};
  app_controller_snapshot(&snapshot);

  if (snapshot.state == APP_STATE_BINDING) {
    if (snapshot.verification_code[0] != '\0') {
      char code[sizeof(snapshot.verification_code) * 2U + 1U];
      esp_err_t err = app_at_escape(snapshot.verification_code, code,
                                    sizeof(code));
      if (err != ESP_OK) {
        return respond_failure(err, "暂时无法读取绑定码");
      }
      return at_server_response("+绑定码,\"%s\"", code);
    }
    (void)at_server_response("+提示,\"正在获取绑定码，请稍候\"");
    return ESP_OK;
  }
  if (snapshot.state == APP_STATE_READY || snapshot.device_id[0] != '\0') {
    (void)at_server_response("+提示,\"设备已经绑定\"");
    return ESP_OK;
  }
  if (!snapshot.wifi_online) {
    return respond_failure(ESP_ERR_INVALID_STATE, "请先完成配网并等待联网");
  }
  if (snapshot.state != APP_STATE_NEED_BINDING) {
    return respond_failure(ESP_ERR_INVALID_STATE, "设备正在启动，请稍候");
  }

  return respond_accepted(
      s_submit(APP_INTENT_BIND_START, NULL, NULL, NULL, false, NULL),
      "正在获取绑定码");
}

static const char *media_profile_text(int profile) {
  switch (profile) {
  case 1:
    return "AI";
  case 2:
    return "通话";
  case 3:
    return "远程查看";
  default:
    return "空闲";
  }
}

static esp_err_t respond_media(void) {
  app_media_snapshot_t snapshot = {0};
  esp_err_t err = app_controller_media_snapshot(&snapshot);
  if (err != ESP_OK) {
    return err;
  }
  return at_server_response(
      "+TIRTC:媒体,\"%s\",\"音频发送=%lu\",\"音频接收=%lu\","
      "\"视频发送=%lu\",\"视频接收=%lu\",\"错误=%lu\"",
      media_profile_text(snapshot.active_profile),
      (unsigned long)snapshot.tx_audio_frames,
      (unsigned long)snapshot.rx_audio_frames,
      (unsigned long)snapshot.tx_video_frames,
      (unsigned long)snapshot.rx_video_frames,
      (unsigned long)snapshot.send_errors);
}

static const char *action_preset(facade_action_t action) {
  switch (action) {
  case FACADE_ACTION_STORY:
    return "STORY";
  case FACADE_ACTION_JOKE:
    return "JOKE";
  case FACADE_ACTION_WEATHER:
    return "WEATHER";
  case FACADE_ACTION_NONE:
  default:
    return NULL;
  }
}

static const char *action_accepted_text(facade_action_t action) {
  switch (action) {
  case FACADE_ACTION_STORY:
    return "正在准备故事";
  case FACADE_ACTION_JOKE:
    return "正在准备笑话";
  case FACADE_ACTION_WEATHER:
    return "正在查询天气";
  case FACADE_ACTION_NONE:
  default:
    return "正在处理";
  }
}

static bool action_caption_matches(facade_action_t action,
                                   const char *caption) {
  const char *keyword = NULL;
  switch (action) {
  case FACADE_ACTION_STORY:
    keyword = "故事";
    break;
  case FACADE_ACTION_JOKE:
    keyword = "笑话";
    break;
  case FACADE_ACTION_WEATHER:
    keyword = "天气";
    break;
  case FACADE_ACTION_NONE:
  default:
    return false;
  }
  return caption != NULL && strstr(caption, keyword) != NULL;
}

static bool parse_number(const char *text, double minimum, double maximum,
                         double *value) {
  if (text == NULL || value == NULL || text[0] == '\0') {
    return false;
  }
  errno = 0;
  char *end = NULL;
  double parsed = strtod(text, &end);
  if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed) ||
      parsed < minimum || parsed > maximum) {
    return false;
  }
  *value = parsed;
  return true;
}

static esp_err_t submit_pending(facade_action_t action, facade_phase_t phase,
                                uint32_t session_generation,
                                const char *update_json,
                                app_intent_type_t intent, const char *first) {
  esp_err_t err = take_lock();
  if (err != ESP_OK) {
    return err;
  }
  if (s_pending.action != FACADE_ACTION_NONE) {
    give_lock();
    return ESP_ERR_INVALID_STATE;
  }
  s_pending.action = action;
  s_pending.phase = phase;
  s_pending.session_generation = session_generation;
  s_pending.deadline_us =
      esp_timer_get_time() + (int64_t)FACADE_PENDING_TIMEOUT_MS * 1000LL;
  if (update_json != NULL) {
    (void)snprintf(s_pending.update_json, sizeof(s_pending.update_json), "%s",
                   update_json);
  }
  uint32_t request_id = 0;
  err = s_submit(intent, first, NULL, NULL, false, &request_id);
  if (err == ESP_OK) {
    s_pending.request_id = request_id;
    if (s_pending_timer != NULL) {
      (void)xTimerReset(s_pending_timer, 0);
    }
  } else {
    clear_pending_locked();
  }
  give_lock();
  return err;
}

static esp_err_t submit_compound_action(facade_action_t action,
                                        const char *update_json) {
  app_snapshot_t snapshot = {0};
  app_controller_snapshot(&snapshot);
  if (!snapshot.tirtc_ready || snapshot.state != APP_STATE_READY) {
    return respond_failure(ESP_ERR_INVALID_STATE, "设备尚未就绪");
  }

  bool idle = strcmp(snapshot.session_owner, "none") == 0 &&
              strcmp(snapshot.session_state, "idle") == 0;
  bool ai_active = strcmp(snapshot.session_owner, "ai") == 0 &&
                   strcmp(snapshot.session_state, "ai-active") == 0;
  bool ai_starting = strcmp(snapshot.session_owner, "ai") == 0 &&
                     (strcmp(snapshot.session_state, "ai-connecting") == 0 ||
                      strcmp(snapshot.session_state, "ai-starting") == 0);
  if (ai_starting) {
    return respond_failure(ESP_ERR_INVALID_STATE, "AI正在启动，请稍后重试");
  }
  if (!idle && !ai_active) {
    return respond_failure(ESP_ERR_INVALID_STATE, "当前正在处理其他会话");
  }

  app_session_snapshot_t session = {0};
  if (app_controller_session_snapshot(&session) != ESP_OK) {
    return respond_failure(ESP_ERR_INVALID_STATE, "暂时无法读取会话状态");
  }

  esp_err_t err;
  if (idle) {
    err = submit_pending(action, FACADE_PHASE_WAIT_AI_ACTIVE, 0, update_json,
                         APP_INTENT_AI_START, NULL);
  } else if (action == FACADE_ACTION_WEATHER && update_json != NULL &&
             update_json[0] != '\0') {
    err = submit_pending(action, FACADE_PHASE_WAIT_AI_UPDATE,
                         session.session_generation, update_json,
                         APP_INTENT_AI_UPDATE, update_json);
  } else {
    const char *preset = action_preset(action);
    err = preset == NULL ? ESP_ERR_INVALID_ARG
                         : submit_pending(action, FACADE_PHASE_WAIT_AI_CAPTION,
                                          session.session_generation, NULL,
                                          APP_INTENT_AI_PROMPT, preset);
  }
  if (err == ESP_ERR_INVALID_STATE) {
    return respond_failure(err, "已有一条AI请求正在处理");
  }
  return respond_accepted(err, action_accepted_text(action));
}

static esp_err_t handle_weather(const app_at_fields_t *fields) {
  if (fields->count == 1) {
    return submit_compound_action(FACADE_ACTION_WEATHER, NULL);
  }
  if (fields->count != 3) {
    return respond_failure(ESP_ERR_INVALID_ARG, "天气参数应为纬度和经度");
  }
  double latitude;
  double longitude;
  if (!parse_number(fields->values[1], -90.0, 90.0, &latitude) ||
      !parse_number(fields->values[2], -180.0, 180.0, &longitude)) {
    return respond_failure(ESP_ERR_INVALID_ARG, "经纬度格式或范围不正确");
  }
  char update_json[APP_TEXT_MEDIUM];
  int length =
      snprintf(update_json, sizeof(update_json),
               "{\"latitude\":%.6f,\"longitude\":%.6f}", latitude, longitude);
  if (length < 0 || (size_t)length >= sizeof(update_json)) {
    return respond_failure(ESP_ERR_INVALID_SIZE, "天气参数过长");
  }
  return submit_compound_action(FACADE_ACTION_WEATHER, update_json);
}

static esp_err_t submit_contact(app_intent_type_t intent, const char *first,
                                const char *second, const char *message) {
  esp_err_t err = take_lock();
  if (err != ESP_OK) {
    return err;
  }
  if (s_contact_request_id != 0) {
    give_lock();
    return respond_failure(ESP_ERR_INVALID_STATE, "上一条联系人指令仍在处理");
  }

  uint32_t request_id = 0;
  err = s_submit(intent, first, second, NULL, false, &request_id);
  if (err == ESP_OK) {
    s_contact_request_id = request_id;
  }
  give_lock();
  return respond_accepted(err, message);
}

static esp_err_t handle_contact(const app_at_fields_t *fields) {
  for (size_t index = 1; index < fields->count; ++index) {
    if (!valid_single_line_text(fields->values[index])) {
      return respond_failure(ESP_ERR_INVALID_ARG,
                             "联系人参数必须是有效的单行文字");
    }
  }
  if (fields->count == 1 && equals(fields->values[0], "联系人")) {
    return submit_contact(APP_INTENT_CONTACTS_LIST, NULL, NULL,
                          "正在查询联系人");
  }
  if (fields->count == 1 && equals(fields->values[0], "待处理")) {
    return submit_contact(APP_INTENT_PENDING_LIST, NULL, NULL,
                          "正在查询好友申请");
  }
  if (fields->count == 2 && equals(fields->values[0], "加好友")) {
    return submit_contact(APP_INTENT_CONTACT_REQUEST, fields->values[1], NULL,
                          "正在提交好友申请");
  }
  if (fields->count == 2 && equals(fields->values[0], "同意好友")) {
    return submit_contact(APP_INTENT_CONTACT_RESPOND, fields->values[1],
                          "ACCEPT", "正在处理好友申请");
  }
  if (fields->count == 2 && equals(fields->values[0], "拒绝好友")) {
    return submit_contact(APP_INTENT_CONTACT_RESPOND, fields->values[1],
                          "REJECT", "正在处理好友申请");
  }
  if (fields->count == 3 && equals(fields->values[0], "备注")) {
    return submit_contact(APP_INTENT_CONTACT_REMARK, fields->values[1],
                          fields->values[2], "正在更新联系人备注");
  }
  if (fields->count == 2 && equals(fields->values[0], "删除好友")) {
    return submit_contact(APP_INTENT_CONTACT_DELETE, fields->values[1], NULL,
                          "正在删除联系人");
  }
  return respond_failure(ESP_ERR_INVALID_ARG, "联系人操作参数不正确");
}

static esp_err_t read_session(app_session_snapshot_t *session) {
  esp_err_t err = app_controller_session_snapshot(session);
  return err == ESP_OK ? ESP_OK : respond_failure(err, "暂时无法读取会话状态");
}

static esp_err_t submit_direct_call(const char *target, const char *media) {
  app_session_snapshot_t session = {0};
  esp_err_t err = read_session(&session);
  if (err != ESP_OK) {
    return err;
  }
  if (strcmp(session.owner, "none") != 0 ||
      strcmp(session.state, "idle") != 0) {
    return respond_failure(ESP_ERR_INVALID_STATE, "当前正在处理其他会话");
  }
  return respond_accepted(
      s_submit(APP_INTENT_CALL_START, target, media, NULL, false, NULL),
      "正在呼叫");
}

static esp_err_t submit_call_control(app_intent_type_t intent,
                                     const char *accepted_message) {
  app_session_snapshot_t session = {0};
  esp_err_t err = read_session(&session);
  if (err != ESP_OK) {
    return err;
  }

  bool allowed = strcmp(session.owner, "call") == 0;
  const char *failure = "当前没有可操作的通话";
  if (intent == APP_INTENT_CALL_ACCEPT || intent == APP_INTENT_CALL_REJECT) {
    allowed = allowed && session.pending_incoming_call;
    failure = "当前没有来电";
  } else if (intent == APP_INTENT_CALL_CANCEL) {
    allowed =
        allowed && session.caller && strcmp(session.state, "in-call") != 0;
    failure = "当前没有正在拨出的呼叫";
  }
  if (!allowed) {
    return respond_failure(ESP_ERR_INVALID_STATE, failure);
  }
  return respond_accepted(s_submit(intent, NULL, NULL, NULL, false, NULL),
                          accepted_message);
}

static esp_err_t handle_set(const app_at_fields_t *fields) {
  if (fields->count == 3 && equals(fields->values[0], "配网")) {
    return respond_accepted(s_submit(APP_INTENT_WIFI_SET, fields->values[1],
                                     fields->values[2], NULL, false, NULL),
                            "正在保存网络配置，成功后设备将重启");
  }
  if (fields->count == 1 && equals(fields->values[0], "绑定")) {
    return handle_bind();
  }
  if (fields->count == 1 && equals(fields->values[0], "设备")) {
    return respond_device();
  }
  if (fields->count == 1 && equals(fields->values[0], "媒体")) {
    return respond_media();
  }
  if (fields->count == 1 && equals(fields->values[0], "开始AI")) {
    return respond_accepted(
        s_submit(APP_INTENT_AI_START, NULL, NULL, NULL, false, NULL),
        "正在启动AI对讲");
  }
  if (fields->count == 1 && equals(fields->values[0], "结束AI")) {
    app_session_snapshot_t session = {0};
    esp_err_t err = read_session(&session);
    if (err != ESP_OK) {
      return err;
    }
    if (strcmp(session.owner, "ai") != 0) {
      return respond_failure(ESP_ERR_INVALID_STATE, "当前没有AI对讲");
    }
    clear_pending();
    return respond_accepted(
        s_submit(APP_INTENT_AI_STOP, NULL, NULL, NULL, false, NULL),
        "正在结束AI对讲");
  }
  if (fields->count == 1 && equals(fields->values[0], "故事")) {
    return submit_compound_action(FACADE_ACTION_STORY, NULL);
  }
  if (fields->count == 1 && equals(fields->values[0], "笑话")) {
    return submit_compound_action(FACADE_ACTION_JOKE, NULL);
  }
  if (equals(fields->values[0], "天气")) {
    return handle_weather(fields);
  }
  if (fields->count == 2 && equals(fields->values[0], "呼叫")) {
    if (!valid_single_line_text(fields->values[1]) ||
        fields->values[1][0] == '\0') {
      return respond_failure(ESP_ERR_INVALID_ARG,
                             "请输入联系人备注或设备ID");
    }
    return submit_direct_call(fields->values[1], "AUDIO");
  }
  if (fields->count >= 2 && fields->count <= 3 &&
      equals(fields->values[0], "直呼")) {
    if (!valid_single_line_text(fields->values[1])) {
      return respond_failure(ESP_ERR_INVALID_ARG,
                             "设备标识必须是有效的单行文字");
    }
    const char *media = "AUDIO";
    if (fields->count == 3) {
      if (!equals(fields->values[2], "视频")) {
        return respond_failure(ESP_ERR_INVALID_ARG, "呼叫类型只支持视频");
      }
      media = "VIDEO";
    }
    return submit_direct_call(fields->values[1], media);
  }
  if (fields->count == 1 && equals(fields->values[0], "接听")) {
    return submit_call_control(APP_INTENT_CALL_ACCEPT, "正在接听");
  }
  if (fields->count == 1 && equals(fields->values[0], "拒接")) {
    return submit_call_control(APP_INTENT_CALL_REJECT, "正在拒接");
  }
  if (fields->count == 1 && equals(fields->values[0], "取消")) {
    return submit_call_control(APP_INTENT_CALL_CANCEL, "正在取消呼叫");
  }
  if (fields->count == 1 && equals(fields->values[0], "挂断")) {
    return submit_call_control(APP_INTENT_CALL_HANGUP, "正在挂断");
  }
  if (fields->count == 2 && equals(fields->values[0], "发消息")) {
    if (!valid_single_line_text(fields->values[1]) ||
        fields->values[1][0] == '\0') {
      return respond_failure(ESP_ERR_INVALID_ARG,
                             "消息必须是128字节以内的单行文字");
    }
    app_session_snapshot_t session = {0};
    if (read_session(&session) != ESP_OK ||
        strcmp(session.owner, "call") != 0 ||
        strcmp(session.state, "in-call") != 0) {
      return respond_failure(ESP_ERR_INVALID_STATE, "请先接通设备通话");
    }
    return respond_accepted(
        s_submit(APP_INTENT_CALL_MESSAGE, fields->values[1], NULL, NULL,
                 false, NULL),
        "正在发送消息");
  }
  if (fields->count == 1 && equals(fields->values[0], "重启")) {
    clear_all_pending();
    return respond_accepted(
        s_submit(APP_INTENT_RESTART, NULL, NULL, NULL, false, NULL),
        "设备即将重启");
  }

  if (equals(fields->values[0], "联系人") ||
      equals(fields->values[0], "待处理") ||
      equals(fields->values[0], "加好友") ||
      equals(fields->values[0], "同意好友") ||
      equals(fields->values[0], "拒绝好友") ||
      equals(fields->values[0], "备注") ||
      equals(fields->values[0], "删除好友")) {
    return handle_contact(fields);
  }
  return respond_failure(ESP_ERR_INVALID_ARG,
                         "未知操作，请发送 AT+帮助 查看帮助");
}

static const char *public_action(app_at_public_command_t command) {
  switch (command) {
  case APP_AT_PUBLIC_WIFI:
    return "配网";
  case APP_AT_PUBLIC_BIND:
    return "绑定";
  case APP_AT_PUBLIC_DEVICE_ID:
    return "设备";
  case APP_AT_PUBLIC_STORY:
    return "故事";
  case APP_AT_PUBLIC_JOKE:
    return "笑话";
  case APP_AT_PUBLIC_WEATHER:
    return "天气";
  case APP_AT_PUBLIC_AI_STOP:
    return "结束AI";
  case APP_AT_PUBLIC_CONTACTS:
    return "联系人";
  case APP_AT_PUBLIC_PENDING:
    return "待处理";
  case APP_AT_PUBLIC_CONTACT_REQUEST:
    return "加好友";
  case APP_AT_PUBLIC_CONTACT_ACCEPT:
    return "同意好友";
  case APP_AT_PUBLIC_CONTACT_REMARK:
    return "备注";
  case APP_AT_PUBLIC_CALL:
    return "呼叫";
  case APP_AT_PUBLIC_CALL_ACCEPT:
    return "接听";
  case APP_AT_PUBLIC_CALL_REJECT:
    return "拒接";
  case APP_AT_PUBLIC_CALL_CANCEL:
    return "取消";
  case APP_AT_PUBLIC_CALL_HANGUP:
    return "挂断";
  case APP_AT_PUBLIC_CALL_MESSAGE:
    return "发消息";
  case APP_AT_PUBLIC_HELP:
  case APP_AT_PUBLIC_STATUS:
  default:
    return NULL;
  }
}

static bool public_command_has_arguments(app_at_public_command_t command) {
  return command == APP_AT_PUBLIC_WIFI ||
         command == APP_AT_PUBLIC_CONTACT_REQUEST ||
         command == APP_AT_PUBLIC_CONTACT_ACCEPT ||
         command == APP_AT_PUBLIC_CONTACT_REMARK ||
         command == APP_AT_PUBLIC_CALL ||
         command == APP_AT_PUBLIC_CALL_MESSAGE;
}

static esp_err_t build_public_fields(const at_server_request_t *request,
                                     app_at_public_command_t command,
                                     app_at_fields_t *fields) {
  const char *action = public_action(command);
  if (request == NULL || fields == NULL || action == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  const bool has_arguments = public_command_has_arguments(command);
  if ((has_arguments && request->operation != AT_SERVER_OP_SET) ||
      (!has_arguments && request->operation != AT_SERVER_OP_EXECUTE)) {
    return ESP_ERR_INVALID_ARG;
  }

  memset(fields, 0, sizeof(*fields));
  (void)snprintf(fields->values[0], sizeof(fields->values[0]), "%s", action);
  fields->count = 1;
  if (!has_arguments) {
    return ESP_OK;
  }

  app_at_fields_t arguments;
  esp_err_t err = parse_set(request, &arguments);
  if (err != ESP_OK || arguments.count + 1U > APP_AT_MAX_FIELDS) {
    secure_zero(&arguments, sizeof(arguments));
    return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
  }
  for (size_t index = 0; index < arguments.count; ++index) {
    (void)snprintf(fields->values[index + 1U],
                   sizeof(fields->values[index + 1U]), "%s",
                   arguments.values[index]);
  }
  fields->count += arguments.count;
  secure_zero(&arguments, sizeof(arguments));
  return ESP_OK;
}

esp_err_t app_at_facade_command(const at_server_request_t *request,
                                void *context) {
  if (request == NULL || s_submit == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  const app_at_public_command_t command =
      (app_at_public_command_t)(uintptr_t)context;
  if (command == APP_AT_PUBLIC_HELP &&
      request->operation == AT_SERVER_OP_EXECUTE) {
    return respond_help();
  }
  if (command == APP_AT_PUBLIC_STATUS &&
      request->operation == AT_SERVER_OP_EXECUTE) {
    return respond_status();
  }

  app_at_fields_t fields;
  esp_err_t err = build_public_fields(request, command, &fields);
  if (err != ESP_OK) {
    secure_zero(&fields, sizeof(fields));
    return respond_failure(err, "指令格式不正确，请发送 AT+帮助");
  }
  err = handle_set(&fields);
  secure_zero(&fields, sizeof(fields));
  return err;
}

esp_err_t app_at_facade_legacy_command(const at_server_request_t *request,
                                       void *context) {
  (void)context;
  if (request == NULL || s_submit == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  if (request->operation == AT_SERVER_OP_EXECUTE ||
      request->operation == AT_SERVER_OP_TEST) {
    return respond_help();
  }
  if (request->operation == AT_SERVER_OP_READ) {
    return respond_status();
  }

  app_at_fields_t fields;
  esp_err_t err = parse_set(request, &fields);
  if (err != ESP_OK) {
    secure_zero(&fields, sizeof(fields));
    return respond_failure(err, "参数格式不正确");
  }
  err = handle_set(&fields);
  secure_zero(&fields, sizeof(fields));
  return err;
}

esp_err_t app_at_protocol_command(const at_server_request_t *request,
                                  void *context) {
  (void)context;
  if (request == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (request->operation == AT_SERVER_OP_READ) {
    esp_err_t err = take_lock();
    if (err != ESP_OK) {
      return err;
    }
    app_at_mode_t mode = s_mode;
    give_lock();
    return at_server_response("+PROTO:%s",
                              mode == APP_AT_MODE_RAW ? "RAW" : "USER");
  }

  app_at_fields_t fields;
  esp_err_t err = parse_set(request, &fields);
  if (err != ESP_OK || fields.count != 1) {
    secure_zero(&fields, sizeof(fields));
    return ESP_ERR_INVALID_ARG;
  }
  app_at_mode_t requested;
  if (equals(fields.values[0], "RAW") || equals(fields.values[0], "详细")) {
    requested = APP_AT_MODE_RAW;
  } else if (equals(fields.values[0], "USER") ||
             equals(fields.values[0], "简洁")) {
    requested = APP_AT_MODE_USER;
  } else {
    secure_zero(&fields, sizeof(fields));
    return ESP_ERR_INVALID_ARG;
  }
  secure_zero(&fields, sizeof(fields));

  err = take_lock();
  if (err != ESP_OK) {
    return err;
  }
  if (requested != s_mode) {
    clear_all_pending_locked();
  }
  err = at_server_flush_urcs();
  if (err == ESP_OK) {
    s_mode = requested;
    err = at_server_response("+PROTO:%s",
                             requested == APP_AT_MODE_RAW ? "RAW" : "USER");
  }
  give_lock();
  return err;
}

bool app_at_facade_raw_mode(void) {
  if (take_lock() != ESP_OK) {
    return false;
  }
  bool raw = s_mode == APP_AT_MODE_RAW;
  give_lock();
  return raw;
}

static void emit_simple_failure_locked(const char *message) {
  if (s_mode == APP_AT_MODE_USER) {
    (void)at_server_urc("+失败,\"%s\"", message);
  }
}

static void pending_timer_callback(TimerHandle_t timer) {
  (void)timer;
  if (take_lock() != ESP_OK) {
    return;
  }
  if (s_pending.action != FACADE_ACTION_NONE && s_pending.deadline_us > 0 &&
      esp_timer_get_time() >= s_pending.deadline_us) {
    clear_pending_locked();
    emit_simple_failure_locked("AI请求超时，请重试");
  }
  give_lock();
}

static bool coordinate_event_locked(const app_event_t *event) {
  if (s_pending.action == FACADE_ACTION_NONE) {
    return true;
  }
  if (event->domain == APP_EVENT_SYSTEM &&
      strcmp(event->name, "RESTARTING") == 0) {
    clear_all_pending_locked();
    return true;
  }
  if (event->domain == APP_EVENT_ERROR &&
      strcmp(event->name, "REQUEST_REJECTED") == 0 &&
      event->request_id == s_pending.request_id) {
    clear_pending_locked();
    return true;
  }
  if (event->domain != APP_EVENT_SESSION) {
    return true;
  }
  if (strcmp(event->name, "AI:STATE") == 0) {
    if (strcmp(event->first, "idle") == 0 &&
        (event->request_id == s_pending.request_id ||
         (s_pending.session_generation != 0 &&
          event->generation == s_pending.session_generation))) {
      clear_pending_locked();
      return true;
    }
    if (strcmp(event->first, "ai-active") == 0 &&
        s_pending.phase == FACADE_PHASE_WAIT_AI_ACTIVE &&
        event->request_id == s_pending.request_id) {
      esp_err_t err;
      uint32_t request_id = 0;
      s_pending.session_generation = event->generation;
      if (s_pending.action == FACADE_ACTION_WEATHER &&
          s_pending.update_json[0] != '\0') {
        s_pending.phase = FACADE_PHASE_WAIT_AI_UPDATE;
        err = s_submit(APP_INTENT_AI_UPDATE, s_pending.update_json, NULL, NULL,
                       false, &request_id);
      } else {
        const char *preset = action_preset(s_pending.action);
        err = preset == NULL ? ESP_ERR_INVALID_ARG
                             : s_submit(APP_INTENT_AI_PROMPT, preset, NULL,
                                        NULL, false, &request_id);
        if (err == ESP_OK) {
          s_pending.phase = FACADE_PHASE_WAIT_AI_CAPTION;
        }
      }
      if (err == ESP_OK) {
        s_pending.request_id = request_id;
      } else {
        clear_pending_locked();
        emit_simple_failure_locked("AI请求未发送，请重试");
      }
    }
    return true;
  }
  if (strcmp(event->name, "AI:OP") == 0 &&
      strcmp(event->first, "ai-update-config") == 0 &&
      s_pending.phase == FACADE_PHASE_WAIT_AI_UPDATE &&
      event->request_id == s_pending.request_id) {
    if (event->code == 0 && strcmp(event->second, "completed") == 0) {
      uint32_t request_id = 0;
      esp_err_t err = s_submit(APP_INTENT_AI_PROMPT, "WEATHER", NULL, NULL,
                               false, &request_id);
      if (err == ESP_OK) {
        s_pending.phase = FACADE_PHASE_WAIT_AI_CAPTION;
        s_pending.request_id = request_id;
        secure_zero(s_pending.update_json, sizeof(s_pending.update_json));
      } else {
        clear_pending_locked();
        emit_simple_failure_locked("天气请求未发送，请重试");
      }
    } else if (event->code != 0 || strcmp(event->second, "rejected") == 0 ||
               strcmp(event->second, "response-timeout") == 0) {
      clear_pending_locked();
    }
    return true;
  }
  if (strcmp(event->name, "AI:OP") == 0 &&
      strcmp(event->first, "ai-prompt") == 0 && event->code != 0 &&
      event->request_id == s_pending.request_id) {
    clear_pending_locked();
    return true;
  }
  if (strcmp(event->name, "AI:CAPTION") == 0 && event->flag &&
      s_pending.phase == FACADE_PHASE_WAIT_AI_CAPTION &&
      event->generation == s_pending.session_generation) {
    if (event->code == 0 &&
        action_caption_matches(s_pending.action, event->payload)) {
      s_pending.phase = FACADE_PHASE_WAIT_AI_ROUND_END;
      return true;
    }
    return false;
  }
  if (strcmp(event->name, "AI:EVENT") == 0 &&
      strcmp(event->first, "round_end") == 0 &&
      event->generation == s_pending.session_generation) {
    if (s_pending.phase == FACADE_PHASE_WAIT_AI_CAPTION) {
      return false;
    }
    if (s_pending.phase == FACADE_PHASE_WAIT_AI_ROUND_END) {
      clear_pending_locked();
    }
  }
  return true;
}

static void coordinate_contact_event_locked(const app_event_t *event) {
  if (s_contact_request_id == 0 || event->request_id != s_contact_request_id) {
    return;
  }
  if (event->domain == APP_EVENT_ERROR &&
      strcmp(event->name, "REQUEST_REJECTED") == 0) {
    clear_contact_pending_locked();
    return;
  }
  if (event->domain != APP_EVENT_SESSION) {
    return;
  }
  if (strcmp(event->name, "CONTACTS:DONE") == 0 ||
      strcmp(event->name, "PENDING:DONE") == 0) {
    clear_contact_pending_locked();
    return;
  }
  if (strcmp(event->name, "CONTACT:OP") == 0 &&
      (event->code != 0 || strcmp(event->second, "submitted") != 0)) {
    clear_contact_pending_locked();
  }
}

static const char *call_error_text(const char *phase) {
  if (strcmp(phase, "target-offline") == 0) {
    return "目标设备不在线";
  }
  if (strcmp(phase, "target-ambiguous") == 0) {
    return "联系人备注重复，请设置唯一备注";
  }
  if (strcmp(phase, "target-not-found") == 0) {
    return "没有找到这个联系人";
  }
  if (strcmp(phase, "target-invalid") == 0) {
    return "联系人名称无效";
  }
  if (strcmp(phase, "contacts-timeout") == 0) {
    return "查询联系人超时，请重试";
  }
  if (strncmp(phase, "contacts-", 9) == 0 ||
      strcmp(phase, "request-failed-or-invalid-response") == 0) {
    return "暂时无法读取联系人，请重试";
  }
  return "呼叫设备失败";
}

static const char *contact_completed_text(const char *operation,
                                          const char *phase) {
  if (strcmp(operation, "contacts-request") == 0) {
    if (strcmp(phase, "auto-accepted") == 0) {
      return "同一账号设备，已自动成为联系人";
    }
    if (strcmp(phase, "accepted") == 0) {
      return "双方已经是联系人";
    }
    if (strcmp(phase, "pending") == 0) {
      return "好友申请已发送，等待对方同意";
    }
    return "好友申请已提交";
  }
  if (strcmp(operation, "contacts-respond") == 0) {
    return "好友申请已处理";
  }
  if (strcmp(operation, "contacts-remark") == 0) {
    return "联系人备注已更新";
  }
  if (strcmp(operation, "contacts-delete") == 0) {
    return "联系人已删除";
  }
  return "联系人操作已完成";
}

static void emit_user_event_locked(const app_event_t *event,
                                   const app_user_log_message_t *message) {
  char first[APP_TEXT_MEDIUM * 2U + 1U];
  char second[APP_TEXT_MEDIUM * 2U + 1U];
  if (event->domain == APP_EVENT_BIND && strcmp(event->name, "CODE") == 0) {
    if (app_at_escape(event->first, first, sizeof(first)) == ESP_OK) {
      (void)at_server_urc("+绑定码,\"%s\"", first);
    }
    return;
  }
  if (event->domain == APP_EVENT_SESSION &&
      strcmp(event->name, "CONTACT") == 0) {
    if (app_at_escape(event->first, first, sizeof(first)) == ESP_OK &&
        app_at_escape(event->second, second, sizeof(second)) == ESP_OK) {
      (void)at_server_urc("+联系人,%d,%d,\"%s\",\"%s\"", event->value1,
                          event->flag, first, second);
    }
    return;
  }
  if (event->domain == APP_EVENT_SESSION &&
      strcmp(event->name, "PENDING") == 0) {
    if (app_at_escape(event->first, first, sizeof(first)) == ESP_OK) {
      (void)at_server_urc("+好友申请,%d,\"%s\"", event->value1, first);
    }
    return;
  }
  if (event->domain == APP_EVENT_SESSION &&
      strcmp(event->name, "CALL:INCOMING") == 0) {
    if (app_at_escape(event->first, first, sizeof(first)) == ESP_OK) {
      const char *media =
          strcmp(event->payload, "video") == 0 ? "视频" : "音频";
      (void)at_server_urc("+来电,\"%s\",\"%s\"", first, media);
    }
    return;
  }
  if (event->domain == APP_EVENT_SESSION &&
      strcmp(event->name, "CONTACT:OP") == 0 && event->code == 0 &&
      strcmp(event->second, "submitted") != 0) {
    (void)at_server_urc("+联系人,\"%s\"",
                        contact_completed_text(event->first, event->second));
    return;
  }
  if (event->domain == APP_EVENT_SESSION &&
      (strcmp(event->name, "CONTACTS:DONE") == 0 ||
       strcmp(event->name, "PENDING:DONE") == 0) &&
      event->code == 0) {
    (void)at_server_urc("+%s,\"共%d个\"",
                        strcmp(event->name, "CONTACTS:DONE") == 0 ? "联系人"
                                                                  : "好友申请",
                        event->value1);
    return;
  }
  if (event->domain == APP_EVENT_SESSION &&
      strcmp(event->name, "AI:EVENT") == 0 &&
      strcmp(event->first, "round_end") == 0) {
    (void)at_server_urc("+AI对讲,\"本轮完成\"");
    return;
  }
  if (event->domain == APP_EVENT_SESSION && strcmp(event->name, "AI:OP") == 0 &&
      strcmp(event->first, "ai-call-device") == 0 && event->code != 0) {
    (void)at_server_urc("+失败,\"%s\"", call_error_text(event->second));
    return;
  }
  if (event->domain == APP_EVENT_SESSION &&
      strcmp(event->name, "CALL:OP") == 0 &&
      strcmp(event->first, "call-start") == 0 && event->code != 0) {
    (void)at_server_urc("+失败,\"%s\"", call_error_text(event->second));
    return;
  }
  if (message == NULL || message->category[0] == '\0') {
    return;
  }

  char category[APP_USER_LOG_CATEGORY_SIZE * 2U + 1U];
  char subject[APP_USER_LOG_SUBJECT_SIZE * 2U + 1U];
  char text[APP_USER_LOG_TEXT_SIZE * 2U + 1U];
  if (app_at_escape(message->category, category, sizeof(category)) != ESP_OK ||
      app_at_escape(message->subject, subject, sizeof(subject)) != ESP_OK ||
      app_at_escape(message->text, text, sizeof(text)) != ESP_OK) {
    return;
  }
  if (subject[0] == '\0') {
    (void)at_server_urc("+%s,\"%s\"", category, text);
  } else {
    (void)at_server_urc("+%s,\"%s\",\"%s\"", category, subject, text);
  }
}

void app_at_facade_route_event(const app_event_t *event,
                               const app_user_log_message_t *user_message,
                               app_at_facade_raw_event_fn raw_event) {
  if (event == NULL || raw_event == NULL || take_lock() != ESP_OK) {
    return;
  }
  coordinate_contact_event_locked(event);
  bool emit = coordinate_event_locked(event);
  if (!emit) {
    give_lock();
    return;
  }
  if (s_mode == APP_AT_MODE_RAW) {
    raw_event(event);
  } else {
    emit_user_event_locked(event, user_message);
  }
  give_lock();
}

esp_err_t app_at_facade_init(app_at_facade_submit_fn submit) {
  if (submit == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (s_lock == NULL) {
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
      return ESP_ERR_NO_MEM;
    }
  }
  if (s_pending_timer == NULL) {
    s_pending_timer =
        xTimerCreate("at-facade", pdMS_TO_TICKS(FACADE_PENDING_TIMEOUT_MS),
                     pdFALSE, NULL, pending_timer_callback);
    if (s_pending_timer == NULL) {
      return ESP_ERR_NO_MEM;
    }
  }
  esp_err_t err = take_lock();
  if (err != ESP_OK) {
    return err;
  }
  s_submit = submit;
  s_mode = APP_AT_MODE_USER;
  clear_all_pending_locked();
  give_lock();
  return ESP_OK;
}
