#include "app_controller.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app_identity.h"
#include "cJSON.h"
#include "device/device_session.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "media_runtime.h"
#include "nvs.h"
#include "platform_client.h"
#include "runtime_config.h"
#include "session_runtime.h"
#include "tirtc_adapter.h"
#include "wifi_manager.h"

#define CONTROLLER_QUEUE_DEPTH 64
#define CONTROLLER_TASK_STACK 16384
#define BOOTSTRAP_TASK_STACK 32768
#define CONFIG_IO_TASK_STACK 6144
#define TIRTC_START_TIMEOUT_MS 15000
#define TIRTC_STOP_TIMEOUT_MS 5000
#define PLATFORM_RETRY_INITIAL_MS 2000
#define PLATFORM_RETRY_MAX_MS 30000

typedef enum {
    CONTROLLER_EVENT_WIFI = 0,
    CONTROLLER_EVENT_PLATFORM,
    CONTROLLER_EVENT_SESSION,
    CONTROLLER_EVENT_INTENT,
} controller_event_type_t;

typedef struct {
    controller_event_type_t type;
    void *payload;
} controller_event_t;

typedef enum {
    BOOTSTRAP_EXISTING = 0,
    BOOTSTRAP_PROVISION,
} bootstrap_mode_t;

typedef struct {
    bootstrap_mode_t mode;
    uint32_t generation;
} bootstrap_context_t;

typedef struct {
    esp_err_t error;
    bootstrap_mode_t mode;
    uint32_t generation;
    bool needs_binding;
    bool attempted_provision;
    bool provisioned;
    char device_id[APP_TEXT_SMALL];
} bootstrap_result_t;

typedef struct {
    runtime_tirtc_config_t config;
    SemaphoreHandle_t completed;
    esp_err_t result;
    bool load;
} config_io_context_t;

static QueueHandle_t s_queue;
static QueueHandle_t s_session_critical_fallback;
static QueueHandle_t s_session_incoming_fallback;
static QueueHandle_t s_session_action_fallback;
static QueueHandle_t s_session_general_fallback;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static TaskHandle_t s_bootstrap_task;
static app_snapshot_t s_snapshot;
static app_event_callback_t s_callback;
static void *s_callback_user_data;
static char s_client_id[APP_CLIENT_ID_SIZE];
static char s_mac_text[APP_MAC_TEXT_SIZE];
static bool s_runtime_started;
static bool s_media_initialized;
static bool s_session_started;
static bool s_tirtc_ready_reported;
static int64_t s_runtime_retry_at_ms;
static char s_runtime_start_stage[APP_TEXT_SMALL] = "idle";
static int64_t s_tirtc_start_deadline_ms;
static int64_t s_tirtc_stop_deadline_ms;
static int64_t s_bootstrap_retry_at_ms;
static uint32_t s_bootstrap_retry_delay_ms = PLATFORM_RETRY_INITIAL_MS;
static bool s_bootstrap_retry_pending;
static device_service_t s_last_session_owner = DEVICE_SERVICE_NONE;
static bootstrap_result_t s_bootstrap_result;
static atomic_bool s_bootstrap_result_pending;

static void secure_zero(void *memory, size_t size);

static void config_io_task(void *argument)
{
    config_io_context_t *context = (config_io_context_t *)argument;
    context->result = context->load
                          ? runtime_config_load_tirtc(&context->config)
                          : runtime_config_save_tirtc(&context->config);
    SemaphoreHandle_t completed = context->completed;
    (void)xSemaphoreGive(completed);
    vTaskDelete(NULL);
}

static esp_err_t execute_runtime_config_io(runtime_tirtc_config_t *config,
                                           bool load)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Bootstrap networking deliberately uses a PSRAM stack. Flash/NVS access
     * can disable the external-memory cache, so perform it on a short-lived
     * internal-stack worker and keep the credential copy in internal RAM.
     */
    config_io_context_t *context = heap_caps_calloc(
        1,
        sizeof(*context),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (context == NULL) {
        return ESP_ERR_NO_MEM;
    }
    context->load = load;
    if (!load) {
        context->config = *config;
    }
    context->completed = xSemaphoreCreateBinary();
    if (context->completed == NULL) {
        secure_zero(context, sizeof(*context));
        heap_caps_free(context);
        return ESP_ERR_NO_MEM;
    }

    BaseType_t created = xTaskCreatePinnedToCore(
        config_io_task,
        load ? "config_load" : "config_persist",
        CONFIG_IO_TASK_STACK,
        context,
        5,
        NULL,
        tskNO_AFFINITY);
    if (created != pdPASS) {
        vSemaphoreDelete(context->completed);
        secure_zero(context, sizeof(*context));
        heap_caps_free(context);
        return ESP_ERR_NO_MEM;
    }

    (void)xSemaphoreTake(context->completed, portMAX_DELAY);
    esp_err_t result = context->result;
    if (load && result == ESP_OK) {
        *config = context->config;
    }
    vSemaphoreDelete(context->completed);
    secure_zero(context, sizeof(*context));
    heap_caps_free(context);
    return result;
}

static esp_err_t load_runtime_config(runtime_tirtc_config_t *config)
{
    return execute_runtime_config_io(config, true);
}

static esp_err_t save_runtime_config(runtime_tirtc_config_t *config)
{
    return execute_runtime_config_io(config, false);
}
static atomic_bool s_rebind_needed;
static atomic_bool s_wifi_reconcile_needed;
static atomic_uint_fast32_t s_session_overflow_count;

static void handle_session(const session_runtime_event_t *source);

static void secure_zero(void *memory, size_t size)
{
    volatile unsigned char *cursor = memory;
    while (cursor != NULL && size-- > 0) {
        *cursor++ = 0;
    }
}
static atomic_bool s_restart_scheduled;

static bool queue_owned(controller_event_type_t type,
                        void *payload,
                        TickType_t wait)
{
    controller_event_t event = {
        .type = type,
        .payload = payload,
    };
    if (s_queue != NULL && xQueueSend(s_queue, &event, wait) == pdTRUE) {
        return true;
    }
    free(payload);
    return false;
}

static void lock_snapshot(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock_snapshot(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static uint32_t snapshot_generation(void)
{
    lock_snapshot();
    uint32_t generation = s_snapshot.generation;
    unlock_snapshot();
    return generation;
}

const char *app_state_name(app_state_t state)
{
    switch (state) {
    case APP_STATE_BOOTING: return "BOOTING";
    case APP_STATE_NEED_WIFI: return "NEED_WIFI";
    case APP_STATE_NETWORKING: return "NETWORKING";
    case APP_STATE_NEED_BINDING: return "NEED_BINDING";
    case APP_STATE_BINDING: return "BINDING";
    case APP_STATE_STARTING_PLATFORM: return "STARTING_PLATFORM";
    case APP_STATE_READY: return "READY";
    case APP_STATE_RESTARTING: return "RESTARTING";
    case APP_STATE_ERROR: return "ERROR";
    default: return "UNKNOWN";
    }
}

static const char *app_session_owner_name(device_service_t owner)
{
    switch (owner) {
    case DEVICE_SERVICE_NONE: return "none";
    case DEVICE_SERVICE_AI: return "ai";
    case DEVICE_SERVICE_CALL: return "call";
    default: return "unknown";
    }
}

static const char *app_intent_name(app_intent_type_t type)
{
    switch (type) {
    case APP_INTENT_WIFI_SET: return "WIFI_SET";
    case APP_INTENT_WIFI_CLEAR: return "WIFI_CLEAR";
    case APP_INTENT_BIND_START: return "BIND_START";
    case APP_INTENT_AI_START: return "AI_START";
    case APP_INTENT_AI_STOP: return "AI_STOP";
    case APP_INTENT_AI_INTERRUPT: return "AI_INTERRUPT";
    case APP_INTENT_AI_SUBMIT: return "AI_SUBMIT";
    case APP_INTENT_AI_UPDATE: return "AI_UPDATE";
    case APP_INTENT_AI_ACTION_RESULT: return "AI_ACTION_RESULT";
    case APP_INTENT_CALL_START: return "CALL_START";
    case APP_INTENT_CALL_ACCEPT: return "CALL_ACCEPT";
    case APP_INTENT_CALL_REJECT: return "CALL_REJECT";
    case APP_INTENT_CALL_CANCEL: return "CALL_CANCEL";
    case APP_INTENT_CALL_HANGUP: return "CALL_HANGUP";
    case APP_INTENT_CONTACTS_LIST: return "CONTACTS_LIST";
    case APP_INTENT_PENDING_LIST: return "PENDING_LIST";
    case APP_INTENT_CONTACT_REQUEST: return "CONTACT_REQUEST";
    case APP_INTENT_CONTACT_RESPOND: return "CONTACT_RESPOND";
    case APP_INTENT_CONTACT_REMARK: return "CONTACT_REMARK";
    case APP_INTENT_CONTACT_DELETE: return "CONTACT_DELETE";
    case APP_INTENT_RESTART: return "RESTART";
    default: return "UNKNOWN";
    }
}

static void emit(const app_event_t *event)
{
    if (s_callback != NULL && event != NULL) {
        s_callback(event, s_callback_user_data);
    }
}

static void copy_text(char *destination,
                      size_t destination_size,
                      const char *source)
{
    if (destination == NULL || destination_size == 0) {
        return;
    }
    size_t length = strlcpy(destination,
                           source == NULL ? "" : source,
                           destination_size);
    if (length >= destination_size) {
        (void)strlcpy(destination,
                      "[truncated-invalid-size]",
                      destination_size);
    }
}

static void emit_simple(app_event_domain_t domain,
                        const char *name,
                        uint32_t generation,
                        uint32_t request_id,
                        int code,
                        const char *first,
                        const char *second)
{
    app_event_t event = {
        .domain = domain,
        .generation = generation,
        .request_id = request_id,
        .code = code,
    };
    (void)snprintf(event.name, sizeof(event.name), "%s", name == NULL ? "" : name);
    (void)snprintf(event.first, sizeof(event.first), "%s",
                   first == NULL ? "" : first);
    (void)snprintf(event.second, sizeof(event.second), "%s",
                   second == NULL ? "" : second);
    emit(&event);
}

static void set_app_state(app_state_t state, esp_err_t error)
{
    lock_snapshot();
    s_snapshot.state = state;
    s_snapshot.last_error = error;
    uint32_t generation = s_snapshot.generation;
    unlock_snapshot();
    emit_simple(error == ESP_OK ? APP_EVENT_SYSTEM : APP_EVENT_ERROR,
                app_state_name(state),
                generation,
                0,
                error,
                error == ESP_OK ? "" : esp_err_to_name(error),
                NULL);
}

static esp_err_t persist_provisioned_credentials(
    const platform_provision_result_t *credentials,
    void *user_data)
{
    (void)user_data;
    if (credentials == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    runtime_tirtc_config_t config = {0};
    (void)snprintf(config.device_id,
                   sizeof(config.device_id),
                   "%s",
                   credentials->device_id);
    (void)snprintf(config.device_secret,
                   sizeof(config.device_secret),
                   "%s",
                   credentials->device_secret);
    (void)snprintf(config.client_id,
                   sizeof(config.client_id),
                   "%s",
                   s_client_id);
    const char *endpoint = platform_client_tirtc_endpoint();
    if (endpoint != NULL) {
        (void)snprintf(config.service_endpoint,
                       sizeof(config.service_endpoint),
                       "%s",
                       endpoint);
    }
    esp_err_t err = save_runtime_config(&config);
    secure_zero(&config, sizeof(config));
    return err;
}

static void post_bootstrap_result(const bootstrap_result_t *source)
{
    lock_snapshot();
    s_bootstrap_result = *source;
    atomic_store_explicit(&s_bootstrap_result_pending,
                          true,
                          memory_order_release);
    unlock_snapshot();
}

static void bootstrap_task(void *argument)
{
    bootstrap_context_t context = *(bootstrap_context_t *)argument;
    free(argument);
    bootstrap_result_t outcome = {
        .mode = context.mode,
        .generation = context.generation,
    };
    runtime_tirtc_config_t config = {0};
    esp_err_t load_error = load_runtime_config(&config);

    if (context.mode == BOOTSTRAP_EXISTING &&
        load_error == ESP_ERR_NVS_NOT_FOUND) {
        outcome.error = ESP_ERR_NOT_FOUND;
        outcome.needs_binding = true;
        secure_zero(&config, sizeof(config));
        post_bootstrap_result(&outcome);
        secure_zero(&outcome, sizeof(outcome));
        vTaskDeleteWithCaps(NULL);
        return;
    }
    if (load_error != ESP_OK && load_error != ESP_ERR_NVS_NOT_FOUND) {
        outcome.error = load_error;
        secure_zero(&config, sizeof(config));
        post_bootstrap_result(&outcome);
        secure_zero(&outcome, sizeof(outcome));
        vTaskDeleteWithCaps(NULL);
        return;
    }

    if (context.mode == BOOTSTRAP_PROVISION) {
        outcome.attempted_provision = true;
        outcome.error = platform_client_stop();
        if (outcome.error != ESP_OK) {
            secure_zero(&config, sizeof(config));
            post_bootstrap_result(&outcome);
            secure_zero(&outcome, sizeof(outcome));
            vTaskDeleteWithCaps(NULL);
            return;
        }
        platform_provision_result_t provisioned = {0};
        platform_provision_config_t provision = {
            .mac_address = s_mac_text,
            .existing_device_id = load_error == ESP_OK ? config.device_id : NULL,
            .existing_device_secret =
                load_error == ESP_OK ? config.device_secret : NULL,
            .timeout_seconds = 190,
            .persist_credentials = persist_provisioned_credentials,
        };
        outcome.error = platform_client_provision(&provision, &provisioned);
        if (outcome.error == ESP_OK) {
            outcome.provisioned = true;
            (void)snprintf(outcome.device_id,
                           sizeof(outcome.device_id),
                           "%s",
                           provisioned.device_id);
        }
        if (outcome.error != ESP_OK) {
            (void)platform_client_stop();
        }
        secure_zero(&provisioned, sizeof(provisioned));
        secure_zero(&config, sizeof(config));
        post_bootstrap_result(&outcome);
        secure_zero(&outcome, sizeof(outcome));
        vTaskDeleteWithCaps(NULL);
        return;
    }

    if (config.client_id[0] == '\0' ||
        strcmp(config.client_id, s_client_id) != 0) {
        (void)snprintf(config.client_id,
                       sizeof(config.client_id),
                       "%s",
                       s_client_id);
        outcome.error = save_runtime_config(&config);
        if (outcome.error != ESP_OK) {
            secure_zero(&config, sizeof(config));
            post_bootstrap_result(&outcome);
            secure_zero(&outcome, sizeof(outcome));
            vTaskDeleteWithCaps(NULL);
            return;
        }
    }
    platform_client_config_t platform = {
        .device_id = config.device_id,
        .device_secret = config.device_secret,
        .client_id = s_client_id,
        .mac_address = s_mac_text,
    };
    outcome.error = platform_client_start(&platform);
    if (outcome.error == ESP_ERR_NOT_FOUND) {
        outcome.attempted_provision = true;
        platform_provision_result_t provisioned = {0};
        platform_provision_config_t provision = {
            .mac_address = s_mac_text,
            .existing_device_id = config.device_id,
            .existing_device_secret = config.device_secret,
            .timeout_seconds = 190,
            .persist_credentials = persist_provisioned_credentials,
        };
        outcome.error = platform_client_provision(&provision, &provisioned);
        if (outcome.error == ESP_OK) {
            outcome.provisioned = true;
            (void)snprintf(outcome.device_id,
                           sizeof(outcome.device_id),
                           "%s",
                           provisioned.device_id);
        }
        secure_zero(&provisioned, sizeof(provisioned));
    } else if (outcome.error == ESP_OK) {
        const char *endpoint = platform_client_tirtc_endpoint();
        if (endpoint != NULL &&
            strcmp(config.service_endpoint, endpoint) != 0) {
            (void)snprintf(config.service_endpoint,
                           sizeof(config.service_endpoint),
                           "%s",
                           endpoint);
            outcome.error = save_runtime_config(&config);
        }
        (void)snprintf(outcome.device_id,
                       sizeof(outcome.device_id),
                       "%s",
                       config.device_id);
    }
    if (outcome.error != ESP_OK) {
        (void)platform_client_stop();
    }
    secure_zero(&config, sizeof(config));
    post_bootstrap_result(&outcome);
    secure_zero(&outcome, sizeof(outcome));
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t start_bootstrap(bootstrap_mode_t mode)
{
    if (s_bootstrap_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    bootstrap_context_t *context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return ESP_ERR_NO_MEM;
    }
    context->mode = mode;
    context->generation = snapshot_generation();
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        bootstrap_task,
        mode == BOOTSTRAP_PROVISION ? "provision" : "platform_boot",
        BOOTSTRAP_TASK_STACK,
        context,
        5,
        &s_bootstrap_task,
        tskNO_AFFINITY,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        free(context);
        s_bootstrap_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    set_app_state(mode == BOOTSTRAP_PROVISION
                      ? APP_STATE_BINDING
                      : APP_STATE_STARTING_PLATFORM,
                  ESP_OK);
    s_bootstrap_retry_pending = false;
    s_bootstrap_retry_at_ms = 0;
    return ESP_OK;
}

static void reset_bootstrap_retry(void)
{
    s_bootstrap_retry_pending = false;
    s_bootstrap_retry_at_ms = 0;
    s_bootstrap_retry_delay_ms = PLATFORM_RETRY_INITIAL_MS;
}

static void schedule_bootstrap_retry(esp_err_t error)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;
    s_bootstrap_retry_pending = true;
    s_bootstrap_retry_at_ms = now_ms + s_bootstrap_retry_delay_ms;
    if (s_bootstrap_retry_delay_ms < PLATFORM_RETRY_MAX_MS) {
        uint32_t next = s_bootstrap_retry_delay_ms * 2U;
        s_bootstrap_retry_delay_ms =
            next > PLATFORM_RETRY_MAX_MS ? PLATFORM_RETRY_MAX_MS : next;
    }
    lock_snapshot();
    s_snapshot.state = APP_STATE_STARTING_PLATFORM;
    s_snapshot.last_error = error;
    unlock_snapshot();
}

static void delayed_restart_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(600));
    if (s_session_started) {
        (void)session_runtime_abort("controlled_restart", 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    media_runtime_stop();

    tirtc_adapter_state_t state = tirtc_adapter_state();
    if (state == TIRTC_ADAPTER_STARTING ||
        state == TIRTC_ADAPTER_RUNNING ||
        state == TIRTC_ADAPTER_ERROR) {
        (void)tirtc_adapter_request_stop();
    }
    int64_t deadline_ms =
        esp_timer_get_time() / 1000 + TIRTC_STOP_TIMEOUT_MS;
    while (tirtc_adapter_state() == TIRTC_ADAPTER_STOPPING &&
           esp_timer_get_time() / 1000 < deadline_ms) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (tirtc_adapter_state() == TIRTC_ADAPTER_STOPPED ||
        tirtc_adapter_state() == TIRTC_ADAPTER_ERROR) {
        (void)tirtc_adapter_deinit();
    }
    (void)platform_client_stop();
    (void)wifi_manager_stop();
    esp_restart();
}

static void schedule_restart(const char *reason)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_restart_scheduled,
                                        &expected,
                                        true)) {
        return;
    }
    session_runtime_snapshot_t session = {0};
    if (s_session_started &&
        session_runtime_get_snapshot(&session) == ESP_OK) {
        if (session.owner == DEVICE_SERVICE_CALL) {
            (void)session_runtime_call_hangup(0);
        } else if (session.owner == DEVICE_SERVICE_AI) {
            (void)session_runtime_ai_stop(0);
        }
    }
    lock_snapshot();
    s_snapshot.state = APP_STATE_RESTARTING;
    s_snapshot.platform_ready = false;
    s_snapshot.mqtt_online = false;
    s_snapshot.tirtc_ready = false;
    unlock_snapshot();
    emit_simple(APP_EVENT_SYSTEM,
                "RESTARTING",
                snapshot_generation(),
                0,
                ESP_OK,
                reason,
                NULL);
    if (xTaskCreate(delayed_restart_task,
                    "restart",
                    3072,
                    NULL,
                    4,
                    NULL) != pdPASS) {
        esp_restart();
    }
}

static void wifi_observer(const wifi_manager_event_t *event, void *context)
{
    (void)context;
    if (event == NULL ||
        atomic_load_explicit(&s_restart_scheduled, memory_order_acquire)) {
        return;
    }
    wifi_manager_event_t *copy = malloc(sizeof(*copy));
    if (copy == NULL) {
        atomic_store_explicit(&s_wifi_reconcile_needed,
                              true,
                              memory_order_release);
        return;
    }
    *copy = *event;
    if (!queue_owned(CONTROLLER_EVENT_WIFI, copy, 0)) {
        atomic_store_explicit(&s_wifi_reconcile_needed,
                              true,
                              memory_order_release);
    }
}

static void platform_observer(const platform_client_event_t *event,
                              void *context)
{
    (void)context;
    if (event == NULL ||
        atomic_load_explicit(&s_restart_scheduled, memory_order_acquire)) {
        return;
    }
    platform_client_event_t *copy = malloc(sizeof(*copy));
    if (copy != NULL) {
        *copy = *event;
        if (queue_owned(CONTROLLER_EVENT_PLATFORM, copy, 0)) {
            return;
        }
    }
    if (event->type == PLATFORM_CLIENT_EVENT_REBIND_REQUIRED) {
        atomic_store_explicit(&s_rebind_needed,
                              true,
                              memory_order_release);
    }
}

static void session_observer(const session_runtime_event_t *event,
                             void *context)
{
    (void)context;
    if (event == NULL ||
        atomic_load_explicit(&s_restart_scheduled, memory_order_acquire)) {
        return;
    }
    session_runtime_event_t *copy = heap_caps_malloc(
        sizeof(*copy), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy != NULL) {
        *copy = *event;
        if (queue_owned(CONTROLLER_EVENT_SESSION, copy, 0)) {
            return;
        }
    }

    QueueHandle_t fallback = s_session_general_fallback;
    if (event->type == SESSION_RUNTIME_EVENT_CALL_INCOMING) {
        fallback = s_session_incoming_fallback;
    } else if (event->type == SESSION_RUNTIME_EVENT_AI_ACTION) {
        fallback = s_session_action_fallback;
    } else if (event->type == SESSION_RUNTIME_EVENT_STATE ||
               event->type == SESSION_RUNTIME_EVENT_CONTACTS_DONE) {
        fallback = s_session_critical_fallback;
    }
    if (fallback != NULL) {
        (void)xQueueOverwrite(fallback, event);
    }
    (void)atomic_fetch_add_explicit(&s_session_overflow_count,
                                    1,
                                    memory_order_acq_rel);
}

static void report_session_overflow(void)
{
    session_runtime_event_t fallback;
    if (s_session_incoming_fallback != NULL &&
        xQueueReceive(s_session_incoming_fallback, &fallback, 0) == pdTRUE) {
        handle_session(&fallback);
        secure_zero(&fallback, sizeof(fallback));
    }
    if (s_session_action_fallback != NULL &&
        xQueueReceive(s_session_action_fallback, &fallback, 0) == pdTRUE) {
        handle_session(&fallback);
        secure_zero(&fallback, sizeof(fallback));
    }
    if (s_session_critical_fallback != NULL &&
        xQueueReceive(s_session_critical_fallback, &fallback, 0) == pdTRUE) {
        handle_session(&fallback);
        secure_zero(&fallback, sizeof(fallback));
    }
    if (s_session_general_fallback != NULL &&
        xQueueReceive(s_session_general_fallback, &fallback, 0) == pdTRUE) {
        handle_session(&fallback);
        secure_zero(&fallback, sizeof(fallback));
    }

    uint32_t dropped = (uint32_t)atomic_exchange_explicit(
        &s_session_overflow_count, 0, memory_order_acq_rel);
    if (dropped == 0U) {
        return;
    }
    char count[16];
    (void)snprintf(count, sizeof(count), "%lu", (unsigned long)dropped);
    emit_simple(APP_EVENT_ERROR,
                "SESSION_EVENT_OVERFLOW",
                snapshot_generation(),
                0,
                ESP_ERR_TIMEOUT,
                count,
                NULL);
}

static void handle_wifi(const wifi_manager_event_t *event)
{
    lock_snapshot();
    bool was_online = s_snapshot.wifi_online;
    s_snapshot.wifi_configured = event->snapshot.credentials_configured;
    s_snapshot.wifi_online = event->snapshot.connected;
    (void)snprintf(s_snapshot.ssid,
                   sizeof(s_snapshot.ssid),
                   "%s",
                   event->snapshot.ssid);
    (void)snprintf(s_snapshot.ip_address,
                   sizeof(s_snapshot.ip_address),
                   "%s",
                   event->snapshot.ip);
    if (was_online && !s_snapshot.wifi_online) {
        ++s_snapshot.generation;
        s_snapshot.platform_ready = false;
        s_snapshot.mqtt_online = false;
        s_snapshot.tirtc_ready = false;
    }
    uint32_t generation = s_snapshot.generation;
    unlock_snapshot();

    const char *name = "SNAPSHOT";
    switch (event->type) {
    case WIFI_MANAGER_EVENT_CONNECTING: name = "CONNECTING"; break;
    case WIFI_MANAGER_EVENT_CONNECTED: name = "ONLINE"; break;
    case WIFI_MANAGER_EVENT_DISCONNECTED: name = "DISCONNECTED"; break;
    case WIFI_MANAGER_EVENT_CREDENTIALS_UPDATED: name = "CONFIG_SAVED"; break;
    case WIFI_MANAGER_EVENT_CREDENTIALS_CLEARED: name = "CONFIG_CLEARED"; break;
    case WIFI_MANAGER_EVENT_STARTED: name = "STARTED"; break;
    case WIFI_MANAGER_EVENT_STOPPED: name = "STOPPED"; break;
    case WIFI_MANAGER_EVENT_INITIALIZED: name = "INITIALIZED"; break;
    default: break;
    }
    char reason[24];
    (void)snprintf(reason,
                   sizeof(reason),
                   "%u",
                   (unsigned)event->snapshot.last_disconnect_reason);
    emit_simple(APP_EVENT_WIFI,
                name,
                generation,
                0,
                ESP_OK,
                event->snapshot.connected ? event->snapshot.ip
                                          : event->snapshot.ssid,
                reason);

    if (event->snapshot.connected &&
        !platform_client_ready() && s_bootstrap_task == NULL) {
        esp_err_t err = start_bootstrap(BOOTSTRAP_EXISTING);
        if (err != ESP_OK) {
            schedule_bootstrap_retry(err);
        }
    } else if (!event->snapshot.credentials_configured) {
        reset_bootstrap_retry();
        set_app_state(APP_STATE_NEED_WIFI, ESP_OK);
    } else if (!event->snapshot.connected &&
               event->type != WIFI_MANAGER_EVENT_CREDENTIALS_UPDATED) {
        set_app_state(APP_STATE_NETWORKING, ESP_OK);
        if (s_runtime_started) {
            (void)session_runtime_abort("wifi_offline", 0);
            s_tirtc_ready_reported = false;
        }
    }
}

static void handle_platform(const platform_client_event_t *event)
{
    uint32_t generation = snapshot_generation();
    switch (event->type) {
    case PLATFORM_CLIENT_EVENT_DISCOVERY_READY:
        emit_simple(APP_EVENT_PLATFORM, "DISCOVERY", generation, 0,
                    ESP_OK, "READY", NULL);
        break;
    case PLATFORM_CLIENT_EVENT_AUTH_READY:
        emit_simple(APP_EVENT_PLATFORM, "AUTH", generation, 0,
                    ESP_OK, "READY", NULL);
        break;
    case PLATFORM_CLIENT_EVENT_MQTT_CONNECTED:
        if (!platform_client_ready()) {
            lock_snapshot();
            s_snapshot.platform_ready = false;
            s_snapshot.mqtt_online = false;
            unlock_snapshot();
            break;
        }
        reset_bootstrap_retry();
        lock_snapshot();
        const bool mqtt_became_online =
            !s_snapshot.platform_ready || !s_snapshot.mqtt_online;
        s_snapshot.platform_ready = true;
        s_snapshot.mqtt_online = true;
        unlock_snapshot();
        if (mqtt_became_online) {
            emit_simple(APP_EVENT_PLATFORM, "MQTT", generation, 0,
                        ESP_OK, "ONLINE", NULL);
        }
        break;
    case PLATFORM_CLIENT_EVENT_MQTT_DISCONNECTED:
        lock_snapshot();
        const bool mqtt_was_online = s_snapshot.mqtt_online;
        s_snapshot.mqtt_online = false;
        unlock_snapshot();
        if (mqtt_was_online) {
            emit_simple(APP_EVENT_PLATFORM, "MQTT", generation, 0,
                        (int)event->reason_code, "OFFLINE", NULL);
            if (s_runtime_started) {
                (void)session_runtime_abort("mqtt_offline", 0);
            }
        }
        break;
    case PLATFORM_CLIENT_EVENT_PROVISION_CODE:
        lock_snapshot();
        (void)snprintf(s_snapshot.verification_code,
                       sizeof(s_snapshot.verification_code),
                       "%s",
                       event->provision_code);
        unlock_snapshot();
        emit_simple(APP_EVENT_BIND, "CODE", generation, 0,
                    ESP_OK, event->provision_code, NULL);
        break;
    case PLATFORM_CLIENT_EVENT_PROVISION_PROGRESS: {
        const char *stage = "unknown";
        switch ((platform_provision_progress_t)event->reason_code) {
        case PLATFORM_PROVISION_PROGRESS_MQTT_CONNECTED:
            stage = "mqtt-connected";
            break;
        case PLATFORM_PROVISION_PROGRESS_SUBSCRIBED:
            stage = "subscribed";
            break;
        case PLATFORM_PROVISION_PROGRESS_MESSAGE_RECEIVED:
            stage = "message-received";
            break;
        case PLATFORM_PROVISION_PROGRESS_GRANT_VALIDATED:
            stage = "grant-validated";
            break;
        case PLATFORM_PROVISION_PROGRESS_CREDENTIALS_PERSISTED:
            stage = "credentials-persisted";
            break;
        case PLATFORM_PROVISION_PROGRESS_ACK_CONFIRMED:
            stage = "ack-confirmed";
            break;
        default:
            break;
        }
        emit_simple(APP_EVENT_BIND, "PROGRESS", generation, 0,
                    ESP_OK, stage, NULL);
        break;
    }
    case PLATFORM_CLIENT_EVENT_REBIND_REQUIRED:
        atomic_store_explicit(&s_rebind_needed,
                              false,
                              memory_order_release);
        lock_snapshot();
        const bool wait_for_explicit_retry =
            s_snapshot.state == APP_STATE_NEED_BINDING;
        if (!wait_for_explicit_retry) {
            s_snapshot.state = APP_STATE_BINDING;
        }
        s_snapshot.platform_ready = false;
        s_snapshot.mqtt_online = false;
        s_snapshot.tirtc_ready = false;
        unlock_snapshot();
        emit_simple(APP_EVENT_BIND, "REQUIRED", generation, 0,
                    event->error, "", NULL);
        if (s_runtime_started) {
            (void)session_runtime_abort("platform_rebind", 0);
        }
        if (s_bootstrap_task == NULL && !wait_for_explicit_retry) {
            esp_err_t err = start_bootstrap(BOOTSTRAP_PROVISION);
            if (err != ESP_OK) {
                set_app_state(APP_STATE_NEED_BINDING, err);
            }
        }
        break;
    case PLATFORM_CLIENT_EVENT_ERROR:
    default:
        lock_snapshot();
        s_snapshot.last_error = event->error;
        unlock_snapshot();
        emit_simple(APP_EVENT_ERROR, "PLATFORM", generation, 0,
                    event->error, esp_err_to_name(event->error), NULL);
        break;
    }
}

static void handle_session(const session_runtime_event_t *source)
{
    app_event_t event = {
        .domain = APP_EVENT_SESSION,
        .generation = source->generation,
        .request_id = source->origin_request_id,
        .code = source->status,
    };
    device_service_t routed_owner = source->owner;
    if (source->owner != DEVICE_SERVICE_NONE) {
        s_last_session_owner = source->owner;
    } else if (source->type == SESSION_RUNTIME_EVENT_STATE) {
        routed_owner = s_last_session_owner;
    }
    const char *owner = app_session_owner_name(source->owner);
    const char *state = device_session_state_name(source->state);
    lock_snapshot();
    (void)snprintf(s_snapshot.session_owner,
                   sizeof(s_snapshot.session_owner),
                   "%s",
                   owner);
    (void)snprintf(s_snapshot.session_state,
                   sizeof(s_snapshot.session_state),
                   "%s",
                   state);
    unlock_snapshot();
    if (source->type == SESSION_RUNTIME_EVENT_STATE &&
        routed_owner == DEVICE_SERVICE_NONE) {
        return;
    }

    switch (source->type) {
    case SESSION_RUNTIME_EVENT_STATE:
        (void)snprintf(event.name, sizeof(event.name), "%s:STATE",
                       routed_owner == DEVICE_SERVICE_AI ? "AI" : "CALL");
        (void)snprintf(event.first, sizeof(event.first), "%s", state);
        (void)snprintf(event.second,
                       sizeof(event.second),
                       "%s",
                       routed_owner == DEVICE_SERVICE_AI
                           ? source->request_id
                           : source->room_id);
        copy_text(event.payload, sizeof(event.payload), source->text);
        if (source->owner == DEVICE_SERVICE_NONE &&
            source->state == DEVICE_SESSION_IDLE) {
            s_last_session_owner = DEVICE_SERVICE_NONE;
        }
        break;
    case SESSION_RUNTIME_EVENT_OPERATION:
        if (strncmp(source->name, "ai-", 3) == 0) {
            (void)snprintf(event.name, sizeof(event.name), "AI:OP");
        } else if (strncmp(source->name, "call-", 5) == 0) {
            (void)snprintf(event.name, sizeof(event.name), "CALL:OP");
        } else {
            (void)snprintf(event.name, sizeof(event.name), "CONTACT:OP");
        }
        (void)snprintf(event.first, sizeof(event.first), "%s", source->name);
        copy_text(event.second, sizeof(event.second), source->text);
        copy_text(event.payload, sizeof(event.payload), source->json);
        break;
    case SESSION_RUNTIME_EVENT_AI_CAPTION:
        (void)snprintf(event.name, sizeof(event.name), "AI:CAPTION");
        event.code = source->caption_type;
        event.value1 = source->caption_mode;
        event.value2 = (int)source->seq_num;
        event.flag = source->final;
        (void)snprintf(event.first,
                       sizeof(event.first),
                       "%s",
                       source->request_id);
        copy_text(event.payload, sizeof(event.payload), source->text);
        break;
    case SESSION_RUNTIME_EVENT_AI_EVENT:
        (void)snprintf(event.name, sizeof(event.name), "AI:EVENT");
        (void)snprintf(event.first, sizeof(event.first), "%s", source->name);
        (void)snprintf(event.second,
                       sizeof(event.second),
                       "%s",
                       source->request_id);
        copy_text(event.payload,
                  sizeof(event.payload),
                  source->json[0] != '\0' ? source->json : source->text);
        break;
    case SESSION_RUNTIME_EVENT_AI_ACTION:
        (void)snprintf(event.name, sizeof(event.name), "AI:ACTION");
        (void)snprintf(event.first,
                       sizeof(event.first),
                       "%s",
                       source->request_id);
        (void)snprintf(event.second, sizeof(event.second), "%s", source->name);
        copy_text(event.payload, sizeof(event.payload), source->json);
        break;
    case SESSION_RUNTIME_EVENT_CALL_INCOMING:
        (void)snprintf(event.name, sizeof(event.name), "CALL:INCOMING");
        (void)snprintf(event.first, sizeof(event.first), "%s", source->peer_id);
        (void)snprintf(event.second, sizeof(event.second), "%s", source->room_id);
        copy_text(event.payload, sizeof(event.payload), source->name);
        break;
    case SESSION_RUNTIME_EVENT_CALL_EVENT:
        (void)snprintf(event.name, sizeof(event.name), "CALL:EVENT");
        (void)snprintf(event.first, sizeof(event.first), "%s", source->name);
        (void)snprintf(event.second, sizeof(event.second), "%s", source->room_id);
        copy_text(event.payload,
                  sizeof(event.payload),
                  source->json[0] != '\0' ? source->json : source->text);
        break;
    case SESSION_RUNTIME_EVENT_CONTACT:
        (void)snprintf(event.name,
                       sizeof(event.name),
                       "%s",
                       strcmp(source->name, "pending") == 0
                           ? "PENDING"
                           : "CONTACT");
        event.value1 = (int)source->index;
        event.flag = source->online;
        (void)snprintf(event.first, sizeof(event.first), "%s", source->peer_id);
        copy_text(event.second, sizeof(event.second), source->text);
        copy_text(event.payload, sizeof(event.payload), source->json);
        break;
    case SESSION_RUNTIME_EVENT_CONTACTS_DONE:
        (void)snprintf(event.name,
                       sizeof(event.name),
                       "%s",
                       strcmp(source->name, "contacts-pending") == 0
                           ? "PENDING:DONE"
                           : "CONTACTS:DONE");
        event.value1 = (int)source->count;
        (void)snprintf(event.first, sizeof(event.first), "%s", source->name);
        break;
    case SESSION_RUNTIME_EVENT_DIAGNOSTIC:
    default:
        (void)snprintf(event.name, sizeof(event.name), "DIAG");
        (void)snprintf(event.first, sizeof(event.first), "%s", source->name);
        copy_text(event.payload, sizeof(event.payload), source->text);
        break;
    }
    emit(&event);
}

static esp_err_t fail_runtime_start(runtime_tirtc_config_t *config,
                                    esp_err_t error,
                                    const char *stage)
{
    secure_zero(config, sizeof(*config));
    media_runtime_stop();
    if (tirtc_adapter_state() == TIRTC_ADAPTER_ERROR) {
        (void)tirtc_adapter_deinit();
    }
    s_runtime_started = false;
    (void)snprintf(s_runtime_start_stage,
                   sizeof(s_runtime_start_stage),
                   "%s",
                   stage == NULL ? "unknown" : stage);
    s_runtime_retry_at_ms = esp_timer_get_time() / 1000 + 5000;
    s_tirtc_start_deadline_ms = 0;
    return error;
}

static esp_err_t start_runtime(void)
{
    if (s_runtime_started) {
        return ESP_OK;
    }
    runtime_tirtc_config_t config = {0};
    esp_err_t err = load_runtime_config(&config);
    if (err != ESP_OK) {
        return fail_runtime_start(&config, err, "config-load");
    }
    const char *endpoint = platform_client_tirtc_endpoint();
    if (endpoint == NULL || endpoint[0] == '\0') {
        endpoint = config.service_endpoint;
    }

    if (!s_media_initialized) {
        err = media_runtime_init();
        if (err == ESP_OK) {
            s_media_initialized = true;
        }
        if (err != ESP_OK) {
            return fail_runtime_start(&config, err, "media-init");
        }
    }
    err = media_runtime_start();
    if (err != ESP_OK) {
        return fail_runtime_start(&config, err, "media-start");
    }
    if (!s_session_started) {
        session_runtime_set_observer(session_observer, NULL);
        err = session_runtime_start();
        if (err == ESP_OK) {
            s_session_started = true;
        }
        if (err != ESP_OK) {
            return fail_runtime_start(&config, err, "session-start");
        }
    }
    tirtc_adapter_state_t state = tirtc_adapter_state();
    if (state == TIRTC_ADAPTER_ERROR ||
        state == TIRTC_ADAPTER_STOPPED) {
        int deinit_error = tirtc_adapter_deinit();
        if (deinit_error != 0) {
            return fail_runtime_start(&config,
                                      ESP_ERR_INVALID_STATE,
                                      "adapter-deinit");
        }
        state = tirtc_adapter_state();
    }
    if (state == TIRTC_ADAPTER_STARTING ||
        state == TIRTC_ADAPTER_RUNNING) {
        s_runtime_started = true;
        (void)snprintf(s_runtime_start_stage,
                       sizeof(s_runtime_start_stage),
                       "%s",
                       "ready");
        if (state == TIRTC_ADAPTER_STARTING &&
            s_tirtc_start_deadline_ms == 0) {
            s_tirtc_start_deadline_ms =
                esp_timer_get_time() / 1000 + TIRTC_START_TIMEOUT_MS;
        }
        secure_zero(&config, sizeof(config));
        return ESP_OK;
    }
    if (state != TIRTC_ADAPTER_IDLE) {
        return fail_runtime_start(&config,
                                  ESP_ERR_INVALID_STATE,
                                  "adapter-state");
    }

    const tirtc_adapter_config_t adapter = {
        .device_id = config.device_id,
        .device_secret = config.device_secret,
        .client_id = s_client_id,
        .service_endpoint = endpoint,
        .max_send_buffer_bytes = 512U * 1024U,
        .max_connections = 1,
        .log_level = 3,
    };
    int adapter_error = tirtc_adapter_start(&adapter);
    if (adapter_error != 0) {
        char stage[APP_TEXT_SMALL];
        char code[16];
        (void)snprintf(code,
                       sizeof(code),
                       ":%d",
                       tirtc_adapter_last_start_error());
        (void)strlcpy(stage, "adapter-", sizeof(stage));
        (void)strlcat(stage,
                      tirtc_adapter_last_start_stage(),
                      sizeof(stage));
        (void)strlcat(stage, code, sizeof(stage));
        return fail_runtime_start(&config, ESP_FAIL, stage);
    }

    secure_zero(&config, sizeof(config));
    s_runtime_started = true;
    s_tirtc_ready_reported = false;
    (void)snprintf(s_runtime_start_stage,
                   sizeof(s_runtime_start_stage),
                   "%s",
                   "starting");
    s_runtime_retry_at_ms = 0;
    s_tirtc_start_deadline_ms =
        esp_timer_get_time() / 1000 + TIRTC_START_TIMEOUT_MS;
    s_tirtc_stop_deadline_ms = 0;
    return ESP_OK;
}

static void reconcile_runtime(void)
{
    if (atomic_load_explicit(&s_restart_scheduled, memory_order_acquire)) {
        return;
    }
    bool wifi_online;
    bool platform_ready;
    bool mqtt_online;
    lock_snapshot();
    wifi_online = s_snapshot.wifi_online;
    platform_ready = s_snapshot.platform_ready;
    mqtt_online = s_snapshot.mqtt_online;
    unlock_snapshot();
    int64_t current_ms = esp_timer_get_time() / 1000;
    bool control_plane_ready = wifi_online && platform_ready && mqtt_online;
    if (control_plane_ready && !s_runtime_started &&
        current_ms >= s_runtime_retry_at_ms) {
        esp_err_t err = start_runtime();
        if (err != ESP_OK) {
            char detail[APP_TEXT_SMALL];
            (void)strlcpy(detail,
                          s_runtime_start_stage,
                          sizeof(detail));
            (void)strlcat(detail, ":", sizeof(detail));
            (void)strlcat(detail, esp_err_to_name(err), sizeof(detail));
            lock_snapshot();
            s_snapshot.last_error = err;
            unlock_snapshot();
            emit_simple(APP_EVENT_ERROR,
                        "TIRTC_START",
                        snapshot_generation(),
                        0,
                        err,
                        detail,
                        NULL);
            return;
        }
    }
    if (!s_runtime_started) {
        return;
    }

    tirtc_adapter_state_t adapter_state = tirtc_adapter_state();
    if (adapter_state == TIRTC_ADAPTER_RUNNING && control_plane_ready) {
        s_tirtc_start_deadline_ms = 0;
        s_tirtc_stop_deadline_ms = 0;
        lock_snapshot();
        bool was_ready = s_snapshot.tirtc_ready;
        s_snapshot.tirtc_ready = true;
        s_snapshot.state = APP_STATE_READY;
        unlock_snapshot();
        if (!was_ready || !s_tirtc_ready_reported) {
            s_tirtc_ready_reported = true;
            emit_simple(APP_EVENT_TIRTC,
                        "READY",
                        snapshot_generation(),
                        0,
                        ESP_OK,
                        NULL,
                        NULL);
            (void)session_runtime_call_recover(0);
        }
    } else if (adapter_state == TIRTC_ADAPTER_RUNNING) {
        s_tirtc_start_deadline_ms = 0;
        lock_snapshot();
        s_snapshot.tirtc_ready = false;
        if (!wifi_online) {
            s_snapshot.state = APP_STATE_NETWORKING;
        } else if (!platform_ready || !mqtt_online) {
            s_snapshot.state = APP_STATE_STARTING_PLATFORM;
        }
        unlock_snapshot();
        s_tirtc_ready_reported = false;
    } else if (adapter_state == TIRTC_ADAPTER_STARTING &&
               s_tirtc_start_deadline_ms != 0 &&
               current_ms >= s_tirtc_start_deadline_ms) {
        (void)session_runtime_abort("tirtc_start_timeout", 0);
        media_runtime_stop();
        int stop_error = tirtc_adapter_request_stop();
        s_tirtc_start_deadline_ms = 0;
        emit_simple(APP_EVENT_ERROR,
                    "TIRTC_START_TIMEOUT",
                    snapshot_generation(),
                    0,
                    stop_error == 0 ? ESP_ERR_TIMEOUT : ESP_FAIL,
                    NULL,
                    NULL);
        if (stop_error != 0) {
            schedule_restart("tirtc_start_stop_failed");
            return;
        }
        s_tirtc_stop_deadline_ms = current_ms + TIRTC_STOP_TIMEOUT_MS;
    } else if (adapter_state == TIRTC_ADAPTER_STOPPING &&
               s_tirtc_stop_deadline_ms != 0 &&
               current_ms >= s_tirtc_stop_deadline_ms) {
        (void)session_runtime_abort("tirtc_stop_timeout", 0);
        schedule_restart("tirtc_stop_timeout");
    } else if (adapter_state == TIRTC_ADAPTER_STOPPED) {
        media_runtime_stop();
        int deinit_error = tirtc_adapter_deinit();
        if (deinit_error != 0) {
            schedule_restart("tirtc_deinit_failed");
            return;
        }
        s_runtime_started = false;
        s_tirtc_ready_reported = false;
        s_tirtc_start_deadline_ms = 0;
        s_tirtc_stop_deadline_ms = 0;
        s_runtime_retry_at_ms = current_ms + 5000;
    } else if (adapter_state == TIRTC_ADAPTER_ERROR) {
        lock_snapshot();
        s_snapshot.tirtc_ready = false;
        unlock_snapshot();
        bool should_report = s_tirtc_ready_reported;
        s_tirtc_ready_reported = false;
        if (should_report) {
            emit_simple(APP_EVENT_TIRTC,
                        "ERROR",
                        snapshot_generation(),
                        0,
                        ESP_FAIL,
                         NULL,
                         NULL);
        }
        (void)session_runtime_abort("tirtc_adapter_recycle", 0);
        media_runtime_stop();
        if (tirtc_adapter_restart_required()) {
            schedule_restart("tirtc_failed_connect_transport");
            return;
        }
        int stop_error = tirtc_adapter_request_stop();
        s_tirtc_start_deadline_ms = 0;
        if (stop_error == 0) {
            s_tirtc_stop_deadline_ms = current_ms + TIRTC_STOP_TIMEOUT_MS;
        } else {
            schedule_restart("tirtc_recycle_stop_failed");
        }
    }

    session_runtime_snapshot_t session = {0};
    if (session_runtime_get_snapshot(&session) == ESP_OK) {
        lock_snapshot();
        (void)snprintf(s_snapshot.session_owner,
                       sizeof(s_snapshot.session_owner),
                       "%s",
                       app_session_owner_name(session.owner));
        (void)snprintf(s_snapshot.session_state,
                       sizeof(s_snapshot.session_state),
                       "%s",
                       device_session_state_name(session.state));
        unlock_snapshot();
    }
}

static esp_err_t dispatch_session_intent(const app_intent_t *intent)
{
    lock_snapshot();
    const bool session_start_ready =
        s_snapshot.state == APP_STATE_READY &&
        s_snapshot.mqtt_online && s_snapshot.tirtc_ready;
    const bool platform_request_ready =
        s_snapshot.wifi_online && s_snapshot.platform_ready;
    unlock_snapshot();

    const bool starts_session =
        intent->type == APP_INTENT_AI_START ||
        intent->type == APP_INTENT_CALL_START;
    const bool uses_platform_only =
        intent->type == APP_INTENT_CONTACTS_LIST ||
        intent->type == APP_INTENT_PENDING_LIST ||
        intent->type == APP_INTENT_CONTACT_REQUEST ||
        intent->type == APP_INTENT_CONTACT_RESPOND ||
        intent->type == APP_INTENT_CONTACT_REMARK ||
        intent->type == APP_INTENT_CONTACT_DELETE;
    if ((starts_session && !session_start_ready) ||
        (uses_platform_only && !platform_request_ready)) {
        return ESP_ERR_INVALID_STATE;
    }
    switch (intent->type) {
    case APP_INTENT_AI_START:
        return session_runtime_ai_start(intent->request_id);
    case APP_INTENT_AI_STOP:
        return session_runtime_ai_stop(intent->request_id);
    case APP_INTENT_AI_INTERRUPT:
        return session_runtime_ai_interrupt(intent->request_id);
    case APP_INTENT_AI_SUBMIT:
        return session_runtime_ai_submit_speech(intent->request_id);
    case APP_INTENT_AI_UPDATE:
        return session_runtime_ai_update_config(intent->first,
                                                intent->request_id);
    case APP_INTENT_AI_ACTION_RESULT:
        return session_runtime_ai_action_result(intent->first,
                                                intent->accepted,
                                                intent->second,
                                                intent->request_id);
    case APP_INTENT_CALL_START:
        return session_runtime_call_start(intent->first,
                                          strcasecmp(intent->second,
                                                     "VIDEO") == 0
                                              ? SESSION_RUNTIME_CALL_VIDEO
                                              : SESSION_RUNTIME_CALL_AUDIO,
                                          intent->request_id);
    case APP_INTENT_CALL_ACCEPT:
        return session_runtime_call_accept(intent->request_id);
    case APP_INTENT_CALL_REJECT:
        return session_runtime_call_reject(intent->request_id);
    case APP_INTENT_CALL_CANCEL:
        return session_runtime_call_cancel(intent->request_id);
    case APP_INTENT_CALL_HANGUP:
        return session_runtime_call_hangup(intent->request_id);
    case APP_INTENT_CONTACTS_LIST:
        return session_runtime_contacts_list(intent->request_id);
    case APP_INTENT_PENDING_LIST:
        return session_runtime_contacts_pending(intent->request_id);
    case APP_INTENT_CONTACT_REQUEST:
        return session_runtime_contacts_request(intent->first,
                                                intent->request_id);
    case APP_INTENT_CONTACT_RESPOND:
        return session_runtime_contacts_respond(
            intent->first,
            strcasecmp(intent->second, "ACCEPT") == 0,
            intent->request_id);
    case APP_INTENT_CONTACT_REMARK:
        return session_runtime_contacts_remark(intent->first,
                                               intent->second,
                                               intent->request_id);
    case APP_INTENT_CONTACT_DELETE:
        return session_runtime_contacts_delete(intent->first,
                                               intent->request_id);
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static void handle_intent(app_intent_t *intent)
{
    esp_err_t err = ESP_OK;
    const char *operation = app_intent_name(intent->type);
    session_runtime_snapshot_t session = {
        .owner = DEVICE_SERVICE_NONE,
    };
    bool session_idle = !s_session_started ||
                        (session_runtime_get_snapshot(&session) == ESP_OK &&
                         session.owner == DEVICE_SERVICE_NONE);
    switch (intent->type) {
    case APP_INTENT_WIFI_SET:
        err = session_idle
                  ? wifi_manager_set_credentials(intent->first, intent->second)
                  : ESP_ERR_INVALID_STATE;
        if (err == ESP_OK) schedule_restart("wifi_config_changed");
        break;
    case APP_INTENT_WIFI_CLEAR:
        err = session_idle
                  ? wifi_manager_clear_credentials()
                  : ESP_ERR_INVALID_STATE;
        if (err == ESP_OK) schedule_restart("wifi_config_cleared");
        break;
    case APP_INTENT_BIND_START:
        lock_snapshot();
        bool wifi_online = s_snapshot.wifi_online;
        bool binding_allowed = s_snapshot.state == APP_STATE_NEED_BINDING;
        unlock_snapshot();
        if (!wifi_online || !binding_allowed) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            err = start_bootstrap(BOOTSTRAP_PROVISION);
        }
        break;
    case APP_INTENT_RESTART:
        schedule_restart("at_request");
        break;
    default:
        err = dispatch_session_intent(intent);
        break;
    }

    emit_simple(err == ESP_OK ? APP_EVENT_SESSION : APP_EVENT_ERROR,
                err == ESP_OK ? "REQUEST" : "REQUEST_REJECTED",
                snapshot_generation(),
                intent->request_id,
                err,
                operation,
                NULL);
    secure_zero(intent, sizeof(*intent));
}

static void handle_bootstrap_done(const bootstrap_result_t *result)
{
    s_bootstrap_task = NULL;
    if (result->generation != snapshot_generation()) {
        return;
    }
    if (result->provisioned && result->error == ESP_OK) {
        reset_bootstrap_retry();
        lock_snapshot();
        (void)snprintf(s_snapshot.device_id,
                       sizeof(s_snapshot.device_id),
                       "%s",
                       result->device_id);
        s_snapshot.verification_code[0] = '\0';
        unlock_snapshot();
        emit_simple(APP_EVENT_BIND,
                    "BOUND",
                    result->generation,
                    0,
                    ESP_OK,
                    result->device_id,
                    NULL);
        schedule_restart("binding_completed");
        return;
    }
    if (result->needs_binding) {
        reset_bootstrap_retry();
        set_app_state(APP_STATE_NEED_BINDING, ESP_OK);
        emit_simple(APP_EVENT_BIND,
                    "REQUIRED",
                    result->generation,
                    0,
                    ESP_OK,
                    NULL,
                    NULL);
        return;
    }
    if (result->error != ESP_OK) {
        emit_simple(APP_EVENT_ERROR,
                    result->attempted_provision ? "BIND" : "PLATFORM_BOOTSTRAP",
                    result->generation,
                    0,
                    result->error,
                    esp_err_to_name(result->error),
                    NULL);
        if (result->attempted_provision) {
            reset_bootstrap_retry();
            lock_snapshot();
            s_snapshot.state = APP_STATE_NEED_BINDING;
            s_snapshot.last_error = result->error;
            unlock_snapshot();
            emit_simple(APP_EVENT_BIND,
                        "REQUIRED",
                        result->generation,
                        0,
                        result->error,
                        "",
                        NULL);
        } else {
            schedule_bootstrap_retry(result->error);
        }
        return;
    }
    reset_bootstrap_retry();
    lock_snapshot();
    (void)snprintf(s_snapshot.device_id,
                   sizeof(s_snapshot.device_id),
                   "%s",
                   result->device_id);
    s_snapshot.platform_ready = platform_client_ready();
    s_snapshot.state = APP_STATE_STARTING_PLATFORM;
    unlock_snapshot();
}

static void reconcile_async_sources(void)
{
    if (atomic_load_explicit(&s_restart_scheduled, memory_order_acquire)) {
        return;
    }
    if (atomic_exchange_explicit(&s_bootstrap_result_pending,
                                 false,
                                 memory_order_acq_rel)) {
        bootstrap_result_t result;
        lock_snapshot();
        result = s_bootstrap_result;
        secure_zero(&s_bootstrap_result, sizeof(s_bootstrap_result));
        unlock_snapshot();
        handle_bootstrap_done(&result);
        secure_zero(&result, sizeof(result));
    }

    if (atomic_exchange_explicit(&s_wifi_reconcile_needed,
                                 false,
                                 memory_order_acq_rel)) {
        wifi_manager_snapshot_t snapshot = {0};
        if (wifi_manager_get_snapshot(&snapshot) == ESP_OK) {
            wifi_manager_event_t event = {
                .type = WIFI_MANAGER_EVENT_SNAPSHOT,
                .snapshot = snapshot,
            };
            handle_wifi(&event);
        }
    }

    if (atomic_exchange_explicit(&s_rebind_needed,
                                 false,
                                 memory_order_acq_rel)) {
        platform_client_event_t event = {
            .type = PLATFORM_CLIENT_EVENT_REBIND_REQUIRED,
            .source = PLATFORM_CLIENT_EVENT_SOURCE_AUTH,
            .error = ESP_ERR_NOT_FOUND,
        };
        handle_platform(&event);
    }

    lock_snapshot();
    bool wifi_online = s_snapshot.wifi_online;
    unlock_snapshot();
    bool actual_mqtt = wifi_online &&
                       platform_client_ready() &&
                       platform_client_mqtt_connected();
    lock_snapshot();
    bool recorded_mqtt = s_snapshot.mqtt_online;
    unlock_snapshot();
    if (actual_mqtt != recorded_mqtt) {
        platform_client_event_t event = {
            .type = actual_mqtt
                        ? PLATFORM_CLIENT_EVENT_MQTT_CONNECTED
                        : PLATFORM_CLIENT_EVENT_MQTT_DISCONNECTED,
            .source = PLATFORM_CLIENT_EVENT_SOURCE_MQTT,
        };
        handle_platform(&event);
    }

    const char *verification_code = platform_client_verification_code();
    if (verification_code != NULL && verification_code[0] != '\0') {
        char code[17];
        (void)strlcpy(code, verification_code, sizeof(code));
        lock_snapshot();
        bool changed = strcmp(s_snapshot.verification_code, code) != 0;
        unlock_snapshot();
        if (changed) {
            platform_client_event_t event = {
                .type = PLATFORM_CLIENT_EVENT_PROVISION_CODE,
                .source = PLATFORM_CLIENT_EVENT_SOURCE_PROVISION,
            };
            (void)strlcpy(event.provision_code,
                          code,
                          sizeof(event.provision_code));
            handle_platform(&event);
        }
    }

    if (s_bootstrap_retry_pending && s_bootstrap_task == NULL &&
        esp_timer_get_time() / 1000 >= s_bootstrap_retry_at_ms) {
        lock_snapshot();
        const bool can_retry = s_snapshot.wifi_online &&
                               !s_snapshot.platform_ready;
        unlock_snapshot();
        if (can_retry) {
            esp_err_t err = start_bootstrap(BOOTSTRAP_EXISTING);
            if (err != ESP_OK) {
                schedule_bootstrap_retry(err);
            }
        }
    }
}

static void controller_task(void *argument)
{
    (void)argument;
    emit_simple(APP_EVENT_SYSTEM,
                "BOOTING",
                snapshot_generation(),
                0,
                ESP_OK,
                NULL,
                NULL);
    for (;;) {
        controller_event_t event;
        if (xQueueReceive(s_queue, &event, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (!atomic_load_explicit(&s_restart_scheduled,
                                      memory_order_acquire)) {
                switch (event.type) {
                case CONTROLLER_EVENT_WIFI:
                    handle_wifi(event.payload);
                    break;
                case CONTROLLER_EVENT_PLATFORM:
                    handle_platform(event.payload);
                    break;
                case CONTROLLER_EVENT_SESSION:
                    handle_session(event.payload);
                    break;
                case CONTROLLER_EVENT_INTENT:
                    handle_intent(event.payload);
                    break;
                default:
                    break;
                }
            }
            if (event.type == CONTROLLER_EVENT_INTENT &&
                event.payload != NULL) {
                secure_zero(event.payload, sizeof(app_intent_t));
            }
            free(event.payload);
        }
        if (atomic_load_explicit(&s_restart_scheduled, memory_order_acquire)) {
            continue;
        }
        report_session_overflow();
        reconcile_async_sources();
        reconcile_runtime();
    }
}

esp_err_t app_controller_init(app_event_callback_t callback, void *user_data)
{
    if (s_task != NULL || callback == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = app_identity_read(s_client_id,
                                      sizeof(s_client_id),
                                      s_mac_text,
                                      sizeof(s_mac_text));
    if (err != ESP_OK) {
        return err;
    }
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(CONTROLLER_QUEUE_DEPTH, sizeof(controller_event_t));
    s_session_critical_fallback =
        xQueueCreate(1, sizeof(session_runtime_event_t));
    s_session_incoming_fallback =
        xQueueCreate(1, sizeof(session_runtime_event_t));
    s_session_action_fallback =
        xQueueCreate(1, sizeof(session_runtime_event_t));
    s_session_general_fallback =
        xQueueCreate(1, sizeof(session_runtime_event_t));
    if (s_lock == NULL || s_queue == NULL ||
        s_session_critical_fallback == NULL ||
        s_session_incoming_fallback == NULL ||
        s_session_action_fallback == NULL ||
        s_session_general_fallback == NULL) {
        if (s_queue != NULL) {
            vQueueDelete(s_queue);
            s_queue = NULL;
        }
        if (s_session_critical_fallback != NULL) {
            vQueueDelete(s_session_critical_fallback);
            s_session_critical_fallback = NULL;
        }
        if (s_session_incoming_fallback != NULL) {
            vQueueDelete(s_session_incoming_fallback);
            s_session_incoming_fallback = NULL;
        }
        if (s_session_action_fallback != NULL) {
            vQueueDelete(s_session_action_fallback);
            s_session_action_fallback = NULL;
        }
        if (s_session_general_fallback != NULL) {
            vQueueDelete(s_session_general_fallback);
            s_session_general_fallback = NULL;
        }
        if (s_lock != NULL) {
            vSemaphoreDelete(s_lock);
            s_lock = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    s_callback = callback;
    s_callback_user_data = user_data;
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.state = APP_STATE_BOOTING;
    s_snapshot.generation = 1;
    reset_bootstrap_retry();
    atomic_store_explicit(&s_session_overflow_count,
                          0,
                          memory_order_release);
    atomic_store_explicit(&s_restart_scheduled, false, memory_order_release);
    (void)snprintf(s_snapshot.session_owner,
                   sizeof(s_snapshot.session_owner),
                   "none");
    (void)snprintf(s_snapshot.session_state,
                   sizeof(s_snapshot.session_state),
                   "offline");

    err = wifi_manager_init();
    if (err == ESP_OK) {
        err = wifi_manager_set_observer(wifi_observer, NULL);
    }
    platform_client_set_observer(platform_observer, NULL);
    session_runtime_set_observer(session_observer, NULL);
    if (err == ESP_OK) {
        err = session_runtime_start();
        if (err == ESP_OK) {
            s_session_started = true;
        }
    }
    if (err == ESP_OK) {
        err = wifi_manager_start();
    }
    if (err == ESP_OK &&
        xTaskCreate(controller_task,
                    "app_control",
                    CONTROLLER_TASK_STACK,
                    NULL,
                    6,
                    &s_task) != pdPASS) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        return ESP_OK;
    }
    if (s_session_started) {
        esp_restart();
        return err;
    }

    (void)wifi_manager_set_observer(NULL, NULL);
    (void)wifi_manager_stop();
    (void)wifi_manager_deinit();
    platform_client_set_observer(NULL, NULL);
    session_runtime_set_observer(NULL, NULL);
    controller_event_t pending;
    while (xQueueReceive(s_queue, &pending, 0) == pdTRUE) {
        if (pending.type == CONTROLLER_EVENT_INTENT &&
            pending.payload != NULL) {
            secure_zero(pending.payload, sizeof(app_intent_t));
        }
        free(pending.payload);
    }
    vQueueDelete(s_queue);
    vQueueDelete(s_session_critical_fallback);
    vQueueDelete(s_session_incoming_fallback);
    vQueueDelete(s_session_action_fallback);
    vQueueDelete(s_session_general_fallback);
    vSemaphoreDelete(s_lock);
    s_queue = NULL;
    s_session_critical_fallback = NULL;
    s_session_incoming_fallback = NULL;
    s_session_action_fallback = NULL;
    s_session_general_fallback = NULL;
    s_lock = NULL;
    s_callback = NULL;
    s_callback_user_data = NULL;
    return err;
}

esp_err_t app_controller_submit(const app_intent_t *intent)
{
    if (intent == NULL || s_queue == NULL || s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (atomic_load_explicit(&s_restart_scheduled, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    app_intent_t *copy = malloc(sizeof(*copy));
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *copy = *intent;
    controller_event_t event = {
        .type = CONTROLLER_EVENT_INTENT,
        .payload = copy,
    };
    if (xQueueSend(s_queue, &event, 0) != pdTRUE) {
        secure_zero(copy, sizeof(*copy));
        free(copy);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void app_controller_snapshot(app_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    lock_snapshot();
    *snapshot = s_snapshot;
    unlock_snapshot();
}

esp_err_t app_controller_session_snapshot(app_session_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    session_runtime_snapshot_t session = {0};
    esp_err_t err = session_runtime_get_snapshot(&session);
    if (err != ESP_OK) {
        return err;
    }

    app_session_snapshot_t result = {
        .app_generation = snapshot_generation(),
        .revision = session.revision,
        .session_generation = session.generation,
        .origin_request_id = session.origin_request_id,
        .pending_incoming_call = session.pending_incoming_call,
        .caller = session.caller,
        .ai_update_pending = session.ai_update_pending,
        .ai_action_pending = session.ai_action_pending,
        .ai_call_handoff_pending =
            session.ai_call_handoff_pending,
        .call_type = (int)session.call_type,
    };
    if (session.deadline_ms > 0) {
        int64_t remaining = session.deadline_ms - esp_timer_get_time() / 1000;
        result.deadline_remaining_ms = remaining > 0 ? remaining : 0;
    }
    if (session.ai_update_deadline_ms > 0) {
        int64_t remaining =
            session.ai_update_deadline_ms - esp_timer_get_time() / 1000;
        result.ai_update_deadline_remaining_ms =
            remaining > 0 ? remaining : 0;
    }
    if (session.ai_action_deadline_ms > 0) {
        int64_t remaining =
            session.ai_action_deadline_ms - esp_timer_get_time() / 1000;
        result.ai_action_deadline_remaining_ms =
            remaining > 0 ? remaining : 0;
    }
    if (session.ai_call_handoff_deadline_ms > 0) {
        int64_t remaining =
            session.ai_call_handoff_deadline_ms -
            esp_timer_get_time() / 1000;
        result.ai_call_handoff_deadline_remaining_ms =
            remaining > 0 ? remaining : 0;
    }
    (void)snprintf(result.owner,
                   sizeof(result.owner),
                   "%s",
                   app_session_owner_name(session.owner));
    (void)snprintf(result.state,
                   sizeof(result.state),
                   "%s",
                   device_session_state_name(session.state));
    (void)snprintf(
        result.ai_call_handoff_phase,
        sizeof(result.ai_call_handoff_phase),
        "%s",
        session_runtime_handoff_phase_name(
            session.ai_call_handoff_phase));
    copy_text(result.room_id, sizeof(result.room_id), session.room_id);
    copy_text(result.peer_id, sizeof(result.peer_id), session.peer_id);
    copy_text(result.ai_session_id,
              sizeof(result.ai_session_id),
              session.ai_session_id);
    copy_text(result.ai_action_id,
              sizeof(result.ai_action_id),
              session.ai_action_id);
    *snapshot = result;
    return ESP_OK;
}

esp_err_t app_controller_media_snapshot(app_media_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    tirtc_adapter_metrics_t metrics = {0};
    tirtc_adapter_get_metrics(&metrics);
    *snapshot = (app_media_snapshot_t) {
        .connected = metrics.connected,
        .adapter_state = (int)metrics.adapter_state,
        .active_profile = (int)metrics.active_profile,
        .measured_profile = (int)metrics.measured_profile,
        .active_session_generation = metrics.active_session_generation,
        .measured_session_generation = metrics.measured_session_generation,
        .connection_generation = metrics.connection_generation,
        .tx_audio_frames = metrics.tx_audio_frames,
        .tx_audio_bytes = metrics.tx_audio_bytes,
        .tx_video_frames = metrics.tx_video_frames,
        .tx_video_bytes = metrics.tx_video_bytes,
        .rx_audio_frames = metrics.rx_audio_frames,
        .rx_audio_bytes = metrics.rx_audio_bytes,
        .rx_video_frames = metrics.rx_video_frames,
        .rx_video_bytes = metrics.rx_video_bytes,
        .send_errors = metrics.send_errors,
        .first_tx_ms = metrics.first_tx_ms,
        .last_tx_ms = metrics.last_tx_ms,
        .first_rx_ms = metrics.first_rx_ms,
        .last_rx_ms = metrics.last_rx_ms,
        .connect_request_pending = metrics.connect_request_pending,
        .connect_callback_pending = metrics.connect_callback_pending,
        .accept_callbacks_pending = metrics.accept_callbacks_pending,
        .disconnects_pending = metrics.disconnects_pending,
        .connection_users = metrics.connection_users,
        .incoming_armed = metrics.incoming_armed,
    };
    return ESP_OK;
}
