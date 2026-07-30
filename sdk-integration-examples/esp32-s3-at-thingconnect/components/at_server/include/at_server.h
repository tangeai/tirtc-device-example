#ifndef AT_SERVER_H
#define AT_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "at_transport.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AT_SERVER_DEFAULT_BAUD_RATE 115200
#define AT_SERVER_DEFAULT_RX_BUFFER_SIZE 1024
#define AT_SERVER_DEFAULT_TASK_STACK_SIZE 5120
#define AT_SERVER_DEFAULT_TASK_PRIORITY 5
#define AT_SERVER_LINE_MAX 512
#define AT_SERVER_COMMAND_NAME_MAX 31
#define AT_SERVER_OUTPUT_MAX 4096

typedef enum {
    AT_SERVER_OP_EXECUTE = 0,
    AT_SERVER_OP_READ,
    AT_SERVER_OP_TEST,
    AT_SERVER_OP_SET,
} at_server_operation_t;

/*
 * The request and its strings are valid only for the duration of the handler.
 * SET arguments are the unparsed bytes after '='; business code owns quoting
 * and field parsing because those rules are command-specific.
 */
typedef struct {
    at_server_operation_t operation;
    const char *name;
    const char *arguments;
    size_t arguments_length;
} at_server_request_t;

typedef esp_err_t (*at_server_command_handler_t)(
    const at_server_request_t *request,
    void *context);

/*
 * Command names omit the "AT+" prefix and are matched case-insensitively.
 * The table and every name string must remain valid until at_server_stop().
 */
typedef struct {
    const char *name;
    at_server_command_handler_t handler;
    void *context;
} at_server_command_t;

typedef struct {
    at_transport_config_t transport;
    uint32_t task_stack_size;
    UBaseType_t task_priority;
    /* Must remain false: echo can expose credentials carried by AT commands. */
    bool echo;
    const at_server_command_t *commands;
    size_t command_count;
} at_server_config_t;

#define AT_SERVER_CONFIG_DEFAULT(command_table, command_table_count) \
    {                                                               \
        .transport = AT_TRANSPORT_USB_SERIAL_JTAG_DEFAULT(),        \
        .task_stack_size = AT_SERVER_DEFAULT_TASK_STACK_SIZE,       \
        .task_priority = AT_SERVER_DEFAULT_TASK_PRIORITY,           \
        .echo = false,                                              \
        .commands = (command_table),                                \
        .command_count = (command_table_count),                     \
    }

esp_err_t at_server_init(const at_server_config_t *config);
esp_err_t at_server_start(void);
esp_err_t at_server_stop(void);
bool at_server_running(void);

esp_err_t at_server_set_echo(bool enabled);
bool at_server_echo_enabled(void);

/*
 * Command handlers run on the AT task and may emit synchronous response lines.
 * Other tasks enqueue URCs; the AT task flushes that queue only between
 * commands, so a URC cannot split a response and its final OK/ERROR. Payloads
 * must be one logical line; embedded CR/LF is rejected. Every command has
 * exactly one automatic final line: OK or ERROR:<signed-decimal esp_err_t>.
 */
esp_err_t at_server_response(const char *format, ...)
    __attribute__((format(printf, 1, 2)));
esp_err_t at_server_urc(const char *format, ...)
    __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif
