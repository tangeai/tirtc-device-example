#include "at_server.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define AT_SERVER_READ_CHUNK 64
#define AT_SERVER_READ_TIMEOUT_MS 100
#define AT_SERVER_WRITE_TIMEOUT_MS 250
#define AT_SERVER_START_TIMEOUT_MS 1000
#define AT_SERVER_STOP_TIMEOUT_MS 1500
#define AT_SERVER_URC_QUEUE_DEPTH 64

static at_server_config_t s_config;
static SemaphoreHandle_t s_state_lock;
static SemaphoreHandle_t s_started;
static SemaphoreHandle_t s_stopped;
static QueueHandle_t s_urc_queue;
static atomic_uintptr_t s_task;
static atomic_bool s_initialized;
static atomic_bool s_running;
static atomic_bool s_transport_owned;
static atomic_uint_fast32_t s_urc_dropped;
static volatile esp_err_t s_start_result;
static at_transport_t s_transport;
static bool s_echo;
static char s_output_buffer[AT_SERVER_OUTPUT_MAX];

static void secure_zero(void *memory, size_t size);

static void free_urc_line(char *line) {
  if (line == NULL) {
    return;
  }
  secure_zero(line, strlen(line));
  free(line);
}

static void discard_pending_urcs(void) {
  if (s_urc_queue == NULL) {
    return;
  }
  char *line = NULL;
  while (xQueueReceive(s_urc_queue, &line, 0) == pdTRUE) {
    free_urc_line(line);
    line = NULL;
  }
}

static void secure_zero(void *memory, size_t size) {
  volatile unsigned char *cursor = memory;
  while (cursor != NULL && size-- > 0U) {
    *cursor++ = 0U;
  }
}

static void wipe_output_buffer(void) {
  secure_zero(s_output_buffer, sizeof(s_output_buffer));
}

static TaskHandle_t task_handle(void) {
  return (TaskHandle_t)atomic_load_explicit(&s_task, memory_order_acquire);
}

static unsigned char ascii_upper(unsigned char character) {
  if (character >= 'a' && character <= 'z') {
    return (unsigned char)(character - ('a' - 'A'));
  }
  return character;
}

static bool utf8_equal_ascii_ignore_case(const char *left, const char *right) {
  if (left == NULL || right == NULL) {
    return false;
  }
  while (*left != '\0' && *right != '\0') {
    if (ascii_upper((unsigned char)*left) !=
        ascii_upper((unsigned char)*right)) {
      return false;
    }
    ++left;
    ++right;
  }
  return *left == '\0' && *right == '\0';
}

static size_t valid_utf8_sequence_length(const unsigned char *text,
                                         size_t remaining) {
  if (text == NULL || remaining == 0U) {
    return 0U;
  }
  if (text[0] >= 0xC2U && text[0] <= 0xDFU && remaining >= 2U &&
      text[1] >= 0x80U && text[1] <= 0xBFU) {
    return 2U;
  }
  if (remaining >= 3U &&
      ((text[0] == 0xE0U && text[1] >= 0xA0U && text[1] <= 0xBFU) ||
       ((text[0] >= 0xE1U && text[0] <= 0xECU) && text[1] >= 0x80U &&
        text[1] <= 0xBFU) ||
       (text[0] == 0xEDU && text[1] >= 0x80U && text[1] <= 0x9FU) ||
       ((text[0] >= 0xEEU && text[0] <= 0xEFU) && text[1] >= 0x80U &&
        text[1] <= 0xBFU)) &&
      text[2] >= 0x80U && text[2] <= 0xBFU) {
    return 3U;
  }
  if (remaining >= 4U &&
      ((text[0] == 0xF0U && text[1] >= 0x90U && text[1] <= 0xBFU) ||
       ((text[0] >= 0xF1U && text[0] <= 0xF3U) && text[1] >= 0x80U &&
        text[1] <= 0xBFU) ||
       (text[0] == 0xF4U && text[1] >= 0x80U && text[1] <= 0x8FU)) &&
      text[2] >= 0x80U && text[2] <= 0xBFU && text[3] >= 0x80U &&
      text[3] <= 0xBFU) {
    return 4U;
  }
  return 0U;
}

static bool command_name_valid(const char *name) {
  if (name == NULL || name[0] == '\0') {
    return false;
  }
  const size_t length = strlen(name);
  if (length > AT_SERVER_COMMAND_NAME_MAX) {
    return false;
  }
  for (size_t index = 0; index < length;) {
    const unsigned char character = (unsigned char)name[index];
    if (character < 0x80U) {
      if (!(isalnum(character) || character == '_')) {
        return false;
      }
      ++index;
      continue;
    }
    const size_t sequence_length = valid_utf8_sequence_length(
        (const unsigned char *)&name[index], length - index);
    if (sequence_length == 0U) {
      return false;
    }
    index += sequence_length;
  }
  return true;
}

static esp_err_t write_locked_line(const char *line) {
  if (!atomic_load_explicit(&s_running, memory_order_acquire) ||
      !atomic_load_explicit(&s_transport_owned, memory_order_acquire) ||
      line == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  if (strchr(line, '\r') != NULL || strchr(line, '\n') != NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  const size_t length = strlen(line);
  int written = at_transport_write(&s_transport, line, length,
                                   pdMS_TO_TICKS(AT_SERVER_WRITE_TIMEOUT_MS));
  if (written < 0 || (size_t)written != length) {
    return ESP_FAIL;
  }
  written = at_transport_write(&s_transport, "\r\n", 2,
                               pdMS_TO_TICKS(AT_SERVER_WRITE_TIMEOUT_MS));
  return written == 2 ? ESP_OK : ESP_FAIL;
}

static void write_final_result(esp_err_t result) {
  if (result == ESP_OK) {
    (void)write_locked_line("OK");
    return;
  }
  char error_line[32];
  (void)snprintf(error_line, sizeof(error_line), "ERROR:%d", (int)result);
  (void)write_locked_line(error_line);
  secure_zero(error_line, sizeof(error_line));
}

static esp_err_t write_formatted_line(const char *format, va_list arguments) {
  if (format == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (xTaskGetCurrentTaskHandle() != task_handle()) {
    return ESP_ERR_INVALID_STATE;
  }
  const int length =
      vsnprintf(s_output_buffer, sizeof(s_output_buffer), format, arguments);
  esp_err_t err = ESP_OK;
  if (length < 0) {
    err = ESP_FAIL;
  } else if ((size_t)length >= sizeof(s_output_buffer)) {
    err = ESP_ERR_INVALID_SIZE;
  } else {
    err = write_locked_line(s_output_buffer);
  }
  secure_zero(s_output_buffer, sizeof(s_output_buffer));
  return err;
}

static esp_err_t queue_formatted_urc(const char *format, va_list arguments) {
  if (format == NULL || s_urc_queue == NULL ||
      !atomic_load_explicit(&s_running, memory_order_acquire)) {
    return ESP_ERR_INVALID_STATE;
  }

  va_list sizing_arguments;
  va_copy(sizing_arguments, arguments);
  int length = vsnprintf(NULL, 0, format, sizing_arguments);
  va_end(sizing_arguments);
  if (length < 0) {
    return ESP_FAIL;
  }
  if ((size_t)length >= AT_SERVER_OUTPUT_MAX) {
    return ESP_ERR_INVALID_SIZE;
  }

  char *line = malloc((size_t)length + 1U);
  if (line == NULL) {
    return ESP_ERR_NO_MEM;
  }
  va_list writing_arguments;
  va_copy(writing_arguments, arguments);
  int written = vsnprintf(line, (size_t)length + 1U, format, writing_arguments);
  va_end(writing_arguments);
  if (written != length || strchr(line, '\r') != NULL ||
      strchr(line, '\n') != NULL) {
    free_urc_line(line);
    return written != length ? ESP_FAIL : ESP_ERR_INVALID_ARG;
  }
  if (xQueueSend(s_urc_queue, &line, 0) != pdTRUE) {
    free_urc_line(line);
    (void)atomic_fetch_add_explicit(&s_urc_dropped, 1, memory_order_relaxed);
    return ESP_ERR_TIMEOUT;
  }
  return ESP_OK;
}

static esp_err_t flush_pending_urcs(void) {
  if (s_urc_queue == NULL) {
    return ESP_OK;
  }
  char *line = NULL;
  UBaseType_t pending = uxQueueMessagesWaiting(s_urc_queue);
  while (pending-- > 0U && xQueueReceive(s_urc_queue, &line, 0) == pdTRUE) {
    esp_err_t err = write_locked_line(line);
    free_urc_line(line);
    line = NULL;
    if (err != ESP_OK) {
      /* A detached host must not leave the AT task draining a stale
       * backlog one write timeout at a time. Recoverable snapshots are
       * the source of truth after the host reconnects. */
      discard_pending_urcs();
      return err;
    }
  }
  uint32_t dropped = (uint32_t)atomic_exchange_explicit(&s_urc_dropped, 0,
                                                        memory_order_acq_rel);
  if (dropped != 0U) {
    int length = snprintf(s_output_buffer, sizeof(s_output_buffer),
                          "+TIRTC:警告,\"消息过多，部分状态已丢失\",%lu",
                          (unsigned long)dropped);
    if (length > 0 && (size_t)length < sizeof(s_output_buffer)) {
      esp_err_t err = write_locked_line(s_output_buffer);
      wipe_output_buffer();
      if (err != ESP_OK) {
        return err;
      }
    } else {
      wipe_output_buffer();
      return ESP_FAIL;
    }
  }
  return ESP_OK;
}

esp_err_t at_server_response(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  const esp_err_t err = write_formatted_line(format, arguments);
  va_end(arguments);
  return err;
}

esp_err_t at_server_urc(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  const esp_err_t err = queue_formatted_urc(format, arguments);
  va_end(arguments);
  return err;
}

esp_err_t at_server_flush_urcs(void) {
  if (!atomic_load_explicit(&s_running, memory_order_acquire) ||
      xTaskGetCurrentTaskHandle() != task_handle()) {
    return ESP_ERR_INVALID_STATE;
  }
  return flush_pending_urcs();
}

static const at_server_command_t *find_command(const char *name) {
  for (size_t index = 0; index < s_config.command_count; ++index) {
    if (utf8_equal_ascii_ignore_case(name, s_config.commands[index].name)) {
      return &s_config.commands[index];
    }
  }
  return NULL;
}

static char *trim_line(char *line) {
  while (isspace((unsigned char)*line)) {
    ++line;
  }
  char *end = line + strlen(line);
  while (end > line && isspace((unsigned char)end[-1])) {
    *--end = '\0';
  }
  return line;
}

static esp_err_t parse_request(char *body, at_server_request_t *request,
                               char *name, size_t name_size) {
  char *name_end = body;
  while (*name_end != '\0' && *name_end != '?' && *name_end != '=') {
    ++name_end;
  }
  const size_t name_length = (size_t)(name_end - body);
  if (name_length == 0 || name_length > AT_SERVER_COMMAND_NAME_MAX ||
      name == NULL || name_size <= name_length) {
    return ESP_ERR_INVALID_ARG;
  }

  memcpy(name, body, name_length);
  name[name_length] = '\0';
  if (!command_name_valid(name)) {
    return ESP_ERR_INVALID_ARG;
  }

  request->name = name;
  request->arguments = NULL;
  request->arguments_length = 0;
  if (*name_end == '\0') {
    request->operation = AT_SERVER_OP_EXECUTE;
    return ESP_OK;
  }
  if (strcmp(name_end, "?") == 0) {
    request->operation = AT_SERVER_OP_READ;
    return ESP_OK;
  }
  if (strcmp(name_end, "=?") == 0) {
    request->operation = AT_SERVER_OP_TEST;
    return ESP_OK;
  }
  if (*name_end == '=') {
    request->operation = AT_SERVER_OP_SET;
    request->arguments = name_end + 1;
    request->arguments_length = strlen(request->arguments);
    return ESP_OK;
  }
  return ESP_ERR_INVALID_ARG;
}

static void dispatch_line(char *raw_line, size_t raw_line_size) {
  char *line = trim_line(raw_line);
  if (*line == '\0') {
    secure_zero(raw_line, raw_line_size);
    return;
  }

  if (s_echo) {
    (void)write_locked_line(line);
  }

  esp_err_t result = ESP_ERR_INVALID_ARG;
  char parsed_name[AT_SERVER_COMMAND_NAME_MAX + 1];
  secure_zero(parsed_name, sizeof(parsed_name));
  if (utf8_equal_ascii_ignore_case(line, "AT")) {
    result = ESP_OK;
  } else if (utf8_equal_ascii_ignore_case(line, "ATE0")) {
    s_echo = false;
    result = ESP_OK;
  } else if (utf8_equal_ascii_ignore_case(line, "ATE1")) {
    s_echo = false;
    result = ESP_ERR_NOT_SUPPORTED;
  } else if (toupper((unsigned char)line[0]) == 'A' &&
             toupper((unsigned char)line[1]) == 'T' && line[2] == '+') {
    at_server_request_t request = {0};
    result =
        parse_request(line + 3, &request, parsed_name, sizeof(parsed_name));
    if (result == ESP_OK) {
      const at_server_command_t *command = find_command(request.name);
      result = command == NULL ? ESP_ERR_NOT_FOUND
                               : command->handler(&request, command->context);
    }
  }

  write_final_result(result);
  secure_zero(parsed_name, sizeof(parsed_name));
  secure_zero(raw_line, raw_line_size);
  secure_zero(s_output_buffer, sizeof(s_output_buffer));
}

static void at_server_task(void *argument) {
  (void)argument;
  uint8_t input[AT_SERVER_READ_CHUNK];
  char line[AT_SERVER_LINE_MAX + 1];
  size_t line_length = 0;
  bool line_invalid = false;
  TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

  /*
   * A newly created higher-priority task may run before at_server_start()
   * publishes its handle. Waiting for that publication makes task ownership
   * unambiguous on both the normal and transport-open failure paths.
   */
  while (task_handle() != current_task) {
    vTaskDelay(1);
  }

  s_start_result = at_transport_open(&s_transport, &s_config.transport);
  if (s_start_result != ESP_OK) {
    atomic_store_explicit(&s_running, false, memory_order_release);
    if (s_started != NULL) {
      (void)xSemaphoreGive(s_started);
    }
    atomic_store_explicit(&s_task, (uintptr_t)NULL, memory_order_release);
    if (s_stopped != NULL) {
      (void)xSemaphoreGive(s_stopped);
    }
    vTaskDelete(NULL);
    return;
  }
  atomic_store_explicit(&s_transport_owned, true, memory_order_release);
  if (s_started != NULL) {
    (void)xSemaphoreGive(s_started);
  }

  secure_zero(input, sizeof(input));
  secure_zero(line, sizeof(line));
  while (atomic_load_explicit(&s_running, memory_order_acquire)) {
    const int received =
        at_transport_read(&s_transport, input, sizeof(input),
                          pdMS_TO_TICKS(AT_SERVER_READ_TIMEOUT_MS));
    if (received < 0) {
      secure_zero(input, sizeof(input));
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    for (int index = 0; index < received; ++index) {
      const uint8_t byte = input[index];
      if (byte == '\r' || byte == '\n') {
        if (line_length == 0 && !line_invalid) {
          continue;
        }
        if (line_invalid) {
          write_final_result(ESP_ERR_INVALID_SIZE);
          secure_zero(s_output_buffer, sizeof(s_output_buffer));
          secure_zero(line, sizeof(line));
        } else {
          line[line_length] = '\0';
          dispatch_line(line, sizeof(line));
          secure_zero(line, sizeof(line));
          (void)flush_pending_urcs();
        }
        line_length = 0;
        line_invalid = false;
        continue;
      }
      if (byte == '\b' || byte == 0x7f) {
        if (line_length > 0) {
          line[--line_length] = '\0';
        }
        continue;
      }
      if (byte == '\0' || (byte < 0x20 && byte != '\t')) {
        line_invalid = true;
        continue;
      }
      if (!line_invalid) {
        if (line_length < AT_SERVER_LINE_MAX) {
          line[line_length++] = (char)byte;
        } else {
          line_invalid = true;
        }
      }
    }
    secure_zero(input, sizeof(input));
    (void)flush_pending_urcs();
  }

  secure_zero(input, sizeof(input));
  secure_zero(line, sizeof(line));
  discard_pending_urcs();
  wipe_output_buffer();
  if (atomic_exchange_explicit(&s_transport_owned, false,
                               memory_order_acq_rel)) {
    (void)at_transport_drain(&s_transport,
                             pdMS_TO_TICKS(AT_SERVER_WRITE_TIMEOUT_MS));
    (void)at_transport_close(&s_transport);
  }
  atomic_store_explicit(&s_task, (uintptr_t)NULL, memory_order_release);
  if (s_stopped != NULL) {
    (void)xSemaphoreGive(s_stopped);
  }
  vTaskDelete(NULL);
}

esp_err_t at_server_init(const at_server_config_t *config) {
  if (config == NULL || at_transport_validate(&config->transport) != ESP_OK ||
      config->task_stack_size < 2048 ||
      (config->command_count > 0 && config->commands == NULL)) {
    return ESP_ERR_INVALID_ARG;
  }
  if (config->echo) {
    return ESP_ERR_NOT_SUPPORTED;
  }
  for (size_t index = 0; index < config->command_count; ++index) {
    const at_server_command_t *command = &config->commands[index];
    if (!command_name_valid(command->name) || command->handler == NULL) {
      return ESP_ERR_INVALID_ARG;
    }
    for (size_t duplicate = index + 1; duplicate < config->command_count;
         ++duplicate) {
      if (utf8_equal_ascii_ignore_case(command->name,
                                       config->commands[duplicate].name)) {
        return ESP_ERR_INVALID_ARG;
      }
    }
  }

  if (s_state_lock == NULL) {
    s_state_lock = xSemaphoreCreateMutex();
  }
  if (s_started == NULL) {
    s_started = xSemaphoreCreateBinary();
  }
  if (s_stopped == NULL) {
    s_stopped = xSemaphoreCreateBinary();
  }
  if (s_urc_queue == NULL) {
    s_urc_queue = xQueueCreate(AT_SERVER_URC_QUEUE_DEPTH, sizeof(char *));
  }
  if (s_state_lock == NULL || s_started == NULL || s_stopped == NULL ||
      s_urc_queue == NULL) {
    return ESP_ERR_NO_MEM;
  }

  if (xSemaphoreTake(s_state_lock, portMAX_DELAY) != pdTRUE) {
    return ESP_FAIL;
  }
  if (atomic_load_explicit(&s_running, memory_order_acquire) ||
      atomic_load_explicit(&s_transport_owned, memory_order_acquire) ||
      task_handle() != NULL) {
    (void)xSemaphoreGive(s_state_lock);
    return ESP_ERR_INVALID_STATE;
  }
  s_config = *config;
  s_echo = false;
  discard_pending_urcs();
  atomic_store_explicit(&s_urc_dropped, 0, memory_order_release);
  secure_zero(s_output_buffer, sizeof(s_output_buffer));
  atomic_store_explicit(&s_initialized, true, memory_order_release);
  (void)xSemaphoreGive(s_state_lock);
  return ESP_OK;
}

esp_err_t at_server_start(void) {
  if (!atomic_load_explicit(&s_initialized, memory_order_acquire)) {
    return ESP_ERR_INVALID_STATE;
  }
  if (s_state_lock == NULL ||
      xSemaphoreTake(s_state_lock, portMAX_DELAY) != pdTRUE) {
    return ESP_ERR_INVALID_STATE;
  }
  if (atomic_load_explicit(&s_running, memory_order_acquire)) {
    (void)xSemaphoreGive(s_state_lock);
    return ESP_OK;
  }
  if (task_handle() != NULL ||
      atomic_load_explicit(&s_transport_owned, memory_order_acquire)) {
    (void)xSemaphoreGive(s_state_lock);
    return ESP_ERR_INVALID_STATE;
  }
  (void)xSemaphoreTake(s_started, 0);
  (void)xSemaphoreTake(s_stopped, 0);
  discard_pending_urcs();
  atomic_store_explicit(&s_urc_dropped, 0, memory_order_release);
  s_start_result = ESP_ERR_INVALID_STATE;
  atomic_store_explicit(&s_running, true, memory_order_release);
  TaskHandle_t created_task = NULL;
  const BaseType_t created = xTaskCreatePinnedToCore(
      at_server_task, "at_server", s_config.task_stack_size, NULL,
      s_config.task_priority, &created_task, xPortGetCoreID());
  if (created != pdPASS) {
    atomic_store_explicit(&s_running, false, memory_order_release);
    wipe_output_buffer();
    (void)xSemaphoreGive(s_state_lock);
    return ESP_ERR_NO_MEM;
  }
  atomic_store_explicit(&s_task, (uintptr_t)created_task, memory_order_release);
  if (xSemaphoreTake(s_started, pdMS_TO_TICKS(AT_SERVER_START_TIMEOUT_MS)) !=
      pdTRUE) {
    atomic_store_explicit(&s_running, false, memory_order_release);
    (void)xSemaphoreGive(s_state_lock);
    return ESP_ERR_TIMEOUT;
  }
  esp_err_t err = s_start_result;
  if (err != ESP_OK) {
    atomic_store_explicit(&s_task, (uintptr_t)NULL, memory_order_release);
  }
  (void)xSemaphoreGive(s_state_lock);
  return err;
}

esp_err_t at_server_stop(void) {
  if (s_state_lock == NULL) {
    return !atomic_load_explicit(&s_running, memory_order_acquire) &&
                   task_handle() == NULL &&
                   !atomic_load_explicit(&s_transport_owned,
                                         memory_order_acquire)
               ? ESP_OK
               : ESP_ERR_INVALID_STATE;
  }
  if (xSemaphoreTake(s_state_lock, portMAX_DELAY) != pdTRUE) {
    return ESP_ERR_INVALID_STATE;
  }
  TaskHandle_t running_task = task_handle();
  if (!atomic_load_explicit(&s_running, memory_order_acquire) &&
      running_task == NULL &&
      !atomic_load_explicit(&s_transport_owned, memory_order_acquire)) {
    (void)xSemaphoreGive(s_state_lock);
    return ESP_OK;
  }
  if (xTaskGetCurrentTaskHandle() == running_task) {
    (void)xSemaphoreGive(s_state_lock);
    return ESP_ERR_INVALID_STATE;
  }

  atomic_store_explicit(&s_running, false, memory_order_release);
  if (running_task != NULL &&
      xSemaphoreTake(s_stopped, pdMS_TO_TICKS(AT_SERVER_STOP_TIMEOUT_MS)) !=
          pdTRUE) {
    (void)xSemaphoreGive(s_state_lock);
    return ESP_ERR_TIMEOUT;
  }
  wipe_output_buffer();
  (void)xSemaphoreGive(s_state_lock);
  return atomic_load_explicit(&s_transport_owned, memory_order_acquire)
             ? ESP_ERR_INVALID_STATE
             : ESP_OK;
}

bool at_server_running(void) {
  return atomic_load_explicit(&s_running, memory_order_acquire);
}

esp_err_t at_server_set_echo(bool enabled) {
  if (!atomic_load_explicit(&s_initialized, memory_order_acquire) ||
      s_state_lock == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  if (enabled) {
    return ESP_ERR_NOT_SUPPORTED;
  }
  s_echo = false;
  return ESP_OK;
}

bool at_server_echo_enabled(void) { return s_echo; }
