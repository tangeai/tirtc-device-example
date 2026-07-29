/*
 * TiRTC 应用层入口：负责 SDK 上线、统一回调、当前连接管理和常用控制接口。
 */
#include "tirtc_app.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "local_media_stream.h"
#include "tirtc_config.h"
#include "tirtc_connect.h"

/*
 * 运行时配置与状态
 *
 * 这里放 demo 内部策略和 SDK 当前状态；客户需要改的业务参数放在
 * tirtc_config.h，避免把示例行为和接入参数混在一起。
 */
static const char *TAG = "tirtc_app";

#define TIRTC_MAX_SEND_BUFFER_BYTES (512U * 1024U)
#define TIRTC_VIDEO_STREAM_ID 11
#define TIRTC_AUDIO_STREAM_ID 10
#define TIRTC_AUTO_SUBSCRIBE_VIDEO 1
#define TIRTC_AUTO_SUBSCRIBE_AUDIO 1
#define TIRTC_AUTO_REQUEST_KEY_FRAME 0
#define TIRTC_CONNECT_CANCEL_WAIT_MS 1000
#define TIRTC_CONN_USERS_WAIT_MS 1000
#define TIRTC_CONN_USERS_POLL_MS 5
#define TIRTC_CLOSE_TASK_STACK 3072
#define TIRTC_CLOSE_TASK_PRIORITY 5
#define TIRTC_STOP_WAIT_MS 1000
#define TIRTC_RX_VIDEO_LOG_FRAMES 25U
#define TIRTC_RX_AUDIO_LOG_PACKETS 50U

typedef enum
{
    TIRTC_LIFECYCLE_UNINITIALIZED = 0,
    TIRTC_LIFECYCLE_STARTING,
    TIRTC_LIFECYCLE_RUNNING,
    TIRTC_LIFECYCLE_STOPPING,
} tirtc_lifecycle_t;

typedef struct
{
    tirtc_conn_t conn;
    tirtc_conn_t closing_conn;
    tirtc_conn_t deferred_conn;
    tirtc_lifecycle_t lifecycle;
    TaskHandle_t lifecycle_owner;
    uint32_t conn_users;
    uint32_t closing_generation;
    uint32_t closing_completed_generation;
    int closing_result;
    bool closing_disconnect_inflight;
    bool deferred_disconnect_inflight;
    bool sdk_initialized;
    bool sdk_started;
    bool deinitializing;
    bool stop_submitting;
    bool stop_in_progress;
    bool stop_notified;
    uint32_t rx_video_count;
    uint32_t rx_audio_count;
} tirtc_runtime_t;

static tirtc_runtime_t s_tirtc = {
    .stop_notified = true,
};
static portMUX_TYPE s_runtime_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_connection_close_task;

typedef enum
{
    TIRTC_REGISTER_ACCEPTED = 0,
    TIRTC_REGISTER_UNCHANGED,
    TIRTC_REGISTER_REJECT,
    TIRTC_REGISTER_DEFERRED_TO_STOP,
    TIRTC_REGISTER_STOP_OWNED,
} tirtc_register_result_t;

bool tirtc_connection_guard_lock(tirtc_conn_t hconn)
{
    bool acquired = false;

    portENTER_CRITICAL(&s_runtime_lock);
    if (hconn != NULL &&
        s_tirtc.sdk_initialized &&
        !s_tirtc.deinitializing &&
        s_tirtc.conn == hconn)
    {
        s_tirtc.conn_users++;
        acquired = true;
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    return acquired;
}

void tirtc_connection_guard_unlock(void)
{
    portENTER_CRITICAL(&s_runtime_lock);
    if (s_tirtc.conn_users > 0U)
    {
        s_tirtc.conn_users--;
    }
    portEXIT_CRITICAL(&s_runtime_lock);
}

static void runtime_set_sdk_initialized(bool initialized)
{
    portENTER_CRITICAL(&s_runtime_lock);
    s_tirtc.sdk_initialized = initialized;
    portEXIT_CRITICAL(&s_runtime_lock);
}

static bool runtime_stop_was_notified(void)
{
    portENTER_CRITICAL(&s_runtime_lock);
    bool notified = s_tirtc.stop_notified;
    portEXIT_CRITICAL(&s_runtime_lock);
    return notified;
}

static void runtime_start_failed(TaskHandle_t owner)
{
    portENTER_CRITICAL(&s_runtime_lock);
    if (s_tirtc.lifecycle == TIRTC_LIFECYCLE_STARTING &&
        s_tirtc.lifecycle_owner == owner)
    {
        s_tirtc = (tirtc_runtime_t){.stop_notified = true};
    }
    portEXIT_CRITICAL(&s_runtime_lock);
}

static bool acquire_current_connection(tirtc_conn_t *hconn)
{
    bool acquired = false;

    portENTER_CRITICAL(&s_runtime_lock);
    if (hconn != NULL &&
        s_tirtc.sdk_initialized &&
        !s_tirtc.deinitializing &&
        s_tirtc.conn != NULL)
    {
        *hconn = s_tirtc.conn;
        s_tirtc.conn_users++;
        acquired = true;
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    return acquired;
}

static void notify_connection_close_task(void)
{
    TaskHandle_t task = s_connection_close_task;
    if (task != NULL)
    {
        xTaskNotifyGive(task);
    }
}

static uint32_t next_closing_generation_locked(void)
{
    s_tirtc.closing_generation++;
    if (s_tirtc.closing_generation == 0U)
    {
        s_tirtc.closing_generation = 1U;
    }
    return s_tirtc.closing_generation;
}

static void connection_close_task_entry(void *arg)
{
    (void)arg;

    while (true)
    {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t waited_ms = 0;

        while (true)
        {
            tirtc_conn_t hconn = NULL;
            uint32_t closing_generation = 0;
            bool stop_owns_connection = false;
            bool close_current = false;
            bool close_deferred = false;

            portENTER_CRITICAL(&s_runtime_lock);
            stop_owns_connection = s_tirtc.stop_in_progress || !s_tirtc.sdk_initialized;
            if (stop_owns_connection)
            {
                if (s_tirtc.closing_conn != NULL)
                {
                    s_tirtc.closing_completed_generation = s_tirtc.closing_generation;
                    s_tirtc.closing_result = 0;
                }
                s_tirtc.closing_conn = NULL;
                s_tirtc.deferred_conn = NULL;
                s_tirtc.closing_disconnect_inflight = false;
                s_tirtc.deferred_disconnect_inflight = false;
                portEXIT_CRITICAL(&s_runtime_lock);
                break;
            }
            if (s_tirtc.closing_conn != NULL &&
                s_tirtc.conn_users == 0U &&
                !s_tirtc.closing_disconnect_inflight)
            {
                hconn = s_tirtc.closing_conn;
                closing_generation = s_tirtc.closing_generation;
                s_tirtc.closing_disconnect_inflight = true;
                close_current = true;
            }
            else if (s_tirtc.deferred_conn != NULL &&
                     !s_tirtc.stop_submitting &&
                     !s_tirtc.deferred_disconnect_inflight)
            {
                hconn = s_tirtc.deferred_conn;
                s_tirtc.deferred_disconnect_inflight = true;
                close_deferred = true;
            }
            bool work_pending = s_tirtc.closing_conn != NULL ||
                                (s_tirtc.deferred_conn != NULL &&
                                 !s_tirtc.stop_submitting);
            portEXIT_CRITICAL(&s_runtime_lock);

            if (hconn == NULL)
            {
                if (!work_pending)
                {
                    break;
                }
                if (waited_ms == TIRTC_CONN_USERS_WAIT_MS)
                {
                    ESP_LOGW(TAG, "连接关闭任务仍在等待活跃 SDK 调用退出");
                }
                vTaskDelay(pdMS_TO_TICKS(TIRTC_CONN_USERS_POLL_MS));
                waited_ms += TIRTC_CONN_USERS_POLL_MS;
                continue;
            }

            local_media_stream_stop(hconn);
            int ret = TiRtcDisconnect(hconn);
            ESP_LOGI(TAG, "关闭连接 hconn=%p ret=%d %s",
                     hconn,
                     ret,
                     TiRtcGetErrorStr(ret));

            portENTER_CRITICAL(&s_runtime_lock);
            if (close_current)
            {
                /* The SDK contract invalidates hconn when Disconnect returns. */
                if (s_tirtc.closing_conn == hconn)
                {
                    s_tirtc.closing_conn = NULL;
                }
                s_tirtc.closing_completed_generation = closing_generation;
                s_tirtc.closing_result = ret;
                s_tirtc.closing_disconnect_inflight = false;
            }
            if (close_deferred)
            {
                if (s_tirtc.deferred_conn == hconn)
                {
                    s_tirtc.deferred_conn = NULL;
                }
                s_tirtc.deferred_disconnect_inflight = false;
            }
            portEXIT_CRITICAL(&s_runtime_lock);

            waited_ms = 0;
        }
    }
}

static esp_err_t ensure_connection_close_task(void)
{
    if (s_connection_close_task != NULL)
    {
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreate(connection_close_task_entry,
                                 "tirtc_close",
                                 TIRTC_CLOSE_TASK_STACK,
                                 NULL,
                                 TIRTC_CLOSE_TASK_PRIORITY,
                                 &s_connection_close_task);
    if (ret != pdPASS)
    {
        s_connection_close_task = NULL;
        ESP_LOGE(TAG, "创建 TiRTC 连接关闭任务失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static tirtc_register_result_t register_connection(tirtc_conn_t hconn)
{
    tirtc_register_result_t result = TIRTC_REGISTER_REJECT;

    portENTER_CRITICAL(&s_runtime_lock);
    if (!s_tirtc.sdk_initialized || s_tirtc.stop_in_progress)
    {
        result = TIRTC_REGISTER_STOP_OWNED;
    }
    else if (s_tirtc.stop_submitting)
    {
        if (s_tirtc.deferred_conn == NULL || s_tirtc.deferred_conn == hconn)
        {
            s_tirtc.deferred_conn = hconn;
            result = TIRTC_REGISTER_DEFERRED_TO_STOP;
        }
    }
    else if (s_tirtc.deinitializing ||
             s_tirtc.closing_conn != NULL ||
             s_tirtc.deferred_conn != NULL ||
             (s_tirtc.conn != NULL && s_tirtc.conn != hconn))
    {
        result = TIRTC_REGISTER_REJECT;
    }
    else if (s_tirtc.conn == hconn)
    {
        result = TIRTC_REGISTER_UNCHANGED;
    }
    else
    {
        s_tirtc.conn = hconn;
        s_tirtc.rx_video_count = 0;
        s_tirtc.rx_audio_count = 0;
        result = TIRTC_REGISTER_ACCEPTED;
    }
    portEXIT_CRITICAL(&s_runtime_lock);

    return result;
}

static void queue_unowned_connection_close(tirtc_conn_t hconn, const char *reason)
{
    bool queued = false;
    bool already_owned = false;
    bool stop_owns_connection = false;

    portENTER_CRITICAL(&s_runtime_lock);
    if (!s_tirtc.sdk_initialized || s_tirtc.stop_in_progress)
    {
        stop_owns_connection = true;
    }
    else if (s_tirtc.closing_conn == hconn || s_tirtc.deferred_conn == hconn)
    {
        already_owned = true;
    }
    else if (s_tirtc.deferred_conn == NULL)
    {
        s_tirtc.deferred_conn = hconn;
        queued = true;
    }
    portEXIT_CRITICAL(&s_runtime_lock);

    if (queued)
    {
        ESP_LOGW(TAG, "%s，排队释放连接 hconn=%p", reason, hconn);
        notify_connection_close_task();
    }
    else if (stop_owns_connection)
    {
        ESP_LOGW(TAG, "TiRTC Stop 已接管未登记连接 hconn=%p", hconn);
    }
    else if (!already_owned)
    {
        ESP_LOGE(TAG, "未登记连接关闭槽已占用；max_connections=1 契约被破坏 hconn=%p", hconn);
    }
}

void tirtc_release_unowned_connection(tirtc_conn_t hconn)
{
    if (hconn != NULL)
    {
        queue_unowned_connection_close(hconn, "收到迟到或未接管的连接结果");
    }
}

/*
 * SDK 生命周期回调
 *
 * TiRtcStart() 是异步上线，请以 TIRTC_EVENT_SYS_STARTED 作为“可以主动连接”
 * 的时机。停止流程同理，等待 TIRTC_EVENT_SYS_STOPPED 后再释放资源更稳。
 */
static void on_tirtc_event(int event, const void *data, int len)
{
    (void)data;
    (void)len;

    ESP_LOGI(TAG, "TiRTC 系统事件: %d", event);
    switch (event)
    {
    case TIRTC_EVENT_SYS_STARTED:
        portENTER_CRITICAL(&s_runtime_lock);
        s_tirtc.sdk_started = true;
        s_tirtc.stop_notified = false;
        portEXIT_CRITICAL(&s_runtime_lock);
        ESP_LOGI(TAG, "TiRTC 已上线，可接收入站连接，也可主动连接远端设备");
        tirtc_connect_on_tirtc_started();
#if TIRTC_AUTO_CONNECT_AFTER_STARTED
        (void)tirtc_connect_configured();
#endif
        break;

    case TIRTC_EVENT_SYS_STOPPED:
        portENTER_CRITICAL(&s_runtime_lock);
        s_tirtc.sdk_started = false;
        s_tirtc.stop_notified = true;
        portEXIT_CRITICAL(&s_runtime_lock);
        ESP_LOGI(TAG, "TiRTC 已停止");
        break;

    case TIRTC_EVENT_ACCESS_HIJACKING:
        ESP_LOGE(TAG, "TiRTC 服务请求发生异常重定向，已阻止继续信任该入口；请检查网络和 endpoint");
        break;

    default:
        break;
    }
}

/*
 * 连接事件回调
 */
static void on_conn_accepted(tirtc_conn_t hconn)
{
    ESP_LOGI(TAG, "TiRTC 收到远端连接 hconn=%p", hconn);

    if (hconn == NULL)
    {
        return;
    }

    tirtc_register_result_t register_result = register_connection(hconn);
    if (register_result == TIRTC_REGISTER_UNCHANGED)
    {
        return;
    }
    if (register_result == TIRTC_REGISTER_REJECT)
    {
        queue_unowned_connection_close(hconn, "单连接示例已有连接或正在释放");
        return;
    }
    if (register_result == TIRTC_REGISTER_DEFERRED_TO_STOP)
    {
        ESP_LOGW(TAG, "TiRtcStop 正在同步提交，暂存新连接并等待提交结果 hconn=%p", hconn);
        return;
    }
    if (register_result == TIRTC_REGISTER_STOP_OWNED)
    {
        ESP_LOGW(TAG, "TiRTC Stop 已接管，忽略停止期间返回的新连接 hconn=%p", hconn);
        return;
    }

    ESP_LOGI(TAG, "当前连接已建立 hconn=%p", hconn);
    ESP_LOGI(TAG, "等待对端订阅本机音视频后再开始发送测试媒体");

#if TIRTC_AUTO_REQUEST_KEY_FRAME
    (void)tirtc_request_remote_key_frame(TIRTC_VIDEO_STREAM_ID);
#endif
}

static void on_conn_error(tirtc_conn_t hconn, int error)
{
    ESP_LOGE(TAG, "TiRTC 连接错误 hconn=%p error=%d %s",
             hconn,
             error,
             TiRtcGetErrorStr(error));

    if (hconn == NULL)
    {
        return;
    }

    bool stop_media = false;
    bool close_queued = false;
    bool may_disconnect_directly = false;
    bool stop_owns_connection = false;

    portENTER_CRITICAL(&s_runtime_lock);
    if (s_tirtc.stop_in_progress || !s_tirtc.sdk_initialized)
    {
        stop_owns_connection = true;
    }
    else if (s_tirtc.stop_submitting)
    {
        if (s_tirtc.deferred_conn == NULL || s_tirtc.deferred_conn == hconn)
        {
            s_tirtc.deferred_conn = hconn;
        }
    }
    else if (hconn == s_tirtc.conn && s_tirtc.closing_conn == NULL)
    {
        s_tirtc.conn = NULL;
        s_tirtc.closing_conn = hconn;
        (void)next_closing_generation_locked();
        stop_media = true;
        close_queued = true;
    }
    else if (hconn == s_tirtc.closing_conn)
    {
        stop_media = true;
    }
    else if (hconn != s_tirtc.deferred_conn)
    {
        may_disconnect_directly = true;
    }
    portEXIT_CRITICAL(&s_runtime_lock);

    if (stop_media)
    {
        local_media_stream_stop(hconn);
        ESP_LOGI(TAG, "当前连接已清理 hconn=%p", hconn);
    }

    if (close_queued)
    {
        notify_connection_close_task();
    }
    else if (may_disconnect_directly)
    {
        queue_unowned_connection_close(hconn, "连接错误来自未接管句柄");
    }
    else if (stop_owns_connection)
    {
        ESP_LOGW(TAG, "TiRTC Stop 已接管错误连接，不再并发调用 Disconnect");
    }
}

static void on_disconnected(tirtc_conn_t hconn)
{
    ESP_LOGI(TAG, "TiRTC 连接断开 hconn=%p", hconn);
    if (hconn == NULL)
    {
        return;
    }

    bool stop_media = false;
    portENTER_CRITICAL(&s_runtime_lock);
    if (hconn == s_tirtc.conn)
    {
        s_tirtc.conn = NULL;
        stop_media = true;
    }
    if (hconn == s_tirtc.closing_conn && !s_tirtc.closing_disconnect_inflight)
    {
        s_tirtc.closing_conn = NULL;
        s_tirtc.closing_completed_generation = s_tirtc.closing_generation;
        s_tirtc.closing_result = 0;
        stop_media = true;
    }
    if (hconn == s_tirtc.deferred_conn && !s_tirtc.deferred_disconnect_inflight)
    {
        s_tirtc.deferred_conn = NULL;
        stop_media = true;
    }
    portEXIT_CRITICAL(&s_runtime_lock);

    if (stop_media)
    {
        local_media_stream_stop(hconn);
        ESP_LOGI(TAG, "当前连接已清理 hconn=%p", hconn);
    }
}

/*
 * 收包与订阅回调
 *
 * demo 只处理当前连接上的事件。被远端呼入时等待对端订阅，收到订阅后发送本地 H264/PCMA；
 * 主动连接时只订阅对端媒体，方便两台板直接做“按键看对端流”的演示。
 */
static void on_audio(tirtc_conn_t hconn, const TIRTCFRAMEINFO *info, void *data)
{
    (void)data;
    if (info == NULL || !tirtc_connection_guard_lock(hconn))
    {
        return;
    }

    portENTER_CRITICAL(&s_runtime_lock);
    s_tirtc.rx_audio_count++;
    uint32_t count = s_tirtc.rx_audio_count;
    portEXIT_CRITICAL(&s_runtime_lock);
    tirtc_connection_guard_unlock();

    if ((count % TIRTC_RX_AUDIO_LOG_PACKETS) == 0)
    {
        ESP_LOGI(TAG,
                 "[RX][audio] 接收统计：音频包=%" PRIu32 "，流ID=%u，长度=%u字节，时间戳=%u",
                 count,
                 info->stream_id,
                 info->length,
                 info->ts);
    }
}

static void on_video(tirtc_conn_t hconn, const TIRTCFRAMEINFO *info, void *data)
{
    (void)data;
    if (info == NULL || !tirtc_connection_guard_lock(hconn))
    {
        return;
    }

    portENTER_CRITICAL(&s_runtime_lock);
    s_tirtc.rx_video_count++;
    uint32_t count = s_tirtc.rx_video_count;
    portEXIT_CRITICAL(&s_runtime_lock);
    tirtc_connection_guard_unlock();

    if ((count % TIRTC_RX_VIDEO_LOG_FRAMES) == 0)
    {
        ESP_LOGI(TAG,
                 "[RX][video] 接收统计：视频帧=%" PRIu32 "，流ID=%u，长度=%u字节，时间戳=%u，关键帧=%s",
                 count,
                 info->stream_id,
                 info->length,
                 info->ts,
                 (info->flags & TIRTC_FRAME_FLAG_KEY_FRAME) != 0 ? "是" : "否");
    }
}

static void on_message(tirtc_conn_t hconn, const TIRTCFRAMEINFO *info, void *data)
{
    (void)data;
    if (info != NULL && tirtc_connection_guard_lock(hconn))
    {
        tirtc_connection_guard_unlock();
        ESP_LOGI(TAG, "收到对端消息 stream=%u len=%u", info->stream_id, info->length);
    }
}

static void on_command(tirtc_conn_t hconn, uint32_t cmdw, const void *data, uint32_t len)
{
    (void)data;
    if (tirtc_connection_guard_lock(hconn))
    {
        tirtc_connection_guard_unlock();
        ESP_LOGI(TAG, "收到对端命令 cmdw=0x%08" PRIx32 " len=%" PRIu32, cmdw, len);
    }
}

static void on_request_key_frame(tirtc_conn_t hconn, uint8_t stream_id)
{
    if (tirtc_connection_guard_lock(hconn))
    {
        ESP_LOGI(TAG, "[CTRL][RX] 对端请求本机关键帧 stream=%u", stream_id);
        local_media_stream_request_key_frame(hconn);
        tirtc_connection_guard_unlock();
    }
}

static int on_subscribe_video(tirtc_conn_t hconn, uint8_t stream_id)
{
    if (tirtc_connection_guard_lock(hconn))
    {
        ESP_LOGI(TAG, "[CTRL][RX] 对端订阅本机视频 stream=%u", stream_id);
        esp_err_t media_ret = local_media_stream_start_video(hconn, stream_id);
        tirtc_connection_guard_unlock();
        if (media_ret != ESP_OK)
        {
            ESP_LOGE(TAG, "[TX][video] 启动本机 H264 测试视频失败: %s", esp_err_to_name(media_ret));
            return TIRTC_E_INVALID_HANDLE;
        }
        local_media_stream_request_key_frame(hconn);
    }
    return 0;
}

static void on_unsubscribe_video(tirtc_conn_t hconn, uint8_t stream_id)
{
    if (tirtc_connection_guard_lock(hconn))
    {
        ESP_LOGI(TAG, "[CTRL][RX] 对端取消订阅本机视频 stream=%u", stream_id);
        local_media_stream_stop_video(hconn, stream_id);
        tirtc_connection_guard_unlock();
    }
}

static int on_subscribe_audio(tirtc_conn_t hconn, uint8_t stream_id)
{
    if (tirtc_connection_guard_lock(hconn))
    {
        ESP_LOGI(TAG, "[CTRL][RX] 对端订阅本机音频 stream=%u", stream_id);
        esp_err_t media_ret = local_media_stream_start_audio(hconn, stream_id);
        tirtc_connection_guard_unlock();
        if (media_ret != ESP_OK)
        {
            ESP_LOGE(TAG, "[TX][audio] 启动本机 PCMA 测试音频失败: %s", esp_err_to_name(media_ret));
            return TIRTC_E_INVALID_HANDLE;
        }
    }
    return 0;
}

static void on_unsubscribe_audio(tirtc_conn_t hconn, uint8_t stream_id)
{
    if (tirtc_connection_guard_lock(hconn))
    {
        ESP_LOGI(TAG, "[CTRL][RX] 对端取消订阅本机音频 stream=%u", stream_id);
        local_media_stream_stop_audio(hconn, stream_id);
        tirtc_connection_guard_unlock();
    }
}

/*
 * SDK 回调表
 *
 * SDK 会保存这里的函数指针，所以回调表必须是静态生命周期。
 */
static const TIRTCCALLBACKS s_tirtc_callbacks = {
    .on_event = on_tirtc_event,
    .on_conn_accepted = on_conn_accepted,
    .on_conn_error = on_conn_error,
    .on_disconnected = on_disconnected,
    .on_audio = on_audio,
    .on_video = on_video,
    .on_message = on_message,
    .on_command = on_command,
    .on_request_key_frame = on_request_key_frame,
    .on_subscribe_video = on_subscribe_video,
    .on_unsubscribe_video = on_unsubscribe_video,
    .on_subscribe_audio = on_subscribe_audio,
    .on_unsubscribe_audio = on_unsubscribe_audio,
};

static bool client_id_is_valid(const char *client_id)
{
    size_t len = strlen(client_id);
    if (len == 0 || len > 64)
    {
        return false;
    }

    for (size_t i = 0; i < len; ++i)
    {
        unsigned char ch = (unsigned char)client_id[i];
        if (ch < 0x20U || ch > 0x7eU)
        {
            return false;
        }
    }
    return true;
}

/*
 * SDK 启动与释放
 *
 * 启动顺序：设置初始化前 option -> TiRtcInit() -> 设置设备密钥和 client_id ->
 * TiRtcStart(device_id)。释放顺序反过来：停主动连接 -> 断当前连接 ->
 * TiRtcStop() -> TiRtcUninit()。
 */
esp_err_t tirtc_start(void)
{
    TaskHandle_t owner = xTaskGetCurrentTaskHandle();

    portENTER_CRITICAL(&s_runtime_lock);
    if (s_tirtc.lifecycle == TIRTC_LIFECYCLE_RUNNING)
    {
        portEXIT_CRITICAL(&s_runtime_lock);
        ESP_LOGW(TAG, "TiRTC 已经初始化，忽略重复启动");
        return ESP_OK;
    }
    if (s_tirtc.lifecycle != TIRTC_LIFECYCLE_UNINITIALIZED)
    {
        portEXIT_CRITICAL(&s_runtime_lock);
        ESP_LOGW(TAG, "TiRTC 生命周期操作正在进行，拒绝并发启动");
        return ESP_ERR_INVALID_STATE;
    }

    s_tirtc.lifecycle = TIRTC_LIFECYCLE_STARTING;
    s_tirtc.lifecycle_owner = owner;
    s_tirtc.deinitializing = false;
    s_tirtc.stop_submitting = false;
    s_tirtc.stop_in_progress = false;
    s_tirtc.stop_notified = true;
    s_tirtc.rx_video_count = 0;
    s_tirtc.rx_audio_count = 0;
    portEXIT_CRITICAL(&s_runtime_lock);

    esp_err_t close_task_ret = ensure_connection_close_task();
    if (close_task_ret != ESP_OK)
    {
        runtime_start_failed(owner);
        return close_task_ret;
    }

    esp_err_t prepare_ret = tirtc_connect_prepare();
    if (prepare_ret != ESP_OK)
    {
        runtime_start_failed(owner);
        return prepare_ret;
    }

    ESP_LOGI(TAG, "TiRTC 版本: %s", TiRtcGetVersion());
    ESP_LOGI(TAG, "TiRTC BuildInfo: %s", TiRtcGetBuildInfo());
    ESP_LOGI(TAG, "PSRAM 总量=%zu 可用=%zu",
             heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    const size_t device_id_len = strlen(TIRTC_DEVICE_ID);
    const size_t device_secret_len = strlen(TIRTC_DEVICE_SECRET_KEY);
    const size_t client_id_len = strlen(TIRTC_CLIENT_ID);
    if (device_id_len == 0 || device_secret_len == 0 ||
        strcmp(TIRTC_DEVICE_ID, "your_device_id") == 0 ||
        strcmp(TIRTC_DEVICE_SECRET_KEY, "your_device_secret_key") == 0)
    {
        ESP_LOGE(TAG, "设备 ID 或设备密钥未配置，请仅在 local_config.h 中写入设备凭证");
        runtime_start_failed(owner);
        return ESP_ERR_INVALID_ARG;
    }
    if (!client_id_is_valid(TIRTC_CLIENT_ID) ||
        strcmp(TIRTC_CLIENT_ID, "your_stable_printable_client_id") == 0)
    {
        ESP_LOGE(TAG, "client_id 必须是 1..64 个可打印 ASCII 字符，并复用首次绑定值");
        runtime_start_failed(owner);
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(TIRTC_SERVICE_ENDPOINT, "https://", 8) == 0)
    {
        ESP_LOGE(TAG, "正式 TiRTC 2.2.1 ESP32-S3 包为 HTTP-only，不能配置 HTTPS endpoint");
        runtime_start_failed(owner);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "TiRTC 启动配置: device_id=%s client_id=%s secret_len=%u",
             TIRTC_DEVICE_ID,
             TIRTC_CLIENT_ID,
             (unsigned int)device_secret_len);

    /* H264/PCMA 测试流会持续发送，发送缓冲要在 TiRtcInit() 前设置。 */
    uint32_t max_send_buffer = TIRTC_MAX_SEND_BUFFER_BYTES;
    int sdk_ret = TiRtcSetOption(TIRTC_OPT_MAX_SEND_BUFFER,
                                 &max_send_buffer,
                                 (uint32_t)sizeof(max_send_buffer));
    if (sdk_ret != 0)
    {
        ESP_LOGE(TAG, "设置 TiRTC 发送缓冲失败: %d %s", sdk_ret, TiRtcGetErrorStr(sdk_ret));
        runtime_start_failed(owner);
        return ESP_FAIL;
    }

    esp_err_t media_ret = local_media_stream_prepare();
    if (media_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "准备本地 H264/PCMA 测试媒体失败: %s", esp_err_to_name(media_ret));
        runtime_start_failed(owner);
        return media_ret;
    }

    sdk_ret = TiRtcInit();
    if (sdk_ret != 0)
    {
        ESP_LOGE(TAG, "TiRtcInit 失败: %d %s", sdk_ret, TiRtcGetErrorStr(sdk_ret));
        runtime_start_failed(owner);
        return ESP_FAIL;
    }
    runtime_set_sdk_initialized(true);

    TiRtcLogSetLevel(TIRTC_LOG_LEVEL);

    int option_value = 1;
    sdk_ret = TiRtcSetOption(TIRTC_OPT_MAX_CONNECTIONS,
                             &option_value,
                             (uint32_t)sizeof(option_value));
    if (sdk_ret != 0)
    {
        ESP_LOGE(TAG, "设置 TiRTC 最大连接数失败: %d %s", sdk_ret, TiRtcGetErrorStr(sdk_ret));
        TiRtcUninit();
        runtime_start_failed(owner);
        return ESP_FAIL;
    }

    option_value = TIRTC_NETCONN_WIFI;
    sdk_ret = TiRtcSetOption(TIRTC_OPT_NETWORK_TYPE,
                             &option_value,
                             (uint32_t)sizeof(option_value));
    if (sdk_ret != 0)
    {
        ESP_LOGE(TAG, "设置 TiRTC 网络类型失败: %d %s", sdk_ret, TiRtcGetErrorStr(sdk_ret));
        TiRtcUninit();
        runtime_start_failed(owner);
        return ESP_FAIL;
    }

    option_value = TIRTC_CONNECT_CACHE_ENABLED ? 1 : 0;
    sdk_ret = TiRtcSetOption(TIRTC_OPT_CONNECT_CACHE,
                             &option_value,
                             (uint32_t)sizeof(option_value));
    if (sdk_ret != 0)
    {
        ESP_LOGE(TAG, "设置 TiRTC 连接参数缓存失败: %d %s", sdk_ret, TiRtcGetErrorStr(sdk_ret));
        TiRtcUninit();
        runtime_start_failed(owner);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "TiRTC 运行策略: max_connections=1 network=Wi-Fi connect_cache=%d",
             option_value);

    /* 2.2.x 起设备密钥必须单独设置，TiRtcStart() 只接收 device_id。 */
    sdk_ret = TiRtcSetOption(TIRTC_OPT_DEVICE_SECRET_KEY,
                             TIRTC_DEVICE_SECRET_KEY,
                             (uint32_t)device_secret_len);
    if (sdk_ret != 0)
    {
        ESP_LOGE(TAG, "设置 TiRTC 设备密钥失败: %d %s", sdk_ret, TiRtcGetErrorStr(sdk_ret));
        TiRtcUninit();
        runtime_start_failed(owner);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "TiRTC 启动选项已设置: device_secret_key length=%u",
             (unsigned int)device_secret_len);

    sdk_ret = TiRtcSetOption(TIRTC_OPT_CLIENT_ID,
                             TIRTC_CLIENT_ID,
                             (uint32_t)client_id_len);
    if (sdk_ret != 0)
    {
        ESP_LOGE(TAG, "设置 TiRTC client_id 失败: %d %s", sdk_ret, TiRtcGetErrorStr(sdk_ret));
        TiRtcUninit();
        runtime_start_failed(owner);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "TiRTC 启动选项已设置: client_id length=%u",
             (unsigned int)client_id_len);

    /* 正常环境留空使用 SDK 默认入口；只有自部署或测试环境才覆盖。 */
    if (strlen(TIRTC_SERVICE_ENDPOINT) > 0)
    {
        sdk_ret = TiRtcSetOption(TIRTC_OPT_SERVICE_ENDPOINT,
                                 TIRTC_SERVICE_ENDPOINT,
                                 (uint32_t)strlen(TIRTC_SERVICE_ENDPOINT));
        if (sdk_ret != 0)
        {
            ESP_LOGE(TAG, "设置 TiRTC 服务地址失败: %d %s", sdk_ret, TiRtcGetErrorStr(sdk_ret));
            TiRtcUninit();
            runtime_start_failed(owner);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "TiRTC 服务地址: %s", TIRTC_SERVICE_ENDPOINT);
    }

    portENTER_CRITICAL(&s_runtime_lock);
    s_tirtc.stop_notified = false;
    portEXIT_CRITICAL(&s_runtime_lock);
    int start_ret = TiRtcStart(TIRTC_DEVICE_ID, &s_tirtc_callbacks);
    if (start_ret != 0)
    {
        if (start_ret > 599)
        {
            ESP_LOGE(TAG, "TiRtcStart 服务端业务错误: %d，请结合上一条 SDK 日志检查设备身份参数", start_ret);
        }
        else
        {
            ESP_LOGE(TAG, "TiRtcStart 失败: %d %s", start_ret, TiRtcGetErrorStr(start_ret));
        }
        TiRtcUninit();
        runtime_start_failed(owner);
        return ESP_FAIL;
    }

    portENTER_CRITICAL(&s_runtime_lock);
    s_tirtc.lifecycle = TIRTC_LIFECYCLE_RUNNING;
    s_tirtc.lifecycle_owner = NULL;
    bool already_started = s_tirtc.sdk_started;
    portEXIT_CRITICAL(&s_runtime_lock);

    ESP_LOGI(TAG, "TiRTC 启动请求已提交: device_id=%s", TIRTC_DEVICE_ID);
#if TIRTC_AUTO_CONNECT_AFTER_STARTED
    if (already_started)
    {
        (void)tirtc_connect_configured();
    }
#else
    (void)already_started;
#endif
    return ESP_OK;
}

void tirtc_deinit(void)
{
    TaskHandle_t owner = xTaskGetCurrentTaskHandle();

    portENTER_CRITICAL(&s_runtime_lock);
    if (s_tirtc.lifecycle == TIRTC_LIFECYCLE_UNINITIALIZED)
    {
        portEXIT_CRITICAL(&s_runtime_lock);
        return;
    }
    if (s_tirtc.lifecycle == TIRTC_LIFECYCLE_STARTING)
    {
        portEXIT_CRITICAL(&s_runtime_lock);
        ESP_LOGW(TAG, "TiRTC 正在启动，拒绝并发停止");
        return;
    }
    if (s_tirtc.lifecycle == TIRTC_LIFECYCLE_STOPPING &&
        s_tirtc.lifecycle_owner != owner)
    {
        portEXIT_CRITICAL(&s_runtime_lock);
        ESP_LOGW(TAG, "TiRTC 已由另一个任务负责停止");
        return;
    }
    if (s_tirtc.lifecycle == TIRTC_LIFECYCLE_RUNNING)
    {
        s_tirtc.lifecycle = TIRTC_LIFECYCLE_STOPPING;
        s_tirtc.lifecycle_owner = owner;
        s_tirtc.deinitializing = true;
    }
    portEXIT_CRITICAL(&s_runtime_lock);

    ESP_LOGI(TAG, "开始释放 TiRTC 资源");

    /* 退出顺序：停主动连接、断当前连接、停 SDK、释放 SDK。 */
    if (!tirtc_connect_cancel_and_wait(TIRTC_CONNECT_CANCEL_WAIT_MS))
    {
        ESP_LOGE(TAG, "主动连接 worker 尚未退出，保留 SDK 资源并延后停止");
        return;
    }
    int disconnect_ret = tirtc_disconnect_current();
    if (disconnect_ret == TIRTC_E_BUSY)
    {
        ESP_LOGE(TAG, "仍有连接 API 正在执行，保留 SDK 资源并延后停止");
        return;
    }

    if (!runtime_stop_was_notified())
    {
        bool submit_stop = false;
        bool stop_already_submitted = false;

        for (unsigned int gate_attempt = 0;
             gate_attempt < 2U && !submit_stop && !stop_already_submitted;
             ++gate_attempt)
        {
            portENTER_CRITICAL(&s_runtime_lock);
            stop_already_submitted = s_tirtc.stop_in_progress;
            bool connections_quiescent =
                s_tirtc.conn == NULL &&
                s_tirtc.closing_conn == NULL &&
                s_tirtc.deferred_conn == NULL &&
                s_tirtc.conn_users == 0U &&
                !s_tirtc.closing_disconnect_inflight &&
                !s_tirtc.deferred_disconnect_inflight;
            if (!stop_already_submitted &&
                !s_tirtc.stop_submitting &&
                connections_quiescent)
            {
                /*
                 * Publish the gate atomically with the quiescent check. New
                 * callbacks can only defer their handle until Stop returns.
                 */
                s_tirtc.stop_submitting = true;
                submit_stop = true;
            }
            portEXIT_CRITICAL(&s_runtime_lock);

            if (!submit_stop && !stop_already_submitted)
            {
                disconnect_ret = tirtc_disconnect_current();
                if (disconnect_ret == TIRTC_E_BUSY)
                {
                    ESP_LOGE(TAG, "停止门禁建立前又出现连接活动，保留 SDK 资源并延后停止");
                    return;
                }
            }
        }

        if (!submit_stop && !stop_already_submitted)
        {
            ESP_LOGE(TAG, "无法建立无连接的 Stop 提交门禁，保留 SDK 资源并等待重试");
            return;
        }

        if (submit_stop)
        {
            int stop_ret = TiRtcStop();
            bool close_deferred = false;

            portENTER_CRITICAL(&s_runtime_lock);
            s_tirtc.stop_submitting = false;
            if (stop_ret == 0)
            {
                s_tirtc.stop_in_progress = true;
                s_tirtc.deferred_conn = NULL;
            }
            else
            {
                s_tirtc.stop_in_progress = false;
                close_deferred = s_tirtc.deferred_conn != NULL;
            }
            portEXIT_CRITICAL(&s_runtime_lock);

            if (stop_ret != 0)
            {
                ESP_LOGE(TAG, "TiRtcStop 提交失败: %d %s", stop_ret, TiRtcGetErrorStr(stop_ret));
                if (close_deferred)
                {
                    notify_connection_close_task();
                }
                return;
            }
        }

        const TickType_t wait_step = pdMS_TO_TICKS(50);
        uint32_t waited_ms = 0;
        while (!runtime_stop_was_notified() && waited_ms < TIRTC_STOP_WAIT_MS)
        {
            vTaskDelay(wait_step);
            waited_ms += 50U;
        }

        if (!runtime_stop_was_notified())
        {
            ESP_LOGE(TAG, "尚未收到 TIRTC_EVENT_SYS_STOPPED，保留 SDK 资源并等待后续再次释放");
            return;
        }
    }

    TiRtcUninit();
    portENTER_CRITICAL(&s_runtime_lock);
    s_tirtc = (tirtc_runtime_t){.stop_notified = true};
    portEXIT_CRITICAL(&s_runtime_lock);
    ESP_LOGI(TAG, "TiRTC 资源已释放");
}

/*
 * 主动连接入口
 *
 * 未连接状态下按 BOOT 会走到这里。token 申请和 TiRtcConnect() 在 tirtc_connect.c，
 * 连接成功后的 hconn 直接回到本文件的 on_active_connect_result()。
 */
static void on_active_connect_result(int error, tirtc_conn_t hconn, void *user_data)
{
    (void)user_data;

    if (error != 0)
    {
        ESP_LOGE(TAG, "主动连接失败 error=%d %s", error, TiRtcGetErrorStr(error));
        return;
    }

    ESP_LOGI(TAG, "主动连接成功 hconn=%p", hconn);

    if (hconn == NULL)
    {
        return;
    }

    tirtc_register_result_t register_result = register_connection(hconn);
    if (register_result == TIRTC_REGISTER_UNCHANGED)
    {
        return;
    }
    if (register_result == TIRTC_REGISTER_REJECT)
    {
        queue_unowned_connection_close(hconn, "单连接示例已有连接或正在释放");
        return;
    }
    if (register_result == TIRTC_REGISTER_DEFERRED_TO_STOP)
    {
        ESP_LOGW(TAG, "TiRtcStop 正在同步提交，暂存主动连接并等待提交结果 hconn=%p", hconn);
        return;
    }
    if (register_result == TIRTC_REGISTER_STOP_OWNED)
    {
        ESP_LOGW(TAG, "TiRTC Stop 已接管，忽略迟到的主动连接 hconn=%p", hconn);
        return;
    }

    ESP_LOGI(TAG, "当前连接已建立 hconn=%p", hconn);

    ESP_LOGI(TAG, "主动连接侧作为观看端，只订阅对端音视频，不发送本地测试媒体");

#if TIRTC_AUTO_SUBSCRIBE_VIDEO
    (void)tirtc_subscribe_remote_video(TIRTC_VIDEO_STREAM_ID);
#endif

#if TIRTC_AUTO_SUBSCRIBE_AUDIO
    (void)tirtc_subscribe_remote_audio(TIRTC_AUDIO_STREAM_ID);
#endif

#if TIRTC_AUTO_REQUEST_KEY_FRAME
    (void)tirtc_request_remote_key_frame(TIRTC_VIDEO_STREAM_ID);
#endif
}

esp_err_t tirtc_connect_configured(void)
{
    portENTER_CRITICAL(&s_runtime_lock);
    bool ready = s_tirtc.sdk_started &&
                 !s_tirtc.deinitializing &&
                 s_tirtc.lifecycle == TIRTC_LIFECYCLE_RUNNING;
    portEXIT_CRITICAL(&s_runtime_lock);
    if (!ready)
    {
        ESP_LOGW(TAG, "TiRTC 尚未上线，暂不能主动连接");
        return ESP_ERR_INVALID_STATE;
    }

    return tirtc_connect_start(on_active_connect_result, NULL);
}

esp_err_t tirtc_toggle_connection(void)
{
    if (tirtc_connect_is_connecting())
    {
        ESP_LOGW(TAG, "BOOT 按键触发：当前正在主动连接，等待连接结果或超时后再操作");
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_runtime_lock);
    bool ready = s_tirtc.sdk_initialized &&
                 !s_tirtc.deinitializing &&
                 s_tirtc.lifecycle == TIRTC_LIFECYCLE_RUNNING;
    bool has_connection = s_tirtc.conn != NULL ||
                          s_tirtc.closing_conn != NULL ||
                          s_tirtc.deferred_conn != NULL;
    portEXIT_CRITICAL(&s_runtime_lock);

    if (!ready)
    {
        ESP_LOGW(TAG, "BOOT 按键触发：TiRTC 正在停止或尚未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    if (has_connection)
    {
        ESP_LOGI(TAG, "BOOT 按键触发：当前已有连接，主动断开两端连接");
        int ret = tirtc_disconnect_current();
        return ret == 0 ? ESP_OK : ESP_FAIL;
    }

    ESP_LOGI(TAG, "BOOT 按键触发：当前无连接，主动连接目标设备");
    esp_err_t ret = tirtc_connect_configured();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "BOOT 主动连接触发失败: %s", esp_err_to_name(ret));
    }
    return ret;
}

int tirtc_disconnect_current(void)
{
    tirtc_conn_t hconn = NULL;
    uint32_t closing_generation = 0;
    bool close_queued = false;
    bool work_pending = false;

    portENTER_CRITICAL(&s_runtime_lock);
    if (s_tirtc.closing_conn != NULL)
    {
        hconn = s_tirtc.closing_conn;
        closing_generation = s_tirtc.closing_generation;
    }
    else if (s_tirtc.conn != NULL)
    {
        hconn = s_tirtc.conn;
        s_tirtc.conn = NULL;
        s_tirtc.closing_conn = hconn;
        closing_generation = next_closing_generation_locked();
        close_queued = true;
    }
    work_pending = s_tirtc.closing_conn != NULL ||
                   s_tirtc.deferred_conn != NULL ||
                   s_tirtc.conn_users != 0U ||
                   s_tirtc.closing_disconnect_inflight ||
                   s_tirtc.deferred_disconnect_inflight;
    portEXIT_CRITICAL(&s_runtime_lock);

    if (hconn != NULL)
    {
        local_media_stream_stop(hconn);
    }
    if (close_queued || work_pending)
    {
        notify_connection_close_task();
    }

    uint32_t waited_ms = 0;
    while (waited_ms < TIRTC_CONN_USERS_WAIT_MS)
    {
        portENTER_CRITICAL(&s_runtime_lock);
        bool all_closed = s_tirtc.closing_conn == NULL &&
                          s_tirtc.deferred_conn == NULL &&
                          s_tirtc.conn_users == 0U &&
                          !s_tirtc.closing_disconnect_inflight &&
                          !s_tirtc.deferred_disconnect_inflight;
        bool current_close_completed =
            hconn == NULL ||
            s_tirtc.closing_completed_generation == closing_generation;
        int close_result = s_tirtc.closing_result;
        portEXIT_CRITICAL(&s_runtime_lock);

        if (all_closed && current_close_completed)
        {
            return hconn != NULL ? close_result : 0;
        }

        vTaskDelay(pdMS_TO_TICKS(TIRTC_CONN_USERS_POLL_MS));
        waited_ms += TIRTC_CONN_USERS_POLL_MS;
    }

    portENTER_CRITICAL(&s_runtime_lock);
    bool all_closed = s_tirtc.closing_conn == NULL &&
                      s_tirtc.deferred_conn == NULL &&
                      s_tirtc.conn_users == 0U &&
                      !s_tirtc.closing_disconnect_inflight &&
                      !s_tirtc.deferred_disconnect_inflight;
    bool current_close_completed =
        hconn == NULL ||
        s_tirtc.closing_completed_generation == closing_generation;
    int close_result = s_tirtc.closing_result;
    portEXIT_CRITICAL(&s_runtime_lock);
    if (!all_closed || !current_close_completed)
    {
        ESP_LOGE(TAG, "等待连接关闭任务完成超时，暂不继续释放 SDK hconn=%p", hconn);
        return TIRTC_E_BUSY;
    }
    return hconn != NULL ? close_result : 0;
}

/*
 * 常用控制接口示例
 *
 * 这些函数直接对应 TiRTC SDK 的公开 API。保留展开写法，是为了让客户
 * 清楚看到每个控制动作实际调用哪个 SDK 函数。
 */
int tirtc_subscribe_remote_video(uint8_t stream_id)
{
    tirtc_conn_t hconn = NULL;
    if (!acquire_current_connection(&hconn))
    {
        ESP_LOGW(TAG, "订阅对端视频失败：当前没有连接");
        return TIRTC_E_INVALID_HANDLE;
    }

    int ret = TiRtcSubscribeVideo(hconn, stream_id);
    tirtc_connection_guard_unlock();
    ESP_LOGI(TAG, "[CTRL][TX] 订阅对端视频 stream=%u ret=%d %s", stream_id, ret, TiRtcGetErrorStr(ret));
    return ret;
}

int tirtc_unsubscribe_remote_video(uint8_t stream_id)
{
    tirtc_conn_t hconn = NULL;
    if (!acquire_current_connection(&hconn))
    {
        ESP_LOGW(TAG, "取消订阅对端视频失败：当前没有连接");
        return TIRTC_E_INVALID_HANDLE;
    }

    int ret = TiRtcUnsubscribeVideo(hconn, stream_id);
    tirtc_connection_guard_unlock();
    ESP_LOGI(TAG, "[CTRL][TX] 取消订阅对端视频 stream=%u ret=%d %s", stream_id, ret, TiRtcGetErrorStr(ret));
    return ret;
}

int tirtc_subscribe_remote_audio(uint8_t stream_id)
{
    tirtc_conn_t hconn = NULL;
    if (!acquire_current_connection(&hconn))
    {
        ESP_LOGW(TAG, "订阅对端音频失败：当前没有连接");
        return TIRTC_E_INVALID_HANDLE;
    }

    int ret = TiRtcSubscribeAudio(hconn, stream_id);
    tirtc_connection_guard_unlock();
    ESP_LOGI(TAG, "[CTRL][TX] 订阅对端音频 stream=%u ret=%d %s", stream_id, ret, TiRtcGetErrorStr(ret));
    return ret;
}

int tirtc_unsubscribe_remote_audio(uint8_t stream_id)
{
    tirtc_conn_t hconn = NULL;
    if (!acquire_current_connection(&hconn))
    {
        ESP_LOGW(TAG, "取消订阅对端音频失败：当前没有连接");
        return TIRTC_E_INVALID_HANDLE;
    }

    int ret = TiRtcUnsubscribeAudio(hconn, stream_id);
    tirtc_connection_guard_unlock();
    ESP_LOGI(TAG, "[CTRL][TX] 取消订阅对端音频 stream=%u ret=%d %s", stream_id, ret, TiRtcGetErrorStr(ret));
    return ret;
}

int tirtc_request_remote_key_frame(uint8_t stream_id)
{
    tirtc_conn_t hconn = NULL;
    if (!acquire_current_connection(&hconn))
    {
        ESP_LOGW(TAG, "请求对端关键帧失败：当前没有连接");
        return TIRTC_E_INVALID_HANDLE;
    }

    int ret = TiRtcRequestKeyFrame(hconn, stream_id);
    tirtc_connection_guard_unlock();
    ESP_LOGI(TAG, "[CTRL][TX] 请求对端关键帧 stream=%u ret=%d %s", stream_id, ret, TiRtcGetErrorStr(ret));
    return ret;
}

/*
 * 状态日志
 *
 * 周期打印当前连接和收包计数，现场联调时能快速判断 SDK 是否在线、
 * 是否已有连接、发送缓冲是否堆积。
 */
void tirtc_log_status(void)
{
    bool connecting = tirtc_connect_is_connecting();
    tirtc_conn_t hconn = NULL;
    bool have_connection = acquire_current_connection(&hconn);

    portENTER_CRITICAL(&s_runtime_lock);
    bool started = s_tirtc.sdk_started;
    uint32_t rx_video_count = s_tirtc.rx_video_count;
    uint32_t rx_audio_count = s_tirtc.rx_audio_count;
    portEXIT_CRITICAL(&s_runtime_lock);

    size_t send_buffer_used = 0U;
    if (have_connection)
    {
        send_buffer_used = TiRtcGetSendBufferUsed(hconn);
        tirtc_connection_guard_unlock();
    }

    if (connecting)
    {
        ESP_LOGI(TAG, "TiRTC 状态：已上线=%d，连接=主动连接中", started ? 1 : 0);
    }
    else if (!have_connection)
    {
        ESP_LOGI(TAG, "TiRTC 状态：已上线=%d，连接=空闲", started ? 1 : 0);
    }
    else
    {
        ESP_LOGI(TAG,
                 "TiRTC 状态：已上线=%d，连接=%p，发送缓冲=%zu字节，接收视频帧=%" PRIu32 "，接收音频包=%" PRIu32,
                 started ? 1 : 0,
                 hconn,
                 send_buffer_used,
                 rx_video_count,
                 rx_audio_count);
    }
}
