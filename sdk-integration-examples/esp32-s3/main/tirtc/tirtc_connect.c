/*
 * 主动连接任务：按下 BOOT 后本地生成测试 token，再调用 TiRtcConnect()。
 */
#include "tirtc_connect.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tirtc_config.h"
#include "tirtc_token.h"

static const char *TAG = "tirtc_connect";

#define CONNECT_TOKEN_MAX_LEN 1536
#define CONNECT_TASK_STACK_BYTES 12288
#define CONNECT_TIMEOUT_TASK_STACK_BYTES 3072
#define CONNECT_TASK_PRIORITY 5
#define CONNECT_RESULT_TIMEOUT_MS 30000U

static bool s_tirtc_online;
static bool s_connecting;
static uint32_t s_connect_generation;
static TIRTCCONNECTCALLBACK s_connect_callback;
static void *s_connect_user_data;

void tirtc_connect_on_tirtc_started(void)
{
    s_tirtc_online = true;
    ESP_LOGI(TAG, "主动连接入口已就绪，可按 BOOT 连接远端设备");
}

static void on_tirtc_connect_result(int error, tirtc_conn_t hconn, void *user_data)
{
    uint32_t generation = (uint32_t)(uintptr_t)user_data;

    TIRTCCONNECTCALLBACK callback = s_connect_callback;
    void *callback_user_data = s_connect_user_data;
    bool current_attempt = s_connecting && generation == s_connect_generation;

    if (!current_attempt)
    {
        if (error == 0 && hconn != NULL)
        {
            ESP_LOGW(TAG, "收到过期主动连接结果，释放连接 hconn=%p", hconn);
            (void)TiRtcDisconnect(hconn);
        }
        return;
    }

    s_connecting = false;
    s_connect_callback = NULL;
    s_connect_user_data = NULL;

    if (!s_tirtc_online)
    {
        if (error == 0 && hconn != NULL)
        {
            ESP_LOGW(TAG, "TiRTC 已停止，释放刚返回的主动连接 hconn=%p", hconn);
            (void)TiRtcDisconnect(hconn);
        }
        return;
    }

    if (callback != NULL)
    {
        callback(error, hconn, callback_user_data);
    }
}

static void finish_connect_attempt(uint32_t generation, int error)
{
    TIRTCCONNECTCALLBACK callback = s_connect_callback;
    void *callback_user_data = s_connect_user_data;

    if (!s_connecting || generation != s_connect_generation)
    {
        return;
    }

    s_connecting = false;
    s_connect_callback = NULL;
    s_connect_user_data = NULL;

    if (callback != NULL && s_tirtc_online)
    {
        callback(error, NULL, callback_user_data);
    }
}

static void tirtc_connect_timeout_task(void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;

    vTaskDelay(pdMS_TO_TICKS(CONNECT_RESULT_TIMEOUT_MS));

    if (s_connecting && generation == s_connect_generation)
    {
        ESP_LOGW(TAG, "主动连接等待结果超时: %u ms", CONNECT_RESULT_TIMEOUT_MS);
        finish_connect_attempt(generation, TIRTC_E_TIMEOUTED);
    }

    vTaskDelete(NULL);
}

static void tirtc_connect_task(void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;

    char connect_token[CONNECT_TOKEN_MAX_LEN];

    /* token 不缓存，每次主动连接都重新生成，方便联调时排查权限和过期问题。 */
    ESP_LOGI(TAG, "开始本地生成本次主动连接 token");
    esp_err_t token_ret = tirtc_token_fetch_connect(TIRTC_REMOTE_DEVICE_ID,
                                                    connect_token,
                                                    sizeof(connect_token));
    if (!s_connecting || generation != s_connect_generation || !s_tirtc_online)
    {
        ESP_LOGW(TAG, "主动连接已取消");
        vTaskDelete(NULL);
        return;
    }

    if (token_ret == ESP_OK)
    {
        ESP_LOGI(TAG, "主动连接开始 remote_id=%s token=local", TIRTC_REMOTE_DEVICE_ID);
        int ret = TiRtcConnect(TIRTC_REMOTE_DEVICE_ID,
                               connect_token,
                               on_tirtc_connect_result,
                               (void *)(uintptr_t)generation);
        if (ret != 0)
        {
            ESP_LOGE(TAG, "TiRtcConnect 调用失败 ret=%d %s", ret, TiRtcGetErrorStr(ret));
            finish_connect_attempt(generation, ret);
        }
    }
    else
    {
        ESP_LOGE(TAG, "本地生成 token 失败: %s", esp_err_to_name(token_ret));
        finish_connect_attempt(generation, TIRTC_E_INVALID_PARAMETER);
    }

    vTaskDelete(NULL);
}

esp_err_t tirtc_connect_start(TIRTCCONNECTCALLBACK callback, void *user_data)
{
    if (!s_tirtc_online)
    {
        ESP_LOGW(TAG, "TiRTC 尚未上线，暂不能主动连接");
        return ESP_ERR_INVALID_STATE;
    }

    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_connecting)
    {
        ESP_LOGW(TAG, "主动连接已经发起，忽略重复调用");
        return ESP_ERR_INVALID_STATE;
    }

    if (strlen(TIRTC_REMOTE_DEVICE_ID) == 0)
    {
        ESP_LOGE(TAG, "主动连接目标设备 ID 为空，请先配置 TIRTC_REMOTE_DEVICE_ID");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "准备创建主动连接任务 remote_id=%s", TIRTC_REMOTE_DEVICE_ID);

    s_connecting = true;
    s_connect_generation++;
    if (s_connect_generation == 0U)
    {
        s_connect_generation = 1U;
    }
    s_connect_callback = callback;
    s_connect_user_data = user_data;
    uint32_t generation = s_connect_generation;

    BaseType_t task_ret = xTaskCreate(tirtc_connect_task,
                                      "tirtc_connect",
                                      CONNECT_TASK_STACK_BYTES,
                                      (void *)(uintptr_t)generation,
                                      CONNECT_TASK_PRIORITY,
                                      NULL);
    if (task_ret != pdPASS)
    {
        ESP_LOGE(TAG, "创建主动连接任务失败");
        s_connecting = false;
        s_connect_callback = NULL;
        s_connect_user_data = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "主动连接任务已创建");

    task_ret = xTaskCreate(tirtc_connect_timeout_task,
                           "tirtc_conn_to",
                           CONNECT_TIMEOUT_TASK_STACK_BYTES,
                           (void *)(uintptr_t)generation,
                           CONNECT_TASK_PRIORITY,
                           NULL);
    if (task_ret != pdPASS)
    {
        ESP_LOGW(TAG, "创建主动连接超时监控任务失败，仍等待 SDK 回调");
    }

    return ESP_OK;
}

bool tirtc_connect_is_connecting(void)
{
    return s_connecting;
}

void tirtc_connect_cancel(void)
{
    s_tirtc_online = false;
    s_connecting = false;
    s_connect_generation++;
    s_connect_callback = NULL;
    s_connect_user_data = NULL;
}
