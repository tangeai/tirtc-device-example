#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "nvs.h"

#include "platform_storage.h"

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT      BIT0
#define WIFI_FAIL_BIT           BIT1
#define WIFI_SCAN_DONE_BIT      BIT2
#define WIFI_MAX_RETRIES        8
#define WIFI_ASSOCIATION_TIMEOUT_MS 22000U
#define WIFI_DHCP_PHASE_TIMEOUT_MS  30000U
#define WIFI_DHCP_MAX_RETRIES       2U
#define WIFI_RETRY_BACKOFF_BASE_MS  1000U
#define WIFI_RETRY_BACKOFF_MAX_MS   4000U
#define WIFI_BACKGROUND_RECOVERY_INTERVAL_MS 30000U
#define WIFI_WATCHDOG_INTERVAL_MS   250U
#define WIFI_INVALID_RSSI       (-127)
#define WIFI_NVS_NAMESPACE      "wifi"
#define WIFI_NVS_KEY_SSID       "ssid"
#define WIFI_NVS_KEY_PASSWORD   "password"
#define WIFI_CHANNEL_HINT_MAX_AGE_MS 30000U
#define WIFI_STA_PROTOCOLS ((uint8_t)(WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N))
#define WIFI_SCAN_WAIT_MS 8000U
#define WIFI_SCAN_START_SETTLE_MS 600U
#define WIFI_SCAN_ACTIVE_MIN_MS 80U
#define WIFI_SCAN_ACTIVE_MAX_MS 180U
#define WIFI_SCAN_PASSIVE_MS 360U
#define WIFI_LOW_LATENCY_TX_POWER_QDBM 84
#define WIFI_SCAN_TASK_PRIORITY 5
#define WIFI_SCAN_TASK_CORE 1

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_wifi_sta_netif;
static wifi_status_t s_wifi_status;
static wifi_scan_snapshot_t s_wifi_scan_snapshot;
static bool s_wifi_initialized;
static bool s_wifi_event_loop_ready;
static bool s_wifi_scan_in_progress;
static bool s_wifi_manual_scan_active;
static bool s_wifi_scan_deferred;
static bool s_wifi_resume_connect_after_scan;
static bool s_wifi_pending_explicit;
static bool s_wifi_release_requested;
static bool s_wifi_reconfig_in_progress;
static uint32_t s_wifi_phase_started_ms;
static uint32_t s_wifi_attempt_started_ms;
static uint32_t s_wifi_associated_ms;
static bool s_wifi_dhcp_renewed;
static bool s_wifi_timeout_abort_pending;
/* Background recovery is non-real-time policy state. Keep it out of scarce
 * internal RAM; the Wi-Fi event and watchdog tasks only access it with cache on. */
EXT_RAM_BSS_ATTR static uint32_t s_wifi_background_recovery_due_ms;
static char s_wifi_saved_ssid[33];
static char s_wifi_saved_password[WIFI_PASSWORD_MAX_LEN + 1];
static char s_wifi_pending_ssid[33];
static char s_wifi_pending_password[WIFI_PASSWORD_MAX_LEN + 1];
static wifi_driver_config_t s_wifi_config = {
    .enabled = true,
};
static TaskHandle_t s_wifi_scan_task;
static TaskHandle_t s_wifi_connect_watchdog_task;
static portMUX_TYPE s_wifi_lock = portMUX_INITIALIZER_UNLOCKED;

typedef enum {
    WIFI_SCAN_RESUME_NONE = 0,
    WIFI_SCAN_RESUME_CONNECT,
} wifi_scan_resume_action_t;

static const char *wifi_disconnect_reason_name(uint8_t reason);
static wifi_connect_failure_t wifi_connect_failure_from_reason(uint8_t reason);
static void wifi_connect_watchdog_task(void *ctx);
static void wifi_scan_task(void *ctx);
static wifi_scan_resume_action_t wifi_finish_scan_state_locked(void);
static void wifi_resume_connect_if_needed(wifi_scan_resume_action_t action);

typedef enum {
    WIFI_WATCHDOG_ACTION_NONE = 0,
    WIFI_WATCHDOG_ACTION_RENEW_DHCP,
    WIFI_WATCHDOG_ACTION_ABORT_ATTEMPT,
    WIFI_WATCHDOG_ACTION_RETRY_ATTEMPT,
    WIFI_WATCHDOG_ACTION_BACKGROUND_RECOVERY,
} wifi_watchdog_action_t;

static uint32_t wifi_uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool wifi_deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return deadline_ms != 0U && (int32_t)(now_ms - deadline_ms) >= 0;
}

static bool wifi_failure_is_transient(wifi_connect_failure_t failure)
{
    return failure != WIFI_CONNECT_FAILURE_AUTHENTICATION;
}

static void wifi_arm_background_recovery_locked(uint32_t now_ms,
                                                wifi_connect_failure_t failure)
{
    if (s_wifi_status.started && s_wifi_status.configured &&
        !s_wifi_release_requested && wifi_failure_is_transient(failure)) {
        s_wifi_background_recovery_due_ms = now_ms + WIFI_BACKGROUND_RECOVERY_INTERVAL_MS;
    } else {
        s_wifi_background_recovery_due_ms = 0U;
    }
}

static uint32_t wifi_retry_backoff_ms(uint8_t retry_count)
{
    uint32_t backoff_ms = WIFI_RETRY_BACKOFF_BASE_MS;
    uint8_t shifts = retry_count > 0U ? (uint8_t)(retry_count - 1U) : 0U;

    while (shifts-- > 0U && backoff_ms < WIFI_RETRY_BACKOFF_MAX_MS) {
        backoff_ms <<= 1U;
    }
    return backoff_ms > WIFI_RETRY_BACKOFF_MAX_MS ?
           WIFI_RETRY_BACKOFF_MAX_MS :
           backoff_ms;
}

static const char *wifi_phase_name(wifi_connection_phase_t phase)
{
    switch (phase) {
    case WIFI_CONNECTION_PHASE_NO_CREDENTIALS:
        return "no-credentials";
    case WIFI_CONNECTION_PHASE_IDLE:
        return "idle";
    case WIFI_CONNECTION_PHASE_ASSOCIATING:
        return "associating";
    case WIFI_CONNECTION_PHASE_WAITING_IP:
        return "waiting-ip";
    case WIFI_CONNECTION_PHASE_IP_READY:
        return "ip-ready";
    case WIFI_CONNECTION_PHASE_RETRY_WAIT:
        return "retry-wait";
    case WIFI_CONNECTION_PHASE_FAILED:
        return "failed";
    case WIFI_CONNECTION_PHASE_STOPPED:
    default:
        return "stopped";
    }
}

static void wifi_set_phase_locked(wifi_connection_phase_t phase, uint32_t now_ms)
{
    s_wifi_status.phase = phase;
    s_wifi_phase_started_ms = now_ms;
    s_wifi_status.phase_elapsed_ms = 0;
}

static void wifi_begin_connect_attempt(void)
{
    const uint32_t now_ms = wifi_uptime_ms();

    taskENTER_CRITICAL(&s_wifi_lock);
    s_wifi_status.attempt_id++;
    if (s_wifi_status.attempt_id == 0U) {
        s_wifi_status.attempt_id = 1U;
    }
    s_wifi_status.associated = false;
    s_wifi_status.connected = false;
    s_wifi_status.association_time_ms = 0;
    s_wifi_status.dhcp_time_ms = 0;
    s_wifi_status.connect_failure = WIFI_CONNECT_FAILURE_NONE;
    s_wifi_attempt_started_ms = now_ms;
    s_wifi_associated_ms = 0;
    s_wifi_dhcp_renewed = false;
    s_wifi_timeout_abort_pending = false;
    s_wifi_background_recovery_due_ms = 0U;
    wifi_set_phase_locked(WIFI_CONNECTION_PHASE_ASSOCIATING, now_ms);
    taskEXIT_CRITICAL(&s_wifi_lock);
}

static void wifi_mark_connect_start_failed(esp_err_t error)
{
    const uint32_t now_ms = wifi_uptime_ms();

    taskENTER_CRITICAL(&s_wifi_lock);
    s_wifi_status.associated = false;
    s_wifi_status.connected = false;
    s_wifi_status.connect_failure = WIFI_CONNECT_FAILURE_OTHER;
    wifi_set_phase_locked(WIFI_CONNECTION_PHASE_FAILED, now_ms);
    wifi_arm_background_recovery_locked(now_ms, s_wifi_status.connect_failure);
    taskEXIT_CRITICAL(&s_wifi_lock);

    ESP_LOGW(TAG, "wifi connect start failed: %s", esp_err_to_name(error));
}

static esp_err_t wifi_start_connect_attempt(void)
{
    wifi_begin_connect_attempt();
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        wifi_mark_connect_start_failed(ret);
    }
    return ret;
}

static esp_err_t wifi_apply_fallback_dns(void)
{
    const char *address = s_wifi_config.fallback_dns_ipv4;
    esp_netif_dns_info_t dns = {0};

    if (address == NULL || address[0] == '\0') {
        return ESP_OK;
    }
    if (s_wifi_sta_netif == NULL || esp_netif_str_to_ip4(address, &dns.ip.u_addr.ip4) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    dns.ip.type = ESP_IPADDR_TYPE_V4;
    return esp_netif_set_dns_info(s_wifi_sta_netif, ESP_NETIF_DNS_FALLBACK, &dns);
}

static void wifi_format_dns_server(esp_netif_dns_type_t type, char *buffer, size_t buffer_size)
{
    esp_netif_dns_info_t dns = {0};

    if (buffer == NULL || buffer_size == 0U) {
        return;
    }
    strlcpy(buffer, "none", buffer_size);
    if (s_wifi_sta_netif == NULL ||
        esp_netif_get_dns_info(s_wifi_sta_netif, type, &dns) != ESP_OK ||
        dns.ip.type != ESP_IPADDR_TYPE_V4 ||
        dns.ip.u_addr.ip4.addr == 0U) {
        return;
    }
    snprintf(buffer, buffer_size, IPSTR, IP2STR(&dns.ip.u_addr.ip4));
}

static void wifi_log_dns_servers(void)
{
    char main_dns[16] = {0};
    char backup_dns[16] = {0};
    char fallback_dns[16] = {0};

    wifi_format_dns_server(ESP_NETIF_DNS_MAIN, main_dns, sizeof(main_dns));
    wifi_format_dns_server(ESP_NETIF_DNS_BACKUP, backup_dns, sizeof(backup_dns));
    wifi_format_dns_server(ESP_NETIF_DNS_FALLBACK, fallback_dns, sizeof(fallback_dns));
    ESP_LOGI(TAG,
             "wifi DNS ready: main=%s backup=%s fallback=%s",
             main_dns,
             backup_dns,
             fallback_dns);
}

static void wifi_sync_pending_with_saved(void)
{
    if (s_wifi_pending_explicit) {
        return;
    }

    strlcpy(s_wifi_pending_ssid, s_wifi_saved_ssid, sizeof(s_wifi_pending_ssid));
    strlcpy(s_wifi_pending_password, s_wifi_saved_password, sizeof(s_wifi_pending_password));
}

static void wifi_load_initial_saved_config(void)
{
    if (s_wifi_saved_ssid[0] != '\0') {
        return;
    }

    if (!s_wifi_config.enabled || s_wifi_config.default_ssid == NULL ||
        s_wifi_config.default_ssid[0] == '\0') {
        wifi_sync_pending_with_saved();
        return;
    }

    strlcpy(s_wifi_saved_ssid, s_wifi_config.default_ssid, sizeof(s_wifi_saved_ssid));
    if (s_wifi_config.default_password != NULL) {
        strlcpy(s_wifi_saved_password,
                s_wifi_config.default_password,
                sizeof(s_wifi_saved_password));
    }
    wifi_sync_pending_with_saved();
}

static void wifi_mark_connect_timeout_if_needed(void)
{
    wifi_watchdog_action_t action = WIFI_WATCHDOG_ACTION_NONE;
    wifi_connection_phase_t timed_out_phase = WIFI_CONNECTION_PHASE_STOPPED;
    uint32_t now_ms = wifi_uptime_ms();
    uint32_t phase_elapsed_ms = 0;
    uint32_t attempt_id = 0;
    uint8_t retry_number = 0;
    bool signal_failure = false;
    wifi_connect_failure_t terminal_failure = WIFI_CONNECT_FAILURE_NONE;
    char current_ssid[sizeof(s_wifi_status.ssid)] = {0};

    taskENTER_CRITICAL(&s_wifi_lock);
    if (s_wifi_status.started && s_wifi_status.configured && !s_wifi_status.connected) {
        phase_elapsed_ms = s_wifi_phase_started_ms != 0U ?
                           (uint32_t)(now_ms - s_wifi_phase_started_ms) :
                           0U;
        attempt_id = s_wifi_status.attempt_id;
        retry_number = s_wifi_status.retry_count;
        strlcpy(current_ssid, s_wifi_status.ssid, sizeof(current_ssid));

        if (s_wifi_status.phase == WIFI_CONNECTION_PHASE_ASSOCIATING &&
            phase_elapsed_ms >= WIFI_ASSOCIATION_TIMEOUT_MS) {
            timed_out_phase = s_wifi_status.phase;
            s_wifi_timeout_abort_pending = true;
            s_wifi_status.connect_failure = WIFI_CONNECT_FAILURE_TIMEOUT;
            wifi_set_phase_locked(WIFI_CONNECTION_PHASE_RETRY_WAIT, now_ms);
            action = WIFI_WATCHDOG_ACTION_ABORT_ATTEMPT;
        } else if (s_wifi_status.phase == WIFI_CONNECTION_PHASE_WAITING_IP &&
                   phase_elapsed_ms >= WIFI_DHCP_PHASE_TIMEOUT_MS) {
            timed_out_phase = s_wifi_status.phase;
            if (!s_wifi_dhcp_renewed) {
                s_wifi_dhcp_renewed = true;
                wifi_set_phase_locked(WIFI_CONNECTION_PHASE_WAITING_IP, now_ms);
                action = WIFI_WATCHDOG_ACTION_RENEW_DHCP;
            } else if (s_wifi_status.retry_count >= WIFI_DHCP_MAX_RETRIES) {
                s_wifi_status.connect_failure = WIFI_CONNECT_FAILURE_TIMEOUT;
                s_wifi_status.retry_count = WIFI_MAX_RETRIES;
                s_wifi_timeout_abort_pending = true;
                wifi_set_phase_locked(WIFI_CONNECTION_PHASE_RETRY_WAIT, now_ms);
                action = WIFI_WATCHDOG_ACTION_ABORT_ATTEMPT;
            } else {
                s_wifi_timeout_abort_pending = true;
                s_wifi_status.connect_failure = WIFI_CONNECT_FAILURE_TIMEOUT;
                wifi_set_phase_locked(WIFI_CONNECTION_PHASE_RETRY_WAIT, now_ms);
                action = WIFI_WATCHDOG_ACTION_ABORT_ATTEMPT;
            }
        } else if (s_wifi_status.phase == WIFI_CONNECTION_PHASE_RETRY_WAIT &&
                   phase_elapsed_ms >= wifi_retry_backoff_ms(s_wifi_status.retry_count)) {
            if (s_wifi_timeout_abort_pending) {
                s_wifi_timeout_abort_pending = false;
                if (s_wifi_status.retry_count < WIFI_MAX_RETRIES) {
                    s_wifi_status.retry_count++;
                    retry_number = s_wifi_status.retry_count;
                    action = WIFI_WATCHDOG_ACTION_RETRY_ATTEMPT;
                } else {
                    wifi_set_phase_locked(WIFI_CONNECTION_PHASE_FAILED, now_ms);
                    wifi_arm_background_recovery_locked(now_ms, s_wifi_status.connect_failure);
                    signal_failure = true;
                }
            } else if (s_wifi_status.retry_count > 0U &&
                       s_wifi_status.retry_count <= WIFI_MAX_RETRIES) {
                action = WIFI_WATCHDOG_ACTION_RETRY_ATTEMPT;
            } else {
                wifi_set_phase_locked(WIFI_CONNECTION_PHASE_FAILED, now_ms);
                wifi_arm_background_recovery_locked(now_ms, s_wifi_status.connect_failure);
                signal_failure = true;
            }
        } else if (s_wifi_status.phase == WIFI_CONNECTION_PHASE_FAILED &&
                   !s_wifi_scan_in_progress &&
                   !s_wifi_scan_deferred &&
                   !s_wifi_reconfig_in_progress &&
                   wifi_deadline_reached(now_ms, s_wifi_background_recovery_due_ms)) {
            s_wifi_background_recovery_due_ms = 0U;
            wifi_set_phase_locked(WIFI_CONNECTION_PHASE_RETRY_WAIT, now_ms);
            action = WIFI_WATCHDOG_ACTION_BACKGROUND_RECOVERY;
        }
    }
    if (signal_failure) {
        terminal_failure = s_wifi_status.connect_failure;
    }
    taskEXIT_CRITICAL(&s_wifi_lock);

    if (signal_failure) {
        ESP_LOGW(TAG,
                 "wifi retry budget exhausted: ssid=%s retries=%u attempt=%u",
                 current_ssid,
                 (unsigned)retry_number,
                 (unsigned)attempt_id);
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        if (wifi_failure_is_transient(terminal_failure)) {
            ESP_LOGI(TAG,
                     "wifi background recovery scheduled: interval_ms=%u",
                     (unsigned)WIFI_BACKGROUND_RECOVERY_INTERVAL_MS);
        }
    }

    if (action == WIFI_WATCHDOG_ACTION_RENEW_DHCP) {
        ESP_LOGW(TAG,
                 "wifi DHCP wait timeout, renewing lease: ssid=%s attempt=%u wait_ms=%u",
                 current_ssid,
                 (unsigned)attempt_id,
                 (unsigned)phase_elapsed_ms);
        esp_err_t stop_ret = esp_netif_dhcpc_stop(s_wifi_sta_netif);
        if (stop_ret != ESP_OK && stop_ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
            ESP_LOGW(TAG, "wifi DHCP stop failed: %s", esp_err_to_name(stop_ret));
        }
        esp_err_t start_ret = esp_netif_dhcpc_start(s_wifi_sta_netif);
        if (start_ret != ESP_OK && start_ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_LOGW(TAG, "wifi DHCP restart failed: %s", esp_err_to_name(start_ret));
        }
        return;
    }

    if (action == WIFI_WATCHDOG_ACTION_ABORT_ATTEMPT) {
        ESP_LOGW(TAG,
                 "wifi phase timeout: phase=%s ssid=%s attempt=%u wait_ms=%u",
                 wifi_phase_name(timed_out_phase),
                 current_ssid,
                 (unsigned)attempt_id,
                 (unsigned)phase_elapsed_ms);
        esp_err_t ret = esp_wifi_disconnect();
        if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_CONNECT) {
            ESP_LOGW(TAG, "wifi timeout abort failed: %s", esp_err_to_name(ret));
        }
        return;
    }

    if (action == WIFI_WATCHDOG_ACTION_RETRY_ATTEMPT) {
        ESP_LOGI(TAG,
                 "wifi retry begins: ssid=%s retry=%u/%u",
                 current_ssid,
                 (unsigned)retry_number,
                 (unsigned)WIFI_MAX_RETRIES);
        (void)wifi_start_connect_attempt();
        return;
    }

    if (action == WIFI_WATCHDOG_ACTION_BACKGROUND_RECOVERY) {
        ESP_LOGI(TAG,
                 "wifi background recovery begins: ssid=%s interval_ms=%u",
                 current_ssid,
                 (unsigned)WIFI_BACKGROUND_RECOVERY_INTERVAL_MS);
        esp_err_t ret = wifi_start_connect_attempt();
        if (ret != ESP_OK) {
            taskENTER_CRITICAL(&s_wifi_lock);
            wifi_arm_background_recovery_locked(wifi_uptime_ms(), s_wifi_status.connect_failure);
            taskEXIT_CRITICAL(&s_wifi_lock);
        }
    }
}

static void wifi_connect_watchdog_task(void *ctx)
{
    (void)ctx;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(WIFI_WATCHDOG_INTERVAL_MS));
        wifi_mark_connect_timeout_if_needed();
    }
}

static void wifi_apply_startup_sta_tuning(void)
{
    esp_err_t ret = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_STA_PROTOCOLS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wifi low latency protocol tuning failed: %s", esp_err_to_name(ret));
    }

    ret = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wifi ht40 bandwidth tuning failed: %s", esp_err_to_name(ret));
    }
}

static void wifi_disable_power_save(void)
{
    esp_err_t ret = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wifi power-save disable failed: %s", esp_err_to_name(ret));
    }

    ret = esp_wifi_set_max_tx_power(WIFI_LOW_LATENCY_TX_POWER_QDBM);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wifi max tx power tuning failed: %s", esp_err_to_name(ret));
    }
}

static bool wifi_has_saved_config(void)
{
    return s_wifi_saved_ssid[0] != '\0';
}

static bool wifi_has_pending_config(void)
{
    return s_wifi_pending_ssid[0] != '\0';
}

static esp_err_t wifi_load_saved_config(void)
{
    nvs_handle_t nvs_handle = 0;
    char saved_ssid[sizeof(s_wifi_saved_ssid)] = {0};
    char saved_password[sizeof(s_wifi_saved_password)] = {0};
    size_t ssid_len = sizeof(saved_ssid);
    size_t password_len = sizeof(saved_password);

    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_get_str(nvs_handle, WIFI_NVS_KEY_SSID, saved_ssid, &ssid_len);
    if (ret != ESP_OK || saved_ssid[0] == '\0') {
        nvs_close(nvs_handle);
        return ret == ESP_OK ? ESP_ERR_NVS_NOT_FOUND : ret;
    }

    ret = nvs_get_str(nvs_handle, WIFI_NVS_KEY_PASSWORD, saved_password, &password_len);
    nvs_close(nvs_handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        saved_password[0] = '\0';
        ret = ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    strlcpy(s_wifi_saved_ssid, saved_ssid, sizeof(s_wifi_saved_ssid));
    strlcpy(s_wifi_saved_password, saved_password, sizeof(s_wifi_saved_password));
    s_wifi_pending_explicit = false;
    wifi_sync_pending_with_saved();
    return ESP_OK;
}

static esp_err_t wifi_save_saved_config(void)
{
    nvs_handle_t nvs_handle = 0;
    char ssid[sizeof(s_wifi_saved_ssid)] = {0};
    char password[sizeof(s_wifi_saved_password)] = {0};

    taskENTER_CRITICAL(&s_wifi_lock);
    strlcpy(ssid, s_wifi_saved_ssid, sizeof(ssid));
    strlcpy(password, s_wifi_saved_password, sizeof(password));
    taskEXIT_CRITICAL(&s_wifi_lock);

    if (ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(nvs_handle, WIFI_NVS_KEY_SSID, ssid);
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs_handle, WIFI_NVS_KEY_PASSWORD, password);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);

    return ret;
}

static int wifi_compare_ap_records(const void *lhs, const void *rhs)
{
    const wifi_ap_record_t *left = (const wifi_ap_record_t *)lhs;
    const wifi_ap_record_t *right = (const wifi_ap_record_t *)rhs;

    if (left->rssi == right->rssi) {
        return strcmp((const char *)left->ssid, (const char *)right->ssid);
    }
    return right->rssi - left->rssi;
}

static uint8_t wifi_find_recent_channel_hint(const char *ssid)
{
    uint8_t channel = 0;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    if (ssid == NULL || ssid[0] == '\0') {
        return 0;
    }

    taskENTER_CRITICAL(&s_wifi_lock);
    if (!s_wifi_scan_snapshot.in_progress && s_wifi_scan_snapshot.count > 0 &&
        (uint32_t)(now_ms - s_wifi_scan_snapshot.last_scan_ms) <= WIFI_CHANNEL_HINT_MAX_AGE_MS) {
        for (uint16_t index = 0; index < s_wifi_scan_snapshot.count; ++index) {
            if (strcmp(s_wifi_scan_snapshot.results[index].ssid, ssid) == 0) {
                channel = s_wifi_scan_snapshot.results[index].channel;
                break;
            }
        }
    }
    taskEXIT_CRITICAL(&s_wifi_lock);

    return channel;
}

static void wifi_build_sta_config(wifi_config_t *wifi_cfg, const char *ssid, const char *password)
{
    uint8_t channel_hint = wifi_find_recent_channel_hint(ssid);

    *wifi_cfg = (wifi_config_t){
        .sta = {
            .scan_method = WIFI_FAST_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.authmode = WIFI_AUTH_OPEN,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };

    strlcpy((char *)wifi_cfg->sta.ssid, ssid != NULL ? ssid : "", sizeof(wifi_cfg->sta.ssid));
    strlcpy((char *)wifi_cfg->sta.password, password != NULL ? password : "", sizeof(wifi_cfg->sta.password));
    if (channel_hint > 0U) {
        wifi_cfg->sta.channel = channel_hint;
    }
}

static void wifi_refresh_rssi(void)
{
    if (!s_wifi_initialized) {
        return;
    }

    taskENTER_CRITICAL(&s_wifi_lock);
    bool connected = s_wifi_status.connected;
    if (!connected) {
        s_wifi_status.rssi = WIFI_INVALID_RSSI;
        taskEXIT_CRITICAL(&s_wifi_lock);
        return;
    }
    taskEXIT_CRITICAL(&s_wifi_lock);

    wifi_ap_record_t ap_info = {0};
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);

    taskENTER_CRITICAL(&s_wifi_lock);
    if (ret == ESP_OK) {
        s_wifi_status.rssi = ap_info.rssi;
    } else if (!s_wifi_status.connected) {
        s_wifi_status.rssi = WIFI_INVALID_RSSI;
    }
    taskEXIT_CRITICAL(&s_wifi_lock);
}

static wifi_scan_resume_action_t wifi_finish_scan_state_locked(void)
{
    bool wants_resume = s_wifi_resume_connect_after_scan &&
                        s_wifi_status.configured &&
                        !s_wifi_status.connected;
    wifi_scan_resume_action_t action = WIFI_SCAN_RESUME_NONE;

    if (wants_resume) {
        /* The UI snapshot is intentionally capped at ten APs. It must never be
         * used as proof that a configured AP does not exist. Let the station
         * driver's own connection scan make that decision. */
        s_wifi_status.retry_count = 0;
        s_wifi_status.disconnect_reason = 0;
        s_wifi_status.connect_failure = WIFI_CONNECT_FAILURE_NONE;
        action = WIFI_SCAN_RESUME_CONNECT;
    }

    s_wifi_scan_in_progress = false;
    s_wifi_scan_snapshot.in_progress = false;
    s_wifi_manual_scan_active = false;
    s_wifi_scan_deferred = false;
    s_wifi_resume_connect_after_scan = false;
    return action;
}

static void wifi_resume_connect_if_needed(wifi_scan_resume_action_t action)
{
    if (action == WIFI_SCAN_RESUME_NONE) {
        return;
    }

    const uint32_t now_ms = wifi_uptime_ms();
    taskENTER_CRITICAL(&s_wifi_lock);
    s_wifi_status.retry_count = 1U;
    wifi_set_phase_locked(WIFI_CONNECTION_PHASE_RETRY_WAIT, now_ms);
    taskEXIT_CRITICAL(&s_wifi_lock);
    ESP_LOGI(TAG, "wifi retry scheduled after scan: backoff_ms=%u",
             (unsigned)wifi_retry_backoff_ms(1U));
}

static const char *wifi_disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
        return "auth-response-timeout";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4way-timeout";
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return "auth-8021x-failed";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "beacon-timeout";
    case WIFI_REASON_NO_AP_FOUND:
        return "no-ap-found";
    case WIFI_REASON_AUTH_FAIL:
        return "auth-failed";
    case WIFI_REASON_ASSOC_FAIL:
        return "assoc-failed";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "handshake-timeout";
    case WIFI_REASON_CONNECTION_FAIL:
        return "connection-failed";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "no-compatible-security";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "authmode-threshold";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "rssi-threshold";
    default:
        return "unknown";
    }
}

static wifi_connect_failure_t wifi_connect_failure_from_reason(uint8_t reason)
{
    switch (reason) {
    case 0:
        return WIFI_CONNECT_FAILURE_NONE;
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return WIFI_CONNECT_FAILURE_AP_NOT_FOUND;
    case WIFI_REASON_802_1X_AUTH_FAILED:
    case WIFI_REASON_AUTH_FAIL:
        return WIFI_CONNECT_FAILURE_AUTHENTICATION;
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_ASSOC_NOT_AUTHED:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        /* A weak or lossy link can time out authentication or the key
         * handshake. Only explicit AUTH_FAIL/802.1X failure is allowed to
         * become the user-facing "wrong password" result. */
        return WIFI_CONNECT_FAILURE_ASSOCIATION;
    case WIFI_REASON_ASSOC_TOOMANY:
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG:
        return WIFI_CONNECT_FAILURE_ASSOCIATION;
    default:
        return WIFI_CONNECT_FAILURE_OTHER;
    }
}

static const char *wifi_event_name(int32_t event_id)
{
    switch (event_id) {
    case WIFI_EVENT_STA_START:
        return "STA_START";
    case WIFI_EVENT_STA_STOP:
        return "STA_STOP";
    case WIFI_EVENT_STA_CONNECTED:
        return "STA_CONNECTED";
    case WIFI_EVENT_STA_DISCONNECTED:
        return "STA_DISCONNECTED";
    case WIFI_EVENT_SCAN_DONE:
        return "SCAN_DONE";
    default:
        return "OTHER";
    }
}

static esp_err_t wifi_start_blocking_scan(wifi_scan_config_t *scan_cfg, bool log_results)
{
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);

    esp_err_t ret = esp_wifi_scan_start(scan_cfg, false);
    if (ret == ESP_ERR_WIFI_STATE) {
        vTaskDelay(pdMS_TO_TICKS(150));
        xEventGroupClearBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
        ret = esp_wifi_scan_start(scan_cfg, false);
    }
    if (ret != ESP_OK && log_results) {
        ESP_LOGW(TAG, "wifi scan start failed: %s", esp_err_to_name(ret));
    }

    if (ret != ESP_OK) {
        return ret;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_SCAN_DONE_BIT,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(WIFI_SCAN_WAIT_MS));
    if ((bits & WIFI_SCAN_DONE_BIT) == 0) {
        (void)esp_wifi_scan_stop();
        ESP_LOGW(TAG, "wifi scan timeout: wait_ms=%u", (unsigned)WIFI_SCAN_WAIT_MS);
        return ESP_ERR_TIMEOUT;
    }

    return ret;
}

static esp_err_t wifi_scan_now(bool log_results)
{
    wifi_scan_config_t scan_cfg = {
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .show_hidden = false,
        .scan_time.active.min = WIFI_SCAN_ACTIVE_MIN_MS,
        .scan_time.active.max = WIFI_SCAN_ACTIVE_MAX_MS,
    };
    wifi_scan_config_t passive_scan_cfg = {
        .scan_type = WIFI_SCAN_TYPE_PASSIVE,
        .show_hidden = false,
        .scan_time.passive = WIFI_SCAN_PASSIVE_MS,
    };
    wifi_ap_record_t ap_records[WIFI_SCAN_RESULT_MAX] = {0};
    uint16_t ap_count = 0;
    uint16_t fetch_count = WIFI_SCAN_RESULT_MAX;
    wifi_scan_resume_action_t resume_connect = WIFI_SCAN_RESUME_NONE;

    taskENTER_CRITICAL(&s_wifi_lock);
    s_wifi_scan_in_progress = true;
    s_wifi_scan_snapshot.in_progress = true;
    s_wifi_manual_scan_active = true;
    s_wifi_resume_connect_after_scan = s_wifi_status.configured && !s_wifi_status.connected;
    taskEXIT_CRITICAL(&s_wifi_lock);

    esp_err_t ret = wifi_start_blocking_scan(&scan_cfg, log_results);
    if (ret != ESP_OK) {
        taskENTER_CRITICAL(&s_wifi_lock);
        resume_connect = wifi_finish_scan_state_locked();
        taskEXIT_CRITICAL(&s_wifi_lock);
        wifi_resume_connect_if_needed(resume_connect);
        return ret;
    }

    ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (ret != ESP_OK) {
        taskENTER_CRITICAL(&s_wifi_lock);
        resume_connect = wifi_finish_scan_state_locked();
        taskEXIT_CRITICAL(&s_wifi_lock);
        wifi_resume_connect_if_needed(resume_connect);
        if (log_results) {
            ESP_LOGW(TAG, "wifi scan ap count failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    if (ap_count == 0) {
        ret = wifi_start_blocking_scan(&passive_scan_cfg, log_results);
        if (ret == ESP_OK) {
            ret = esp_wifi_scan_get_ap_num(&ap_count);
        }
    }

    if (ret != ESP_OK) {
        taskENTER_CRITICAL(&s_wifi_lock);
        resume_connect = wifi_finish_scan_state_locked();
        taskEXIT_CRITICAL(&s_wifi_lock);
        wifi_resume_connect_if_needed(resume_connect);
        if (log_results) {
            ESP_LOGW(TAG, "wifi passive scan failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    if (ap_count == 0) {
        taskENTER_CRITICAL(&s_wifi_lock);
        s_wifi_scan_snapshot.count = 0;
        s_wifi_scan_snapshot.last_scan_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        resume_connect = wifi_finish_scan_state_locked();
        taskEXIT_CRITICAL(&s_wifi_lock);
        wifi_resume_connect_if_needed(resume_connect);
        return ESP_OK;
    }

    if (ap_count < fetch_count) {
        fetch_count = ap_count;
    }

    ret = esp_wifi_scan_get_ap_records(&fetch_count, ap_records);
    if (ret != ESP_OK) {
        taskENTER_CRITICAL(&s_wifi_lock);
        resume_connect = wifi_finish_scan_state_locked();
        taskEXIT_CRITICAL(&s_wifi_lock);
        wifi_resume_connect_if_needed(resume_connect);
        if (log_results) {
            ESP_LOGW(TAG, "wifi scan readback failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    qsort(ap_records, fetch_count, sizeof(ap_records[0]), wifi_compare_ap_records);

    taskENTER_CRITICAL(&s_wifi_lock);
    memset(&s_wifi_scan_snapshot.results, 0, sizeof(s_wifi_scan_snapshot.results));
    s_wifi_scan_snapshot.count = 0;
    s_wifi_scan_snapshot.last_scan_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    for (uint16_t index = 0; index < fetch_count; ++index) {
        bool duplicate_ssid = false;

        if (ap_records[index].ssid[0] == '\0') {
            continue;
        }

        for (uint16_t existing = 0; existing < s_wifi_scan_snapshot.count; ++existing) {
            if (strcmp(s_wifi_scan_snapshot.results[existing].ssid, (const char *)ap_records[index].ssid) == 0) {
                duplicate_ssid = true;
                break;
            }
        }
        if (duplicate_ssid) {
            continue;
        }

        wifi_scan_result_t *dst = &s_wifi_scan_snapshot.results[s_wifi_scan_snapshot.count++];
        strlcpy(dst->ssid, (const char *)ap_records[index].ssid, sizeof(dst->ssid));
        dst->rssi = ap_records[index].rssi;
        dst->authmode = (uint8_t)ap_records[index].authmode;
        dst->channel = ap_records[index].primary;
        dst->secure = ap_records[index].authmode != WIFI_AUTH_OPEN;

        if (s_wifi_scan_snapshot.count >= WIFI_SCAN_RESULT_MAX) {
            break;
        }
    }
    resume_connect = wifi_finish_scan_state_locked();
    taskEXIT_CRITICAL(&s_wifi_lock);
    wifi_resume_connect_if_needed(resume_connect);

    (void)log_results;
    return ESP_OK;
}

static bool wifi_is_configured(void)
{
    return wifi_has_pending_config() || wifi_has_saved_config();
}

static void wifi_copy_status(wifi_status_t *status)
{
    const uint32_t now_ms = wifi_uptime_ms();

    taskENTER_CRITICAL(&s_wifi_lock);
    *status = s_wifi_status;
    status->phase_elapsed_ms = s_wifi_phase_started_ms != 0U ?
                               (uint32_t)(now_ms - s_wifi_phase_started_ms) :
                               0U;
    taskEXIT_CRITICAL(&s_wifi_lock);
}

static void wifi_event_handler(void *arg,
                                        esp_event_base_t event_base,
                                        int32_t event_id,
                                        void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        const uint32_t now_ms = wifi_uptime_ms();
        taskENTER_CRITICAL(&s_wifi_lock);
        s_wifi_status.started = true;
        bool should_connect = wifi_has_pending_config();
        if (!should_connect) {
            wifi_set_phase_locked(WIFI_CONNECTION_PHASE_NO_CREDENTIALS, now_ms);
        } else if (s_wifi_status.phase != WIFI_CONNECTION_PHASE_ASSOCIATING &&
                   s_wifi_status.phase != WIFI_CONNECTION_PHASE_WAITING_IP &&
                   s_wifi_status.phase != WIFI_CONNECTION_PHASE_IP_READY) {
            wifi_set_phase_locked(WIFI_CONNECTION_PHASE_IDLE, now_ms);
        }
        char current_ssid[sizeof(s_wifi_status.ssid)] = {0};
        strlcpy(current_ssid, s_wifi_status.ssid, sizeof(current_ssid));
        taskEXIT_CRITICAL(&s_wifi_lock);

        ESP_LOGI(TAG, "wifi event: %s configured=%d ssid=%s", wifi_event_name(event_id), should_connect, current_ssid);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *connected =
            (const wifi_event_sta_connected_t *)event_data;
        const uint32_t now_ms = wifi_uptime_ms();
        uint32_t association_time_ms = 0;
        uint32_t attempt_id = 0;
        uint8_t channel = connected != NULL ? connected->channel : 0U;
        bool accepted = false;
        char current_ssid[sizeof(s_wifi_status.ssid)] = {0};

        taskENTER_CRITICAL(&s_wifi_lock);
        if (s_wifi_status.started && s_wifi_status.configured && !s_wifi_release_requested) {
            accepted = true;
            s_wifi_status.associated = true;
            s_wifi_status.connected = false;
            s_wifi_status.disconnect_reason = 0;
            s_wifi_status.connect_failure = WIFI_CONNECT_FAILURE_NONE;
            association_time_ms = s_wifi_attempt_started_ms != 0U ?
                                  (uint32_t)(now_ms - s_wifi_attempt_started_ms) :
                                  0U;
            s_wifi_status.association_time_ms = association_time_ms;
            s_wifi_associated_ms = now_ms;
            s_wifi_dhcp_renewed = false;
            s_wifi_timeout_abort_pending = false;
            wifi_set_phase_locked(WIFI_CONNECTION_PHASE_WAITING_IP, now_ms);
            attempt_id = s_wifi_status.attempt_id;
            strlcpy(current_ssid, s_wifi_status.ssid, sizeof(current_ssid));
        }
        taskEXIT_CRITICAL(&s_wifi_lock);

        if (accepted) {
            ESP_LOGI(TAG,
                     "wifi associated: ssid=%s channel=%u attempt=%u association_ms=%u, waiting for DHCP",
                     current_ssid,
                     (unsigned)channel,
                     (unsigned)attempt_id,
                     (unsigned)association_time_ms);
        } else {
            ESP_LOGW(TAG, "stale wifi associated event ignored");
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
        }
        ESP_LOGI(TAG, "wifi event: %s", wifi_event_name(event_id));
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected = (const wifi_event_sta_disconnected_t *)event_data;
        const uint32_t now_ms = wifi_uptime_ms();
        uint8_t retry_count = 0;
        bool should_reconnect = false;
        bool manual_scan_active = false;
        bool configured = false;
        bool release_requested = false;
        bool reconfig_in_progress = false;
        bool start_deferred_scan = false;
        bool timeout_abort = false;
        bool signal_failure = false;
        wifi_connect_failure_t failure = WIFI_CONNECT_FAILURE_OTHER;
        char current_ssid[sizeof(s_wifi_status.ssid)] = {0};

        taskENTER_CRITICAL(&s_wifi_lock);
        timeout_abort = s_wifi_timeout_abort_pending;
        s_wifi_timeout_abort_pending = false;
        failure = timeout_abort ?
                  WIFI_CONNECT_FAILURE_TIMEOUT :
                  wifi_connect_failure_from_reason(disconnected->reason);
        s_wifi_status.associated = false;
        s_wifi_status.connected = false;
        s_wifi_status.disconnect_reason = disconnected->reason;
        s_wifi_status.connect_failure = failure;
        s_wifi_status.rssi = WIFI_INVALID_RSSI;
        s_wifi_status.ip_addr[0] = '\0';
        s_wifi_associated_ms = 0;
        s_wifi_dhcp_renewed = false;
        manual_scan_active = s_wifi_manual_scan_active || s_wifi_scan_in_progress || s_wifi_scan_deferred;
        configured = s_wifi_status.configured;
        release_requested = s_wifi_release_requested || !s_wifi_status.started;
        reconfig_in_progress = s_wifi_reconfig_in_progress;
        strlcpy(current_ssid, s_wifi_status.ssid, sizeof(current_ssid));
        if (s_wifi_scan_deferred && !s_wifi_scan_in_progress) {
            s_wifi_scan_deferred = false;
            s_wifi_scan_in_progress = true;
            s_wifi_scan_snapshot.in_progress = true;
            start_deferred_scan = true;
        }
        if (release_requested) {
            s_wifi_background_recovery_due_ms = 0U;
            wifi_set_phase_locked(WIFI_CONNECTION_PHASE_STOPPED, now_ms);
        } else if (reconfig_in_progress || manual_scan_active) {
            s_wifi_background_recovery_due_ms = 0U;
            wifi_set_phase_locked(WIFI_CONNECTION_PHASE_IDLE, now_ms);
        } else if (s_wifi_status.configured &&
                   failure != WIFI_CONNECT_FAILURE_AUTHENTICATION &&
                   s_wifi_status.retry_count < WIFI_MAX_RETRIES) {
            if (s_wifi_status.retry_count < UINT8_MAX) {
                s_wifi_status.retry_count++;
            }
            retry_count = s_wifi_status.retry_count;
            should_reconnect = true;
            s_wifi_background_recovery_due_ms = 0U;
            wifi_set_phase_locked(WIFI_CONNECTION_PHASE_RETRY_WAIT, now_ms);
        } else if (s_wifi_status.configured) {
            if (failure == WIFI_CONNECT_FAILURE_AUTHENTICATION) {
                s_wifi_status.retry_count = WIFI_MAX_RETRIES;
            }
            retry_count = s_wifi_status.retry_count;
            wifi_set_phase_locked(WIFI_CONNECTION_PHASE_FAILED, now_ms);
            wifi_arm_background_recovery_locked(now_ms, failure);
            signal_failure = true;
        } else {
            s_wifi_background_recovery_due_ms = 0U;
            wifi_set_phase_locked(WIFI_CONNECTION_PHASE_NO_CREDENTIALS, now_ms);
        }
        taskEXIT_CRITICAL(&s_wifi_lock);

        if (manual_scan_active) {
            if (start_deferred_scan && s_wifi_scan_task != NULL) {
                ESP_LOGI(TAG,
                         "wifi scan starts after connect ended: ssid=%s reason=%u(%s)",
                         current_ssid,
                         (unsigned)disconnected->reason,
                         wifi_disconnect_reason_name(disconnected->reason));
                xTaskNotifyGive(s_wifi_scan_task);
            }
            return;
        }

        if (release_requested) {
            ESP_LOGI(TAG, "wifi disconnected for release: ssid=%s", current_ssid);
            return;
        }

        if (reconfig_in_progress) {
            ESP_LOGI(TAG, "wifi disconnected for reconfig: ssid=%s", current_ssid);
            return;
        }

        if (configured) {
            ESP_LOGW(TAG,
                     "wifi disconnected: ssid=%s reason=%u(%s) retry=%u/%u",
                     current_ssid,
                     (unsigned)disconnected->reason,
                     wifi_disconnect_reason_name(disconnected->reason),
                     (unsigned)retry_count,
                     (unsigned)WIFI_MAX_RETRIES);
        }

        if (should_reconnect) {
            ESP_LOGI(TAG,
                     "wifi retry scheduled: retry=%u/%u backoff_ms=%u reason=%u",
                     (unsigned)retry_count,
                     (unsigned)WIFI_MAX_RETRIES,
                     (unsigned)wifi_retry_backoff_ms(retry_count),
                     (unsigned)disconnected->reason);
            return;
        }

        if (configured) {
            ESP_LOGW(TAG,
                     "wifi reconnect stopped: attempts=%u reason=%u",
                     (unsigned)WIFI_MAX_RETRIES,
                     (unsigned)disconnected->reason);
            if (wifi_failure_is_transient(failure)) {
                ESP_LOGI(TAG,
                         "wifi background recovery scheduled: interval_ms=%u",
                         (unsigned)WIFI_BACKGROUND_RECOVERY_INTERVAL_MS);
            }
        }
        if (signal_failure && s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        return;
    }

    if (event_base == WIFI_EVENT) {
        ESP_LOGD(TAG, "wifi event: %s(%ld)", wifi_event_name(event_id), (long)event_id);
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *got_ip = (const ip_event_got_ip_t *)event_data;
        const uint32_t now_ms = wifi_uptime_ms();
        char connected_ssid[sizeof(s_wifi_status.ssid)] = {0};
        char connected_ip[sizeof(s_wifi_status.ip_addr)] = {0};
        bool start_deferred_scan = false;
        bool accept_ip = false;
        uint32_t attempt_id = 0;
        uint32_t association_time_ms = 0;
        uint32_t dhcp_time_ms = 0;
        uint32_t total_time_ms = 0;

        esp_err_t dns_ret = wifi_apply_fallback_dns();
        if (dns_ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "wifi fallback DNS apply failed: address=%s ret=%s",
                     s_wifi_config.fallback_dns_ipv4 != NULL ? s_wifi_config.fallback_dns_ipv4 : "",
                     esp_err_to_name(dns_ret));
        }
        wifi_log_dns_servers();

        taskENTER_CRITICAL(&s_wifi_lock);
        accept_ip = s_wifi_status.started && s_wifi_status.configured && !s_wifi_release_requested;
        if (accept_ip) {
            strlcpy(s_wifi_saved_ssid, s_wifi_pending_ssid, sizeof(s_wifi_saved_ssid));
            strlcpy(s_wifi_saved_password, s_wifi_pending_password, sizeof(s_wifi_saved_password));
            s_wifi_pending_explicit = false;
            s_wifi_status.associated = true;
            s_wifi_status.connected = true;
            s_wifi_status.retry_count = 0;
            s_wifi_status.disconnect_reason = 0;
            s_wifi_status.connect_failure = WIFI_CONNECT_FAILURE_NONE;
            association_time_ms = s_wifi_status.association_time_ms;
            dhcp_time_ms = s_wifi_associated_ms != 0U ?
                           (uint32_t)(now_ms - s_wifi_associated_ms) :
                           0U;
            total_time_ms = s_wifi_attempt_started_ms != 0U ?
                            (uint32_t)(now_ms - s_wifi_attempt_started_ms) :
                            0U;
            s_wifi_status.dhcp_time_ms = dhcp_time_ms;
            s_wifi_timeout_abort_pending = false;
            s_wifi_background_recovery_due_ms = 0U;
            wifi_set_phase_locked(WIFI_CONNECTION_PHASE_IP_READY, now_ms);
            snprintf(s_wifi_status.ip_addr,
                     sizeof(s_wifi_status.ip_addr),
                     IPSTR,
                     IP2STR(&got_ip->ip_info.ip));
            strlcpy(connected_ssid, s_wifi_status.ssid, sizeof(connected_ssid));
            strlcpy(connected_ip, s_wifi_status.ip_addr, sizeof(connected_ip));
            attempt_id = s_wifi_status.attempt_id;
            if (s_wifi_scan_deferred && !s_wifi_scan_in_progress) {
                s_wifi_scan_deferred = false;
                s_wifi_scan_in_progress = true;
                s_wifi_scan_snapshot.in_progress = true;
                start_deferred_scan = true;
            }
        }
        taskEXIT_CRITICAL(&s_wifi_lock);
        if (!accept_ip) {
            ESP_LOGW(TAG, "stale wifi got-ip event ignored");
            return;
        }
        wifi_refresh_rssi();

        esp_err_t save_ret = wifi_save_saved_config();
        if (save_ret != ESP_OK) {
            ESP_LOGW(TAG, "save wifi credentials failed: %s", esp_err_to_name(save_ret));
        }
        wifi_sync_pending_with_saved();

        ESP_LOGI(TAG,
                 "wifi connected: ssid=%s ip=%s attempt=%u association_ms=%u dhcp_ms=%u total_ms=%u",
                 connected_ssid,
                 connected_ip,
                 (unsigned)attempt_id,
                 (unsigned)association_time_ms,
                 (unsigned)dhcp_time_ms,
                 (unsigned)total_time_ms);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (start_deferred_scan && s_wifi_scan_task != NULL) {
            ESP_LOGI(TAG, "wifi scan starts after connection established");
            xTaskNotifyGive(s_wifi_scan_task);
        }
    }
}

static esp_err_t wifi_nvs_init(void)
{
    return platform_storage_init();
}

static void wifi_scan_task(void *ctx)
{
    (void)ctx;

    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        bool can_scan = s_wifi_status.started;

        if (!can_scan) {
            taskENTER_CRITICAL(&s_wifi_lock);
            s_wifi_scan_in_progress = false;
            s_wifi_scan_snapshot.in_progress = false;
            taskEXIT_CRITICAL(&s_wifi_lock);
            continue;
        }
        (void)wifi_scan_now(false);
    }
}

esp_err_t wifi_prepare(const wifi_driver_config_t *config)
{
    bool started = false;

    if (config != NULL) {
        s_wifi_config = *config;
    }

    if (s_wifi_initialized) {
        if (!s_wifi_config.enabled) {
            return ESP_OK;
        }

        taskENTER_CRITICAL(&s_wifi_lock);
        started = s_wifi_status.started;
        taskEXIT_CRITICAL(&s_wifi_lock);
        if (started) {
            return ESP_OK;
        }

        wifi_sync_pending_with_saved();
        wifi_config_t wifi_cfg = {0};
        if (wifi_is_configured()) {
            wifi_build_sta_config(&wifi_cfg, s_wifi_pending_ssid, s_wifi_pending_password);
        }

        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "wifi config restore failed");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi restart failed");
        wifi_disable_power_save();

        taskENTER_CRITICAL(&s_wifi_lock);
        s_wifi_release_requested = false;
        s_wifi_reconfig_in_progress = false;
        s_wifi_status.started = true;
        s_wifi_status.configured = wifi_is_configured();
        s_wifi_status.associated = false;
        s_wifi_status.connected = false;
        s_wifi_status.retry_count = 0;
        s_wifi_status.disconnect_reason = 0;
        s_wifi_status.connect_failure = WIFI_CONNECT_FAILURE_NONE;
        s_wifi_status.rssi = WIFI_INVALID_RSSI;
        s_wifi_status.ip_addr[0] = '\0';
        s_wifi_timeout_abort_pending = false;
        s_wifi_attempt_started_ms = 0;
        s_wifi_associated_ms = 0;
        s_wifi_dhcp_renewed = false;
        s_wifi_background_recovery_due_ms = 0U;
        wifi_set_phase_locked(s_wifi_status.configured ?
                                  WIFI_CONNECTION_PHASE_IDLE :
                                  WIFI_CONNECTION_PHASE_NO_CREDENTIALS,
                              wifi_uptime_ms());
        strlcpy(s_wifi_status.ssid, s_wifi_pending_ssid, sizeof(s_wifi_status.ssid));
        taskEXIT_CRITICAL(&s_wifi_lock);

        if (!wifi_is_configured()) {
            ESP_LOGI(TAG, "wifi restarted: no credentials, scan mode");
            return ESP_OK;
        }

        if (!s_wifi_config.auto_connect) {
            ESP_LOGI(TAG, "wifi restarted: credentials saved ssid=%s auto_connect=0", s_wifi_status.ssid);
            return ESP_OK;
        }

        ESP_LOGI(TAG, "wifi reconnecting: ssid=%s", s_wifi_status.ssid);
        esp_err_t connect_ret = wifi_start_connect_attempt();
        if (connect_ret != ESP_OK) {
            ESP_LOGW(TAG, "wifi reconnect start failed: %s", esp_err_to_name(connect_ret));
            if (s_wifi_event_group != NULL) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
            return connect_ret;
        }
        return ESP_OK;
    }

    bool seed_default_credentials = false;

    esp_log_level_set("wifi", ESP_LOG_ERROR);
    wifi_load_initial_saved_config();

    if (!s_wifi_config.enabled) {
        ESP_LOGW(TAG, "wifi disabled by configuration");
        taskENTER_CRITICAL(&s_wifi_lock);
        memset(&s_wifi_status, 0, sizeof(s_wifi_status));
        s_wifi_background_recovery_due_ms = 0U;
        s_wifi_status.phase = WIFI_CONNECTION_PHASE_STOPPED;
        s_wifi_status.rssi = WIFI_INVALID_RSSI;
        taskEXIT_CRITICAL(&s_wifi_lock);
        s_wifi_initialized = true;
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(wifi_nvs_init(), TAG, "nvs init failed");

    if (!s_wifi_pending_explicit) {
        esp_err_t load_ret = wifi_load_saved_config();
        if (load_ret != ESP_OK && load_ret != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "load saved wifi credentials failed: %s", esp_err_to_name(load_ret));
        } else if (load_ret == ESP_ERR_NVS_NOT_FOUND && wifi_has_saved_config()) {
            seed_default_credentials = true;
        }
    }

    if (seed_default_credentials) {
        esp_err_t save_ret = wifi_save_saved_config();
        if (save_ret != ESP_OK) {
            ESP_LOGW(TAG, "seed default wifi credentials failed: %s", esp_err_to_name(save_ret));
        }
    }

    wifi_sync_pending_with_saved();

    taskENTER_CRITICAL(&s_wifi_lock);
    s_wifi_status.configured = wifi_is_configured();
    s_wifi_status.started = false;
    s_wifi_status.associated = false;
    s_wifi_status.connected = false;
    s_wifi_status.retry_count = 0;
    s_wifi_status.disconnect_reason = 0;
    s_wifi_status.connect_failure = WIFI_CONNECT_FAILURE_NONE;
    s_wifi_status.rssi = WIFI_INVALID_RSSI;
    s_wifi_status.ip_addr[0] = '\0';
    s_wifi_timeout_abort_pending = false;
    s_wifi_attempt_started_ms = 0;
    s_wifi_associated_ms = 0;
    s_wifi_dhcp_renewed = false;
    s_wifi_background_recovery_due_ms = 0U;
    wifi_set_phase_locked(WIFI_CONNECTION_PHASE_STOPPED, wifi_uptime_ms());
    strlcpy(s_wifi_status.ssid, s_wifi_pending_ssid, sizeof(s_wifi_status.ssid));
    s_wifi_release_requested = false;
    s_wifi_reconfig_in_progress = false;
    taskEXIT_CRITICAL(&s_wifi_lock);

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret == ESP_OK) {
        s_wifi_event_loop_ready = true;
    } else if (ret == ESP_ERR_INVALID_STATE) {
        s_wifi_event_loop_ready = true;
    } else {
        return ret;
    }

    s_wifi_event_group = xEventGroupCreateWithCaps(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_wifi_event_group != NULL, ESP_ERR_NO_MEM, TAG, "wifi event group alloc failed");

    s_wifi_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_wifi_sta_netif != NULL,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "wifi station netif create failed");
    ESP_RETURN_ON_ERROR(wifi_apply_fallback_dns(), TAG, "wifi fallback DNS configure failed");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL),
                        TAG,
                        "register wifi event handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL),
                        TAG,
                        "register ip event handler failed");

    wifi_config_t wifi_cfg = {0};
    if (wifi_is_configured()) {
        wifi_build_sta_config(&wifi_cfg, s_wifi_pending_ssid, s_wifi_pending_password);
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode set failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage set failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "wifi config set failed");
    wifi_apply_startup_sta_tuning();
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
    wifi_disable_power_save();

    taskENTER_CRITICAL(&s_wifi_lock);
    s_wifi_status.started = true;
    wifi_set_phase_locked(s_wifi_status.configured ?
                              WIFI_CONNECTION_PHASE_IDLE :
                              WIFI_CONNECTION_PHASE_NO_CREDENTIALS,
                          wifi_uptime_ms());
    taskEXIT_CRITICAL(&s_wifi_lock);
    s_wifi_initialized = true;

    if (s_wifi_scan_task == NULL) {
        BaseType_t task_ok = xTaskCreatePinnedToCoreWithCaps(wifi_scan_task,
                                                             "wifi_scan",
                                                             8 * 1024,
                                                             NULL,
                                                             WIFI_SCAN_TASK_PRIORITY,
                                                             &s_wifi_scan_task,
                                                             WIFI_SCAN_TASK_CORE,
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(task_ok == pdPASS, ESP_ERR_NO_MEM, TAG, "wifi scan task alloc failed");
    }

    if (s_wifi_connect_watchdog_task == NULL) {
        BaseType_t watchdog_ok = xTaskCreatePinnedToCoreWithCaps(wifi_connect_watchdog_task,
                                                                 "wifi_watchdog",
                                                                 6 * 1024,
                                                                 NULL,
                                                                 WIFI_SCAN_TASK_PRIORITY,
                                                                 &s_wifi_connect_watchdog_task,
                                                                 WIFI_SCAN_TASK_CORE,
                                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(watchdog_ok == pdPASS,
                            ESP_ERR_NO_MEM,
                            TAG,
                            "wifi watchdog task alloc failed");
    }

    if (!wifi_is_configured()) {
        ESP_LOGI(TAG, "wifi ready: no credentials, scan mode");
        return ESP_OK;
    }

    if (!s_wifi_config.auto_connect) {
        ESP_LOGI(TAG, "wifi ready: credentials saved ssid=%s auto_connect=0", s_wifi_status.ssid);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "wifi connecting: ssid=%s", s_wifi_status.ssid);
    esp_err_t connect_ret = wifi_start_connect_attempt();
    if (connect_ret != ESP_OK) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        return connect_ret;
    }
    return ESP_OK;
}

void wifi_release(void)
{
    bool started = false;

    if (!s_wifi_initialized || !s_wifi_config.enabled) {
        return;
    }

    taskENTER_CRITICAL(&s_wifi_lock);
    started = s_wifi_status.started;
    s_wifi_release_requested = true;
    s_wifi_scan_in_progress = false;
    s_wifi_scan_snapshot.in_progress = false;
    s_wifi_manual_scan_active = false;
    s_wifi_scan_deferred = false;
    s_wifi_resume_connect_after_scan = false;
    s_wifi_reconfig_in_progress = false;
    s_wifi_timeout_abort_pending = false;
    s_wifi_status.associated = false;
    s_wifi_status.connected = false;
    s_wifi_status.retry_count = 0;
    s_wifi_status.disconnect_reason = 0;
    s_wifi_status.connect_failure = WIFI_CONNECT_FAILURE_NONE;
    s_wifi_status.rssi = WIFI_INVALID_RSSI;
    s_wifi_status.ip_addr[0] = '\0';
    s_wifi_attempt_started_ms = 0;
    s_wifi_associated_ms = 0;
    s_wifi_dhcp_renewed = false;
    s_wifi_background_recovery_due_ms = 0U;
    wifi_set_phase_locked(WIFI_CONNECTION_PHASE_STOPPED, wifi_uptime_ms());
    taskEXIT_CRITICAL(&s_wifi_lock);

    if (!started) {
        return;
    }

    (void)esp_wifi_scan_stop();
    esp_err_t disconnect_ret = esp_wifi_disconnect();
    if (disconnect_ret != ESP_OK && disconnect_ret != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "wifi release disconnect failed: %s", esp_err_to_name(disconnect_ret));
    }

    esp_err_t stop_ret = esp_wifi_stop();
    if (stop_ret != ESP_OK && stop_ret != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "wifi release stop failed: %s", esp_err_to_name(stop_ret));
    }

    taskENTER_CRITICAL(&s_wifi_lock);
    s_wifi_status.started = false;
    s_wifi_status.connected = false;
    s_wifi_status.rssi = WIFI_INVALID_RSSI;
    s_wifi_status.ip_addr[0] = '\0';
    s_wifi_reconfig_in_progress = false;
    taskEXIT_CRITICAL(&s_wifi_lock);
    ESP_LOGI(TAG, "wifi released");
}

esp_err_t wifi_connect(const char *ssid, const char *password)
{
    if (!s_wifi_config.enabled) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_RETURN_ON_FALSE(ssid != NULL, ESP_ERR_INVALID_ARG, TAG, "ssid is required");
    ESP_RETURN_ON_FALSE(strlen(ssid) > 0, ESP_ERR_INVALID_ARG, TAG, "ssid is empty");

    char next_ssid[sizeof(s_wifi_pending_ssid)] = {0};
    char next_password[sizeof(s_wifi_pending_password)] = {0};
    strlcpy(next_ssid, ssid, sizeof(next_ssid));
    if (password != NULL && password[0] != '\0') {
        strlcpy(next_password, password, sizeof(next_password));
    } else {
        taskENTER_CRITICAL(&s_wifi_lock);
        if (s_wifi_saved_ssid[0] != '\0' && strcmp(next_ssid, s_wifi_saved_ssid) == 0) {
            strlcpy(next_password, s_wifi_saved_password, sizeof(next_password));
        }
        taskEXIT_CRITICAL(&s_wifi_lock);
    }

    taskENTER_CRITICAL(&s_wifi_lock);
    bool was_connected = s_wifi_status.connected;
    bool was_connecting = s_wifi_status.started &&
                          !s_wifi_status.connected &&
                          (s_wifi_status.phase == WIFI_CONNECTION_PHASE_ASSOCIATING ||
                           s_wifi_status.phase == WIFI_CONNECTION_PHASE_WAITING_IP ||
                           s_wifi_status.phase == WIFI_CONNECTION_PHASE_RETRY_WAIT);
    bool station_needs_reset = was_connected || was_connecting || s_wifi_status.associated;
    bool same_connect_request = (was_connected || was_connecting) &&
                                strcmp(next_ssid, s_wifi_pending_ssid) == 0 &&
                                strcmp(next_password, s_wifi_pending_password) == 0;
    bool started = s_wifi_status.started;
    if (same_connect_request) {
        strlcpy(s_wifi_status.ssid, s_wifi_pending_ssid, sizeof(s_wifi_status.ssid));
        taskEXIT_CRITICAL(&s_wifi_lock);
        ESP_LOGD(TAG, "wifi connect already in progress: ssid=%s", next_ssid);
        return ESP_OK;
    }
    if (s_wifi_scan_deferred) {
        s_wifi_scan_deferred = false;
        if (!s_wifi_scan_in_progress) {
            s_wifi_scan_snapshot.in_progress = false;
        }
    }
    strlcpy(s_wifi_pending_ssid, next_ssid, sizeof(s_wifi_pending_ssid));
    strlcpy(s_wifi_pending_password, next_password, sizeof(s_wifi_pending_password));
    s_wifi_pending_explicit = true;
    s_wifi_status.configured = true;
    s_wifi_status.associated = false;
    s_wifi_status.connected = false;
    s_wifi_status.retry_count = 0;
    s_wifi_status.disconnect_reason = 0;
    s_wifi_status.connect_failure = WIFI_CONNECT_FAILURE_NONE;
    s_wifi_attempt_started_ms = 0;
    s_wifi_associated_ms = 0;
    s_wifi_dhcp_renewed = false;
    s_wifi_timeout_abort_pending = false;
    s_wifi_background_recovery_due_ms = 0U;
    wifi_set_phase_locked(WIFI_CONNECTION_PHASE_IDLE, wifi_uptime_ms());
    s_wifi_status.rssi = WIFI_INVALID_RSSI;
    s_wifi_status.ip_addr[0] = '\0';
    strlcpy(s_wifi_status.ssid, s_wifi_pending_ssid, sizeof(s_wifi_status.ssid));
    s_wifi_reconfig_in_progress = station_needs_reset;
    taskEXIT_CRITICAL(&s_wifi_lock);

    if (!started) {
        return wifi_prepare(&s_wifi_config);
    }

    if (s_wifi_event_group != NULL) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }

    wifi_config_t wifi_cfg = {0};
    wifi_build_sta_config(&wifi_cfg, next_ssid, next_password);

    if (station_needs_reset) {
        esp_err_t disconnect_ret = esp_wifi_disconnect();
        if (disconnect_ret != ESP_OK && disconnect_ret != ESP_ERR_WIFI_NOT_CONNECT) {
            taskENTER_CRITICAL(&s_wifi_lock);
            s_wifi_reconfig_in_progress = false;
            taskEXIT_CRITICAL(&s_wifi_lock);
            ESP_LOGW(TAG, "wifi disconnect before reconnect failed: %s", esp_err_to_name(disconnect_ret));
            return disconnect_ret;
        }
        if (!was_connected) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    esp_err_t set_ret = ESP_FAIL;
    for (uint8_t attempt = 0; attempt < 5; ++attempt) {
        set_ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        if (set_ret == ESP_OK || set_ret != ESP_ERR_WIFI_STATE) {
            break;
        }
        ESP_LOGW(TAG,
                 "wifi config update waits for sta idle: attempt=%u err=%s",
                 (unsigned)(attempt + 1),
                 esp_err_to_name(set_ret));
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    if (set_ret != ESP_OK) {
        taskENTER_CRITICAL(&s_wifi_lock);
        s_wifi_reconfig_in_progress = false;
        taskEXIT_CRITICAL(&s_wifi_lock);
        ESP_LOGE(TAG, "wifi config update failed: %s", esp_err_to_name(set_ret));
        return set_ret;
    }

    esp_err_t connect_ret = wifi_start_connect_attempt();
    if (connect_ret != ESP_OK) {
        taskENTER_CRITICAL(&s_wifi_lock);
        s_wifi_reconfig_in_progress = false;
        taskEXIT_CRITICAL(&s_wifi_lock);
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        return connect_ret;
    }
    taskENTER_CRITICAL(&s_wifi_lock);
    s_wifi_reconfig_in_progress = false;
    taskEXIT_CRITICAL(&s_wifi_lock);
    ESP_LOGI(TAG, "wifi connect requested: ssid=%s", next_ssid);
    return ESP_OK;
}

esp_err_t wifi_retry_connection(void)
{
    char ssid[sizeof(s_wifi_pending_ssid)] = {0};
    char password[sizeof(s_wifi_pending_password)] = {0};

    if (!s_wifi_initialized || !s_wifi_config.enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_wifi_lock);
    strlcpy(ssid, s_wifi_pending_ssid, sizeof(ssid));
    strlcpy(password, s_wifi_pending_password, sizeof(password));
    taskEXIT_CRITICAL(&s_wifi_lock);
    if (ssid[0] == '\0') {
        memset(password, 0, sizeof(password));
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = wifi_connect(ssid, password);
    memset(password, 0, sizeof(password));
    return ret;
}

esp_err_t wifi_request_scan(void)
{
    if (!s_wifi_config.enabled) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!s_wifi_initialized || s_wifi_scan_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_wifi_lock);
    bool connecting = s_wifi_status.started &&
                      s_wifi_status.configured &&
                      !s_wifi_status.connected &&
                      (s_wifi_status.phase == WIFI_CONNECTION_PHASE_ASSOCIATING ||
                       s_wifi_status.phase == WIFI_CONNECTION_PHASE_WAITING_IP ||
                       s_wifi_status.phase == WIFI_CONNECTION_PHASE_RETRY_WAIT);
    bool should_defer = connecting && !s_wifi_scan_in_progress && !s_wifi_scan_deferred;
    bool should_notify = !connecting && !s_wifi_scan_in_progress && !s_wifi_scan_deferred;
    if (should_defer) {
        s_wifi_scan_deferred = true;
        s_wifi_scan_snapshot.in_progress = true;
    } else if (should_notify) {
        s_wifi_scan_in_progress = true;
        s_wifi_scan_snapshot.in_progress = true;
    }
    taskEXIT_CRITICAL(&s_wifi_lock);

    if (should_defer) {
        ESP_LOGI(TAG, "wifi scan deferred until current connect attempt ends");
    } else if (should_notify) {
        xTaskNotifyGive(s_wifi_scan_task);
    }
    return ESP_OK;
}

void wifi_get_status(wifi_status_t *status)
{
    if (status == NULL) {
        return;
    }

    wifi_refresh_rssi();
    wifi_copy_status(status);
}

void wifi_get_scan_snapshot(wifi_scan_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_wifi_lock);
    *snapshot = s_wifi_scan_snapshot;
    taskEXIT_CRITICAL(&s_wifi_lock);
}

void wifi_get_saved_config(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    taskENTER_CRITICAL(&s_wifi_lock);
    if (ssid != NULL && ssid_size > 0) {
        strlcpy(ssid, s_wifi_saved_ssid, ssid_size);
    }
    if (password != NULL && password_size > 0) {
        strlcpy(password, s_wifi_saved_password, password_size);
    }
    taskEXIT_CRITICAL(&s_wifi_lock);
}
