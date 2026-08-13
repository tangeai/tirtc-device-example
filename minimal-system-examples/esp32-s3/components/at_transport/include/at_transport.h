#ifndef AT_TRANSPORT_H
#define AT_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AT_TRANSPORT_USB_SERIAL_JTAG = 0,
    AT_TRANSPORT_UART,
} at_transport_kind_t;

typedef struct {
    at_transport_kind_t kind;
    size_t rx_buffer_size;
    size_t tx_buffer_size;
    uart_port_t uart_num;
    int baud_rate;
    int tx_pin;
    int rx_pin;
    int rts_pin;
    int cts_pin;
} at_transport_config_t;

typedef struct {
    at_transport_config_t config;
    bool opened;
} at_transport_t;

#define AT_TRANSPORT_USB_SERIAL_JTAG_DEFAULT() \
    {                                         \
        .kind = AT_TRANSPORT_USB_SERIAL_JTAG, \
        .rx_buffer_size = 1024,               \
        .tx_buffer_size = 4096,               \
        .uart_num = UART_NUM_0,               \
        .baud_rate = 115200,                  \
        .tx_pin = UART_PIN_NO_CHANGE,         \
        .rx_pin = UART_PIN_NO_CHANGE,         \
        .rts_pin = UART_PIN_NO_CHANGE,        \
        .cts_pin = UART_PIN_NO_CHANGE,        \
    }

#define AT_TRANSPORT_UART_DEFAULT()            \
    {                                          \
        .kind = AT_TRANSPORT_UART,             \
        .rx_buffer_size = 1024,                \
        .tx_buffer_size = 0,                   \
        .uart_num = UART_NUM_0,                \
        .baud_rate = 115200,                   \
        .tx_pin = UART_PIN_NO_CHANGE,          \
        .rx_pin = UART_PIN_NO_CHANGE,          \
        .rts_pin = UART_PIN_NO_CHANGE,         \
        .cts_pin = UART_PIN_NO_CHANGE,         \
    }

esp_err_t at_transport_validate(const at_transport_config_t *config);
esp_err_t at_transport_open(at_transport_t *transport,
                            const at_transport_config_t *config);
int at_transport_read(at_transport_t *transport,
                      void *buffer,
                      size_t length,
                      TickType_t timeout);
int at_transport_write(at_transport_t *transport,
                       const void *buffer,
                       size_t length,
                       TickType_t timeout);
esp_err_t at_transport_drain(at_transport_t *transport, TickType_t timeout);
esp_err_t at_transport_close(at_transport_t *transport);

#ifdef __cplusplus
}
#endif

#endif
