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
 * 这里放示例内部策略和 SDK 当前状态；需要修改的业务参数放在
 * tirtc_config.h，避免把示例行为和接入参数混在一起。
 */
static const char *TAG = "tirtc_app";

#define TIRTC_SDK_LOG_LEVEL 0
#define TIRTC_MAX_SEND_BUFFER_BYTES (128U * 1024U)
#define TIRTC_VIDEO_STREAM_ID 11
#define TIRTC_AUDIO_STREAM_ID 10
#define TIRTC_AUTO_SUBSCRIBE_REMOTE_ON_CONNECT 0
#define TIRTC_AUTO_SUBSCRIBE_VIDEO 1
#define TIRTC_AUTO_SUBSCRIBE_AUDIO 1
#define TIRTC_AUTO_REQUEST_KEY_FRAME 0
#define TIRTC_LOCAL_MEDIA_STOP_WAIT_MS 500
#define TIRTC_STOP_WAIT_MS 1000
#define TIRTC_RX_VIDEO_LOG_FRAMES 25U
#define TIRTC_RX_AUDIO_LOG_PACKETS 50U

typedef struct
{
    tirtc_conn_t conn;
    bool sdk_initialized;
    bool sdk_started;
    bool deinitializing;
    bool stop_notified;
    uint32_t rx_video_count;
    uint32_t rx_audio_count;
} tirtc_runtime_t;

static tirtc_runtime_t s_tirtc = {
    .stop_notified = true,
};

static void tirtc_start_local_media(tirtc_conn_t hconn, const char *reason)
{
#if TIRTC_AUTO_PUSH_LOCAL_MEDIA_ON_CONNECT
    if (hconn == NULL)
    {
        return;
    }

    ESP_LOGI(TAG,
             "本机默认主动推流: %s video_stream=%u audio_stream=%u",
             reason != NULL ? reason : "连接建立",
             TIRTC_VIDEO_STREAM_ID,
             TIRTC_AUDIO_STREAM_ID);

    esp_err_t media_ret = local_media_stream_start_video(hconn, TIRTC_VIDEO_STREAM_ID);
    if (media_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "[TX][video] 启动本机 H264 测试视频失败: %s", esp_err_to_name(media_ret));
    }
    else
    {
        local_media_stream_request_key_frame(hconn);
    }

    media_ret = local_media_stream_start_audio(hconn, TIRTC_AUDIO_STREAM_ID);
    if (media_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "[TX][audio] 启动本机 PCMA 测试音频失败: %s", esp_err_to_name(media_ret));
    }
#else
    (void)hconn;
    (void)reason;
#endif
}

static void tirtc_auto_subscribe_remote_media(const char *reason)
{
#if TIRTC_AUTO_SUBSCRIBE_REMOTE_ON_CONNECT
    ESP_LOGI(TAG, "自动订阅对端媒体: %s video_stream=%u audio_stream=%u",
             reason != NULL ? reason : "连接建立",
             TIRTC_VIDEO_STREAM_ID,
             TIRTC_AUDIO_STREAM_ID);

#if TIRTC_AUTO_SUBSCRIBE_VIDEO
    (void)tirtc_subscribe_remote_video(TIRTC_VIDEO_STREAM_ID);
#endif

#if TIRTC_AUTO_SUBSCRIBE_AUDIO
    (void)tirtc_subscribe_remote_audio(TIRTC_AUDIO_STREAM_ID);
#endif
#else
    (void)reason;
#endif
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
        s_tirtc.sdk_started = true;
        s_tirtc.stop_notified = false;
        ESP_LOGI(TAG, "TiRTC 已上线，可接收入站连接，也可主动连接远端设备");
        tirtc_connect_on_tirtc_started();
#if TIRTC_AUTO_CONNECT_AFTER_STARTED
        (void)tirtc_connect_configured();
#endif
        break;

    case TIRTC_EVENT_SYS_STOPPED:
        s_tirtc.sdk_started = false;
        s_tirtc.stop_notified = true;
        ESP_LOGI(TAG, "TiRTC 已停止");
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

    if (!s_tirtc.sdk_initialized || s_tirtc.deinitializing)
    {
        ESP_LOGW(TAG, "TiRTC 正在停止，释放新连接 hconn=%p", hconn);
        int ret = TiRtcDisconnect(hconn);
        ESP_LOGI(TAG, "释放停止期间返回的连接 ret=%d %s", ret, TiRtcGetErrorStr(ret));
        return;
    }

    if (s_tirtc.conn == hconn)
    {
        return;
    }

    if (s_tirtc.conn != NULL)
    {
        tirtc_conn_t old_conn = s_tirtc.conn;
        s_tirtc.conn = NULL;

        /* 旧连接只发停止信号，避免连接切换卡住 SDK 回调。 */
        local_media_stream_stop(old_conn);
        ESP_LOGW(TAG, "已有当前连接，断开旧连接 hconn=%p", old_conn);

        int ret = TiRtcDisconnect(old_conn);
        ESP_LOGI(TAG, "断开旧连接 ret=%d %s", ret, TiRtcGetErrorStr(ret));
    }

    s_tirtc.conn = hconn;
    s_tirtc.rx_video_count = 0;
    s_tirtc.rx_audio_count = 0;
    ESP_LOGI(TAG, "当前连接已建立 hconn=%p", hconn);
    tirtc_start_local_media(hconn, "远端呼入");
    tirtc_auto_subscribe_remote_media("远端呼入");

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

    if (hconn != NULL && hconn == s_tirtc.conn)
    {
        /* 回调线程里只发停止信号，不等待发送任务退出。 */
        local_media_stream_stop(hconn);
        s_tirtc.conn = NULL;
        ESP_LOGI(TAG, "当前连接已清理 hconn=%p", hconn);
    }

    if (hconn != NULL)
    {
        int ret = TiRtcDisconnect(hconn);
        ESP_LOGI(TAG, "连接错误后释放连接 ret=%d %s", ret, TiRtcGetErrorStr(ret));
    }
}

static void on_disconnected(tirtc_conn_t hconn)
{
    ESP_LOGI(TAG, "TiRTC 连接断开 hconn=%p", hconn);
    if (hconn != NULL && hconn == s_tirtc.conn)
    {
        /* 回调线程里只发停止信号，不等待发送任务退出。 */
        local_media_stream_stop(hconn);
        s_tirtc.conn = NULL;
        ESP_LOGI(TAG, "当前连接已清理 hconn=%p", hconn);
    }
}

/*
 * 收包与订阅回调
 *
 * demo 只处理当前连接上的事件。连接建立后不自动出图；收到订阅后发送
 * 本地 H264/PCMA，同时保留取消订阅、再次订阅、请求关键帧等控制回调。
 */
static void on_audio(tirtc_conn_t hconn, const TIRTCFRAMEINFO *info, void *data)
{
    (void)data;
    if (hconn == NULL || hconn != s_tirtc.conn || info == NULL)
    {
        return;
    }

    s_tirtc.rx_audio_count++;
    if ((s_tirtc.rx_audio_count % TIRTC_RX_AUDIO_LOG_PACKETS) == 0)
    {
        ESP_LOGI(TAG,
                 "[RX][audio] 接收统计：音频包=%" PRIu32 "，流ID=%u，长度=%u字节，时间戳=%u",
                 s_tirtc.rx_audio_count,
                 info->stream_id,
                 info->length,
                 info->ts);
    }
}

static void on_video(tirtc_conn_t hconn, const TIRTCFRAMEINFO *info, void *data)
{
    (void)data;
    if (hconn == NULL || hconn != s_tirtc.conn || info == NULL)
    {
        return;
    }

    s_tirtc.rx_video_count++;
    if ((s_tirtc.rx_video_count % TIRTC_RX_VIDEO_LOG_FRAMES) == 0)
    {
        ESP_LOGI(TAG,
                 "[RX][video] 接收统计：视频帧=%" PRIu32 "，流ID=%u，长度=%u字节，时间戳=%u，关键帧=%s",
                 s_tirtc.rx_video_count,
                 info->stream_id,
                 info->length,
                 info->ts,
                 (info->flags & TIRTC_FRAME_FLAG_KEY_FRAME) != 0 ? "是" : "否");
    }
}

static void on_message(tirtc_conn_t hconn, const TIRTCFRAMEINFO *info, void *data)
{
    (void)data;
    if (hconn != NULL && hconn == s_tirtc.conn && info != NULL)
    {
        ESP_LOGI(TAG, "收到对端消息 stream=%u len=%u", info->stream_id, info->length);
    }
}

static void on_command(tirtc_conn_t hconn, uint32_t cmdw, const void *data, uint32_t len)
{
    (void)data;
    if (hconn != NULL && hconn == s_tirtc.conn)
    {
        ESP_LOGI(TAG, "收到对端命令 cmdw=0x%08" PRIx32 " len=%" PRIu32, cmdw, len);
    }
}

static void on_request_key_frame(tirtc_conn_t hconn, uint8_t stream_id)
{
    if (hconn != NULL && hconn == s_tirtc.conn)
    {
        ESP_LOGI(TAG, "[CTRL][RX] 对端请求本机关键帧 stream=%u", stream_id);
        local_media_stream_request_key_frame(hconn);
    }
}

static int on_subscribe_video(tirtc_conn_t hconn, uint8_t stream_id)
{
    if (hconn != NULL && hconn == s_tirtc.conn)
    {
        ESP_LOGI(TAG, "[CTRL][RX] 对端订阅本机视频 stream=%u", stream_id);
        esp_err_t media_ret = local_media_stream_start_video(hconn, stream_id);
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
    if (hconn != NULL && hconn == s_tirtc.conn)
    {
        ESP_LOGI(TAG, "[CTRL][RX] 对端取消订阅本机视频 stream=%u", stream_id);
        local_media_stream_stop_video(hconn);
    }
}

static int on_subscribe_audio(tirtc_conn_t hconn, uint8_t stream_id)
{
    if (hconn != NULL && hconn == s_tirtc.conn)
    {
        ESP_LOGI(TAG, "[CTRL][RX] 对端订阅本机音频 stream=%u", stream_id);
        esp_err_t media_ret = local_media_stream_start_audio(hconn, stream_id);
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
    if (hconn != NULL && hconn == s_tirtc.conn)
    {
        ESP_LOGI(TAG, "[CTRL][RX] 对端取消订阅本机音频 stream=%u", stream_id);
        local_media_stream_stop_audio(hconn);
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

/*
 * SDK 启动与释放
 *
 * 启动顺序：设置 SDK option -> TiRtcInit() -> TiRtcStart()。
 * 释放顺序反过来：停主动连接 -> 断当前连接 -> TiRtcStop()
 * -> TiRtcUninit()。
 */
esp_err_t tirtc_start(void)
{
    if (s_tirtc.sdk_initialized)
    {
        ESP_LOGW(TAG, "TiRTC 已经初始化，忽略重复启动");
        return ESP_OK;
    }

    s_tirtc.deinitializing = false;
    s_tirtc.rx_video_count = 0;
    s_tirtc.rx_audio_count = 0;

    ESP_LOGI(TAG, "TiRTC 版本: %s", TiRtcGetVersion());
    ESP_LOGI(TAG, "PSRAM 总量=%zu 可用=%zu",
             heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    TiRtcLogSetLevel(TIRTC_SDK_LOG_LEVEL);

    /* H264/PCMA 测试流会持续发送，发送缓冲要在 TiRtcInit() 前设置。 */
    uint32_t max_send_buffer = TIRTC_MAX_SEND_BUFFER_BYTES;
    int sdk_ret = TiRtcSetOption(TIRTC_OPT_MAX_SEND_BUFFER,
                                 &max_send_buffer,
                                 (uint32_t)sizeof(max_send_buffer));
    if (sdk_ret != 0)
    {
        ESP_LOGE(TAG, "设置 TiRTC 发送缓冲失败: %d %s", sdk_ret, TiRtcGetErrorStr(sdk_ret));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "TiRTC 发送缓冲: %" PRIu32 " bytes", max_send_buffer);

    /* 服务地址放在配置文件里，方便 HTTP/HTTPS 或测试/正式环境切换。 */
    if (strlen(TIRTC_SERVICE_ENDPOINT) > 0)
    {
        sdk_ret = TiRtcSetOption(TIRTC_OPT_SERVICE_ENDPOINT,
                                 TIRTC_SERVICE_ENDPOINT,
                                 (uint32_t)strlen(TIRTC_SERVICE_ENDPOINT) + 1U);
        if (sdk_ret != 0)
        {
            ESP_LOGE(TAG, "设置 TiRTC 服务地址失败: %d %s", sdk_ret, TiRtcGetErrorStr(sdk_ret));
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "TiRTC 服务地址: %s", TIRTC_SERVICE_ENDPOINT);
    }

    esp_err_t media_ret = local_media_stream_prepare();
    if (media_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "准备本地 H264/PCMA 测试媒体失败: %s", esp_err_to_name(media_ret));
        return media_ret;
    }

    sdk_ret = TiRtcInit();
    if (sdk_ret != 0)
    {
        ESP_LOGE(TAG, "TiRtcInit 失败: %d %s", sdk_ret, TiRtcGetErrorStr(sdk_ret));
        return ESP_FAIL;
    }
    s_tirtc.sdk_initialized = true;

    if (strlen(TIRTC_DEVICE_ID) == 0 || strlen(TIRTC_DEVICE_SECRET_KEY) == 0)
    {
        ESP_LOGE(TAG, "设备 ID 或设备密钥为空，无法上线");
        TiRtcUninit();
        s_tirtc.sdk_initialized = false;
        return ESP_ERR_INVALID_STATE;
    }

    sdk_ret = TiRtcSetOption(TIRTC_OPT_DEVICE_SECRET_KEY,
                             TIRTC_DEVICE_SECRET_KEY,
                             (uint32_t)strlen(TIRTC_DEVICE_SECRET_KEY) + 1U);
    if (sdk_ret != 0)
    {
        ESP_LOGE(TAG, "设置设备密钥失败: %d %s", sdk_ret, TiRtcGetErrorStr(sdk_ret));
        TiRtcUninit();
        s_tirtc.sdk_initialized = false;
        return ESP_FAIL;
    }

    int start_ret = TiRtcStart(TIRTC_DEVICE_ID, &s_tirtc_callbacks);
    if (start_ret != 0)
    {
        ESP_LOGE(TAG, "TiRtcStart 失败: %d %s", start_ret, TiRtcGetErrorStr(start_ret));
        TiRtcUninit();
        s_tirtc.sdk_initialized = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TiRTC 启动请求已提交: device_id=%s", TIRTC_DEVICE_ID);
    return ESP_OK;
}

void tirtc_deinit(void)
{
    if (!s_tirtc.sdk_initialized)
    {
        return;
    }

    ESP_LOGI(TAG, "开始释放 TiRTC 资源");

    /* 退出顺序：停主动连接、断当前连接、停 SDK、释放 SDK。 */
    s_tirtc.deinitializing = true;
    tirtc_connect_cancel();
    (void)tirtc_disconnect_current();

    s_tirtc.stop_notified = false;
    int stop_ret = TiRtcStop();
    if (stop_ret == 0)
    {
        const TickType_t wait_step = pdMS_TO_TICKS(50);
        uint32_t waited_ms = 0;

        while (!s_tirtc.stop_notified && waited_ms < TIRTC_STOP_WAIT_MS)
        {
            vTaskDelay(wait_step);
            waited_ms += 50U;
        }

        if (!s_tirtc.stop_notified)
        {
            ESP_LOGW(TAG, "等待 TiRTC 停止事件超时，继续释放运行时资源");
        }
    }
    else
    {
        ESP_LOGW(TAG, "TiRtcStop 返回: %d %s", stop_ret, TiRtcGetErrorStr(stop_ret));
    }

    TiRtcUninit();
    s_tirtc = (tirtc_runtime_t){.stop_notified = true};
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

    if (!s_tirtc.sdk_initialized || s_tirtc.deinitializing)
    {
        ESP_LOGW(TAG, "TiRTC 正在停止，释放主动连接 hconn=%p", hconn);
        int ret = TiRtcDisconnect(hconn);
        ESP_LOGI(TAG, "释放停止期间返回的连接 ret=%d %s", ret, TiRtcGetErrorStr(ret));
        return;
    }

    if (s_tirtc.conn == hconn)
    {
        return;
    }

    if (s_tirtc.conn != NULL)
    {
        tirtc_conn_t old_conn = s_tirtc.conn;
        s_tirtc.conn = NULL;

        /* 旧连接只发停止信号，避免连接切换卡住 SDK 回调。 */
        local_media_stream_stop(old_conn);
        ESP_LOGW(TAG, "已有当前连接，断开旧连接 hconn=%p", old_conn);

        int ret = TiRtcDisconnect(old_conn);
        ESP_LOGI(TAG, "断开旧连接 ret=%d %s", ret, TiRtcGetErrorStr(ret));
    }

    s_tirtc.conn = hconn;
    s_tirtc.rx_video_count = 0;
    s_tirtc.rx_audio_count = 0;
    ESP_LOGI(TAG, "当前连接已建立 hconn=%p", hconn);

    tirtc_start_local_media(hconn, "主动连接");
    tirtc_auto_subscribe_remote_media("主动连接");

#if TIRTC_AUTO_REQUEST_KEY_FRAME
    (void)tirtc_request_remote_key_frame(TIRTC_VIDEO_STREAM_ID);
#endif
}

esp_err_t tirtc_connect_configured(void)
{
    if (!s_tirtc.sdk_started || s_tirtc.deinitializing)
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

    if (s_tirtc.conn != NULL)
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
    if (s_tirtc.conn == NULL)
    {
        return 0;
    }

    tirtc_conn_t hconn = s_tirtc.conn;
    s_tirtc.conn = NULL;
    local_media_stream_stop_and_wait(hconn, TIRTC_LOCAL_MEDIA_STOP_WAIT_MS);
    int ret = TiRtcDisconnect(hconn);
    ESP_LOGI(TAG, "主动断开当前连接 ret=%d %s", ret, TiRtcGetErrorStr(ret));
    return ret;
}

/*
 * 常用控制接口示例
 *
 * 这些函数直接对应 TiRTC SDK 的公开 API。保留展开写法，便于查看
 * 每个控制动作实际调用哪个 SDK 函数。
 */
int tirtc_subscribe_remote_video(uint8_t stream_id)
{
    if (s_tirtc.conn == NULL)
    {
        ESP_LOGW(TAG, "订阅对端视频失败：当前没有连接");
        return TIRTC_E_INVALID_HANDLE;
    }

    int ret = TiRtcSubscribeVideo(s_tirtc.conn, stream_id);
    ESP_LOGI(TAG, "[CTRL][TX] 订阅对端视频 stream=%u ret=%d %s", stream_id, ret, TiRtcGetErrorStr(ret));
    return ret;
}

int tirtc_unsubscribe_remote_video(uint8_t stream_id)
{
    if (s_tirtc.conn == NULL)
    {
        ESP_LOGW(TAG, "取消订阅对端视频失败：当前没有连接");
        return TIRTC_E_INVALID_HANDLE;
    }

    int ret = TiRtcUnsubscribeVideo(s_tirtc.conn, stream_id);
    ESP_LOGI(TAG, "[CTRL][TX] 取消订阅对端视频 stream=%u ret=%d %s", stream_id, ret, TiRtcGetErrorStr(ret));
    return ret;
}

int tirtc_subscribe_remote_audio(uint8_t stream_id)
{
    if (s_tirtc.conn == NULL)
    {
        ESP_LOGW(TAG, "订阅对端音频失败：当前没有连接");
        return TIRTC_E_INVALID_HANDLE;
    }

    int ret = TiRtcSubscribeAudio(s_tirtc.conn, stream_id);
    ESP_LOGI(TAG, "[CTRL][TX] 订阅对端音频 stream=%u ret=%d %s", stream_id, ret, TiRtcGetErrorStr(ret));
    return ret;
}

int tirtc_unsubscribe_remote_audio(uint8_t stream_id)
{
    if (s_tirtc.conn == NULL)
    {
        ESP_LOGW(TAG, "取消订阅对端音频失败：当前没有连接");
        return TIRTC_E_INVALID_HANDLE;
    }

    int ret = TiRtcUnsubscribeAudio(s_tirtc.conn, stream_id);
    ESP_LOGI(TAG, "[CTRL][TX] 取消订阅对端音频 stream=%u ret=%d %s", stream_id, ret, TiRtcGetErrorStr(ret));
    return ret;
}

int tirtc_request_remote_key_frame(uint8_t stream_id)
{
    if (s_tirtc.conn == NULL)
    {
        ESP_LOGW(TAG, "请求对端关键帧失败：当前没有连接");
        return TIRTC_E_INVALID_HANDLE;
    }

    int ret = TiRtcRequestKeyFrame(s_tirtc.conn, stream_id);
    ESP_LOGI(TAG, "[CTRL][TX] 请求对端关键帧 stream=%u ret=%d %s", stream_id, ret, TiRtcGetErrorStr(ret));
    return ret;
}

/*
 * 状态日志
 *
 * 周期打印当前连接和收包计数，便于判断 SDK 是否在线、
 * 是否已有连接、发送缓冲是否堆积。
 */
void tirtc_log_status(void)
{
    if (tirtc_connect_is_connecting())
    {
        ESP_LOGI(TAG, "TiRTC 状态：已上线=%d，连接=主动连接中", s_tirtc.sdk_started ? 1 : 0);
    }
    else if (s_tirtc.conn == NULL)
    {
        ESP_LOGI(TAG, "TiRTC 状态：已上线=%d，连接=空闲", s_tirtc.sdk_started ? 1 : 0);
    }
    else
    {
        ESP_LOGI(TAG,
                 "TiRTC 状态：已上线=%d，连接=%p，发送缓冲=%zu字节，接收视频帧=%" PRIu32 "，接收音频包=%" PRIu32,
                 s_tirtc.sdk_started ? 1 : 0,
                 s_tirtc.conn,
                 TiRtcGetSendBufferUsed(s_tirtc.conn),
                 s_tirtc.rx_video_count,
                 s_tirtc.rx_audio_count);
    }
}
