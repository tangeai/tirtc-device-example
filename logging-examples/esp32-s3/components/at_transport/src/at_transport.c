#include "at_transport.h"

#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "freertos/task.h"

#define AT_TRANSPORT_MIN_RX_BUFFER 256U
#define AT_TRANSPORT_WRITE_SLICE_MS 100U

esp_err_t at_transport_validate(const at_transport_config_t *config)
{
    if (config == NULL || config->rx_buffer_size < AT_TRANSPORT_MIN_RX_BUFFER) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->kind == AT_TRANSPORT_USB_SERIAL_JTAG) {
        return config->tx_buffer_size >= AT_TRANSPORT_MIN_RX_BUFFER
                   ? ESP_OK
                   : ESP_ERR_INVALID_ARG;
    }
    if (config->kind != AT_TRANSPORT_UART ||
        config->uart_num < UART_NUM_0 ||
        config->uart_num >= UART_NUM_MAX ||
        config->baud_rate <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t at_transport_open(at_transport_t *transport,
                            const at_transport_config_t *config)
{
    if (transport == NULL || at_transport_validate(config) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (transport->opened) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err;
    if (config->kind == AT_TRANSPORT_USB_SERIAL_JTAG) {
        usb_serial_jtag_driver_config_t usb_config = {
            .rx_buffer_size = config->rx_buffer_size,
            .tx_buffer_size = config->tx_buffer_size,
        };
        err = usb_serial_jtag_driver_install(&usb_config);
    } else {
        if (uart_is_driver_installed(config->uart_num)) {
            return ESP_ERR_INVALID_STATE;
        }
        const uart_config_t uart_config = {
            .baud_rate = config->baud_rate,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        err = uart_param_config(config->uart_num, &uart_config);
        if (err == ESP_OK) {
            err = uart_set_pin(config->uart_num,
                               config->tx_pin,
                               config->rx_pin,
                               config->rts_pin,
                               config->cts_pin);
        }
        if (err == ESP_OK) {
            err = uart_driver_install(config->uart_num,
                                      config->rx_buffer_size,
                                      0,
                                      0,
                                      NULL,
                                      0);
        }
    }
    if (err == ESP_OK) {
        transport->config = *config;
        transport->opened = true;
    }
    return err;
}

int at_transport_read(at_transport_t *transport,
                      void *buffer,
                      size_t length,
                      TickType_t timeout)
{
    if (transport == NULL || !transport->opened || buffer == NULL ||
        length == 0U || length > UINT32_MAX) {
        return -1;
    }
    if (transport->config.kind == AT_TRANSPORT_USB_SERIAL_JTAG) {
        return usb_serial_jtag_read_bytes(buffer, (uint32_t)length, timeout);
    }
    return uart_read_bytes(transport->config.uart_num,
                           buffer,
                           (uint32_t)length,
                           timeout);
}

int at_transport_write(at_transport_t *transport,
                       const void *buffer,
                       size_t length,
                       TickType_t timeout)
{
    if (transport == NULL || !transport->opened || buffer == NULL ||
        length == 0U) {
        return -1;
    }
    if (transport->config.kind == AT_TRANSPORT_UART) {
        return uart_write_bytes(transport->config.uart_num,
                                buffer,
                                length);
    }

    const uint8_t *cursor = (const uint8_t *)buffer;
    size_t remaining = length;
    TickType_t deadline = timeout == portMAX_DELAY
                              ? portMAX_DELAY
                              : xTaskGetTickCount() + timeout;
    while (remaining > 0U) {
        TickType_t wait = pdMS_TO_TICKS(AT_TRANSPORT_WRITE_SLICE_MS);
        if (deadline != portMAX_DELAY) {
            TickType_t now = xTaskGetTickCount();
            if ((int32_t)(deadline - now) <= 0) {
                break;
            }
            TickType_t left = deadline - now;
            if (wait > left) {
                wait = left;
            }
        }
        int written = usb_serial_jtag_write_bytes(cursor, remaining, wait);
        if (written < 0) {
            return written;
        }
        if (written == 0) {
            continue;
        }
        cursor += written;
        remaining -= (size_t)written;
    }
    return (int)(length - remaining);
}

esp_err_t at_transport_drain(at_transport_t *transport, TickType_t timeout)
{
    if (transport == NULL || !transport->opened) {
        return ESP_ERR_INVALID_STATE;
    }
    if (transport->config.kind == AT_TRANSPORT_USB_SERIAL_JTAG) {
        return usb_serial_jtag_wait_tx_done(timeout);
    }
    return uart_wait_tx_done(transport->config.uart_num, timeout);
}

esp_err_t at_transport_close(at_transport_t *transport)
{
    if (transport == NULL || !transport->opened) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err;
    if (transport->config.kind == AT_TRANSPORT_USB_SERIAL_JTAG) {
        err = usb_serial_jtag_driver_uninstall();
    } else {
        (void)uart_flush_input(transport->config.uart_num);
        err = uart_driver_delete(transport->config.uart_num);
    }
    if (err == ESP_OK) {
        memset(transport, 0, sizeof(*transport));
    }
    return err;
}
