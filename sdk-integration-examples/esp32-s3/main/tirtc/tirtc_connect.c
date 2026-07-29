/*
 * 主动连接任务：优先复用 SDK 缓存，未命中时使用服务端签发的一次性 token。
 */
#include "tirtc_connect.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#include "tirtc_config.h"
#include "tirtc_app.h"

static const char *TAG = "tirtc_connect";

#define CONNECT_TASK_STACK_BYTES 4096
#define CONNECT_TASK_PRIORITY 5
#define CONNECT_RESULT_TIMEOUT_MS 30000U
#define CONNECT_CANCEL_POLL_MS 5U
#define CONNECT_TOKEN_MARKER_A_PATH "/spiffs/.tirtc_tok_a"
#define CONNECT_TOKEN_MARKER_B_PATH "/spiffs/.tirtc_tok_b"
#define CONNECT_TOKEN_MARKER_MAGIC 0x544B4E31U

typedef struct
{
    uint32_t magic;
    uint32_t sequence;
    uint8_t fingerprint[32];
    uint32_t crc32;
} connect_token_marker_t;

static portMUX_TYPE s_connect_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_tirtc_online;
static bool s_connecting;
static bool s_configured_token_consumed;
static uint32_t s_connect_workers_inflight;
static uint32_t s_connect_callbacks_inflight;
static uint32_t s_connect_generation;
static uint32_t s_timeout_generation;
static int64_t s_timeout_deadline_us;
static TIRTCCONNECTCALLBACK s_connect_callback;
static void *s_connect_user_data;
static esp_timer_handle_t s_timeout_timer;

static bool attempt_is_current_locked(uint32_t generation)
{
    return s_tirtc_online && s_connecting && generation == s_connect_generation;
}

static void reset_attempt_locked(uint32_t generation)
{
    if (generation == s_connect_generation)
    {
        s_connecting = false;
        s_connect_callback = NULL;
        s_connect_user_data = NULL;
    }
}

static void release_connect_worker(void)
{
    portENTER_CRITICAL(&s_connect_lock);
    if (s_connect_workers_inflight > 0U)
    {
        s_connect_workers_inflight--;
    }
    portEXIT_CRITICAL(&s_connect_lock);
}

static void connect_worker_exit(void)
{
    release_connect_worker();
    vTaskDelete(NULL);
}

static void connect_callback_exit(void)
{
    portENTER_CRITICAL(&s_connect_lock);
    if (s_connect_callbacks_inflight > 0U)
    {
        s_connect_callbacks_inflight--;
    }
    portEXIT_CRITICAL(&s_connect_lock);
}

static void invalidate_timeout_generation(uint32_t generation)
{
    portENTER_CRITICAL(&s_connect_lock);
    if (generation == s_timeout_generation)
    {
        s_timeout_generation = 0U;
        s_timeout_deadline_us = 0;
    }
    portEXIT_CRITICAL(&s_connect_lock);
}

static void stop_timeout_timer_all(void)
{
    portENTER_CRITICAL(&s_connect_lock);
    s_timeout_generation = 0U;
    s_timeout_deadline_us = 0;
    portEXIT_CRITICAL(&s_connect_lock);
    if (s_timeout_timer != NULL)
    {
        (void)esp_timer_stop(s_timeout_timer);
    }
}

static esp_err_t arm_timeout_timer(uint32_t generation)
{
    bool current_attempt = false;

    portENTER_CRITICAL(&s_connect_lock);
    current_attempt = attempt_is_current_locked(generation);
    if (current_attempt)
    {
        s_timeout_generation = generation;
        s_timeout_deadline_us = esp_timer_get_time() +
                                (int64_t)CONNECT_RESULT_TIMEOUT_MS * 1000LL;
    }
    portEXIT_CRITICAL(&s_connect_lock);
    if (!current_attempt)
    {
        return ESP_ERR_INVALID_STATE;
    }

    (void)esp_timer_stop(s_timeout_timer);
    esp_err_t ret = esp_timer_start_once(s_timeout_timer,
                                         (uint64_t)CONNECT_RESULT_TIMEOUT_MS * 1000ULL);
    if (ret != ESP_OK)
    {
        portENTER_CRITICAL(&s_connect_lock);
        if (generation == s_timeout_generation)
        {
            s_timeout_generation = 0U;
            s_timeout_deadline_us = 0;
        }
        portEXIT_CRITICAL(&s_connect_lock);
        return ret;
    }

    portENTER_CRITICAL(&s_connect_lock);
    current_attempt = attempt_is_current_locked(generation) &&
                      generation == s_timeout_generation;
    if (!current_attempt && generation == s_timeout_generation)
    {
        s_timeout_generation = 0U;
        s_timeout_deadline_us = 0;
    }
    portEXIT_CRITICAL(&s_connect_lock);
    if (!current_attempt)
    {
        (void)esp_timer_stop(s_timeout_timer);
    }
    return current_attempt ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/*
 * 在短临界区内完成“仍可连接”的最终判断。cancel 会等待 worker 退出，因此
 * 一旦本函数成功，TiRtcConnect() 可以先完成本次同步提交，再由 Stop 接管。
 */
static bool claim_sdk_connect(uint32_t generation,
                              bool consume_configured_token,
                              bool *token_was_consumed)
{
    bool claimed = false;

    if (token_was_consumed != NULL)
    {
        *token_was_consumed = false;
    }

    portENTER_CRITICAL(&s_connect_lock);
    if (attempt_is_current_locked(generation))
    {
        if (consume_configured_token && s_configured_token_consumed)
        {
            if (token_was_consumed != NULL)
            {
                *token_was_consumed = true;
            }
        }
        else
        {
            if (consume_configured_token)
            {
                s_configured_token_consumed = true;
            }
            claimed = true;
        }
    }
    portEXIT_CRITICAL(&s_connect_lock);

    return claimed;
}

void tirtc_connect_on_tirtc_started(void)
{
    portENTER_CRITICAL(&s_connect_lock);
    s_tirtc_online = true;
    portEXIT_CRITICAL(&s_connect_lock);
    ESP_LOGI(TAG, "主动连接入口已就绪，可按 BOOT 连接远端设备");
}

static void on_tirtc_connect_result(int error, tirtc_conn_t hconn, void *user_data)
{
    uint32_t generation = (uint32_t)(uintptr_t)user_data;
    TIRTCCONNECTCALLBACK callback = NULL;
    void *callback_user_data = NULL;
    bool current_attempt = false;
    bool may_use_sdk = false;

    portENTER_CRITICAL(&s_connect_lock);
    s_connect_callbacks_inflight++;
    current_attempt = attempt_is_current_locked(generation);
    may_use_sdk = s_tirtc_online;
    if (current_attempt && may_use_sdk)
    {
        callback = s_connect_callback;
        callback_user_data = s_connect_user_data;
        reset_attempt_locked(generation);
    }
    portEXIT_CRITICAL(&s_connect_lock);

    if (!current_attempt || !may_use_sdk)
    {
        if (error == 0 && hconn != NULL)
        {
            ESP_LOGW(TAG,
                     "收到已取消或停止阶段迟到的主动连接结果，交给统一连接 owner hconn=%p",
                     hconn);
            tirtc_release_unowned_connection(hconn);
        }
        connect_callback_exit();
        return;
    }

    if (callback != NULL)
    {
        callback(error, hconn, callback_user_data);
    }
    connect_callback_exit();
}

static void finish_connect_attempt(uint32_t generation, int error)
{
    TIRTCCONNECTCALLBACK callback = NULL;
    void *callback_user_data = NULL;
    bool notify = false;

    portENTER_CRITICAL(&s_connect_lock);
    if (attempt_is_current_locked(generation))
    {
        callback = s_connect_callback;
        callback_user_data = s_connect_user_data;
        reset_attempt_locked(generation);
        notify = true;
    }
    portEXIT_CRITICAL(&s_connect_lock);

    /*
     * Only invalidate the generation here. Stopping the shared timer after
     * dropping the state lock could stop a newer generation that has already
     * re-armed the same handle.
     */
    invalidate_timeout_generation(generation);

    if (notify && callback != NULL)
    {
        callback(error, NULL, callback_user_data);
    }
}

static void tirtc_connect_timeout_callback(void *arg)
{
    (void)arg;

    uint32_t generation = 0;
    TIRTCCONNECTCALLBACK callback = NULL;
    void *callback_user_data = NULL;
    bool notify = false;

    portENTER_CRITICAL(&s_connect_lock);
    s_connect_callbacks_inflight++;
    portEXIT_CRITICAL(&s_connect_lock);

    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_connect_lock);
    generation = s_timeout_generation;
    if (generation != 0U &&
        attempt_is_current_locked(generation) &&
        now_us + 1000LL >= s_timeout_deadline_us)
    {
        callback = s_connect_callback;
        callback_user_data = s_connect_user_data;
        reset_attempt_locked(generation);
        s_timeout_generation = 0U;
        s_timeout_deadline_us = 0;
        notify = true;
    }
    portEXIT_CRITICAL(&s_connect_lock);

    if (notify)
    {
        ESP_LOGW(TAG, "主动连接等待结果超时: %u ms", CONNECT_RESULT_TIMEOUT_MS);
        if (callback != NULL)
        {
            callback(TIRTC_E_TIMEOUTED, NULL, callback_user_data);
        }
    }

    connect_callback_exit();
}

esp_err_t tirtc_connect_prepare(void)
{
    if (s_timeout_timer != NULL)
    {
        return ESP_OK;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = tirtc_connect_timeout_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "tirtc_conn_to",
        .skip_unhandled_events = true,
    };

    esp_err_t ret = esp_timer_create(&timer_args, &s_timeout_timer);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "创建主动连接超时定时器失败: %s", esp_err_to_name(ret));
    }
    return ret;
}

static bool configured_token_fingerprint(uint8_t fingerprint[32])
{
    return mbedtls_sha256((const unsigned char *)TIRTC_CONNECT_TOKEN,
                          strlen(TIRTC_CONNECT_TOKEN),
                          fingerprint,
                          0) == 0;
}

static uint32_t token_marker_crc(const connect_token_marker_t *marker)
{
    return esp_rom_crc32_le(0U,
                            (const uint8_t *)marker,
                            (uint32_t)offsetof(connect_token_marker_t, crc32));
}

static bool read_token_marker(const char *path,
                              connect_token_marker_t *marker,
                              bool *exists)
{
    if (marker == NULL || exists == NULL)
    {
        return false;
    }

    *exists = false;
    memset(marker, 0, sizeof(*marker));

    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
    {
        return false;
    }

    *exists = true;
    size_t bytes_read = fread(marker, 1, sizeof(*marker), fp);
    int trailing = fgetc(fp);
    fclose(fp);

    return bytes_read == sizeof(*marker) &&
           trailing == EOF &&
           marker->magic == CONNECT_TOKEN_MARKER_MAGIC &&
           marker->sequence != 0U &&
           marker->crc32 == token_marker_crc(marker);
}

static bool write_token_marker(const char *path, connect_token_marker_t *marker)
{
    marker->crc32 = token_marker_crc(marker);

    FILE *fp = fopen(path, "wb");
    if (fp == NULL)
    {
        return false;
    }

    size_t bytes_written = fwrite(marker, 1, sizeof(*marker), fp);
    int flush_ret = fflush(fp);
    int close_ret = fclose(fp);
    if (bytes_written != sizeof(*marker) || flush_ret != 0 || close_ret != 0)
    {
        return false;
    }

    connect_token_marker_t verified = {0};
    bool exists = false;
    return read_token_marker(path, &verified, &exists) &&
           exists &&
           memcmp(marker, &verified, sizeof(*marker)) == 0;
}

static esp_err_t reserve_configured_token(void)
{
    uint8_t fingerprint[32] = {0};
    connect_token_marker_t marker_a = {0};
    connect_token_marker_t marker_b = {0};
    bool exists_a = false;
    bool exists_b = false;

    if (!configured_token_fingerprint(fingerprint))
    {
        ESP_LOGE(TAG, "计算一次性 token 指纹失败");
        return ESP_FAIL;
    }

    bool valid_a = read_token_marker(CONNECT_TOKEN_MARKER_A_PATH, &marker_a, &exists_a);
    bool valid_b = read_token_marker(CONNECT_TOKEN_MARKER_B_PATH, &marker_b, &exists_b);

    const connect_token_marker_t *latest = NULL;
    const char *target_path = CONNECT_TOKEN_MARKER_A_PATH;
    if (valid_a && (!valid_b || marker_a.sequence >= marker_b.sequence))
    {
        latest = &marker_a;
        target_path = CONNECT_TOKEN_MARKER_B_PATH;
    }
    else if (valid_b)
    {
        latest = &marker_b;
        target_path = CONNECT_TOKEN_MARKER_A_PATH;
    }

    if ((exists_a && !valid_a) || (exists_b && !valid_b))
    {
        ESP_LOGW(TAG, "发现无效 token 标记槽；保留有效槽，并在下次预留新 token 时覆盖无效槽");
    }

    if ((valid_a &&
         memcmp(fingerprint, marker_a.fingerprint, sizeof(fingerprint)) == 0) ||
        (valid_b &&
         memcmp(fingerprint, marker_b.fingerprint, sizeof(fingerprint)) == 0))
    {
        ESP_LOGE(TAG, "当前一次性 token 已命中本机近期退役记录，请签发新 token");
        return ESP_ERR_INVALID_STATE;
    }

    connect_token_marker_t next = {
        .magic = CONNECT_TOKEN_MARKER_MAGIC,
        .sequence = latest != NULL ? latest->sequence + 1U : 1U,
    };
    if (next.sequence == 0U)
    {
        next.sequence = 1U;
    }
    memcpy(next.fingerprint, fingerprint, sizeof(next.fingerprint));

    if (!write_token_marker(target_path, &next))
    {
        ESP_LOGE(TAG, "持久化一次性 token 双槽标记失败，拒绝预留");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void tirtc_connect_task(void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;

#if TIRTC_CONNECT_CACHE_ENABLED
    if (!claim_sdk_connect(generation, false, NULL))
    {
        ESP_LOGW(TAG, "主动连接已取消");
        connect_worker_exit();
        return;
    }

    ESP_LOGI(TAG, "主动连接先尝试复用 SDK 连接参数缓存 remote_id=%s", TIRTC_REMOTE_DEVICE_ID);
    int cache_ret = TiRtcConnect(TIRTC_REMOTE_DEVICE_ID,
                                 NULL,
                                 on_tirtc_connect_result,
                                 (void *)(uintptr_t)generation);
    if (cache_ret == 0)
    {
        esp_err_t timer_ret = arm_timeout_timer(generation);
        if (timer_ret == ESP_OK)
        {
            ESP_LOGI(TAG, "缓存连接请求已提交，最终结果以异步回调为准");
        }
        else if (timer_ret != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGE(TAG, "启动主动连接超时定时器失败: %s", esp_err_to_name(timer_ret));
            finish_connect_attempt(generation, TIRTC_E_TIMEOUTED);
        }
        connect_worker_exit();
        return;
    }
    if (cache_ret != TIRTC_E_CACHE_EXPIRED)
    {
        ESP_LOGE(TAG, "缓存连接调用失败 ret=%d %s", cache_ret, TiRtcGetErrorStr(cache_ret));
        finish_connect_attempt(generation, cache_ret);
        connect_worker_exit();
        return;
    }
    ESP_LOGI(TAG, "连接参数缓存未命中或已过期，改用新的一次性 token");
#endif

    if (TIRTC_CONNECT_TOKEN[0] == '\0' ||
        strcmp(TIRTC_CONNECT_TOKEN, "short_lived_one_time_connect_token") == 0)
    {
        ESP_LOGE(TAG, "缺少一次性连接 token，请从业务服务端或 TiRTC DevTools 获取后写入 local_config.h");
        finish_connect_attempt(generation, TIRTC_E_INVALID_PARAMETER);
        connect_worker_exit();
        return;
    }

    bool token_was_consumed = false;
    if (!claim_sdk_connect(generation, true, &token_was_consumed))
    {
        if (token_was_consumed)
        {
            ESP_LOGE(TAG, "本机已预留过当前一次性 token，请签发新 token 后重新编译并烧录");
            finish_connect_attempt(generation, TIRTC_E_CACHE_EXPIRED);
        }
        else
        {
            ESP_LOGW(TAG, "主动连接已取消");
        }
        connect_worker_exit();
        return;
    }

    esp_err_t token_ret = reserve_configured_token();
    if (token_ret != ESP_OK)
    {
        finish_connect_attempt(generation, TIRTC_E_CACHE_EXPIRED);
        connect_worker_exit();
        return;
    }

    if (!claim_sdk_connect(generation, false, NULL))
    {
        ESP_LOGW(TAG, "主动连接在 token 持久化后已取消，当前 token 保持已消费状态");
        connect_worker_exit();
        return;
    }

    ESP_LOGI(TAG, "使用一次性 token 发起主动连接 remote_id=%s", TIRTC_REMOTE_DEVICE_ID);
    int connect_ret = TiRtcConnect(TIRTC_REMOTE_DEVICE_ID,
                                   TIRTC_CONNECT_TOKEN,
                                   on_tirtc_connect_result,
                                   (void *)(uintptr_t)generation);
    if (connect_ret != 0)
    {
        ESP_LOGE(TAG, "TiRtcConnect 调用失败 ret=%d %s", connect_ret, TiRtcGetErrorStr(connect_ret));
        finish_connect_attempt(generation, connect_ret);
    }
    else
    {
        esp_err_t timer_ret = arm_timeout_timer(generation);
        if (timer_ret != ESP_OK && timer_ret != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGE(TAG, "启动主动连接超时定时器失败: %s", esp_err_to_name(timer_ret));
            finish_connect_attempt(generation, TIRTC_E_TIMEOUTED);
        }
    }

    connect_worker_exit();
}

esp_err_t tirtc_connect_start(TIRTCCONNECTCALLBACK callback, void *user_data)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(TIRTC_REMOTE_DEVICE_ID) == 0 ||
        strcmp(TIRTC_REMOTE_DEVICE_ID, "peer_device_id") == 0)
    {
        ESP_LOGE(TAG, "主动连接目标设备 ID 未配置，请先设置 TIRTC_REMOTE_DEVICE_ID");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t prepare_ret = tirtc_connect_prepare();
    if (prepare_ret != ESP_OK)
    {
        return prepare_ret;
    }

    uint32_t generation = 0;
    portENTER_CRITICAL(&s_connect_lock);
    if (!s_tirtc_online)
    {
        portEXIT_CRITICAL(&s_connect_lock);
        ESP_LOGW(TAG, "TiRTC 尚未上线，暂不能主动连接");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_connecting)
    {
        portEXIT_CRITICAL(&s_connect_lock);
        ESP_LOGW(TAG, "主动连接已经发起，忽略重复调用");
        return ESP_ERR_INVALID_STATE;
    }

    s_connect_generation++;
    if (s_connect_generation == 0U)
    {
        s_connect_generation = 1U;
    }
    generation = s_connect_generation;
    s_connecting = true;
    s_connect_workers_inflight++;
    s_connect_callback = callback;
    s_connect_user_data = user_data;
    portEXIT_CRITICAL(&s_connect_lock);

    ESP_LOGI(TAG, "准备创建主动连接任务 remote_id=%s", TIRTC_REMOTE_DEVICE_ID);

    bool current_attempt = false;
    portENTER_CRITICAL(&s_connect_lock);
    current_attempt = attempt_is_current_locked(generation);
    portEXIT_CRITICAL(&s_connect_lock);
    if (!current_attempt)
    {
        release_connect_worker();
        ESP_LOGW(TAG, "主动连接在任务创建前已取消");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t task_ret = xTaskCreate(tirtc_connect_task,
                                      "tirtc_connect",
                                      CONNECT_TASK_STACK_BYTES,
                                      (void *)(uintptr_t)generation,
                                      CONNECT_TASK_PRIORITY,
                                      NULL);
    if (task_ret != pdPASS)
    {
        portENTER_CRITICAL(&s_connect_lock);
        reset_attempt_locked(generation);
        portEXIT_CRITICAL(&s_connect_lock);
        release_connect_worker();
        ESP_LOGE(TAG, "创建主动连接任务失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "主动连接任务已创建");
    return ESP_OK;
}

bool tirtc_connect_is_connecting(void)
{
    bool connecting = false;
    portENTER_CRITICAL(&s_connect_lock);
    connecting = s_connecting;
    portEXIT_CRITICAL(&s_connect_lock);
    return connecting;
}

bool tirtc_connect_cancel_and_wait(uint32_t timeout_ms)
{
    uint32_t waited_ms = 0;

    portENTER_CRITICAL(&s_connect_lock);
    s_tirtc_online = false;
    s_connecting = false;
    s_connect_generation++;
    s_connect_callback = NULL;
    s_connect_user_data = NULL;
    portEXIT_CRITICAL(&s_connect_lock);

    stop_timeout_timer_all();

    while (waited_ms < timeout_ms)
    {
        bool busy = false;
        portENTER_CRITICAL(&s_connect_lock);
        busy = s_connect_workers_inflight > 0U || s_connect_callbacks_inflight > 0U;
        portEXIT_CRITICAL(&s_connect_lock);

        if (!busy)
        {
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(CONNECT_CANCEL_POLL_MS));
        waited_ms += CONNECT_CANCEL_POLL_MS;
    }

    portENTER_CRITICAL(&s_connect_lock);
    bool busy = s_connect_workers_inflight > 0U || s_connect_callbacks_inflight > 0U;
    portEXIT_CRITICAL(&s_connect_lock);
    return !busy;
}
