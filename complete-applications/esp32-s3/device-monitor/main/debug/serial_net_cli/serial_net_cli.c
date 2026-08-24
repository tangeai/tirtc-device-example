#include "serial_net_cli.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "audio_alaw_codec.h"
#include "audio_device.h"
#include "audio_echo_cancel.h"
#include "device_binding.h"
#include "device_call.h"
#include "device_online.h"
#include "display.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "media_sink.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/inet.h"
#include "lwip/priv/sockets_priv.h"
#include "mbedtls/base64.h"
#include "network.h"
#include "rtc_media_bridge.h"
#include "sender_test.h"
#include "tirtc_session.h"
#include "sdkconfig.h"
#include "system_time.h"
#include "thing_service_registry.h"

static const char *TAG = "serial_net_cli";

#define SERIAL_NET_CLI_LINE_MAX              256U
#define SERIAL_NET_CLI_UART_RX_BUFFER_SIZE   256U
#define SERIAL_NET_CLI_USB_BUFFER_SIZE       128U
#define SERIAL_NET_CLI_IO_CHUNK_SIZE         64U
#define SERIAL_NET_CLI_TASK_STACK_SIZE       (12U * 1024U)
#define SERIAL_NET_CLI_TASK_PRIORITY         2U
#define SERIAL_NET_CLI_TASK_CORE             1
#define SERIAL_NET_CLI_POLL_MS               50U
#define SERIAL_NET_CLI_CONNECT_TIMEOUT_MS    240000U
#define SERIAL_NET_CLI_SCAN_TIMEOUT_MS       30000U
#define SERIAL_NET_CLI_PROBE_TIMEOUT_MS      15000U
/* Base64 plus the response prefix must fit SERIAL_NET_CLI_LINE_MAX. */
#define SERIAL_NET_CLI_AEC_DUMP_RAW_BYTES       150U
#define SERIAL_NET_CLI_ALAW_PCM_BYTES            640U
#define SERIAL_NET_CLI_ALAW_BYTES                160U
#define SERIAL_NET_CLI_ALAW_TEST_MAX_ITERATIONS  20000U

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
#define SERIAL_NET_CLI_USE_USB_SERIAL_JTAG   1
#elif CONFIG_ESP_CONSOLE_UART
#define SERIAL_NET_CLI_USE_USB_SERIAL_JTAG   0
#else
#define SERIAL_NET_CLI_USE_USB_SERIAL_JTAG   0
#endif

typedef struct {
    uint32_t seq;
    uint32_t started_ms;
} serial_net_cli_operation_t;

typedef struct {
    serial_net_cli_operation_t connect;
    serial_net_cli_operation_t scan;
    serial_net_cli_operation_t probe;
    uint32_t scan_baseline_ms;
    bool probe_degraded;
    bool probe_dns_done;
    bool probe_dns_reported;
    bool probe_dns_ready;
    int probe_dns_ret;
    uint32_t probe_dns_started_ms;
    uint32_t probe_dns_elapsed_ms;
    char probe_dns_host[96];
    bool probe_ping_reported;
} serial_net_cli_operations_t;

static EXT_RAM_BSS_ATTR TaskHandle_t s_serial_net_cli_task;
static EXT_RAM_BSS_ATTR uint32_t s_next_seq;
static EXT_RAM_BSS_ATTR bool s_trace_disabled;
/* Diagnostic state is cold-path data; keep internal RAM for Wi-Fi/RTC control. */
static EXT_RAM_BSS_ATTR serial_net_cli_operations_t s_operations;
static EXT_RAM_BSS_ATTR network_state_t s_last_network_state;
static EXT_RAM_BSS_ATTR bool s_last_network_state_valid;
static EXT_RAM_BSS_ATTR volatile bool s_aec_dump_observer_registered;
static portMUX_TYPE s_serial_net_cli_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t serial_net_cli_uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static const char *serial_net_cli_transport_name(void)
{
#if SERIAL_NET_CLI_USE_USB_SERIAL_JTAG
    return "usb_serial_jtag";
#else
    return "uart";
#endif
}

static int serial_net_cli_read(void *buffer, size_t size, TickType_t ticks_to_wait)
{
#if SERIAL_NET_CLI_USE_USB_SERIAL_JTAG
    return usb_serial_jtag_read_bytes(buffer, size, ticks_to_wait);
#else
    return uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, buffer, size, ticks_to_wait);
#endif
}

static void serial_net_cli_write(const void *buffer, size_t size)
{
    const uint8_t *cursor = (const uint8_t *)buffer;

    while (size > 0U) {
        size_t chunk_size = size > SERIAL_NET_CLI_IO_CHUNK_SIZE ?
                            SERIAL_NET_CLI_IO_CHUNK_SIZE :
                            size;
#if SERIAL_NET_CLI_USE_USB_SERIAL_JTAG
        int written = usb_serial_jtag_write_bytes(cursor,
                                                   chunk_size,
                                                   pdMS_TO_TICKS(100));
#else
        int written = uart_write_bytes(CONFIG_ESP_CONSOLE_UART_NUM,
                                       cursor,
                                       chunk_size);
#endif
        if (written <= 0) {
            return;
        }
        cursor += (size_t)written;
        size -= (size_t)written;
    }
}

static const char *serial_net_cli_phase_name(network_connection_phase_t phase)
{
    switch (phase) {
    case NETWORK_CONNECTION_PHASE_NO_CREDENTIALS:
        return "no_credentials";
    case NETWORK_CONNECTION_PHASE_IDLE:
        return "idle";
    case NETWORK_CONNECTION_PHASE_ASSOCIATING:
        return "associating";
    case NETWORK_CONNECTION_PHASE_WAITING_IP:
        return "waiting_ip";
    case NETWORK_CONNECTION_PHASE_IP_READY:
        return "ip_ready";
    case NETWORK_CONNECTION_PHASE_RETRY_WAIT:
        return "retry_wait";
    case NETWORK_CONNECTION_PHASE_FAILED:
        return "failed";
    case NETWORK_CONNECTION_PHASE_STOPPED:
    default:
        return "stopped";
    }
}

static const char *serial_net_cli_failure_name(network_connect_failure_t failure)
{
    switch (failure) {
    case NETWORK_CONNECT_FAILURE_TIMEOUT:
        return "timeout";
    case NETWORK_CONNECT_FAILURE_AP_NOT_FOUND:
        return "ap_not_found";
    case NETWORK_CONNECT_FAILURE_AUTHENTICATION:
        return "authentication";
    case NETWORK_CONNECT_FAILURE_ASSOCIATION:
        return "association";
    case NETWORK_CONNECT_FAILURE_OTHER:
        return "other";
    case NETWORK_CONNECT_FAILURE_NONE:
    default:
        return "none";
    }
}

static void serial_net_cli_writef(const char *format, ...)
{
    char buffer[514];
    va_list args;

    va_start(args, format);
    int length = vsnprintf(buffer, sizeof(buffer) - 2U, format, args);
    va_end(args);
    if (length < 0) {
        return;
    }

    size_t write_len = (size_t)length;
    if (write_len >= sizeof(buffer) - 2U) {
        write_len = sizeof(buffer) - 3U;
    }
    buffer[write_len++] = '\r';
    buffer[write_len++] = '\n';
    serial_net_cli_write(buffer, write_len);
}

static void serial_net_cli_escape_field(const char *input, char *output, size_t output_size)
{
    size_t write_index = 0;

    if (output == NULL || output_size == 0U) {
        return;
    }
    if (input == NULL) {
        output[0] = '\0';
        return;
    }

    for (size_t index = 0; input[index] != '\0' && write_index + 1U < output_size; ++index) {
        const unsigned char value = (unsigned char)input[index];
        if ((value == '"' || value == '\\') && write_index + 2U < output_size) {
            output[write_index++] = '\\';
            output[write_index++] = (char)value;
        } else if (value < 0x20U || value == 0x7FU) {
            output[write_index++] = '?';
        } else {
            output[write_index++] = (char)value;
        }
    }
    output[write_index] = '\0';
}

static uint32_t serial_net_cli_next_seq(void)
{
    s_next_seq++;
    if (s_next_seq == 0U) {
        s_next_seq = 1U;
    }
    return s_next_seq;
}

static bool serial_net_cli_network_state_changed(const network_state_t *current,
                                                 const network_state_t *previous)
{
    return current->configured != previous->configured ||
           current->started != previous->started ||
           current->associated != previous->associated ||
           current->connected != previous->connected ||
           current->phase != previous->phase ||
           current->attempt_id != previous->attempt_id ||
           current->retry_count != previous->retry_count ||
           current->disconnect_reason != previous->disconnect_reason ||
           current->connect_failure != previous->connect_failure ||
           strcmp(current->ssid, previous->ssid) != 0 ||
           strcmp(current->ip_addr, previous->ip_addr) != 0;
}

static void serial_net_cli_emit_network_state(uint32_t seq,
                                              const char *stage,
                                              const network_state_t *state)
{
    char escaped_ssid[72] = {0};

    serial_net_cli_escape_field(state->ssid, escaped_ssid, sizeof(escaped_ssid));
    serial_net_cli_writef(
        "+NETEVT:seq=%u,stage=%s,phase=%s,configured=%u,associated=%u,connected=%u,"
        "attempt=%u,retry=%u,phase_ms=%u,association_ms=%u,dhcp_ms=%u,rssi=%d,"
        "reason=%u,failure=%s,ssid=\"%s\",ip=%s",
        (unsigned)seq,
        stage != NULL ? stage : "state",
        serial_net_cli_phase_name(state->phase),
        state->configured ? 1U : 0U,
        state->associated ? 1U : 0U,
        state->connected ? 1U : 0U,
        (unsigned)state->attempt_id,
        (unsigned)state->retry_count,
        (unsigned)state->phase_elapsed_ms,
        (unsigned)state->association_time_ms,
        (unsigned)state->dhcp_time_ms,
        (int)state->rssi,
        (unsigned)state->disconnect_reason,
        serial_net_cli_failure_name(state->connect_failure),
        escaped_ssid,
        state->ip_addr[0] != '\0' ? state->ip_addr : "0.0.0.0");
}

static bool serial_net_cli_parse_quoted(const char **cursor, char *output, size_t output_size)
{
    const char *read = *cursor;
    size_t write_index = 0;
    bool closed = false;

    while (*read == ' ' || *read == '\t') {
        read++;
    }
    if (*read != '"') {
        return false;
    }
    read++;

    while (*read != '\0') {
        char value = *read++;
        if (value == '"') {
            closed = true;
            break;
        }
        if (value == '\\' && (*read == '\\' || *read == '"')) {
            value = *read++;
        }
        if (write_index + 1U >= output_size) {
            return false;
        }
        output[write_index++] = value;
    }
    if (!closed) {
        return false;
    }
    output[write_index] = '\0';
    *cursor = read;
    return true;
}

static bool serial_net_cli_parse_wifi_connect(const char *arguments,
                                              char *ssid,
                                              size_t ssid_size,
                                              char *password,
                                              size_t password_size)
{
    const char *cursor = arguments;

    if (!serial_net_cli_parse_quoted(&cursor, ssid, ssid_size)) {
        return false;
    }
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    if (*cursor++ != ',') {
        return false;
    }
    if (!serial_net_cli_parse_quoted(&cursor, password, password_size)) {
        return false;
    }
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    return *cursor == '\0' && ssid[0] != '\0';
}

static bool serial_net_cli_extract_host(const char *url, char *host, size_t host_size)
{
    const char *start = url;
    size_t length = 0;

    if (url == NULL || url[0] == '\0' || host == NULL || host_size < 2U) {
        return false;
    }
    const char *scheme = strstr(url, "://");
    if (scheme != NULL) {
        start = scheme + 3;
    }
    while (start[length] != '\0' && start[length] != '/' && start[length] != ':' &&
           length + 1U < host_size) {
        length++;
    }
    if (length == 0U) {
        return false;
    }
    memcpy(host, start, length);
    host[length] = '\0';
    return true;
}

static void serial_net_cli_dns_result(const char *name,
                                      const ip_addr_t *resolved_address,
                                      void *ctx)
{
    const uint32_t seq = (uint32_t)(uintptr_t)ctx;
    const uint32_t now_ms = serial_net_cli_uptime_ms();

    taskENTER_CRITICAL(&s_serial_net_cli_lock);
    if (seq == s_operations.probe.seq &&
        name != NULL &&
        strcmp(name, s_operations.probe_dns_host) == 0) {
        s_operations.probe_dns_done = true;
        s_operations.probe_dns_ready = resolved_address != NULL && !ip_addr_isany(resolved_address);
        s_operations.probe_dns_ret = s_operations.probe_dns_ready ? ERR_OK : ERR_VAL;
        s_operations.probe_dns_elapsed_ms =
            (uint32_t)(now_ms - s_operations.probe_dns_started_ms);
    }
    taskEXIT_CRITICAL(&s_serial_net_cli_lock);
}

static void serial_net_cli_print_wifi(void)
{
    network_state_t state = {0};
    char saved_ssid[33] = {0};
    char escaped_saved_ssid[72] = {0};

    network_get_state(&state);
    network_get_saved_config(saved_ssid, sizeof(saved_ssid), NULL, 0);
    serial_net_cli_escape_field(saved_ssid, escaped_saved_ssid, sizeof(escaped_saved_ssid));
    serial_net_cli_emit_network_state(0, "query", &state);
    serial_net_cli_writef("+WIFI:saved=%u,saved_ssid=\"%s\"",
                          saved_ssid[0] != '\0' ? 1U : 0U,
                          escaped_saved_ssid);
    serial_net_cli_writef("OK");
}

static void serial_net_cli_print_net(void)
{
    network_state_t state = {0};
    device_binding_snapshot_t binding = {0};
    device_online_snapshot_t online = {0};

    network_get_state(&state);
    device_binding_get_snapshot(&binding);
    device_online_get_snapshot(&online);
    serial_net_cli_emit_network_state(0, "query", &state);
    serial_net_cli_writef(
        "+NETREADY:time=%u,services_ready=%u,services_discovered=%u,binding_state=%d,"
        "binding_running=%u,online_state=%d,mqtt=%u",
        system_time_has_valid_time() ? 1U : 0U,
        thing_service_registry_is_ready() ? 1U : 0U,
        thing_service_registry_is_discovered() ? 1U : 0U,
        (int)binding.state,
        binding.running ? 1U : 0U,
        (int)online.state,
        online.mqtt_connected ? 1U : 0U);
    serial_net_cli_writef("OK");
}

static void serial_net_cli_start_scan(void)
{
    network_scan_snapshot_t scan = {0};
    const uint32_t seq = serial_net_cli_next_seq();

    network_get_scan_results(&scan);
    s_operations.scan = (serial_net_cli_operation_t){
        .seq = seq,
        .started_ms = serial_net_cli_uptime_ms(),
    };
    s_operations.scan_baseline_ms = scan.last_scan_ms;
    serial_net_cli_writef("OK,QUEUED,%u", (unsigned)seq);
    esp_err_t ret = network_request_scan();
    if (ret != ESP_OK) {
        s_operations.scan.seq = 0;
        serial_net_cli_writef("+NETEVT:seq=%u,stage=scan,state=failed,err=%s",
                              (unsigned)seq,
                              esp_err_to_name(ret));
    }
}

static void serial_net_cli_start_connect(const char *arguments)
{
    char ssid[33] = {0};
    char password[NETWORK_PASSWORD_MAX_LEN + 1] = {0};

    if (!serial_net_cli_parse_wifi_connect(arguments,
                                           ssid,
                                           sizeof(ssid),
                                           password,
                                           sizeof(password))) {
        serial_net_cli_writef("ERROR,INVALID_ARGUMENT,WIFICONN");
        memset(password, 0, sizeof(password));
        return;
    }

    const uint32_t seq = serial_net_cli_next_seq();
    s_operations.connect = (serial_net_cli_operation_t){
        .seq = seq,
        .started_ms = serial_net_cli_uptime_ms(),
    };
    serial_net_cli_writef("OK,QUEUED,%u", (unsigned)seq);
    esp_err_t ret = network_connect(ssid, password);
    memset(password, 0, sizeof(password));
    if (ret != ESP_OK) {
        s_operations.connect.seq = 0;
        serial_net_cli_writef("+NETEVT:seq=%u,stage=connect,state=failed,err=%s",
                              (unsigned)seq,
                              esp_err_to_name(ret));
    }
}

static void serial_net_cli_retry_connect(void)
{
    const uint32_t seq = serial_net_cli_next_seq();
    network_state_t state = {0};

    network_get_state(&state);
    if (state.connected) {
        /* A retry command is idempotent once the station has an IP. Avoid
         * republishing an unchanged connected state, which would otherwise
         * retrigger service bootstrap callbacks above the network layer. */
        serial_net_cli_writef("OK,QUEUED,%u", (unsigned)seq);
        serial_net_cli_emit_network_state(seq, "connect_done", &state);
        return;
    }

    s_operations.connect = (serial_net_cli_operation_t){
        .seq = seq,
        .started_ms = serial_net_cli_uptime_ms(),
    };
    serial_net_cli_writef("OK,QUEUED,%u", (unsigned)seq);
    esp_err_t ret = network_retry_connection();
    if (ret != ESP_OK) {
        s_operations.connect.seq = 0;
        serial_net_cli_writef("+NETEVT:seq=%u,stage=retry,state=failed,err=%s",
                              (unsigned)seq,
                              esp_err_to_name(ret));
    }
}

static void serial_net_cli_start_probe(void)
{
    network_state_t state = {0};
    char host[sizeof(s_operations.probe_dns_host)] = {0};
    ip_addr_t resolved_address = {0};
    const uint32_t seq = serial_net_cli_next_seq();

    if (s_operations.probe.seq != 0U) {
        serial_net_cli_writef("ERROR,BUSY,NETPROBE");
        return;
    }
    const uint32_t started_ms = serial_net_cli_uptime_ms();
    taskENTER_CRITICAL(&s_serial_net_cli_lock);
    s_operations.probe = (serial_net_cli_operation_t){
        .seq = seq,
        .started_ms = started_ms,
    };
    s_operations.probe_degraded = false;
    s_operations.probe_dns_done = false;
    s_operations.probe_dns_reported = false;
    s_operations.probe_dns_ready = false;
    s_operations.probe_dns_ret = ERR_INPROGRESS;
    s_operations.probe_dns_started_ms = started_ms;
    s_operations.probe_dns_elapsed_ms = 0;
    s_operations.probe_dns_host[0] = '\0';
    s_operations.probe_ping_reported = false;
    taskEXIT_CRITICAL(&s_serial_net_cli_lock);
    serial_net_cli_writef("OK,QUEUED,%u", (unsigned)seq);

    network_get_state(&state);
    serial_net_cli_emit_network_state(seq, "wifi", &state);
    if (!state.connected) {
        serial_net_cli_writef("+NETEVT:seq=%u,stage=probe,state=failed,reason=no_ip",
                              (unsigned)seq);
        taskENTER_CRITICAL(&s_serial_net_cli_lock);
        s_operations.probe.seq = 0;
        taskEXIT_CRITICAL(&s_serial_net_cli_lock);
        return;
    }

    const char *discovery_url = thing_service_registry_discovery_url();
    if (!serial_net_cli_extract_host(discovery_url, host, sizeof(host))) {
        taskENTER_CRITICAL(&s_serial_net_cli_lock);
        s_operations.probe_dns_done = true;
        s_operations.probe_dns_ret = ERR_ARG;
        s_operations.probe_degraded = true;
        taskEXIT_CRITICAL(&s_serial_net_cli_lock);
    } else {
        taskENTER_CRITICAL(&s_serial_net_cli_lock);
        strlcpy(s_operations.probe_dns_host,
                host,
                sizeof(s_operations.probe_dns_host));
        taskEXIT_CRITICAL(&s_serial_net_cli_lock);

        err_t dns_ret = dns_gethostbyname(host,
                                          &resolved_address,
                                          serial_net_cli_dns_result,
                                          (void *)(uintptr_t)seq);
        if (dns_ret != ERR_INPROGRESS) {
            taskENTER_CRITICAL(&s_serial_net_cli_lock);
            s_operations.probe_dns_done = true;
            s_operations.probe_dns_ready = dns_ret == ERR_OK && !ip_addr_isany(&resolved_address);
            s_operations.probe_dns_ret = dns_ret;
            s_operations.probe_dns_elapsed_ms = serial_net_cli_uptime_ms() - started_ms;
            if (!s_operations.probe_dns_ready) {
                s_operations.probe_degraded = true;
            }
            taskEXIT_CRITICAL(&s_serial_net_cli_lock);
        }
    }
    const bool time_ready = system_time_has_valid_time();
    const bool services_ready = thing_service_registry_is_ready();
    const bool services_discovered = thing_service_registry_is_discovered();
    serial_net_cli_writef("+NETEVT:seq=%u,stage=time,state=%s",
                          (unsigned)seq,
                          time_ready ? "ready" : "pending");
    serial_net_cli_writef("+NETEVT:seq=%u,stage=services,state=%s,discovered=%u",
                          (unsigned)seq,
                          services_ready ? "ready" : "pending",
                          services_discovered ? 1U : 0U);
    if (!time_ready || !services_ready) {
        s_operations.probe_degraded = true;
    }

    esp_err_t ping_ret = network_start_ping("223.5.5.5");
    if (ping_ret != ESP_OK) {
        serial_net_cli_writef("+NETEVT:seq=%u,stage=ping,state=failed,err=%s",
                              (unsigned)seq,
                              esp_err_to_name(ping_ret));
        taskENTER_CRITICAL(&s_serial_net_cli_lock);
        s_operations.probe_ping_reported = true;
        s_operations.probe_degraded = true;
        taskEXIT_CRITICAL(&s_serial_net_cli_lock);
    } else {
        serial_net_cli_writef("+NETEVT:seq=%u,stage=ping,state=running,target=223.5.5.5",
                              (unsigned)seq);
    }
}
static const char *serial_net_cli_media_profile_name(media_sink_audio_profile_t profile)
{
    switch (profile) {
    case MEDIA_SINK_AUDIO_PROFILE_DEVICE_CALL:
        return "device_call";
    case MEDIA_SINK_AUDIO_PROFILE_JITTER_SAFE:
        return "jitter_safe";
    case MEDIA_SINK_AUDIO_PROFILE_IPC_TALKBACK:
        return "ipc_talkback";
    case MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY:
    default:
        return "low_latency";
    }
}

static void serial_net_cli_print_media(void)
{
    media_sink_audio_diagnostics_t media = {0};
    tirtc_session_stats_t rtc = {0};
    size_t send_buffer_used = 0;
    esp_err_t send_buffer_ret = ESP_ERR_INVALID_STATE;

    if (!media_sink_get_audio_diagnostics(&media)) {
        serial_net_cli_writef("+MEDIA:ready=0");
        serial_net_cli_writef("OK");
        return;
    }

    tirtc_session_get_stats(&rtc);
    send_buffer_ret = tirtc_session_get_active_send_buffer_used(&send_buffer_used);
    serial_net_cli_writef(
        "+MEDIA:ready=1,profile=%s,packet_ms=%u,prebuffer_ms=%u,target_ms=%u,"
        "buffered_ms=%u,queued=%u,playback=%u,talkspurt=%u,first_play_ms=%u,"
        "jitter_ms=%u/%u/%u,max_gap_ms=%u,rx=%u/%u,played=%u/%u,"
        "drop=%u/%u/%u,underflow=%u/%u,grace=%u/%u,delayed=%u,"
        "source_late=%u/%u/%u,source_clock=%d,gap_pending=%d/%u,gap_fill=%u/%u,conceal=%u/%u,clock_recovery=%u/%u,"
        "sdk_send=%u/%s,rtc_rx=%u",
        serial_net_cli_media_profile_name(media.profile),
        (unsigned)media.source_packet_ms,
        (unsigned)media.prebuffer_ms,
        (unsigned)media.target_ms,
        (unsigned)media.buffered_ms,
        (unsigned)media.queued_packets,
        media.playback_active ? 1U : 0U,
        media.talkspurt_active ? 1U : 0U,
        (unsigned)media.first_play_delay_ms,
        (unsigned)media.jitter_ewma_ms,
        (unsigned)media.jitter_peak_ms,
        (unsigned)media.jitter_boost_ms,
        (unsigned)media.max_arrival_gap_ms,
        (unsigned)media.rx_packets,
        (unsigned)media.rx_ms,
        (unsigned)media.played_packets,
        (unsigned)media.played_ms,
        (unsigned)media.play_drop_packets,
        (unsigned)media.queue_drop_packets,
        (unsigned)media.trim_drop_packets,
        (unsigned)media.underflow_events,
        (unsigned)media.active_underflow_events,
        (unsigned)media.underflow_grace_waits,
        (unsigned)media.underflow_grace_recoveries,
        (unsigned)media.delayed_burst_events,
        (unsigned)media.source_late_events,
        (unsigned)media.source_late_ms,
        (unsigned)media.max_source_late_ms,
        (int)media.source_clock_error_ms,
        (int)media.source_gap_pending_ms,
        (unsigned)media.source_gap_pending_packets,
        (unsigned)media.source_gap_fill_events,
        (unsigned)media.source_gap_fill_ms,
        (unsigned)media.concealment_events,
        (unsigned)media.concealed_ms,
        (unsigned)media.clock_recovery_events,
        (unsigned)media.clock_recovery_frames,
        (unsigned)send_buffer_used,
        esp_err_to_name(send_buffer_ret),
        (unsigned)rtc.rx_audio_frames);
    serial_net_cli_writef(
        "+MEDIATX:queue=%u,high=%u,pressure_drop=%u,stale_drop=%u,generation_drop=%u,"
        "lock_fail=%u,lock_us=%u/%u,sdk_us=%u/%u,rtc_tx=%u,fail=%u",
        (unsigned)rtc.tx_audio_queue_depth_packets,
        (unsigned)rtc.tx_audio_queue_high_water_packets,
        (unsigned)rtc.tx_audio_queue_pressure_drops,
        (unsigned)rtc.tx_audio_queue_stale_drops,
        (unsigned)rtc.tx_audio_queue_generation_drops,
        (unsigned)rtc.tx_audio_sdk_lock_failures,
        (unsigned)rtc.tx_audio_sdk_lock_wait_last_us,
        (unsigned)rtc.tx_audio_sdk_lock_wait_max_us,
        (unsigned)rtc.tx_audio_sdk_send_last_us,
        (unsigned)rtc.tx_audio_sdk_send_max_us,
        (unsigned)rtc.tx_audio_frames,
        (unsigned)rtc.tx_failures);
    serial_net_cli_writef("OK");
}

static void serial_net_cli_format_socket_address(const struct sockaddr_storage *address,
                                                 char *output,
                                                 size_t output_size)
{
    if (address == NULL || output == NULL || output_size == 0U) {
        return;
    }

    output[0] = '\0';
    if (address->ss_family == AF_INET) {
        const struct sockaddr_in *address_v4 = (const struct sockaddr_in *)address;
        char ip[INET_ADDRSTRLEN] = {0};

        if (inet_ntoa_r(address_v4->sin_addr, ip, sizeof(ip)) != NULL) {
            snprintf(output, output_size, "%s:%u", ip, (unsigned)ntohs(address_v4->sin_port));
        }
    }
    if (output[0] == '\0') {
        snprintf(output, output_size, "-");
    }
}

static void serial_net_cli_print_sockets(void)
{
    unsigned sockets_used = 0;

    for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + MEMP_NUM_NETCONN; ++fd) {
        const struct lwip_sock *sock = lwip_socket_dbg_get_socket(fd);
        struct sockaddr_storage local_address = {0};
        struct sockaddr_storage peer_address = {0};
        socklen_t local_length = sizeof(local_address);
        socklen_t peer_length = sizeof(peer_address);
        socklen_t type_length = sizeof(int);
        char local[32] = "-";
        char peer[32] = "-";
        int socket_type = 0;

        if (sock == NULL || sock->conn == NULL) {
            continue;
        }
        sockets_used++;
        if (lwip_getsockname(fd, (struct sockaddr *)&local_address, &local_length) == 0) {
            serial_net_cli_format_socket_address(&local_address, local, sizeof(local));
        }
        if (lwip_getpeername(fd, (struct sockaddr *)&peer_address, &peer_length) == 0) {
            serial_net_cli_format_socket_address(&peer_address, peer, sizeof(peer));
        }
        if (lwip_getsockopt(fd, SOL_SOCKET, SO_TYPE, &socket_type, &type_length) != 0) {
            socket_type = 0;
        }
        serial_net_cli_writef("+SOCKET:fd=%d,type=%s,local=%s,peer=%s",
                              fd,
                              socket_type == SOCK_STREAM ? "tcp" :
                              socket_type == SOCK_DGRAM ? "udp" : "other",
                              local,
                              peer);
    }
    serial_net_cli_writef("+SOCKETS:used=%u,total=%u", sockets_used, (unsigned)MEMP_NUM_NETCONN);
    serial_net_cli_writef("OK");
}


static void serial_net_cli_set_rtc_log_level(const char *argument)
{
    char *end = NULL;
    long level = 0;

    if (argument == NULL || argument[0] == '\0') {
        serial_net_cli_writef("ERROR,INVALID_ARGUMENT,RTCLOG");
        return;
    }

    level = strtol(argument, &end, 10);
    if (end == argument || *end != '\0' || level < 0 || level > 15) {
        serial_net_cli_writef("ERROR,INVALID_ARGUMENT,RTCLOG");
        return;
    }

    esp_err_t ret = tirtc_session_set_sdk_log_level((int)level);
    serial_net_cli_writef("+RTCLOG:level=%ld,ret=%s", level, esp_err_to_name(ret));
    if (ret == ESP_OK) {
        serial_net_cli_writef("OK");
    }
}

static void serial_net_cli_print_rtc_log_level(void)
{
    serial_net_cli_writef("+RTCLOG:level=%d", tirtc_session_get_sdk_log_level());
    serial_net_cli_writef("OK");
}

static const char *serial_net_cli_rtc_link_mode_name(tirtc_session_link_mode_t mode)
{
    switch (mode) {
    case TIRTC_SESSION_LINK_MODE_DIRECT_ONLY:
        return "DIRECT";
    case TIRTC_SESSION_LINK_MODE_RELAY_ONLY:
        return "RELAY";
    case TIRTC_SESSION_LINK_MODE_DEFAULT:
    default:
        return "DEFAULT";
    }
}

static void serial_net_cli_set_rtc_link_mode(const char *argument)
{
    tirtc_session_link_mode_t mode = TIRTC_SESSION_LINK_MODE_DEFAULT;

    if (argument == NULL) {
        serial_net_cli_writef("ERROR,INVALID_ARGUMENT,RTCLINK");
        return;
    }
    if (strcasecmp(argument, "DEFAULT") == 0) {
        mode = TIRTC_SESSION_LINK_MODE_DEFAULT;
    } else if (strcasecmp(argument, "DIRECT") == 0) {
        mode = TIRTC_SESSION_LINK_MODE_DIRECT_ONLY;
    } else if (strcasecmp(argument, "RELAY") == 0) {
        mode = TIRTC_SESSION_LINK_MODE_RELAY_ONLY;
    } else {
        serial_net_cli_writef("ERROR,INVALID_ARGUMENT,RTCLINK");
        return;
    }

    esp_err_t ret = tirtc_session_set_link_mode(mode);
    serial_net_cli_writef("+RTCLINK:mode=%s,ret=%s",
                          serial_net_cli_rtc_link_mode_name(mode),
                          esp_err_to_name(ret));
    if (ret == ESP_OK) {
        serial_net_cli_writef("OK");
    }
}

static void serial_net_cli_print_rtc_link_mode(void)
{
    serial_net_cli_writef("+RTCLINK:mode=%s",
                          serial_net_cli_rtc_link_mode_name(tirtc_session_get_link_mode()));
    serial_net_cli_writef("OK");
}

static void serial_net_cli_print_help(void)
{
    serial_net_cli_writef("+HELP:AT");
    serial_net_cli_writef("+HELP:AT+HELP");
    serial_net_cli_writef("+HELP:AT+NET?");
    serial_net_cli_writef("+HELP:AT+WIFI?");
    serial_net_cli_writef("+HELP:AT+WIFISCAN");
    serial_net_cli_writef("+HELP:AT+WIFICONN=\"SSID\",\"password\"");
    serial_net_cli_writef("+HELP:AT+WIFIRETRY");
    serial_net_cli_writef("+HELP:AT+NETPROBE");
    serial_net_cli_writef("+HELP:AT+HEAP?");
    serial_net_cli_writef("+HELP:AT+SOCKETS?");
    serial_net_cli_writef("+HELP:AT+MEDIA?");
    serial_net_cli_writef("+HELP:AT+AUDIOPATH?");
    serial_net_cli_writef("+HELP:AT+RTCLOG=0..15|AT+RTCLOG?");
    serial_net_cli_writef("+HELP:AT+RTCLINK=DEFAULT|DIRECT|RELAY|AT+RTCLINK?");
    serial_net_cli_writef("+HELP:AT+AUDIO=<speaker 0..100>,<mic 0..100>|AT+AUDIO?");
    serial_net_cli_writef("+HELP:AT+AUDIOSOURCE=VIRTUAL|VIRTUAL_ALAW|MIC|AT+AUDIOSOURCE?");
    serial_net_cli_writef("+HELP:AT+AUDIOCHECK=RESET|AT+AUDIOCHECK?");
    serial_net_cli_writef("+HELP:AT+ALAWTEST=FIXED|ALLOC,<1..20000>");
    serial_net_cli_writef("+HELP:AT+ALAWPLAY=<1..1000 frames>");
    serial_net_cli_writef("+HELP:AT+TONE=<frequency 200..2000>,<duration 50..1000>");
    serial_net_cli_writef("+HELP:AT+APP=DEVICE|CALL|AI|HOME");
    serial_net_cli_writef("+HELP:AT+CALL=<12-char device id>|AT+CALL?|AT+ANSWER|AT+HANGUP");
    serial_net_cli_writef("+HELP:AT+AECDUMP=START,<250..5000ms>|READ,<offset>|FREE|AT+AECDUMP?");
    serial_net_cli_writef("+HELP:AT+AECINJECT=<frequency 0|200..3000>,<amplitude 0..12000>");
    serial_net_cli_writef("+HELP:AT+TRACE=0|1");
    serial_net_cli_writef("+HELP:AT+REBOOT");
    serial_net_cli_writef("OK");
}

static void serial_net_cli_print_heap(void)
{
    unsigned sockets_used = 0;
    unsigned sockets_closing = 0;

    for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + MEMP_NUM_NETCONN; ++fd) {
        const struct lwip_sock *sock = lwip_socket_dbg_get_socket(fd);
        if (sock == NULL || sock->conn == NULL) {
            continue;
        }
        sockets_used++;
#if LWIP_NETCONN_FULLDUPLEX
        if (sock->fd_free_pending != 0U) {
            sockets_closing++;
        }
#endif
    }

    serial_net_cli_writef(
        "+HEAP:internal_free=%u,internal_largest=%u,internal_min=%u,"
        "psram_free=%u,psram_largest=%u,stack_hwm=%u,sockets=%u/%u,sockets_closing=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        (unsigned)uxTaskGetStackHighWaterMark(NULL),
        sockets_used,
        (unsigned)MEMP_NUM_NETCONN,
        sockets_closing);
    serial_net_cli_writef("OK");
}

static void serial_net_cli_request_app(const char *target)
{
    esp_err_t ret;

    if (strcmp(target, "DEVICE") == 0) {
        ret = app_request_enter_app(APP_ID_DEVICE);
    } else if (strcmp(target, "CALL") == 0) {
        ret = app_request_enter_app(APP_ID_CALL);
    } else if (strcmp(target, "AI") == 0) {
        ret = app_request_enter_app(APP_ID_AI_CHAT);
    } else if (strcmp(target, "HOME") == 0) {
        ret = app_request_return_home();
    } else {
        serial_net_cli_writef("+APP:target=%s,ret=ESP_ERR_INVALID_ARG", target);
        return;
    }

    /*
     * AT-driven tests bypass the display button that normally changes both
     * the application owner and the visible page. Keep the call page aligned
     * with APP_ID_CALL so an exposed home tile cannot inject an unrelated app
     * transition while a device-to-device call is under test.
     */
    if (ret == ESP_OK && strcmp(target, "CALL") == 0) {
        esp_err_t display_ret = display_open_call_page_async();
        if (display_ret != ESP_OK) {
            ESP_LOGW(TAG, "sync call page after AT app switch failed: %s", esp_err_to_name(display_ret));
        }
    }

    serial_net_cli_writef("+APP:target=%s,ret=%s", target, esp_err_to_name(ret));
}

static void serial_net_cli_print_aec_dump(void)
{
    audio_echo_cancel_debug_capture_status_t status = {0};

    audio_echo_cancel_debug_capture_get_status(&status);
    serial_net_cli_writef(
        "+AECDUMP:allocated=%u,state=%s,rate=%u,channels=%u,frames=%u,capacity=%u,bytes=%u",
        status.allocated ? 1U : 0U,
        status.recording ? "recording" : (status.complete ? "ready" : "idle"),
        (unsigned)status.sample_rate_hz,
        (unsigned)status.channel_count,
        (unsigned)status.captured_frames,
        (unsigned)status.capacity_frames,
        (unsigned)status.data_bytes);
    serial_net_cli_writef("OK");
}

static void serial_net_cli_aec_dump_observer(const uint8_t *data,
                                             size_t data_len,
                                             const audio_format_t *format,
                                             void *ctx)
{
    audio_echo_cancel_debug_capture_status_t status = {0};

    (void)data;
    (void)data_len;
    (void)format;
    (void)ctx;
    audio_echo_cancel_debug_capture_get_status(&status);
    if (status.complete || !status.recording) {
        microphone_unregister_observer(serial_net_cli_aec_dump_observer, NULL);
        s_aec_dump_observer_registered = false;
    }
}

static void serial_net_cli_aec_dump_observer_stop(void)
{
    if (!s_aec_dump_observer_registered) {
        return;
    }
    microphone_unregister_observer(serial_net_cli_aec_dump_observer, NULL);
    s_aec_dump_observer_registered = false;
}

static void serial_net_cli_aec_dump_start(const char *argument)
{
    char *end = NULL;
    unsigned long duration_ms = strtoul(argument, &end, 10);

    if (end == argument || *end != '\0' || duration_ms > UINT32_MAX) {
        serial_net_cli_writef("ERROR,INVALID_ARGUMENT,AECDUMP_START");
        return;
    }

    serial_net_cli_aec_dump_observer_stop();
    esp_err_t ret = microphone_prepare_capture_path();
    if (ret == ESP_OK) {
        ret = audio_echo_cancel_debug_capture_start((uint32_t)duration_ms);
    }
    if (ret == ESP_OK) {
        ret = microphone_register_observer(serial_net_cli_aec_dump_observer, NULL);
        if (ret == ESP_OK) {
            s_aec_dump_observer_registered = true;
            ret = microphone_set_observer_enabled(serial_net_cli_aec_dump_observer, NULL, true);
            if (ret != ESP_OK) {
                serial_net_cli_aec_dump_observer_stop();
                (void)audio_echo_cancel_debug_capture_free();
            }
        } else {
            (void)audio_echo_cancel_debug_capture_free();
        }
    }
    serial_net_cli_writef("+AECDUMP:start=%lu,ret=%s",
                          duration_ms,
                          esp_err_to_name(ret));
    if (ret == ESP_OK) {
        serial_net_cli_writef("OK");
    }
}

static void serial_net_cli_aec_dump_read(const char *argument)
{
    char *end = NULL;
    unsigned long offset_value = strtoul(argument, &end, 10);
    uint8_t raw[SERIAL_NET_CLI_AEC_DUMP_RAW_BYTES] = {0};
    uint8_t encoded[((SERIAL_NET_CLI_AEC_DUMP_RAW_BYTES + 2U) / 3U) * 4U + 1U] = {0};
    size_t bytes_read = 0;
    size_t encoded_size = 0;

    if (end == argument || *end != '\0' || offset_value > SIZE_MAX) {
        serial_net_cli_writef("ERROR,INVALID_ARGUMENT,AECDUMP_READ");
        return;
    }

    esp_err_t ret = audio_echo_cancel_debug_capture_read((size_t)offset_value,
                                                          raw,
                                                          sizeof(raw),
                                                          &bytes_read);
    if (ret != ESP_OK) {
        serial_net_cli_writef("+AECDATA:offset=%lu,ret=%s",
                              offset_value,
                              esp_err_to_name(ret));
        return;
    }
    if (mbedtls_base64_encode(encoded,
                              sizeof(encoded),
                              &encoded_size,
                              raw,
                              bytes_read) != 0) {
        serial_net_cli_writef("+AECDATA:offset=%lu,ret=ESP_FAIL", offset_value);
        return;
    }
    if (encoded_size >= sizeof(encoded)) {
        serial_net_cli_writef("+AECDATA:offset=%lu,ret=ESP_ERR_INVALID_SIZE", offset_value);
        return;
    }
    encoded[encoded_size] = '\0';
    serial_net_cli_writef("+AECDATA:offset=%lu,bytes=%u,data=%s",
                          offset_value,
                          (unsigned)bytes_read,
                          (const char *)encoded);
    serial_net_cli_writef("OK");
}

static void serial_net_cli_aec_dump_command(const char *argument)
{
    if (strncmp(argument, "START,", strlen("START,")) == 0) {
        serial_net_cli_aec_dump_start(argument + strlen("START,"));
    } else if (strncmp(argument, "READ,", strlen("READ,")) == 0) {
        serial_net_cli_aec_dump_read(argument + strlen("READ,"));
    } else if (strcmp(argument, "FREE") == 0) {
        serial_net_cli_aec_dump_observer_stop();
        esp_err_t ret = audio_echo_cancel_debug_capture_free();
        serial_net_cli_writef("+AECDUMP:free=%s", esp_err_to_name(ret));
        if (ret == ESP_OK) {
            serial_net_cli_writef("OK");
        }
    } else {
        serial_net_cli_writef("ERROR,INVALID_ARGUMENT,AECDUMP");
    }
}

static void serial_net_cli_aec_injection_command(const char *argument)
{
    char *end = NULL;
    unsigned long frequency_hz = strtoul(argument, &end, 10);
    if (end == argument || *end != ',' || frequency_hz > UINT32_MAX) {
        serial_net_cli_writef("ERROR,INVALID_ARGUMENT,AECINJECT");
        return;
    }

    const char *amplitude_text = end + 1;
    unsigned long amplitude = strtoul(amplitude_text, &end, 10);
    if (end == amplitude_text || *end != '\0' || amplitude > UINT16_MAX) {
        serial_net_cli_writef("ERROR,INVALID_ARGUMENT,AECINJECT");
        return;
    }

    esp_err_t ret = audio_echo_cancel_debug_injection_set((uint32_t)frequency_hz,
                                                           (uint16_t)amplitude);
    serial_net_cli_writef("+AECINJECT:frequency=%lu,amplitude=%lu,ret=%s",
                          frequency_hz,
                          amplitude,
                          esp_err_to_name(ret));
    if (ret == ESP_OK) {
        serial_net_cli_writef("OK");
    }
}

static void serial_net_cli_print_audio(void)
{
    audio_stats_t audio = {0};

    /* Audio diagnostics must not take the application snapshot lock chain.
     * This command is also used while call teardown is in progress. */
    audio_device_get_stats(&audio);
    serial_net_cli_writef("+AUDIO:speaker=%u,mic=%u,input=%u,output=%u,frames=%lu,interval_us=%lu/%lu,read_us=%lu,process_us=%lu,dispatch_us=%lu,pipeline_wait_us=%lu/%lu,pipeline_high=%lu,overrun=%lu,read_err=%lu",
                          (unsigned)audio.speaker_volume_percent,
                          (unsigned)audio.capture_gain_percent,
                          (unsigned)audio.input_level,
                          (unsigned)audio.output_level,
                          (unsigned long)audio.capture_frames,
                          (unsigned long)audio.capture_interval_us,
                          (unsigned long)audio.capture_interval_max_us,
                          (unsigned long)audio.capture_read_us,
                          (unsigned long)audio.capture_process_us,
                          (unsigned long)audio.capture_dispatch_us,
                          (unsigned long)audio.capture_pipeline_wait_us,
                          (unsigned long)audio.capture_pipeline_wait_max_us,
                          (unsigned long)audio.capture_pipeline_high_water,
                          (unsigned long)audio.capture_pipeline_overruns,
                          (unsigned long)audio.capture_read_errors);
    serial_net_cli_writef("OK");
}

static void serial_net_cli_print_audio_source(void)
{
    sender_test_snapshot_t source = {0};

    sender_test_get_snapshot(&source);
    serial_net_cli_writef(
        "+AUDIOSOURCE:source=%s,running=%u,frames=%llu,bytes=%llu,fail=%llu,last_seq=%lu,seq_valid=%u,status=%s",
        source.running ? (source.use_alaw ? "virtual_alaw" : "virtual") : "mic",
        source.running ? 1U : 0U,
        (unsigned long long)source.frames_sent,
        (unsigned long long)source.bytes_sent,
        (unsigned long long)source.send_failures,
        (unsigned long)source.last_sequence,
        source.last_sequence_valid ? 1U : 0U,
        source.status);
    serial_net_cli_writef("OK");
}

static void serial_net_cli_set_audio_source(const char *argument)
{
    if (strcmp(argument, "VIRTUAL") == 0) {
        sender_test_set_audio_alaw(false);
        esp_err_t ret = sender_test_start(SENDER_TEST_MODE_AUDIO);
        serial_net_cli_writef("+AUDIOSOURCE:source=virtual,ret=%s", esp_err_to_name(ret));
        if (ret == ESP_OK) {
            serial_net_cli_writef("OK");
        }
        return;
    }
    if (strcmp(argument, "VIRTUAL_ALAW") == 0) {
        sender_test_set_audio_alaw(true);
        esp_err_t ret = sender_test_start(SENDER_TEST_MODE_AUDIO);
        serial_net_cli_writef("+AUDIOSOURCE:source=virtual_alaw,ret=%s", esp_err_to_name(ret));
        if (ret == ESP_OK) {
            serial_net_cli_writef("OK");
        }
        return;
    }
    if (strcmp(argument, "MIC") == 0) {
        sender_test_stop();
        sender_test_set_audio_alaw(false);
        serial_net_cli_writef("+AUDIOSOURCE:source=mic,ret=ESP_OK");
        serial_net_cli_writef("OK");
        return;
    }

    serial_net_cli_writef("ERROR,INVALID_ARGUMENT,AUDIOSOURCE");
}

static void serial_net_cli_reset_audio_check(void)
{
    rtc_media_bridge_reset_audio_integrity_stats();
    media_sink_reset_audio_integrity_stats();
    serial_net_cli_writef("+AUDIOCHECK:reset=1");
    serial_net_cli_writef("OK");
}

static void serial_net_cli_alaw_test(const char *argument)
{
    const char *comma = strchr(argument, ',');
    if (comma == NULL || comma == argument) {
        serial_net_cli_writef("ERROR,INVALID_ARGUMENT,ALAWTEST");
        return;
    }

    char mode[8] = {0};
    const size_t mode_len = (size_t)(comma - argument);
    if (mode_len >= sizeof(mode)) {
        serial_net_cli_writef("ERROR,INVALID_ARGUMENT,ALAWTEST");
        return;
    }
    memcpy(mode, argument, mode_len);

    char *end = NULL;
    const unsigned long parsed_iterations = strtoul(comma + 1, &end, 10);
    if (end == comma + 1 || *end != '\0' || parsed_iterations == 0U ||
        parsed_iterations > SERIAL_NET_CLI_ALAW_TEST_MAX_ITERATIONS ||
        (strcmp(mode, "FIXED") != 0 && strcmp(mode, "ALLOC") != 0)) {
        serial_net_cli_writef("ERROR,INVALID_ARGUMENT,ALAWTEST");
        return;
    }

    uint8_t *pcm = heap_caps_malloc(SERIAL_NET_CLI_ALAW_PCM_BYTES,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *alaw = heap_caps_malloc(SERIAL_NET_CLI_ALAW_BYTES,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *decoded = NULL;
    if (strcmp(mode, "FIXED") == 0) {
        decoded = heap_caps_malloc(SERIAL_NET_CLI_ALAW_BYTES * sizeof(int16_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (pcm == NULL || alaw == NULL || (strcmp(mode, "FIXED") == 0 && decoded == NULL)) {
        heap_caps_free(decoded);
        heap_caps_free(alaw);
        heap_caps_free(pcm);
        serial_net_cli_writef("+ALAWTEST:mode=%s,ret=ESP_ERR_NO_MEM", mode);
        return;
    }

    int16_t *samples = (int16_t *)pcm;
    for (size_t index = 0; index < SERIAL_NET_CLI_ALAW_PCM_BYTES / sizeof(int16_t); ++index) {
        const int32_t ramp = (int32_t)((index * 257U) & 0x3FFFU) - 8192;
        samples[index] = (int16_t)ramp;
    }

    const size_t internal_before =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_before =
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const int64_t started_us = esp_timer_get_time();
    uint32_t checksum = 2166136261U;
    esp_err_t ret = ESP_OK;

    for (unsigned long iteration = 0; iteration < parsed_iterations; ++iteration) {
        size_t encoded_len = 0;
        ret = audio_alaw_encode_16k_mono_to_8k(pcm,
                                                SERIAL_NET_CLI_ALAW_PCM_BYTES,
                                                alaw,
                                                SERIAL_NET_CLI_ALAW_BYTES,
                                                &encoded_len);
        if (ret != ESP_OK || encoded_len != SERIAL_NET_CLI_ALAW_BYTES) {
            if (ret == ESP_OK) {
                ret = ESP_ERR_INVALID_SIZE;
            }
            break;
        }

        uint8_t *decoded_frame = decoded;
        size_t decoded_len = SERIAL_NET_CLI_ALAW_BYTES * sizeof(int16_t);
        if (strcmp(mode, "ALLOC") == 0) {
            decoded_frame = NULL;
            decoded_len = 0;
            ret = audio_alaw_decode(alaw, encoded_len, &decoded_frame, &decoded_len);
        } else {
            ret = audio_alaw_decode_to_pcm(alaw, encoded_len, decoded_frame, decoded_len);
        }
        if (ret != ESP_OK || decoded_frame == NULL ||
            decoded_len != SERIAL_NET_CLI_ALAW_BYTES * sizeof(int16_t)) {
            if (strcmp(mode, "ALLOC") == 0) {
                free(decoded_frame);
            }
            if (ret == ESP_OK) {
                ret = ESP_ERR_INVALID_SIZE;
            }
            break;
        }

        checksum ^= alaw[iteration % encoded_len];
        checksum *= 16777619U;
        checksum ^= decoded_frame[(iteration * 2U) % decoded_len];
        checksum *= 16777619U;
        if (strcmp(mode, "ALLOC") == 0) {
            free(decoded_frame);
        }

        if ((iteration & 0xFFU) == 0xFFU) {
            vTaskDelay(1);
        }
    }

    const uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - started_us) / 1000);
    const size_t internal_after =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_after =
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const bool heap_ok = heap_caps_check_integrity_all(false);

    heap_caps_free(decoded);
    heap_caps_free(alaw);
    heap_caps_free(pcm);
    serial_net_cli_writef(
        "+ALAWTEST:mode=%s,iterations=%lu,ret=%s,elapsed_ms=%lu,checksum=%08lx,heap_ok=%u,internal_delta=%ld,psram_delta=%ld",
        mode,
        parsed_iterations,
        esp_err_to_name(ret),
        (unsigned long)elapsed_ms,
        (unsigned long)checksum,
        heap_ok ? 1U : 0U,
        (long)internal_after - (long)internal_before,
        (long)psram_after - (long)psram_before);
    if (ret == ESP_OK && heap_ok) {
        serial_net_cli_writef("OK");
    }
}

static void serial_net_cli_alaw_play(const char *argument)
{
    char *end = NULL;
    const unsigned long frames = strtoul(argument, &end, 10);
    if (end == argument || *end != '\0' || frames == 0U || frames > 1000U) {
        serial_net_cli_writef("ERROR,INVALID_ARGUMENT,ALAWPLAY");
        return;
    }

    uint8_t *pcm = heap_caps_malloc(SERIAL_NET_CLI_ALAW_PCM_BYTES,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *alaw = heap_caps_malloc(SERIAL_NET_CLI_ALAW_BYTES,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcm == NULL || alaw == NULL) {
        heap_caps_free(alaw);
        heap_caps_free(pcm);
        serial_net_cli_writef("+ALAWPLAY:ret=ESP_ERR_NO_MEM");
        return;
    }

    int16_t *samples = (int16_t *)pcm;
    for (size_t index = 0; index < SERIAL_NET_CLI_ALAW_PCM_BYTES / sizeof(int16_t); ++index) {
        const int32_t phase = (int32_t)(index % 32U);
        samples[index] = (int16_t)((phase < 16 ? phase : 31 - phase) * 160 - 1200);
    }

    size_t encoded_len = 0;
    esp_err_t ret = audio_alaw_encode_16k_mono_to_8k(pcm,
                                                      SERIAL_NET_CLI_ALAW_PCM_BYTES,
                                                      alaw,
                                                      SERIAL_NET_CLI_ALAW_BYTES,
                                                      &encoded_len);
    if (ret == ESP_OK) {
        ret = media_sink_init();
    }

    const audio_format_t format = {
        .sample_rate_hz = 8000U,
        .bits_per_sample = 16U,
        .channels = 1U,
    };
    unsigned long submitted = 0;
    unsigned long failed = 0;
    media_sink_set_audio_profile(MEDIA_SINK_AUDIO_PROFILE_DEVICE_CALL);
    media_sink_set_remote_audio_talkspurt(true);
    const uint32_t started_ms = serial_net_cli_uptime_ms();

    for (unsigned long frame = 0; ret == ESP_OK && frame < frames; ++frame) {
        uint8_t *decoded = NULL;
        size_t decoded_len = 0;
        ret = audio_alaw_decode(alaw, encoded_len, &decoded, &decoded_len);
        if (ret == ESP_OK) {
            ret = media_sink_submit_remote_audio_owned(decoded,
                                                       decoded_len,
                                                       &format,
                                                       started_ms + (uint32_t)(frame * 20U));
            if (ret == ESP_OK) {
                decoded = NULL;
                submitted++;
            } else {
                failed++;
            }
        }
        free(decoded);
        if (ret != ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    media_sink_set_remote_audio_talkspurt(false);
    vTaskDelay(pdMS_TO_TICKS(300));
    media_sink_flush();
    media_sink_set_audio_profile(MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY);
    const bool heap_ok = heap_caps_check_integrity_all(false);
    heap_caps_free(alaw);
    heap_caps_free(pcm);
    serial_net_cli_writef(
        "+ALAWPLAY:frames=%lu,submitted=%lu,failed=%lu,ret=%s,elapsed_ms=%lu,heap_ok=%u",
        frames,
        submitted,
        failed,
        esp_err_to_name(ret),
        (unsigned long)(serial_net_cli_uptime_ms() - started_ms),
        heap_ok ? 1U : 0U);
    if (ret == ESP_OK && heap_ok && submitted == frames) {
        serial_net_cli_writef("OK");
    }
}

static void serial_net_cli_print_audio_check(void)
{
    sender_test_snapshot_t source = {0};
    rtc_media_bridge_audio_integrity_stats_t rx = {0};
    media_sink_audio_diagnostics_t media = {0};
    bool media_ready = false;

    sender_test_get_snapshot(&source);
    rtc_media_bridge_get_audio_integrity_stats(&rx);
    media_ready = media_sink_get_audio_diagnostics(&media);

    serial_net_cli_writef(
        "+AUDIOCHECK:stage=tx,running=%u,frames=%llu,bytes=%llu,fail=%llu,last_seq=%lu,seq_valid=%u",
        source.running ? 1U : 0U,
        (unsigned long long)source.frames_sent,
        (unsigned long long)source.bytes_sent,
        (unsigned long long)source.send_failures,
        (unsigned long)source.last_sequence,
        source.last_sequence_valid ? 1U : 0U);
    serial_net_cli_writef(
        "+AUDIOCHECK:stage=rx,valid=%lu,crc_fail=%lu,missing=%lu,duplicate=%lu,reordered=%lu,first_seq=%lu,last_seq=%lu,seq_valid=%u",
        (unsigned long)rx.valid_frames,
        (unsigned long)rx.checksum_failures,
        (unsigned long)rx.missing_frames,
        (unsigned long)rx.duplicate_frames,
        (unsigned long)rx.reordered_frames,
        (unsigned long)rx.first_sequence,
        (unsigned long)rx.last_sequence,
        rx.sequence_valid ? 1U : 0U);
    if (media_ready) {
        serial_net_cli_writef(
            "+AUDIOCHECK:stage=consume,rx=%lu,buffered=%lu,played=%lu,queued=%lu,buffered_ms=%lu,crc_fail=%lu,missing=%lu,duplicate=%lu,reordered=%lu,first_seq=%lu,last_seq=%lu,queue_test_drop=%lu,trim_test_drop=%lu,play_fail=%lu,play_missing=%lu,play_duplicate=%lu,play_reordered=%lu,play_first_seq=%lu,play_last_seq=%lu,play_seq_valid=%u,drop=%lu/%lu/%lu,underflow=%lu/%lu",
            (unsigned long)media.integrity_rx_frames,
            (unsigned long)media.integrity_buffered_frames,
            (unsigned long)media.integrity_played_frames,
            (unsigned long)media.queued_packets,
            (unsigned long)media.buffered_ms,
            (unsigned long)media.integrity_checksum_failures,
            (unsigned long)media.integrity_missing_frames,
            (unsigned long)media.integrity_duplicate_frames,
            (unsigned long)media.integrity_reordered_frames,
            (unsigned long)media.integrity_first_sequence,
            (unsigned long)media.integrity_last_sequence,
            (unsigned long)media.integrity_queue_dropped_frames,
            (unsigned long)media.integrity_trimmed_frames,
            (unsigned long)media.integrity_play_failures,
            (unsigned long)media.integrity_play_missing_frames,
            (unsigned long)media.integrity_play_duplicate_frames,
            (unsigned long)media.integrity_play_reordered_frames,
            (unsigned long)media.integrity_first_play_sequence,
            (unsigned long)media.integrity_last_play_sequence,
            media.integrity_play_sequence_valid ? 1U : 0U,
            (unsigned long)media.play_drop_packets,
            (unsigned long)media.queue_drop_packets,
            (unsigned long)media.trim_drop_packets,
            (unsigned long)media.underflow_events,
            (unsigned long)media.active_underflow_events);
    } else {
        serial_net_cli_writef("+AUDIOCHECK:stage=consume,ready=0");
    }
    serial_net_cli_writef("OK");
}

static void serial_net_cli_print_audio_path(void)
{
    audio_stats_t audio = {0};
    media_sink_audio_diagnostics_t media = {0};
    tirtc_session_stats_t rtc = {0};
    bool media_ready = false;
    bool rtc_tx_ready = false;
    uint32_t capture_window_age_ms = 0;

    audio_device_get_stats(&audio);
    media_ready = media_sink_get_audio_diagnostics(&media);
    tirtc_session_get_stats(&rtc);
    rtc_tx_ready = rtc.active_connection && rtc.call_active &&
                   rtc.local_audio_send_enabled &&
                   rtc.local_audio_stream_id != UINT8_MAX;
    if (audio.capture_window_valid) {
        capture_window_age_ms = serial_net_cli_uptime_ms() - audio.capture_window_updated_ms;
    }

    serial_net_cli_writef(
        "+AUDIOPATH:node=mic_hw,ready=%u,capture=%u,window=%u,age_ms=%lu,codec_gain=%u,last_raw_dbfs_x10=%ld/%ld,read_us=%lu,read_err=%lu",
        audio.ready ? 1U : 0U,
        audio.capture_enabled ? 1U : 0U,
        audio.capture_window_valid ? 1U : 0U,
        (unsigned long)capture_window_age_ms,
        (unsigned)audio.capture_codec_gain_percent,
        (long)audio.capture_raw_ch0_dbfs_x10,
        (long)audio.capture_raw_ch1_dbfs_x10,
        (unsigned long)audio.capture_read_us,
        (unsigned long)audio.capture_read_errors);
    serial_net_cli_writef(
        "+AUDIOPATH:node=capture_pipeline,frames=%lu,interval_us=%lu/%lu,process_us=%lu,dispatch_us=%lu,wait_us=%lu/%lu,high=%lu,overrun=%lu",
        (unsigned long)audio.capture_frames,
        (unsigned long)audio.capture_interval_us,
        (unsigned long)audio.capture_interval_max_us,
        (unsigned long)audio.capture_process_us,
        (unsigned long)audio.capture_dispatch_us,
        (unsigned long)audio.capture_pipeline_wait_us,
        (unsigned long)audio.capture_pipeline_wait_max_us,
        (unsigned long)audio.capture_pipeline_high_water,
        (unsigned long)audio.capture_pipeline_overruns);
    serial_net_cli_writef(
        "+AUDIOPATH:node=aec,continuous=%u,near_protect=%u,far_guard=%u,suppression=%s,frames=%lu/%lu/%lu,warmup=%lu/%lu,near_detected/protected=%lu/%lu,guard=%lu,peak=%lu/%lu/%lu/%lu,suppress=%u,near_decisions=%lu,near_diag_retained_nlp_coherence=%u/%u/%u,near_reject_peak_retained_nlp_coherence=%lu/%lu/%lu/%lu",
        audio.echo_continuous_processing ? 1U : 0U,
        audio.echo_near_end_protection_enabled ? 1U : 0U,
        audio.far_end_gain_guard_enabled ? 1U : 0U,
        audio.echo_suppression == AUDIO_ECHO_SUPPRESSION_STRONG ? "strong" : "balanced",
        (unsigned long)audio.echo_reference_frames,
        (unsigned long)audio.echo_active_frames,
        (unsigned long)audio.echo_bypass_frames,
        (unsigned long)audio.echo_warmup_frames,
        (unsigned long)audio.echo_warmup_passthrough_frames,
        (unsigned long)audio.echo_near_end_detected_frames,
        (unsigned long)audio.echo_near_end_frames,
        (unsigned long)audio.echo_far_end_guard_frames,
        (unsigned long)audio.echo_ref_peak,
        (unsigned long)audio.echo_mic_peak,
        (unsigned long)audio.echo_linear_peak,
        (unsigned long)audio.echo_out_peak,
        (unsigned)audio.echo_suppress_percent,
        (unsigned long)audio.echo_near_decisions,
        (unsigned)audio.echo_near_retained_energy_percent,
        (unsigned)audio.echo_near_nlp_reduction_percent,
        (unsigned)audio.echo_near_reference_coherence_percent,
        (unsigned long)audio.echo_near_reject_low_peak,
        (unsigned long)audio.echo_near_reject_low_retained,
        (unsigned long)audio.echo_near_reject_low_nlp,
        (unsigned long)audio.echo_near_reject_high_coherence);
    serial_net_cli_writef(
        "+AUDIOPATH:node=uplink_gain,send=%u,upload=%u,base_q8=%u,auto_q8=%u,target_peak=%u,auto_max=%u,last_auto_max=%u,gate=%u,pre_dbfs_x10=%ld/%ld,post_dbfs_x10=%ld/%ld,meter=%lu",
        (unsigned)audio.capture_gain_percent,
        (unsigned)audio.capture_upload_gain_percent,
        (unsigned)audio.capture_base_gain_q8,
        (unsigned)audio.capture_auto_gain_q8,
        (unsigned)audio.capture_auto_gain_target_peak,
        (unsigned)audio.capture_auto_gain_max_percent,
        (unsigned)audio.capture_effective_auto_gain_max_percent,
        audio.capture_noise_gate_enabled ? 1U : 0U,
        (long)audio.capture_pre_peak_dbfs_x10,
        (long)audio.capture_pre_rms_dbfs_x10,
        (long)audio.capture_post_peak_dbfs_x10,
        (long)audio.capture_post_rms_dbfs_x10,
        (unsigned long)audio.input_level);
    serial_net_cli_writef(
        "+AUDIOPATH:node=rtc_tx,configured=%u,ready=%u,active=%u,call=%u,stream=%u,frames=%lu,queue=%lu/%lu,drops=%lu/%lu/%lu,lock_fail=%lu,lock_us=%lu/%lu,sdk_us=%lu/%lu,fail=%lu",
        rtc.local_audio_send_enabled ? 1U : 0U,
        rtc_tx_ready ? 1U : 0U,
        rtc.active_connection ? 1U : 0U,
        rtc.call_active ? 1U : 0U,
        (unsigned)rtc.local_audio_stream_id,
        (unsigned long)rtc.tx_audio_frames,
        (unsigned long)rtc.tx_audio_queue_depth_packets,
        (unsigned long)rtc.tx_audio_queue_high_water_packets,
        (unsigned long)rtc.tx_audio_queue_pressure_drops,
        (unsigned long)rtc.tx_audio_queue_stale_drops,
        (unsigned long)rtc.tx_audio_queue_generation_drops,
        (unsigned long)rtc.tx_audio_sdk_lock_failures,
        (unsigned long)rtc.tx_audio_sdk_lock_wait_last_us,
        (unsigned long)rtc.tx_audio_sdk_lock_wait_max_us,
        (unsigned long)rtc.tx_audio_sdk_send_last_us,
        (unsigned long)rtc.tx_audio_sdk_send_max_us,
        (unsigned long)rtc.tx_failures);
    serial_net_cli_writef(
        "+AUDIOPATH:node=rtc_rx,active=%u,frames=%lu,bytes=%u,media_ready=%u",
        rtc.active_connection ? 1U : 0U,
        (unsigned long)rtc.rx_audio_frames,
        (unsigned)rtc.rx_audio_bytes,
        media_ready ? 1U : 0U);
    if (media_ready) {
        serial_net_cli_writef(
            "+AUDIOPATH:node=jitter,packet_ms=%u,prebuffer_ms=%u,target_ms=%u,buffered_ms=%u,queued=%u,jitter_ms=%u/%u/%u,max_gap_ms=%u,source_late=%u/%u/%u,clock=%ld,gap_pending=%ld/%u,gap_fill=%u/%u",
            (unsigned)media.source_packet_ms,
            (unsigned)media.prebuffer_ms,
            (unsigned)media.target_ms,
            (unsigned)media.buffered_ms,
            (unsigned)media.queued_packets,
            (unsigned)media.jitter_ewma_ms,
            (unsigned)media.jitter_peak_ms,
            (unsigned)media.jitter_boost_ms,
            (unsigned)media.max_arrival_gap_ms,
            (unsigned)media.source_late_events,
            (unsigned)media.source_late_ms,
            (unsigned)media.max_source_late_ms,
            (long)media.source_clock_error_ms,
            (long)media.source_gap_pending_ms,
            (unsigned)media.source_gap_pending_packets,
            (unsigned)media.source_gap_fill_events,
            (unsigned)media.source_gap_fill_ms);
        serial_net_cli_writef(
            "+AUDIOPATH:node=play_buffer,active=%u,talkspurt=%u,rx=%u/%u,played=%u/%u,drop=%u/%u/%u,underflow=%u/%u,grace=%u/%u,conceal=%u/%u,recovery=%u/%u,paced=%u,pace_wait=%u/%u/%u,pace_late=%u/%u/%u",
            media.playback_active ? 1U : 0U,
            media.talkspurt_active ? 1U : 0U,
            (unsigned)media.rx_packets,
            (unsigned)media.rx_ms,
            (unsigned)media.played_packets,
            (unsigned)media.played_ms,
            (unsigned)media.play_drop_packets,
            (unsigned)media.queue_drop_packets,
            (unsigned)media.trim_drop_packets,
            (unsigned)media.underflow_events,
            (unsigned)media.active_underflow_events,
            (unsigned)media.underflow_grace_waits,
            (unsigned)media.underflow_grace_recoveries,
            (unsigned)media.concealment_events,
            (unsigned)media.concealed_ms,
            (unsigned)media.clock_recovery_events,
            (unsigned)media.clock_recovery_frames,
            media.playback_pacing_enabled ? 1U : 0U,
            (unsigned)media.pacing_wait_events,
            (unsigned)media.pacing_wait_ms,
            (unsigned)media.pacing_wait_max_ms,
            (unsigned)media.pacing_late_events,
            (unsigned)media.pacing_late_ms,
            (unsigned)media.pacing_late_max_ms);
        serial_net_cli_writef(
            "+AUDIOPATH:node=integrity,rx=%u,buffered=%u,played=%u,crc_fail=%u,missing=%u,duplicate=%u,reordered=%u,first_seq=%u,last_seq=%u,seq_valid=%u,queue_test_drop=%u,trim_test_drop=%u,play_fail=%u,play_missing=%u,play_duplicate=%u,play_reordered=%u,play_first_seq=%u,play_last_seq=%u,play_seq_valid=%u",
            (unsigned)media.integrity_rx_frames,
            (unsigned)media.integrity_buffered_frames,
            (unsigned)media.integrity_played_frames,
            (unsigned)media.integrity_checksum_failures,
            (unsigned)media.integrity_missing_frames,
            (unsigned)media.integrity_duplicate_frames,
            (unsigned)media.integrity_reordered_frames,
            (unsigned)media.integrity_first_sequence,
            (unsigned)media.integrity_last_sequence,
            media.integrity_sequence_valid ? 1U : 0U,
            (unsigned)media.integrity_queue_dropped_frames,
            (unsigned)media.integrity_trimmed_frames,
            (unsigned)media.integrity_play_failures,
            (unsigned)media.integrity_play_missing_frames,
            (unsigned)media.integrity_play_duplicate_frames,
            (unsigned)media.integrity_play_reordered_frames,
            (unsigned)media.integrity_first_play_sequence,
            (unsigned)media.integrity_last_play_sequence,
            media.integrity_play_sequence_valid ? 1U : 0U);
    }
    serial_net_cli_writef(
        "+AUDIOPATH:node=speaker,enabled=%u,volume=%u,level=%lu,writes=%lu,prepare_err=%lu,write_err=%lu,prepare_us=%lu/%lu,write_us=%lu/%lu",
        audio.speaker_enabled ? 1U : 0U,
        (unsigned)audio.speaker_volume_percent,
        (unsigned long)audio.output_level,
        (unsigned long)audio.speaker_write_frames,
        (unsigned long)audio.speaker_prepare_errors,
        (unsigned long)audio.speaker_write_errors,
        (unsigned long)audio.speaker_prepare_last_us,
        (unsigned long)audio.speaker_prepare_max_us,
        (unsigned long)audio.speaker_write_last_us,
        (unsigned long)audio.speaker_write_max_us);
    serial_net_cli_writef("OK");
}

static void serial_net_cli_set_audio(const char *argument)
{
    char *end = NULL;
    long speaker = strtol(argument, &end, 10);
    if (end == argument || *end != ',' || speaker < 0 || speaker > 100) {
        serial_net_cli_writef("ERROR,INVALID_ARGUMENT,AUDIO");
        return;
    }
    const char *mic_text = end + 1;
    long mic = strtol(mic_text, &end, 10);
    if (end == mic_text || *end != '\0' || mic < 0 || mic > 100) {
        serial_net_cli_writef("ERROR,INVALID_ARGUMENT,AUDIO");
        return;
    }

    esp_err_t speaker_ret = app_set_speaker_volume((uint8_t)speaker);
    esp_err_t mic_ret = app_set_capture_gain((uint8_t)mic);
    serial_net_cli_writef("+AUDIO:speaker=%ld/%s,mic=%ld/%s",
                          speaker,
                          esp_err_to_name(speaker_ret),
                          mic,
                          esp_err_to_name(mic_ret));
    if (speaker_ret == ESP_OK && mic_ret == ESP_OK) {
        serial_net_cli_writef("OK");
    }
}

static void serial_net_cli_play_tone(const char *argument)
{
    char *end = NULL;
    long frequency_hz = strtol(argument, &end, 10);
    if (end == argument || *end != ',' || frequency_hz < 200 || frequency_hz > 2000) {
        serial_net_cli_writef("ERROR,INVALID_ARGUMENT,TONE");
        return;
    }

    const char *duration_text = end + 1;
    long duration_ms = strtol(duration_text, &end, 10);
    if (end == duration_text || *end != '\0' || duration_ms < 50 || duration_ms > 1000) {
        serial_net_cli_writef("ERROR,INVALID_ARGUMENT,TONE");
        return;
    }

    esp_err_t ret = speaker_play_test_tone((uint32_t)frequency_hz, (uint32_t)duration_ms);
    serial_net_cli_writef("+TONE:frequency=%ld,duration=%ld,ret=%s",
                          frequency_hz,
                          duration_ms,
                          esp_err_to_name(ret));
    if (ret == ESP_OK) {
        serial_net_cli_writef("OK");
    }
}

static void serial_net_cli_print_call(void)
{
    /* A call query must not traverse the full UI/application snapshot graph. */
    device_call_snapshot_t snapshot = {0};
    device_call_get_snapshot(&snapshot);
    serial_net_cli_writef("+CALL:state=%u,pending=%u",
                          (unsigned)snapshot.state,
                          snapshot.pending_incoming ? 1U : 0U);
    serial_net_cli_writef("OK");
}

static void serial_net_cli_call(const char *device_id)
{
    esp_err_t ret = app_request_call_contact(device_id);
    if (ret == ESP_OK) {
        esp_err_t display_ret = display_open_call_active_page_async();
        if (display_ret != ESP_OK) {
            ESP_LOGW(TAG, "sync active call page after AT call failed: %s", esp_err_to_name(display_ret));
        }
    }
    serial_net_cli_writef("+CALL:target=%s,ret=%s", device_id, esp_err_to_name(ret));
}

static void serial_net_cli_answer(void)
{
    esp_err_t ret = app_request_accept_call();
    if (ret == ESP_OK) {
        esp_err_t display_ret = display_open_call_active_page_async();
        if (display_ret != ESP_OK) {
            ESP_LOGW(TAG, "sync active call page after AT answer failed: %s", esp_err_to_name(display_ret));
        }
    }
    serial_net_cli_writef("+ANSWER:ret=%s", esp_err_to_name(ret));
}

static void serial_net_cli_process_line(const char *line)
{
    if (strcmp(line, "AT") == 0) {
        serial_net_cli_writef("OK");
    } else if (strcmp(line, "AT+HELP") == 0) {
        serial_net_cli_print_help();
    } else if (strcmp(line, "AT+NET?") == 0) {
        serial_net_cli_print_net();
    } else if (strcmp(line, "AT+WIFI?") == 0) {
        serial_net_cli_print_wifi();
    } else if (strcmp(line, "AT+WIFISCAN") == 0) {
        serial_net_cli_start_scan();
    } else if (strncmp(line, "AT+WIFICONN=", strlen("AT+WIFICONN=")) == 0) {
        serial_net_cli_start_connect(line + strlen("AT+WIFICONN="));
    } else if (strcmp(line, "AT+WIFIRETRY") == 0) {
        serial_net_cli_retry_connect();
    } else if (strcmp(line, "AT+NETPROBE") == 0) {
        serial_net_cli_start_probe();
    } else if (strcmp(line, "AT+HEAP?") == 0) {
        serial_net_cli_print_heap();
    } else if (strcmp(line, "AT+SOCKETS?") == 0) {
        serial_net_cli_print_sockets();
    } else if (strcmp(line, "AT+MEDIA?") == 0) {
        serial_net_cli_print_media();
    } else if (strcmp(line, "AT+AUDIOPATH?") == 0) {
        serial_net_cli_print_audio_path();
    } else if (strcmp(line, "AT+RTCLOG?") == 0) {
        serial_net_cli_print_rtc_log_level();
    } else if (strncmp(line, "AT+RTCLOG=", strlen("AT+RTCLOG=")) == 0) {
        serial_net_cli_set_rtc_log_level(line + strlen("AT+RTCLOG="));
    } else if (strcmp(line, "AT+RTCLINK?") == 0) {
        serial_net_cli_print_rtc_link_mode();
    } else if (strncmp(line, "AT+RTCLINK=", strlen("AT+RTCLINK=")) == 0) {
        serial_net_cli_set_rtc_link_mode(line + strlen("AT+RTCLINK="));
    } else if (strcmp(line, "AT+AUDIO?") == 0) {
        serial_net_cli_print_audio();
    } else if (strncmp(line, "AT+AUDIO=", strlen("AT+AUDIO=")) == 0) {
        serial_net_cli_set_audio(line + strlen("AT+AUDIO="));
    } else if (strcmp(line, "AT+AUDIOSOURCE?") == 0) {
        serial_net_cli_print_audio_source();
    } else if (strncmp(line, "AT+AUDIOSOURCE=", strlen("AT+AUDIOSOURCE=")) == 0) {
        serial_net_cli_set_audio_source(line + strlen("AT+AUDIOSOURCE="));
    } else if (strcmp(line, "AT+AUDIOCHECK?") == 0) {
        serial_net_cli_print_audio_check();
    } else if (strcmp(line, "AT+AUDIOCHECK=RESET") == 0) {
        serial_net_cli_reset_audio_check();
    } else if (strncmp(line, "AT+ALAWTEST=", strlen("AT+ALAWTEST=")) == 0) {
        serial_net_cli_alaw_test(line + strlen("AT+ALAWTEST="));
    } else if (strncmp(line, "AT+ALAWPLAY=", strlen("AT+ALAWPLAY=")) == 0) {
        serial_net_cli_alaw_play(line + strlen("AT+ALAWPLAY="));
    } else if (strncmp(line, "AT+TONE=", strlen("AT+TONE=")) == 0) {
        serial_net_cli_play_tone(line + strlen("AT+TONE="));
    } else if (strcmp(line, "AT+AECDUMP?") == 0) {
        serial_net_cli_print_aec_dump();
    } else if (strncmp(line, "AT+AECDUMP=", strlen("AT+AECDUMP=")) == 0) {
        serial_net_cli_aec_dump_command(line + strlen("AT+AECDUMP="));
    } else if (strncmp(line, "AT+AECINJECT=", strlen("AT+AECINJECT=")) == 0) {
        serial_net_cli_aec_injection_command(line + strlen("AT+AECINJECT="));
    } else if (strncmp(line, "AT+APP=", strlen("AT+APP=")) == 0) {
        serial_net_cli_request_app(line + strlen("AT+APP="));
    } else if (strcmp(line, "AT+CALL?") == 0) {
        serial_net_cli_print_call();
    } else if (strncmp(line, "AT+CALL=", strlen("AT+CALL=")) == 0) {
        serial_net_cli_call(line + strlen("AT+CALL="));
    } else if (strcmp(line, "AT+ANSWER") == 0) {
        serial_net_cli_answer();
    } else if (strcmp(line, "AT+HANGUP") == 0) {
        serial_net_cli_writef("+HANGUP:ret=%s", esp_err_to_name(app_request_hangup_call()));
    } else if (strcmp(line, "AT+TRACE=0") == 0) {
        s_trace_disabled = true;
        serial_net_cli_writef("OK");
    } else if (strcmp(line, "AT+TRACE=1") == 0) {
        s_trace_disabled = false;
        serial_net_cli_writef("OK");
    } else if (strcmp(line, "AT+TRACE?") == 0) {
        serial_net_cli_writef("+TRACE:%u", s_trace_disabled ? 0U : 1U);
        serial_net_cli_writef("OK");
    } else if (strcmp(line, "AT+REBOOT") == 0) {
        serial_net_cli_writef("OK");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else {
        serial_net_cli_writef("ERROR,UNKNOWN_COMMAND");
    }
}

static void serial_net_cli_poll_operations(void)
{
    const uint32_t now_ms = serial_net_cli_uptime_ms();
    network_state_t state = {0};

    network_get_state(&state);
    if (!s_last_network_state_valid ||
        serial_net_cli_network_state_changed(&state, &s_last_network_state)) {
        if (!s_trace_disabled) {
            serial_net_cli_emit_network_state(s_operations.connect.seq, "state", &state);
        }
        s_last_network_state = state;
        s_last_network_state_valid = true;
    }

    if (s_operations.connect.seq != 0U) {
        if (state.connected) {
            serial_net_cli_emit_network_state(s_operations.connect.seq, "connect_done", &state);
            s_operations.connect.seq = 0;
        } else if (state.phase == NETWORK_CONNECTION_PHASE_FAILED) {
            serial_net_cli_emit_network_state(s_operations.connect.seq, "connect_failed", &state);
            s_operations.connect.seq = 0;
        } else if ((uint32_t)(now_ms - s_operations.connect.started_ms) >=
                   SERIAL_NET_CLI_CONNECT_TIMEOUT_MS) {
            serial_net_cli_writef("+NETEVT:seq=%u,stage=connect,state=timeout",
                                  (unsigned)s_operations.connect.seq);
            s_operations.connect.seq = 0;
        }
    }

    if (s_operations.scan.seq != 0U) {
        network_scan_snapshot_t scan = {0};
        network_get_scan_results(&scan);
        if (!scan.in_progress && scan.last_scan_ms != s_operations.scan_baseline_ms) {
            serial_net_cli_writef("+WIFISCAN:seq=%u,count=%u,t_ms=%u",
                                  (unsigned)s_operations.scan.seq,
                                  (unsigned)scan.count,
                                  (unsigned)(now_ms - s_operations.scan.started_ms));
            for (uint16_t index = 0; index < scan.count; ++index) {
                char escaped_ssid[72] = {0};
                serial_net_cli_escape_field(scan.results[index].ssid,
                                            escaped_ssid,
                                            sizeof(escaped_ssid));
                serial_net_cli_writef(
                    "+AP:seq=%u,index=%u,ssid=\"%s\",rssi=%d,channel=%u,secure=%u",
                    (unsigned)s_operations.scan.seq,
                    (unsigned)index,
                    escaped_ssid,
                    (int)scan.results[index].rssi,
                    (unsigned)scan.results[index].channel,
                    scan.results[index].secure ? 1U : 0U);
            }
            serial_net_cli_writef("+NETEVT:seq=%u,stage=scan,state=done",
                                  (unsigned)s_operations.scan.seq);
            s_operations.scan.seq = 0;
        } else if ((uint32_t)(now_ms - s_operations.scan.started_ms) >=
                   SERIAL_NET_CLI_SCAN_TIMEOUT_MS) {
            serial_net_cli_writef("+NETEVT:seq=%u,stage=scan,state=timeout",
                                  (unsigned)s_operations.scan.seq);
            s_operations.scan.seq = 0;
        }
    }

    uint32_t probe_seq = 0;
    uint32_t probe_started_ms = 0;
    bool dns_should_report = false;
    bool dns_ready = false;
    int dns_ret = ERR_INPROGRESS;
    uint32_t dns_elapsed_ms = 0;
    char dns_host[sizeof(s_operations.probe_dns_host)] = {0};

    taskENTER_CRITICAL(&s_serial_net_cli_lock);
    probe_seq = s_operations.probe.seq;
    probe_started_ms = s_operations.probe.started_ms;
    if (probe_seq != 0U &&
        s_operations.probe_dns_done &&
        !s_operations.probe_dns_reported) {
        dns_should_report = true;
        dns_ready = s_operations.probe_dns_ready;
        dns_ret = s_operations.probe_dns_ret;
        dns_elapsed_ms = s_operations.probe_dns_elapsed_ms;
        strlcpy(dns_host, s_operations.probe_dns_host, sizeof(dns_host));
        s_operations.probe_dns_reported = true;
        if (!dns_ready) {
            s_operations.probe_degraded = true;
        }
    }
    taskEXIT_CRITICAL(&s_serial_net_cli_lock);

    if (probe_seq != 0U) {
        if (dns_should_report) {
            serial_net_cli_writef(
                "+NETEVT:seq=%u,stage=dns,state=%s,host=%s,t_ms=%u,ret=%d",
                (unsigned)probe_seq,
                dns_ready ? "ready" : "failed",
                dns_host[0] != '\0' ? dns_host : "none",
                (unsigned)dns_elapsed_ms,
                dns_ret);
        }

        bool ping_reported = false;
        taskENTER_CRITICAL(&s_serial_net_cli_lock);
        ping_reported = s_operations.probe_ping_reported;
        taskEXIT_CRITICAL(&s_serial_net_cli_lock);
        if (!ping_reported) {
            network_ping_status_t ping = {0};
            network_get_ping_status(&ping);
            if (!ping.running && ping.valid) {
                const bool ping_ready = ping.received > 0U;
                serial_net_cli_writef(
                    "+NETEVT:seq=%u,stage=ping,state=%s,tx=%u,rx=%u,loss=%u,avg_ms=%u",
                    (unsigned)probe_seq,
                    ping_ready ? "ready" : "failed",
                    (unsigned)ping.transmitted,
                    (unsigned)ping.received,
                    (unsigned)ping.loss_percent,
                    (unsigned)ping.avg_time_ms);
                taskENTER_CRITICAL(&s_serial_net_cli_lock);
                s_operations.probe_ping_reported = true;
                if (!ping_ready) {
                    s_operations.probe_degraded = true;
                }
                taskEXIT_CRITICAL(&s_serial_net_cli_lock);
            }
        }

        bool probe_done = false;
        bool probe_degraded = false;
        taskENTER_CRITICAL(&s_serial_net_cli_lock);
        probe_done = s_operations.probe_dns_reported &&
                     s_operations.probe_ping_reported;
        probe_degraded = s_operations.probe_degraded;
        if (probe_done) {
            s_operations.probe.seq = 0;
        }
        taskEXIT_CRITICAL(&s_serial_net_cli_lock);

        if (probe_done) {
            serial_net_cli_writef("+NETEVT:seq=%u,stage=probe,state=%s,t_ms=%u",
                                  (unsigned)probe_seq,
                                  probe_degraded ? "degraded" : "ready",
                                  (unsigned)(now_ms - probe_started_ms));
        } else if ((uint32_t)(now_ms - probe_started_ms) >=
                   SERIAL_NET_CLI_PROBE_TIMEOUT_MS) {
            network_cancel_ping();
            taskENTER_CRITICAL(&s_serial_net_cli_lock);
            if (s_operations.probe.seq == probe_seq) {
                s_operations.probe.seq = 0;
            }
            taskEXIT_CRITICAL(&s_serial_net_cli_lock);
            serial_net_cli_writef("+NETEVT:seq=%u,stage=probe,state=timeout",
                                  (unsigned)probe_seq);
        }
    }
}

static void serial_net_cli_task(void *ctx)
{
    (void)ctx;
    char line[SERIAL_NET_CLI_LINE_MAX] = {0};
    uint8_t input[SERIAL_NET_CLI_IO_CHUNK_SIZE] = {0};
    size_t line_length = 0;
    bool discard_line = false;

    serial_net_cli_writef("+READY:serial_net_cli,transport=%s,baud=%u",
                          serial_net_cli_transport_name(),
                          (unsigned)CONFIG_ESP_CONSOLE_UART_BAUDRATE);
    while (true) {
        int read_len = serial_net_cli_read(input,
                                           sizeof(input),
                                           pdMS_TO_TICKS(SERIAL_NET_CLI_POLL_MS));
        for (int input_index = 0; input_index < read_len; ++input_index) {
            const uint8_t value = input[input_index];
            if (value == '\r' || value == '\n') {
                if (discard_line) {
                    serial_net_cli_writef("ERROR,LINE_TOO_LONG");
                } else if (line_length > 0U) {
                    line[line_length] = '\0';
                    serial_net_cli_process_line(line);
                }
                memset(line, 0, sizeof(line));
                line_length = 0;
                discard_line = false;
            } else if (value == 0x08U || value == 0x7FU) {
                if (!discard_line && line_length > 0U) {
                    line[--line_length] = '\0';
                }
            } else if (!discard_line) {
                if (line_length + 1U < sizeof(line)) {
                    line[line_length++] = (char)value;
                } else {
                    memset(line, 0, sizeof(line));
                    line_length = 0;
                    discard_line = true;
                }
            }
        }
        serial_net_cli_poll_operations();
    }
}

esp_err_t serial_net_cli_start(void)
{
#if !CONFIG_ESP_CONSOLE_UART && \
    !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG && \
    !CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_serial_net_cli_task != NULL) {
        return ESP_OK;
    }

#if SERIAL_NET_CLI_USE_USB_SERIAL_JTAG
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t usb_config = {
            .tx_buffer_size = SERIAL_NET_CLI_USB_BUFFER_SIZE,
            .rx_buffer_size = SERIAL_NET_CLI_USB_BUFFER_SIZE,
        };
        esp_err_t ret = usb_serial_jtag_driver_install(&usb_config);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    usb_serial_jtag_vfs_use_driver();
#else
    if (!uart_is_driver_installed(CONFIG_ESP_CONSOLE_UART_NUM)) {
        const uart_config_t uart_config = {
            .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        esp_err_t ret = uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM,
                                            SERIAL_NET_CLI_UART_RX_BUFFER_SIZE,
                                            0,
                                            0,
                                            NULL,
                                            0);
        if (ret != ESP_OK) {
            return ret;
        }
        ret = uart_param_config(CONFIG_ESP_CONSOLE_UART_NUM, &uart_config);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
#endif

    BaseType_t task_ok = xTaskCreatePinnedToCoreWithCaps(serial_net_cli_task,
                                                         "serial_net_cli",
                                                         SERIAL_NET_CLI_TASK_STACK_SIZE,
                                                         NULL,
                                                         SERIAL_NET_CLI_TASK_PRIORITY,
                                                         &s_serial_net_cli_task,
                                                         SERIAL_NET_CLI_TASK_CORE,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ok != pdPASS) {
        s_serial_net_cli_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "serial network CLI ready: transport=%s",
             serial_net_cli_transport_name());
    return ESP_OK;
#endif
}
