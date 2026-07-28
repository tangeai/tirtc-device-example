#include "local_media_stream.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "h264_file_source.h"

static const char *TAG = "local_media";

#define LOCAL_MEDIA_SPIFFS_BASE "/spiffs"
#define LOCAL_MEDIA_SPIFFS_LABEL "storage"
#define LOCAL_MEDIA_VIDEO_PATH "/spiffs/send_video.h264"
#define LOCAL_MEDIA_AUDIO_PATH "/spiffs/send_audio.pcma"

#define LOCAL_MEDIA_VIDEO_FPS 25U
#define LOCAL_MEDIA_VIDEO_INTERVAL_MS (1000U / LOCAL_MEDIA_VIDEO_FPS)
#define LOCAL_MEDIA_VIDEO_LOG_FRAMES LOCAL_MEDIA_VIDEO_FPS
#define LOCAL_MEDIA_AUDIO_CLOCK_HZ 8000U
#define LOCAL_MEDIA_AUDIO_PACKET_BYTES 160U
#define LOCAL_MEDIA_AUDIO_LOG_PACKETS 50U
#define LOCAL_MEDIA_VIDEO_TASK_STACK (10 * 1024)
#define LOCAL_MEDIA_AUDIO_TASK_STACK (4 * 1024)
#define LOCAL_MEDIA_TASK_PRIORITY 6
#define LOCAL_MEDIA_TASK_CORE 1
#define LOCAL_MEDIA_STOP_POLL_MS 10U
#define LOCAL_MEDIA_INVALID_HANDLE_LOG_INTERVAL 50U

static portMUX_TYPE s_media_lock = portMUX_INITIALIZER_UNLOCKED;
static tirtc_conn_t s_media_conn;
static TaskHandle_t s_video_task;
static TaskHandle_t s_audio_task;
static uint8_t s_video_stream_id;
static uint8_t s_audio_stream_id;
static bool s_video_starting;
static bool s_audio_starting;
static bool s_spiffs_ready;
static bool s_video_enabled;
static bool s_audio_enabled;
static bool s_video_restart_requested;
static bool s_audio_restart_requested;

/* 测试媒体放在 SPIFFS，启动发送前先确认两个文件都能打开。 */
static bool file_size(const char *path, long *size)
{
    FILE *fp = NULL;
    long local_size = 0;

    if (path == NULL || size == NULL)
    {
        return false;
    }

    fp = fopen(path, "rb");
    if (fp == NULL)
    {
        return false;
    }

    if (fseek(fp, 0, SEEK_END) != 0)
    {
        fclose(fp);
        return false;
    }

    local_size = ftell(fp);
    fclose(fp);

    if (local_size <= 0)
    {
        return false;
    }

    *size = local_size;
    return true;
}

static esp_err_t mount_spiffs_once(void)
{
    long video_size = 0;
    long audio_size = 0;

    portENTER_CRITICAL(&s_media_lock);
    bool ready = s_spiffs_ready;
    portEXIT_CRITICAL(&s_media_lock);
    if (ready)
    {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = LOCAL_MEDIA_SPIFFS_BASE,
        .partition_label = LOCAL_MEDIA_SPIFFS_LABEL,
        .max_files = 4,
        .format_if_mount_failed = false,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret == ESP_ERR_INVALID_STATE)
    {
        ret = ESP_OK;
    }
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "挂载 SPIFFS 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    if (!file_size(LOCAL_MEDIA_VIDEO_PATH, &video_size))
    {
        ESP_LOGE(TAG, "缺少本地 H264 测试文件: %s", LOCAL_MEDIA_VIDEO_PATH);
        return ESP_ERR_NOT_FOUND;
    }
    if (!file_size(LOCAL_MEDIA_AUDIO_PATH, &audio_size))
    {
        ESP_LOGE(TAG, "缺少本地 PCMA 测试文件: %s", LOCAL_MEDIA_AUDIO_PATH);
        return ESP_ERR_NOT_FOUND;
    }

    portENTER_CRITICAL(&s_media_lock);
    s_spiffs_ready = true;
    portEXIT_CRITICAL(&s_media_lock);

    ESP_LOGI(TAG, "本地测试媒体已就绪: H264=%ld bytes PCMA=%ld bytes", video_size, audio_size);
    return ESP_OK;
}

esp_err_t local_media_stream_prepare(void)
{
    /*
     * SPIFFS 挂载会访问 flash 分区表并短暂关闭 cache。这个动作不能放在
     * TiRTC SDK 回调线程里做，因为 SDK 任务栈可能在 PSRAM，关 cache 时会触发
     * ESP-IDF 的栈安全断言。开机启动 TiRTC 前先准备好本地媒体最稳。
     */
    return mount_spiffs_once();
}

static BaseType_t create_media_task(TaskFunction_t task_func,
                                    const char *name,
                                    uint32_t stack_size,
                                    TaskHandle_t *task_handle)
{
#if CONFIG_FREERTOS_UNICORE
    return xTaskCreate(task_func, name, stack_size, NULL, LOCAL_MEDIA_TASK_PRIORITY, task_handle);
#else
    BaseType_t ret = xTaskCreatePinnedToCore(task_func,
                                             name,
                                             stack_size,
                                             NULL,
                                             LOCAL_MEDIA_TASK_PRIORITY,
                                             task_handle,
                                             LOCAL_MEDIA_TASK_CORE);
    if (ret != pdPASS)
    {
        ret = xTaskCreate(task_func, name, stack_size, NULL, LOCAL_MEDIA_TASK_PRIORITY, task_handle);
    }
    return ret;
#endif
}

static tirtc_conn_t current_video_connection(uint8_t *stream_id, bool *restart_video)
{
    tirtc_conn_t conn = NULL;

    portENTER_CRITICAL(&s_media_lock);
    if (s_video_enabled)
    {
        conn = s_media_conn;
    }
    if (stream_id != NULL)
    {
        *stream_id = s_video_stream_id;
    }
    if (restart_video != NULL)
    {
        *restart_video = s_video_restart_requested;
        s_video_restart_requested = false;
    }
    portEXIT_CRITICAL(&s_media_lock);

    return conn;
}

static tirtc_conn_t current_audio_connection(uint8_t *stream_id, bool *restart_audio)
{
    tirtc_conn_t conn = NULL;

    portENTER_CRITICAL(&s_media_lock);
    if (s_audio_enabled)
    {
        conn = s_media_conn;
    }
    if (stream_id != NULL)
    {
        *stream_id = s_audio_stream_id;
    }
    if (restart_audio != NULL)
    {
        *restart_audio = s_audio_restart_requested;
        s_audio_restart_requested = false;
    }
    portEXIT_CRITICAL(&s_media_lock);

    return conn;
}

/* 视频任务循环读取 Annex-B H264，关键帧请求时回到文件头重发。 */
static void video_task_entry(void *arg)
{
    (void)arg;

    while (true)
    {
        portENTER_CRITICAL(&s_media_lock);
        bool starting = s_video_starting;
        portEXIT_CRITICAL(&s_media_lock);
        if (!starting)
        {
            break;
        }
        vTaskDelay(1);
    }

    h264_file_source_t source = {0};
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t frame_index = 0;
    uint32_t invalid_handle_count = 0;
    uint32_t media_ts_ms = 0;
    int ret = h264_file_source_open(LOCAL_MEDIA_VIDEO_PATH, &source);

    if (ret != H264_FILE_SOURCE_OK)
    {
        ESP_LOGE(TAG, "打开本地 H264 失败 ret=%d path=%s", ret, LOCAL_MEDIA_VIDEO_PATH);
        goto exit_task;
    }

    ESP_LOGI(TAG, "[TX][video] 发流开始：视频=H264，目标帧率=%u fps，流ID=%u",
             LOCAL_MEDIA_VIDEO_FPS,
             s_video_stream_id);

    while (true)
    {
        uint8_t stream_id = 0;
        bool restart_video = false;
        tirtc_conn_t conn = NULL;
        const uint8_t *data = NULL;
        size_t length = 0;
        bool is_key_frame = false;

        conn = current_video_connection(&stream_id, &restart_video);
        if (conn == NULL)
        {
            break;
        }

        if (restart_video)
        {
            h264_file_source_reset(&source);
            frame_index = 0;
            media_ts_ms = 0;
            last_wake = xTaskGetTickCount();
            ESP_LOGI(TAG, "[TX][video] 收到关键帧请求，从 H264 文件头重新发送");
        }

        ret = h264_file_source_next_frame(&source, &data, &length, &is_key_frame);
        if (ret == H264_FILE_SOURCE_EOF)
        {
            h264_file_source_reset(&source);
            frame_index = 0;
            media_ts_ms = 0;
            last_wake = xTaskGetTickCount();
            continue;
        }
        if (ret != H264_FILE_SOURCE_OK)
        {
            ESP_LOGE(TAG, "读取 H264 帧失败 ret=%d，稍后从头重试", ret);
            h264_file_source_reset(&source);
            last_wake = xTaskGetTickCount();
            vTaskDelay(pdMS_TO_TICKS(LOCAL_MEDIA_VIDEO_INTERVAL_MS));
            continue;
        }

        TIRTCFRAMEINFO frame = {
            .stream_id = stream_id,
            .media = TIRTC_VIDEO_H264,
            .flags = is_key_frame ? TIRTC_FRAME_FLAG_KEY_FRAME : 0,
            .reserved = 0,
            .ts = media_ts_ms,
            .length = (uint32_t)length,
        };

        ret = TiRtcSendVideoStream(conn, &frame, (void *)data);

        if (ret == TIRTC_E_BUSY)
        {
            frame_index++;
            media_ts_ms += LOCAL_MEDIA_VIDEO_INTERVAL_MS;
            if ((frame_index % LOCAL_MEDIA_VIDEO_FPS) == 0)
            {
                ESP_LOGW(TAG, "[TX][video] 发送缓冲忙：已丢帧，视频帧=%" PRIu32, frame_index);
            }
        }
        else if (ret == TIRTC_E_INVALID_HANDLE)
        {
            invalid_handle_count++;
            if (invalid_handle_count == 1 ||
                (invalid_handle_count % LOCAL_MEDIA_INVALID_HANDLE_LOG_INTERVAL) == 0)
            {
                ESP_LOGW(TAG,
                         "[TX][video] 连接暂不可发送，继续重试：视频帧=%" PRIu32 "，连续无效句柄=%" PRIu32,
                         frame_index + 1U,
                         invalid_handle_count);
            }

            h264_file_source_reset(&source);
            frame_index = 0;
            media_ts_ms = 0;
            last_wake = xTaskGetTickCount();
        }
        else if (ret < 0)
        {
            frame_index++;
            media_ts_ms += LOCAL_MEDIA_VIDEO_INTERVAL_MS;
            invalid_handle_count = 0;
            ESP_LOGE(TAG, "[TX][video] 发送失败：视频帧=%" PRIu32 "，错误=%d %s",
                     frame_index,
                     ret,
                     TiRtcGetErrorStr(ret));
        }
        else
        {
            frame_index++;
            media_ts_ms += LOCAL_MEDIA_VIDEO_INTERVAL_MS;
            invalid_handle_count = 0;
            if ((frame_index % LOCAL_MEDIA_VIDEO_LOG_FRAMES) == 0)
            {
                ESP_LOGI(TAG,
                         "[TX][video] 发送统计：视频帧=%" PRIu32 "，流ID=%u，发送缓冲=%zu字节",
                         frame_index,
                         stream_id,
                         TiRtcGetSendBufferUsed(conn));
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(LOCAL_MEDIA_VIDEO_INTERVAL_MS));
    }

exit_task:
    h264_file_source_close(&source);

    portENTER_CRITICAL(&s_media_lock);
    if (s_video_task == xTaskGetCurrentTaskHandle())
    {
        s_video_task = NULL;
    }
    s_video_starting = false;
    portEXIT_CRITICAL(&s_media_lock);

    ESP_LOGI(TAG, "[TX][video] 本机 H264 发送任务已停止");
    vTaskDelete(NULL);
}

/* 音频任务按 8k A-law 20ms 左右一包发送，文件结束后循环。 */
static void audio_task_entry(void *arg)
{
    (void)arg;

    while (true)
    {
        portENTER_CRITICAL(&s_media_lock);
        bool starting = s_audio_starting;
        portEXIT_CRITICAL(&s_media_lock);
        if (!starting)
        {
            break;
        }
        vTaskDelay(1);
    }

    FILE *fp = fopen(LOCAL_MEDIA_AUDIO_PATH, "rb");
    uint8_t packet[LOCAL_MEDIA_AUDIO_PACKET_BYTES] = {0};
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t packet_index = 0;
    uint32_t invalid_handle_count = 0;
    uint32_t media_ts_ms = 0;

    if (fp == NULL)
    {
        ESP_LOGE(TAG, "打开本地 PCMA 失败 path=%s", LOCAL_MEDIA_AUDIO_PATH);
        goto exit_task;
    }

    ESP_LOGI(TAG, "[TX][audio] 发流开始：音频=PCMA，采样=8k A-law，流ID=%u", s_audio_stream_id);

    while (true)
    {
        uint8_t stream_id = 0;
        tirtc_conn_t conn = NULL;
        size_t bytes_read = 0;
        uint32_t duration_ms = 0;
        bool restart_audio = false;

        conn = current_audio_connection(&stream_id, &restart_audio);
        if (conn == NULL)
        {
            break;
        }

        if (restart_audio)
        {
            if (fseek(fp, 0, SEEK_SET) != 0)
            {
                ESP_LOGE(TAG, "PCMA 回到文件头失败，退出音频发送");
                break;
            }
            packet_index = 0;
            media_ts_ms = 0;
            last_wake = xTaskGetTickCount();
            ESP_LOGI(TAG, "[TX][audio] 连接切换，PCMA 从文件头重新发送");
        }

        bytes_read = fread(packet, 1, sizeof(packet), fp);
        if (bytes_read == 0)
        {
            if (ferror(fp))
            {
                ESP_LOGE(TAG, "读取 PCMA 文件失败，重新从头尝试");
                clearerr(fp);
            }

            if (fseek(fp, 0, SEEK_SET) != 0)
            {
                ESP_LOGE(TAG, "PCMA 回到文件头失败，退出音频发送");
                break;
            }
            media_ts_ms = 0;
            last_wake = xTaskGetTickCount();
            continue;
        }

        duration_ms = (uint32_t)(((uint64_t)bytes_read * 1000ULL + (LOCAL_MEDIA_AUDIO_CLOCK_HZ / 2U)) /
                                 LOCAL_MEDIA_AUDIO_CLOCK_HZ);
        if (duration_ms == 0)
        {
            duration_ms = 1;
        }

        TIRTCFRAMEINFO frame = {
            .stream_id = stream_id,
            .media = TIRTC_AUDIO_ALAW,
            .flags = TIRTC_AUDIOSAMPLE_8K16B1C,
            .reserved = 0,
            .ts = media_ts_ms,
            .length = (uint32_t)bytes_read,
        };

        int ret = TiRtcSendAudioStream(conn, &frame, packet);

        if (ret == TIRTC_E_BUSY)
        {
            packet_index++;
            media_ts_ms += duration_ms;
            if ((packet_index % LOCAL_MEDIA_AUDIO_LOG_PACKETS) == 0)
            {
                ESP_LOGW(TAG, "[TX][audio] 发送缓冲忙：已丢包，音频包=%" PRIu32, packet_index);
            }
        }
        else if (ret == TIRTC_E_INVALID_HANDLE)
        {
            invalid_handle_count++;
            if (invalid_handle_count == 1 ||
                (invalid_handle_count % LOCAL_MEDIA_INVALID_HANDLE_LOG_INTERVAL) == 0)
            {
                ESP_LOGW(TAG,
                         "[TX][audio] 连接暂不可发送，继续重试：音频包=%" PRIu32 "，连续无效句柄=%" PRIu32,
                         packet_index + 1U,
                         invalid_handle_count);
            }

            if (fseek(fp, -(long)bytes_read, SEEK_CUR) != 0)
            {
                if (fseek(fp, 0, SEEK_SET) != 0)
                {
                    ESP_LOGE(TAG, "PCMA 回退当前包失败，退出音频发送");
                    break;
                }
            }
            last_wake = xTaskGetTickCount();
        }
        else if (ret < 0)
        {
            packet_index++;
            media_ts_ms += duration_ms;
            invalid_handle_count = 0;
            ESP_LOGE(TAG, "[TX][audio] 发送失败：音频包=%" PRIu32 "，错误=%d %s",
                     packet_index,
                     ret,
                     TiRtcGetErrorStr(ret));
        }
        else
        {
            packet_index++;
            media_ts_ms += duration_ms;
            invalid_handle_count = 0;
            if ((packet_index % LOCAL_MEDIA_AUDIO_LOG_PACKETS) == 0)
            {
                ESP_LOGI(TAG,
                         "[TX][audio] 发送统计：音频包=%" PRIu32 "，流ID=%u，发送缓冲=%zu字节",
                         packet_index,
                         stream_id,
                         TiRtcGetSendBufferUsed(conn));
            }
        }

        if (feof(fp))
        {
            if (fseek(fp, 0, SEEK_SET) != 0)
            {
                ESP_LOGE(TAG, "PCMA 回到文件头失败，退出音频发送");
                break;
            }
            media_ts_ms = 0;
            last_wake = xTaskGetTickCount();
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(duration_ms));
    }

exit_task:
    if (fp != NULL)
    {
        fclose(fp);
    }

    portENTER_CRITICAL(&s_media_lock);
    if (s_audio_task == xTaskGetCurrentTaskHandle())
    {
        s_audio_task = NULL;
    }
    s_audio_starting = false;
    portEXIT_CRITICAL(&s_media_lock);

    ESP_LOGI(TAG, "[TX][audio] 本机 PCMA 发送任务已停止");
    vTaskDelete(NULL);
}

static esp_err_t check_media_ready(void)
{
    portENTER_CRITICAL(&s_media_lock);
    bool ready = s_spiffs_ready;
    portEXIT_CRITICAL(&s_media_lock);

    if (!ready)
    {
        ESP_LOGE(TAG, "本地测试媒体尚未准备，不能在 TiRTC 回调里挂载 SPIFFS");
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

esp_err_t local_media_stream_start_video(tirtc_conn_t hconn, uint8_t stream_id)
{
    bool create_video = false;
    bool new_connection = false;
    TaskHandle_t task = NULL;

    if (hconn == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = check_media_ready();
    if (ret != ESP_OK)
    {
        return ret;
    }

    portENTER_CRITICAL(&s_media_lock);
    new_connection = (s_media_conn != hconn);
    s_media_conn = hconn;
    s_video_stream_id = stream_id;
    s_video_enabled = true;
    if (new_connection)
    {
        s_video_restart_requested = true;
    }

    create_video = (s_video_task == NULL && !s_video_starting);
    if (create_video)
    {
        s_video_starting = true;
    }
    portEXIT_CRITICAL(&s_media_lock);

    if (create_video)
    {
        BaseType_t task_ret = create_media_task(video_task_entry,
                                                "h264_tx",
                                                LOCAL_MEDIA_VIDEO_TASK_STACK,
                                                &task);
        portENTER_CRITICAL(&s_media_lock);
        if (task_ret == pdPASS)
        {
            s_video_task = task;
        }
        s_video_starting = false;
        portEXIT_CRITICAL(&s_media_lock);

        if (task_ret != pdPASS)
        {
            local_media_stream_stop(hconn);
            ESP_LOGE(TAG, "创建 H264 发送任务失败");
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "[TX][video] 视频发送准备完成：格式=H264，流ID=%u", stream_id);
    return ESP_OK;
}

esp_err_t local_media_stream_start_audio(tirtc_conn_t hconn, uint8_t stream_id)
{
    bool create_audio = false;
    bool new_connection = false;
    TaskHandle_t task = NULL;

    if (hconn == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = check_media_ready();
    if (ret != ESP_OK)
    {
        return ret;
    }

    portENTER_CRITICAL(&s_media_lock);
    new_connection = (s_media_conn != hconn);
    s_media_conn = hconn;
    s_audio_stream_id = stream_id;
    s_audio_enabled = true;
    if (new_connection)
    {
        s_audio_restart_requested = true;
    }

    create_audio = (s_audio_task == NULL && !s_audio_starting);
    if (create_audio)
    {
        s_audio_starting = true;
    }
    portEXIT_CRITICAL(&s_media_lock);

    if (create_audio)
    {
        task = NULL;
        BaseType_t task_ret = create_media_task(audio_task_entry,
                                                "pcma_tx",
                                                LOCAL_MEDIA_AUDIO_TASK_STACK,
                                                &task);
        portENTER_CRITICAL(&s_media_lock);
        if (task_ret == pdPASS)
        {
            s_audio_task = task;
        }
        s_audio_starting = false;
        portEXIT_CRITICAL(&s_media_lock);

        if (task_ret != pdPASS)
        {
            local_media_stream_stop(hconn);
            ESP_LOGE(TAG, "创建 PCMA 发送任务失败");
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "[TX][audio] 音频发送准备完成：格式=PCMA，流ID=%u", stream_id);
    return ESP_OK;
}

void local_media_stream_request_key_frame(tirtc_conn_t hconn)
{
    portENTER_CRITICAL(&s_media_lock);
    if (hconn != NULL && hconn == s_media_conn && s_video_enabled)
    {
        s_video_restart_requested = true;
    }
    portEXIT_CRITICAL(&s_media_lock);
}

void local_media_stream_stop_video(tirtc_conn_t hconn)
{
    portENTER_CRITICAL(&s_media_lock);
    if (hconn == NULL || hconn == s_media_conn)
    {
        s_video_enabled = false;
        s_video_restart_requested = false;
    }
    portEXIT_CRITICAL(&s_media_lock);
}

void local_media_stream_stop_audio(tirtc_conn_t hconn)
{
    portENTER_CRITICAL(&s_media_lock);
    if (hconn == NULL || hconn == s_media_conn)
    {
        s_audio_enabled = false;
        s_audio_restart_requested = false;
    }
    portEXIT_CRITICAL(&s_media_lock);
}

void local_media_stream_stop(tirtc_conn_t hconn)
{
    portENTER_CRITICAL(&s_media_lock);
    if (hconn == NULL || hconn == s_media_conn)
    {
        s_media_conn = NULL;
        s_video_enabled = false;
        s_audio_enabled = false;
        s_video_restart_requested = false;
        s_audio_restart_requested = false;
    }
    portEXIT_CRITICAL(&s_media_lock);
}

void local_media_stream_stop_and_wait(tirtc_conn_t hconn, uint32_t timeout_ms)
{
    uint32_t waited_ms = 0;

    local_media_stream_stop(hconn);

    while (waited_ms < timeout_ms)
    {
        bool running = false;

        portENTER_CRITICAL(&s_media_lock);
        running = (s_video_task != NULL || s_audio_task != NULL || s_video_starting || s_audio_starting);
        portEXIT_CRITICAL(&s_media_lock);

        if (!running)
        {
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(LOCAL_MEDIA_STOP_POLL_MS));
        waited_ms += LOCAL_MEDIA_STOP_POLL_MS;
    }

    ESP_LOGW(TAG, "等待本地测试音视频停止超时");
}
