#include "wifi_manager.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#define WIFI_NVS_NAMESPACE "tirtc_wifi"
#define WIFI_NVS_CREDENTIALS_KEY "credentials"
#define WIFI_NVS_SSID_KEY "ssid"
#define WIFI_NVS_PASSWORD_KEY "password"
#define WIFI_CREDENTIALS_MAGIC 0x57494649U
#define WIFI_CREDENTIALS_VERSION 1U
#define WIFI_RECONNECT_DELAY_US (2ULL * 1000ULL * 1000ULL)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    char ssid[WIFI_MANAGER_SSID_MAX + 1];
    char password[WIFI_MANAGER_PASSWORD_MAX + 1];
    uint16_t reserved;
    uint32_t crc32;
} wifi_credentials_record_t;

_Static_assert(offsetof(wifi_credentials_record_t, crc32) == 108,
               "Wi-Fi credential record layout changed");
_Static_assert(sizeof(wifi_credentials_record_t) == 112,
               "Wi-Fi credential record size changed");

typedef struct {
    bool valid;
    wifi_manager_observer_t observer;
    void *context;
    wifi_manager_event_t event;
} observer_delivery_t;

typedef enum {
    WIFI_LINK_IDLE = 0,
    WIFI_LINK_CONNECTING,
    WIFI_LINK_CONNECTED,
    /* No new connect is issued until the old generation disconnects. */
    WIFI_LINK_RECONFIGURING,
} wifi_link_state_t;

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_driver_lock;
static bool s_initializing;
static bool s_initialized;
static bool s_started;
static bool s_connected;
static bool s_connect_in_flight;
static bool s_credentials_configured;
static wifi_link_state_t s_link_state = WIFI_LINK_IDLE;
static uint32_t s_credentials_generation;
static uint32_t s_connect_generation;
static uint32_t s_reconnect_attempt;
static uint16_t s_last_disconnect_reason;
static wifi_manager_state_t s_state = WIFI_MANAGER_STATE_UNINITIALIZED;
static wifi_manager_credentials_t s_credentials;
static char s_ip[WIFI_MANAGER_IPV4_STRING_MAX];
static char s_netmask[WIFI_MANAGER_IPV4_STRING_MAX];
static char s_gateway[WIFI_MANAGER_IPV4_STRING_MAX];
static wifi_manager_observer_t s_observer;
static void *s_observer_context;

static esp_netif_t *s_station_netif;
static bool s_station_netif_owned;
static bool s_wifi_initialized;
static esp_event_handler_instance_t s_wifi_event_instance;
static esp_event_handler_instance_t s_ip_event_instance;
static esp_timer_handle_t s_reconnect_timer;

static void secure_zero(void *memory, size_t size)
{
    volatile unsigned char *cursor = memory;
    while (cursor != NULL && size-- > 0U) {
        *cursor++ = 0U;
    }
}

static uint32_t credentials_crc32(const wifi_credentials_record_t *record)
{
    const uint8_t *bytes = (const uint8_t *)record;
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0;
         index < offsetof(wifi_credentials_record_t, crc32);
         ++index) {
        crc ^= bytes[index];
        for (unsigned bit = 0; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

static uint32_t next_generation(uint32_t generation)
{
    ++generation;
    return generation == 0U ? 1U : generation;
}

static bool credentials_load_error_recoverable(esp_err_t error)
{
    switch (error) {
    case ESP_ERR_NVS_NOT_FOUND:
    case ESP_ERR_NVS_TYPE_MISMATCH:
    case ESP_ERR_NVS_INVALID_LENGTH:
    case ESP_ERR_INVALID_SIZE:
    case ESP_ERR_INVALID_CRC:
        return true;
    default:
        return false;
    }
}

static bool string_field_canonical(const char *field, size_t capacity)
{
    const char *terminator = memchr(field, '\0', capacity);
    if (terminator == NULL) {
        return false;
    }
    for (const char *cursor = terminator + 1;
         cursor < field + capacity;
         ++cursor) {
        if (*cursor != '\0') {
            return false;
        }
    }
    return true;
}

static esp_err_t ensure_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_driver_lock == NULL) {
        s_driver_lock = xSemaphoreCreateMutex();
    }
    return s_lock == NULL || s_driver_lock == NULL
               ? ESP_ERR_NO_MEM
               : ESP_OK;
}

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size > 0) {
        (void)snprintf(error, error_size, "%s", message);
    }
}

static bool raw_psk_valid(const char *password, size_t length)
{
    if (length != WIFI_MANAGER_PASSWORD_MAX) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        if (!isxdigit((unsigned char)password[index])) {
            return false;
        }
    }
    return true;
}

bool wifi_manager_credentials_valid(const char *ssid,
                                    const char *password,
                                    char *error,
                                    size_t error_size)
{
    if (ssid == NULL || password == NULL) {
        set_error(error, error_size, "SSID/password is null");
        return false;
    }

    const size_t ssid_length = strlen(ssid);
    const size_t password_length = strlen(password);
    if (ssid_length == 0 || ssid_length > WIFI_MANAGER_SSID_MAX) {
        set_error(error, error_size, "SSID length must be 1..32 bytes");
        return false;
    }
    if (password_length != 0 &&
        (password_length < 8 || password_length > WIFI_MANAGER_PASSWORD_MAX)) {
        set_error(error, error_size, "password length must be 0 or 8..64 bytes");
        return false;
    }
    if (password_length == WIFI_MANAGER_PASSWORD_MAX &&
        !raw_psk_valid(password, password_length)) {
        set_error(error, error_size, "64-byte password must be a hexadecimal PSK");
        return false;
    }
    set_error(error, error_size, "");
    return true;
}

static void clear_ip_locked(void)
{
    s_ip[0] = '\0';
    s_netmask[0] = '\0';
    s_gateway[0] = '\0';
}

static void fill_snapshot_locked(wifi_manager_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = s_state;
    snapshot->initialized = s_initialized;
    snapshot->started = s_started;
    snapshot->credentials_configured = s_credentials_configured;
    snapshot->connected = s_connected;
    snapshot->last_disconnect_reason = s_last_disconnect_reason;
    snapshot->reconnect_attempt = s_reconnect_attempt;
    if (s_credentials_configured) {
        (void)snprintf(
            snapshot->ssid,
            sizeof(snapshot->ssid),
            "%s",
            s_credentials.ssid);
    }
    (void)snprintf(snapshot->ip, sizeof(snapshot->ip), "%s", s_ip);
    (void)snprintf(
        snapshot->netmask,
        sizeof(snapshot->netmask),
        "%s",
        s_netmask);
    (void)snprintf(
        snapshot->gateway,
        sizeof(snapshot->gateway),
        "%s",
        s_gateway);
}

static observer_delivery_t capture_delivery_locked(
    wifi_manager_event_type_t type)
{
    observer_delivery_t delivery = {
        .valid = s_observer != NULL,
        .observer = s_observer,
        .context = s_observer_context,
        .event = {
            .type = type,
        },
    };
    fill_snapshot_locked(&delivery.event.snapshot);
    return delivery;
}

static void deliver(const observer_delivery_t *delivery)
{
    if (delivery->valid) {
        delivery->observer(&delivery->event, delivery->context);
    }
}

static esp_err_t erase_key_if_present(nvs_handle_t handle, const char *key)
{
    esp_err_t err = nvs_erase_key(handle, key);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

static void credentials_record_build(
    const wifi_manager_credentials_t *credentials,
    wifi_credentials_record_t *record)
{
    secure_zero(record, sizeof(*record));
    record->magic = WIFI_CREDENTIALS_MAGIC;
    record->version = WIFI_CREDENTIALS_VERSION;
    record->record_size = (uint16_t)sizeof(*record);
    (void)strlcpy(record->ssid, credentials->ssid, sizeof(record->ssid));
    (void)strlcpy(
        record->password,
        credentials->password,
        sizeof(record->password));
    record->crc32 = credentials_crc32(record);
}

static esp_err_t credentials_record_parse(
    const wifi_credentials_record_t *record,
    wifi_manager_credentials_t *credentials)
{
    if (record->magic != WIFI_CREDENTIALS_MAGIC ||
        record->version != WIFI_CREDENTIALS_VERSION ||
        record->record_size != sizeof(*record) ||
        record->crc32 != credentials_crc32(record) ||
        record->reserved != 0U ||
        !string_field_canonical(record->ssid, sizeof(record->ssid)) ||
        !string_field_canonical(record->password, sizeof(record->password)) ||
        !wifi_manager_credentials_valid(
            record->ssid,
            record->password,
            NULL,
            0)) {
        return ESP_ERR_INVALID_CRC;
    }
    secure_zero(credentials, sizeof(*credentials));
    (void)strlcpy(credentials->ssid, record->ssid, sizeof(credentials->ssid));
    (void)strlcpy(
        credentials->password,
        record->password,
        sizeof(credentials->password));
    return ESP_OK;
}

static esp_err_t read_credentials_record(
    nvs_handle_t handle,
    wifi_manager_credentials_t *credentials)
{
    size_t size = 0;
    esp_err_t err =
        nvs_get_blob(handle, WIFI_NVS_CREDENTIALS_KEY, NULL, &size);
    if (err != ESP_OK) {
        return err;
    }
    if (size != sizeof(wifi_credentials_record_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    wifi_credentials_record_t record;
    secure_zero(&record, sizeof(record));
    err = nvs_get_blob(
        handle,
        WIFI_NVS_CREDENTIALS_KEY,
        &record,
        &size);
    if (err == ESP_OK && size == sizeof(record)) {
        err = credentials_record_parse(&record, credentials);
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_SIZE;
    }
    secure_zero(&record, sizeof(record));
    return err;
}

static esp_err_t read_legacy_credentials(
    nvs_handle_t handle,
    wifi_manager_credentials_t *credentials)
{
    secure_zero(credentials, sizeof(*credentials));
    size_t ssid_size = sizeof(credentials->ssid);
    size_t password_size = sizeof(credentials->password);
    esp_err_t err = nvs_get_str(
        handle,
        WIFI_NVS_SSID_KEY,
        credentials->ssid,
        &ssid_size);
    if (err == ESP_OK) {
        err = nvs_get_str(
            handle,
            WIFI_NVS_PASSWORD_KEY,
            credentials->password,
            &password_size);
    }
    if (err == ESP_OK &&
        !wifi_manager_credentials_valid(
            credentials->ssid,
            credentials->password,
            NULL,
            0)) {
        err = ESP_ERR_INVALID_CRC;
    }
    if (err != ESP_OK) {
        secure_zero(credentials, sizeof(*credentials));
    }
    return err;
}

static esp_err_t write_credentials_record(
    nvs_handle_t handle,
    const wifi_manager_credentials_t *credentials)
{
    wifi_credentials_record_t record;
    credentials_record_build(credentials, &record);
    esp_err_t err = nvs_set_blob(
        handle,
        WIFI_NVS_CREDENTIALS_KEY,
        &record,
        sizeof(record));
    if (err == ESP_OK) {
        err = erase_key_if_present(handle, WIFI_NVS_SSID_KEY);
    }
    if (err == ESP_OK) {
        err = erase_key_if_present(handle, WIFI_NVS_PASSWORD_KEY);
    }
    secure_zero(&record, sizeof(record));
    return err;
}

static esp_err_t load_credentials_from_nvs(
    wifi_manager_credentials_t *credentials)
{
    secure_zero(credentials, sizeof(*credentials));
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    const esp_err_t record_err =
        read_credentials_record(handle, credentials);
    if (record_err == ESP_OK) {
        nvs_close(handle);
        return ESP_OK;
    }

    esp_err_t legacy_err = read_legacy_credentials(handle, credentials);
    if (legacy_err == ESP_OK) {
        /* Publish the blob and retire both legacy keys in one NVS commit. */
        err = write_credentials_record(handle, credentials);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        if (err != ESP_OK) {
            secure_zero(credentials, sizeof(*credentials));
        }
    } else {
        err = record_err == ESP_ERR_NVS_NOT_FOUND ? legacy_err : record_err;
        secure_zero(credentials, sizeof(*credentials));
    }
    nvs_close(handle);
    return err;
}

static esp_err_t store_credentials_to_nvs(
    const wifi_manager_credentials_t *credentials)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = write_credentials_record(handle, credentials);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    return err;
}

static esp_err_t clear_credentials_from_nvs(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = erase_key_if_present(handle, WIFI_NVS_CREDENTIALS_KEY);
    }
    if (err == ESP_OK) {
        err = erase_key_if_present(handle, WIFI_NVS_SSID_KEY);
    }
    if (err == ESP_OK) {
        err = erase_key_if_present(handle, WIFI_NVS_PASSWORD_KEY);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    return err;
}

static void make_station_config(
    const wifi_manager_credentials_t *credentials,
    wifi_config_t *station)
{
    secure_zero(station, sizeof(*station));
    const size_t ssid_length = strlen(credentials->ssid);
    const size_t password_length = strlen(credentials->password);
    memcpy(station->sta.ssid, credentials->ssid, ssid_length);
    memcpy(station->sta.password, credentials->password, password_length);
    station->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    station->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    station->sta.threshold.authmode = password_length == 0
                                          ? WIFI_AUTH_OPEN
                                          : WIFI_AUTH_WPA2_PSK;
    station->sta.pmf_cfg.capable = true;
    station->sta.pmf_cfg.required = false;
}

static void arm_reconnect_timer(void)
{
    if (s_reconnect_timer == NULL) {
        return;
    }
    (void)esp_timer_stop(s_reconnect_timer);
    (void)esp_timer_start_once(s_reconnect_timer, WIFI_RECONNECT_DELAY_US);
}

static void schedule_reconnect(void)
{
    if (s_reconnect_timer == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    const bool should_schedule =
        s_initialized && s_started && s_credentials_configured &&
        s_link_state == WIFI_LINK_IDLE;
    (void)xSemaphoreGive(s_lock);
    if (!should_schedule) {
        return;
    }

    arm_reconnect_timer();
}

static esp_err_t request_connect(void)
{
    if (s_driver_lock == NULL ||
        xSemaphoreTake(s_driver_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        (void)xSemaphoreGive(s_driver_lock);
        return ESP_FAIL;
    }
    if (!s_initialized || !s_started || !s_credentials_configured) {
        (void)xSemaphoreGive(s_lock);
        (void)xSemaphoreGive(s_driver_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_link_state != WIFI_LINK_IDLE) {
        (void)xSemaphoreGive(s_lock);
        (void)xSemaphoreGive(s_driver_lock);
        return ESP_OK;
    }

    wifi_manager_credentials_t credentials = s_credentials;
    const uint32_t generation = s_credentials_generation;
    s_link_state = WIFI_LINK_CONNECTING;
    s_connect_generation = generation;
    s_connected = false;
    s_connect_in_flight = true;
    s_state = WIFI_MANAGER_STATE_CONNECTING;
    const observer_delivery_t connecting =
        capture_delivery_locked(WIFI_MANAGER_EVENT_CONNECTING);
    (void)xSemaphoreGive(s_lock);

    if (s_reconnect_timer != NULL) {
        (void)esp_timer_stop(s_reconnect_timer);
    }
    deliver(&connecting);

    wifi_config_t station;
    make_station_config(&credentials, &station);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &station);
    secure_zero(&station, sizeof(station));
    secure_zero(&credentials, sizeof(credentials));
    if (err == ESP_OK &&
        xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        const bool current =
            s_initialized && s_started && s_credentials_configured &&
            s_link_state == WIFI_LINK_CONNECTING &&
            s_connect_generation == generation &&
            s_credentials_generation == generation;
        (void)xSemaphoreGive(s_lock);
        if (current) {
            err = esp_wifi_connect();
        } else {
            err = ESP_OK;
        }
    } else if (err == ESP_OK) {
        err = ESP_FAIL;
    }
    (void)xSemaphoreGive(s_driver_lock);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        const bool current =
            s_link_state == WIFI_LINK_CONNECTING &&
            s_connect_generation == generation;
        if (current) {
            s_link_state = WIFI_LINK_IDLE;
            s_connect_generation = 0;
            s_connect_in_flight = false;
        }
        if (current && s_initialized && s_started &&
            s_credentials_configured) {
            s_state = WIFI_MANAGER_STATE_DISCONNECTED;
            ++s_reconnect_attempt;
        }
        const observer_delivery_t disconnected =
            capture_delivery_locked(WIFI_MANAGER_EVENT_DISCONNECTED);
        (void)xSemaphoreGive(s_lock);
        if (current) {
            deliver(&disconnected);
        }
    }
    schedule_reconnect();
    return err;
}

static void reconnect_timer_callback(void *argument)
{
    (void)argument;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        if (s_initialized && s_started &&
            s_link_state == WIFI_LINK_RECONFIGURING) {
            s_link_state = WIFI_LINK_IDLE;
            s_connect_generation = 0U;
            s_connect_in_flight = false;
            s_state = s_credentials_configured
                          ? WIFI_MANAGER_STATE_DISCONNECTED
                          : WIFI_MANAGER_STATE_WAITING_FOR_CREDENTIALS;
        }
        (void)xSemaphoreGive(s_lock);
    }
    (void)request_connect();
}

static void wifi_event_handler(void *argument,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)argument;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        (void)request_connect();
        return;
    }

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected_data = event_data;
        if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
            return;
        }
        if (!s_initialized || !s_started) {
            (void)xSemaphoreGive(s_lock);
            return;
        }

        s_connected = false;
        clear_ip_locked();
        if (s_link_state == WIFI_LINK_RECONFIGURING) {
            s_link_state = WIFI_LINK_IDLE;
            s_connect_generation = 0;
            s_connect_in_flight = false;
            const bool reconnect =
                s_credentials_configured && s_started;
            s_state = reconnect
                          ? WIFI_MANAGER_STATE_DISCONNECTED
                          : WIFI_MANAGER_STATE_WAITING_FOR_CREDENTIALS;
            (void)xSemaphoreGive(s_lock);
            if (s_reconnect_timer != NULL) {
                (void)esp_timer_stop(s_reconnect_timer);
            }
            if (reconnect) {
                (void)request_connect();
            }
            return;
        }

        s_link_state = WIFI_LINK_IDLE;
        s_connect_generation = 0;
        s_connect_in_flight = false;
        s_last_disconnect_reason =
            disconnected_data == NULL ? 0 : disconnected_data->reason;
        const bool can_reconnect = s_credentials_configured;
        if (can_reconnect) {
            s_state = WIFI_MANAGER_STATE_DISCONNECTED;
            ++s_reconnect_attempt;
        } else {
            s_state = WIFI_MANAGER_STATE_WAITING_FOR_CREDENTIALS;
        }
        const observer_delivery_t disconnected =
            capture_delivery_locked(WIFI_MANAGER_EVENT_DISCONNECTED);
        (void)xSemaphoreGive(s_lock);

        deliver(&disconnected);
        if (can_reconnect) {
            schedule_reconnect();
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *got_ip = event_data;
        if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
            return;
        }
        if (!s_initialized || !s_started || !s_credentials_configured) {
            (void)xSemaphoreGive(s_lock);
            (void)esp_wifi_disconnect();
            return;
        }
        if ((s_link_state != WIFI_LINK_CONNECTING &&
             s_link_state != WIFI_LINK_CONNECTED) ||
            s_connect_generation != s_credentials_generation) {
            (void)xSemaphoreGive(s_lock);
            return;
        }

        s_link_state = WIFI_LINK_CONNECTED;
        s_connected = true;
        s_connect_in_flight = false;
        s_state = WIFI_MANAGER_STATE_CONNECTED;
        s_reconnect_attempt = 0;
        s_last_disconnect_reason = 0;
        if (got_ip != NULL) {
            (void)snprintf(
                s_ip,
                sizeof(s_ip),
                IPSTR,
                IP2STR(&got_ip->ip_info.ip));
            (void)snprintf(
                s_netmask,
                sizeof(s_netmask),
                IPSTR,
                IP2STR(&got_ip->ip_info.netmask));
            (void)snprintf(
                s_gateway,
                sizeof(s_gateway),
                IPSTR,
                IP2STR(&got_ip->ip_info.gw));
        }
        const observer_delivery_t connected =
            capture_delivery_locked(WIFI_MANAGER_EVENT_CONNECTED);
        (void)xSemaphoreGive(s_lock);

        if (s_reconnect_timer != NULL) {
            (void)esp_timer_stop(s_reconnect_timer);
        }
        deliver(&connected);
    }
}

static void cleanup_init_resources(void)
{
    if (s_wifi_event_instance != NULL) {
        (void)esp_event_handler_instance_unregister(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            s_wifi_event_instance);
        s_wifi_event_instance = NULL;
    }
    if (s_ip_event_instance != NULL) {
        (void)esp_event_handler_instance_unregister(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            s_ip_event_instance);
        s_ip_event_instance = NULL;
    }
    if (s_reconnect_timer != NULL) {
        (void)esp_timer_stop(s_reconnect_timer);
        (void)esp_timer_delete(s_reconnect_timer);
        s_reconnect_timer = NULL;
    }
    if (s_station_netif_owned && s_station_netif != NULL) {
        esp_netif_destroy_default_wifi(s_station_netif);
    }
    s_station_netif = NULL;
    s_station_netif_owned = false;
    if (s_wifi_initialized) {
        (void)esp_wifi_deinit();
        s_wifi_initialized = false;
    }
}

esp_err_t wifi_manager_init(void)
{
    esp_err_t err = ensure_lock();
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    if (s_initialized) {
        (void)xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    if (s_initializing) {
        (void)xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_initializing = true;
    (void)xSemaphoreGive(s_lock);

    wifi_manager_credentials_t loaded_credentials = {0};
    bool credentials_configured = false;

    err = nvs_flash_init();
    if (err == ESP_OK) {
        err = load_credentials_from_nvs(&loaded_credentials);
        if (err == ESP_OK) {
            credentials_configured = wifi_manager_credentials_valid(
                loaded_credentials.ssid,
                loaded_credentials.password,
                NULL,
                0);
            if (!credentials_configured) {
                secure_zero(&loaded_credentials, sizeof(loaded_credentials));
            }
        } else if (credentials_load_error_recoverable(err)) {
            /*
             * Keep UART AT available when only the credential record is
             * unusable. AT+WIFI=<...> can replace it and AT+WIFI=CLEAR can
             * erase it; NVS subsystem failures still abort initialization.
             */
            secure_zero(&loaded_credentials, sizeof(loaded_credentials));
            err = ESP_OK;
        }
    }

    if (err == ESP_OK) {
        err = esp_netif_init();
        if (err == ESP_ERR_INVALID_STATE) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = esp_event_loop_create_default();
        if (err == ESP_ERR_INVALID_STATE) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        s_station_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (s_station_netif == NULL) {
            s_station_netif = esp_netif_create_default_wifi_sta();
            s_station_netif_owned = s_station_netif != NULL;
        }
        if (s_station_netif == NULL) {
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err == ESP_OK) {
        wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&config);
        if (err == ESP_OK) {
            s_wifi_initialized = true;
        }
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    if (err == ESP_OK) {
        const esp_timer_create_args_t timer_arguments = {
            .callback = reconnect_timer_callback,
            .name = "wifi_reconnect",
            .skip_unhandled_events = true,
        };
        err = esp_timer_create(&timer_arguments, &s_reconnect_timer);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_event_handler,
            NULL,
            &s_wifi_event_instance);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            wifi_event_handler,
            NULL,
            &s_ip_event_instance);
    }

    if (err != ESP_OK) {
        cleanup_init_resources();
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            s_initializing = false;
            (void)xSemaphoreGive(s_lock);
        }
        secure_zero(&loaded_credentials, sizeof(loaded_credentials));
        return err;
    }

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        cleanup_init_resources();
        secure_zero(&loaded_credentials, sizeof(loaded_credentials));
        return ESP_FAIL;
    }
    s_credentials = loaded_credentials;
    s_credentials_configured = credentials_configured;
    s_initialized = true;
    s_initializing = false;
    s_started = false;
    s_connected = false;
    s_connect_in_flight = false;
    s_link_state = WIFI_LINK_IDLE;
    s_credentials_generation = 1U;
    s_connect_generation = 0U;
    s_reconnect_attempt = 0;
    s_last_disconnect_reason = 0;
    s_state = WIFI_MANAGER_STATE_IDLE;
    clear_ip_locked();
    const observer_delivery_t initialized =
        capture_delivery_locked(WIFI_MANAGER_EVENT_INITIALIZED);
    (void)xSemaphoreGive(s_lock);

    secure_zero(&loaded_credentials, sizeof(loaded_credentials));
    deliver(&initialized);
    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    if (s_lock == NULL ||
        xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_initialized) {
        (void)xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_started) {
        (void)xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    (void)xSemaphoreGive(s_lock);

    if (s_driver_lock == NULL ||
        xSemaphoreTake(s_driver_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        (void)xSemaphoreGive(s_driver_lock);
        return err;
    }

    err = esp_wifi_start();
    (void)xSemaphoreGive(s_driver_lock);
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        (void)esp_wifi_stop();
        return ESP_FAIL;
    }
    s_started = true;
    const bool configured = s_credentials_configured;
    s_connected = false;
    s_connect_in_flight = false;
    s_link_state = WIFI_LINK_IDLE;
    s_connect_generation = 0U;
    s_state = configured ? WIFI_MANAGER_STATE_DISCONNECTED
                         : WIFI_MANAGER_STATE_WAITING_FOR_CREDENTIALS;
    const observer_delivery_t started =
        capture_delivery_locked(WIFI_MANAGER_EVENT_STARTED);
    (void)xSemaphoreGive(s_lock);
    deliver(&started);

    return configured ? request_connect() : ESP_OK;
}

esp_err_t wifi_manager_stop(void)
{
    if (s_lock == NULL || s_driver_lock == NULL ||
        xSemaphoreTake(s_driver_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        (void)xSemaphoreGive(s_driver_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_initialized) {
        (void)xSemaphoreGive(s_lock);
        (void)xSemaphoreGive(s_driver_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_started) {
        (void)xSemaphoreGive(s_lock);
        (void)xSemaphoreGive(s_driver_lock);
        return ESP_OK;
    }

    s_started = false;
    s_connected = false;
    s_connect_in_flight = false;
    s_link_state = WIFI_LINK_IDLE;
    s_connect_generation = 0U;
    s_state = WIFI_MANAGER_STATE_IDLE;
    clear_ip_locked();
    const observer_delivery_t stopped =
        capture_delivery_locked(WIFI_MANAGER_EVENT_STOPPED);
    (void)xSemaphoreGive(s_lock);

    if (s_reconnect_timer != NULL) {
        (void)esp_timer_stop(s_reconnect_timer);
    }
    esp_err_t disconnect_err = esp_wifi_disconnect();
    if (disconnect_err == ESP_ERR_WIFI_NOT_CONNECT) {
        disconnect_err = ESP_OK;
    }
    const esp_err_t stop_err = esp_wifi_stop();
    (void)xSemaphoreGive(s_driver_lock);
    deliver(&stopped);
    return disconnect_err != ESP_OK ? disconnect_err : stop_err;
}

esp_err_t wifi_manager_deinit(void)
{
    if (s_lock == NULL) {
        return ESP_OK;
    }

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    const bool initialized = s_initialized;
    (void)xSemaphoreGive(s_lock);
    if (!initialized) {
        return ESP_OK;
    }

    esp_err_t err = wifi_manager_stop();
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    s_initialized = false;
    s_state = WIFI_MANAGER_STATE_UNINITIALIZED;
    secure_zero(&s_credentials, sizeof(s_credentials));
    s_credentials_configured = false;
    const observer_delivery_t deinitialized =
        capture_delivery_locked(WIFI_MANAGER_EVENT_DEINITIALIZED);
    (void)xSemaphoreGive(s_lock);

    cleanup_init_resources();
    deliver(&deinitialized);
    return ESP_OK;
}

esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password)
{
    if (!wifi_manager_credentials_valid(ssid, password, NULL, 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL ||
        xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_initialized) {
        (void)xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    (void)xSemaphoreGive(s_lock);

    if (s_driver_lock == NULL ||
        xSemaphoreTake(s_driver_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_manager_credentials_t credentials = {0};
    (void)snprintf(credentials.ssid, sizeof(credentials.ssid), "%s", ssid);
    (void)snprintf(
        credentials.password,
        sizeof(credentials.password),
        "%s",
        password);
    esp_err_t err = store_credentials_to_nvs(&credentials);
    if (err != ESP_OK) {
        secure_zero(&credentials, sizeof(credentials));
        (void)xSemaphoreGive(s_driver_lock);
        return err;
    }

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        secure_zero(&credentials, sizeof(credentials));
        (void)xSemaphoreGive(s_driver_lock);
        return ESP_FAIL;
    }
    const bool started = s_started;
    const bool begin_reconfiguration =
        started && (s_link_state == WIFI_LINK_CONNECTING ||
                    s_link_state == WIFI_LINK_CONNECTED);
    s_credentials_generation = next_generation(s_credentials_generation);
    s_credentials = credentials;
    s_credentials_configured = true;
    s_connected = false;
    s_connect_in_flight = false;
    s_reconnect_attempt = 0;
    s_last_disconnect_reason = 0;
    clear_ip_locked();
    if (started) {
        s_state = WIFI_MANAGER_STATE_DISCONNECTED;
    }
    if (begin_reconfiguration) {
        s_link_state = WIFI_LINK_RECONFIGURING;
    }
    const observer_delivery_t updated =
        capture_delivery_locked(WIFI_MANAGER_EVENT_CREDENTIALS_UPDATED);
    (void)xSemaphoreGive(s_lock);

    if (s_reconnect_timer != NULL) {
        (void)esp_timer_stop(s_reconnect_timer);
    }
    deliver(&updated);
    if (begin_reconfiguration) {
        const esp_err_t disconnect_err = esp_wifi_disconnect();
        if (disconnect_err != ESP_OK) {
            if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
                if (s_link_state == WIFI_LINK_RECONFIGURING) {
                    s_link_state = WIFI_LINK_IDLE;
                    s_connect_generation = 0U;
                    s_connect_in_flight = false;
                }
                (void)xSemaphoreGive(s_lock);
            }
        }
    }
    secure_zero(&credentials, sizeof(credentials));

    bool connect_now = false;
    bool reconfiguration_pending = false;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        connect_now = s_initialized && s_started &&
                      s_credentials_configured &&
                      s_link_state == WIFI_LINK_IDLE;
        reconfiguration_pending =
            s_initialized && s_started &&
            s_link_state == WIFI_LINK_RECONFIGURING;
        (void)xSemaphoreGive(s_lock);
    }
    if (reconfiguration_pending) {
        arm_reconnect_timer();
    }
    (void)xSemaphoreGive(s_driver_lock);
    if (started && connect_now) {
        /*
         * Credentials are already committed. Connection submission is
         * asynchronous and schedules its own retry on a transient failure.
         */
        (void)request_connect();
    }
    return ESP_OK;
}

esp_err_t wifi_manager_get_credentials(
    wifi_manager_credentials_t *credentials)
{
    if (credentials == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    secure_zero(credentials, sizeof(*credentials));
    if (s_lock == NULL ||
        xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_initialized) {
        (void)xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_credentials_configured) {
        (void)xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    *credentials = s_credentials;
    (void)xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t wifi_manager_clear_credentials(void)
{
    if (s_lock == NULL ||
        xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_initialized) {
        (void)xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    (void)xSemaphoreGive(s_lock);

    if (s_driver_lock == NULL ||
        xSemaphoreTake(s_driver_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = clear_credentials_from_nvs();
    if (err != ESP_OK) {
        (void)xSemaphoreGive(s_driver_lock);
        return err;
    }

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        (void)xSemaphoreGive(s_driver_lock);
        return ESP_FAIL;
    }
    const bool started = s_started;
    const bool begin_reconfiguration =
        started && (s_link_state == WIFI_LINK_CONNECTING ||
                    s_link_state == WIFI_LINK_CONNECTED);
    s_credentials_generation = next_generation(s_credentials_generation);
    secure_zero(&s_credentials, sizeof(s_credentials));
    s_credentials_configured = false;
    s_connected = false;
    s_connect_in_flight = false;
    s_reconnect_attempt = 0;
    s_last_disconnect_reason = 0;
    clear_ip_locked();
    s_state = started ? WIFI_MANAGER_STATE_WAITING_FOR_CREDENTIALS
                      : WIFI_MANAGER_STATE_IDLE;
    if (begin_reconfiguration) {
        s_link_state = WIFI_LINK_RECONFIGURING;
    } else if (s_link_state != WIFI_LINK_RECONFIGURING) {
        s_link_state = WIFI_LINK_IDLE;
        s_connect_generation = 0U;
    }
    const observer_delivery_t cleared =
        capture_delivery_locked(WIFI_MANAGER_EVENT_CREDENTIALS_CLEARED);
    (void)xSemaphoreGive(s_lock);

    if (s_reconnect_timer != NULL) {
        (void)esp_timer_stop(s_reconnect_timer);
    }
    deliver(&cleared);
    if (begin_reconfiguration) {
        const esp_err_t disconnect_err = esp_wifi_disconnect();
        if (disconnect_err != ESP_OK) {
            if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
                if (s_link_state == WIFI_LINK_RECONFIGURING) {
                    s_link_state = WIFI_LINK_IDLE;
                    s_connect_generation = 0U;
                    s_connect_in_flight = false;
                }
                (void)xSemaphoreGive(s_lock);
            }
        }
    }
    if (started) {
        wifi_config_t empty_station = {0};
        (void)esp_wifi_set_config(WIFI_IF_STA, &empty_station);
        secure_zero(&empty_station, sizeof(empty_station));
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        const bool reconfiguration_pending =
            s_initialized && s_started &&
            s_link_state == WIFI_LINK_RECONFIGURING;
        (void)xSemaphoreGive(s_lock);
        if (reconfiguration_pending) {
            arm_reconnect_timer();
        }
    }
    (void)xSemaphoreGive(s_driver_lock);
    return ESP_OK;
}

esp_err_t wifi_manager_get_snapshot(wifi_manager_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    fill_snapshot_locked(snapshot);
    (void)xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t wifi_manager_set_observer(wifi_manager_observer_t observer,
                                    void *context)
{
    esp_err_t err = ensure_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    s_observer = observer;
    s_observer_context = context;
    const observer_delivery_t snapshot =
        capture_delivery_locked(WIFI_MANAGER_EVENT_SNAPSHOT);
    (void)xSemaphoreGive(s_lock);
    deliver(&snapshot);
    return ESP_OK;
}
