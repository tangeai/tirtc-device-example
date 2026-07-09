#include "media_sink.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "audio_device.h"
#include "platform/app_task_affinity.h"

static const char *TAG = "media_sink";

#define MEDIA_SINK_AUDIO_QUEUE_LEN 16
#define MEDIA_SINK_AUDIO_TASK_STACK (5 * 1024)
#define MEDIA_SINK_AUDIO_TASK_PRIORITY 13
#define MEDIA_SINK_AUDIO_TASK_CORE APP_TASK_CORE_AUDIO
#define MEDIA_SINK_AUDIO_BACKLOG_TRIM_THRESHOLD 12
#define MEDIA_SINK_AUDIO_BACKLOG_TARGET_PACKETS 4
#define MEDIA_SINK_AUDIO_DRAIN_BURST_MAX 0
#define MEDIA_SINK_AUDIO_PLAY_CHUNK_MS 20
#define MEDIA_SINK_AUDIO_REALTIME_PREBUFFER_MS 20
#define MEDIA_SINK_AUDIO_BULK_PREBUFFER_MS 200
#define MEDIA_SINK_AUDIO_PCM_BUFFER_MS 1600
#define MEDIA_SINK_AUDIO_REALTIME_PACKET_MAX_MS 60
#define MEDIA_SINK_AUDIO_REALTIME_TARGET_MS 60
#define MEDIA_SINK_AUDIO_JITTER_BOOST_STEP_MS 20
#define MEDIA_SINK_AUDIO_JITTER_BOOST_MAX_MS 40
#define MEDIA_SINK_AUDIO_BULK_TARGET_MS 240
#define MEDIA_SINK_AUDIO_SLOW_PLAY_US 25000

typedef struct {
    uint32_t generation;
    audio_format_t format;
    uint8_t *data;
    size_t data_len;
} media_sink_audio_packet_t;

static QueueHandle_t s_audio_queue;
static TaskHandle_t s_audio_task;
static bool s_initialized;
static bool s_remote_audio_playback_started_logged;
static bool s_remote_audio_enqueue_logged;
static bool s_remote_audio_render_logged;
static portMUX_TYPE s_sink_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_generation;
static TickType_t s_last_audio_trim_log_tick;
static TickType_t s_last_audio_enqueue_log_tick;
static TickType_t s_last_audio_render_log_tick;
static TickType_t s_last_audio_slow_log_tick;
static TickType_t s_last_audio_write_drop_log_tick;
static TickType_t s_last_audio_rate_log_tick;
static uint32_t s_audio_rx_packets_in_window;
static uint32_t s_audio_rx_ms_in_window;
static uint32_t s_audio_play_ok_packets_in_window;
static uint32_t s_audio_play_ok_ms_in_window;
static uint32_t s_audio_play_drop_packets_in_window;
static uint32_t s_audio_play_drop_ms_in_window;
static uint32_t s_audio_trim_drop_packets_in_window;
static uint32_t s_audio_trim_drop_ms_in_window;
static uint32_t s_audio_jitter_boost_ms;
static uint32_t s_audio_last_source_packet_ms;
static uint8_t *s_audio_pcm_buffer;
static size_t s_audio_pcm_buffer_size;
static size_t s_audio_pcm_read_offset;
static size_t s_audio_pcm_used_bytes;
static uint8_t *s_audio_pcm_chunk_buffer;
static size_t s_audio_pcm_chunk_buffer_size;

static void media_sink_audio_task(void *ctx);

static size_t media_sink_audio_frame_bytes(const audio_format_t *format)
{
    if (format == NULL || format->bits_per_sample == 0 || format->channels == 0) {
        return 0;
    }

    return ((size_t)format->bits_per_sample / 8U) * format->channels;
}

static size_t media_sink_audio_bytes_for_duration_ms(uint32_t duration_ms,
                                                              const audio_format_t *format)
{
    size_t frame_bytes = media_sink_audio_frame_bytes(format);

    if (frame_bytes == 0 || format == NULL || format->sample_rate_hz == 0) {
        return 0;
    }

    return (size_t)(((uint64_t)format->sample_rate_hz * duration_ms * frame_bytes) / 1000ULL);
}

static uint32_t media_sink_audio_duration_ms_for_bytes(size_t bytes,
                                                                 const audio_format_t *format)
{
    size_t frame_bytes = media_sink_audio_frame_bytes(format);
    size_t bytes_per_second = 0;

    if (frame_bytes == 0 || format == NULL || format->sample_rate_hz == 0) {
        return 0;
    }

    bytes_per_second = (size_t)format->sample_rate_hz * frame_bytes;
    if (bytes_per_second == 0) {
        return 0;
    }

    return (uint32_t)(((uint64_t)bytes * 1000ULL) / bytes_per_second);
}

static uint32_t media_sink_audio_latency_target_ms(uint32_t source_packet_ms)
{
    if (source_packet_ms > 0U && source_packet_ms <= MEDIA_SINK_AUDIO_REALTIME_PACKET_MAX_MS) {
        uint32_t target_ms = MEDIA_SINK_AUDIO_REALTIME_TARGET_MS;
        uint32_t packet_floor_ms = source_packet_ms + MEDIA_SINK_AUDIO_PLAY_CHUNK_MS;

        if (target_ms < packet_floor_ms) {
            target_ms = packet_floor_ms;
        }
        return target_ms + s_audio_jitter_boost_ms;
    }
    return MEDIA_SINK_AUDIO_BULK_TARGET_MS;
}

static uint32_t media_sink_audio_prebuffer_ms(uint32_t source_packet_ms)
{
    if (source_packet_ms > 0U && source_packet_ms <= MEDIA_SINK_AUDIO_REALTIME_PACKET_MAX_MS) {
        return MEDIA_SINK_AUDIO_REALTIME_PREBUFFER_MS;
    }
    return MEDIA_SINK_AUDIO_BULK_PREBUFFER_MS;
}

static void media_sink_set_last_source_packet_ms(uint32_t source_packet_ms)
{
    taskENTER_CRITICAL(&s_sink_lock);
    s_audio_last_source_packet_ms = source_packet_ms;
    taskEXIT_CRITICAL(&s_sink_lock);
}

static uint32_t media_sink_get_last_source_packet_ms(void)
{
    uint32_t source_packet_ms = 0;

    taskENTER_CRITICAL(&s_sink_lock);
    source_packet_ms = s_audio_last_source_packet_ms;
    taskEXIT_CRITICAL(&s_sink_lock);
    return source_packet_ms;
}

static void media_sink_audio_note_underflow(void)
{
    if (s_audio_jitter_boost_ms < MEDIA_SINK_AUDIO_JITTER_BOOST_MAX_MS) {
        s_audio_jitter_boost_ms += MEDIA_SINK_AUDIO_JITTER_BOOST_STEP_MS;
        if (s_audio_jitter_boost_ms > MEDIA_SINK_AUDIO_JITTER_BOOST_MAX_MS) {
            s_audio_jitter_boost_ms = MEDIA_SINK_AUDIO_JITTER_BOOST_MAX_MS;
        }
    }
}

static void media_sink_audio_decay_jitter_boost(void)
{
    if (s_audio_jitter_boost_ms >= MEDIA_SINK_AUDIO_JITTER_BOOST_STEP_MS) {
        s_audio_jitter_boost_ms -= MEDIA_SINK_AUDIO_JITTER_BOOST_STEP_MS;
    } else {
        s_audio_jitter_boost_ms = 0;
    }
}

static uint8_t *media_sink_alloc_audio_buffer(size_t size)
{
    uint8_t *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = malloc(size);
    }
    return buffer;
}

static esp_err_t media_sink_ensure_audio_playback_buffers(void)
{
    const audio_format_t *playback_format = speaker_get_playback_format();
    const size_t pcm_buffer_size =
        media_sink_audio_bytes_for_duration_ms(MEDIA_SINK_AUDIO_PCM_BUFFER_MS,
                                                        playback_format);
    const size_t chunk_buffer_size =
        media_sink_audio_bytes_for_duration_ms(MEDIA_SINK_AUDIO_PLAY_CHUNK_MS,
                                                        playback_format);

    if (pcm_buffer_size == 0 || chunk_buffer_size == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (s_audio_pcm_buffer == NULL) {
        s_audio_pcm_buffer = media_sink_alloc_audio_buffer(pcm_buffer_size);
        ESP_RETURN_ON_FALSE(s_audio_pcm_buffer != NULL, ESP_ERR_NO_MEM, TAG, "audio pcm buffer alloc failed");
        s_audio_pcm_buffer_size = pcm_buffer_size;
    }

    if (s_audio_pcm_chunk_buffer == NULL) {
        s_audio_pcm_chunk_buffer = media_sink_alloc_audio_buffer(chunk_buffer_size);
        ESP_RETURN_ON_FALSE(s_audio_pcm_chunk_buffer != NULL,
                            ESP_ERR_NO_MEM,
                            TAG,
                            "audio chunk buffer alloc failed");
        s_audio_pcm_chunk_buffer_size = chunk_buffer_size;
    }

    return ESP_OK;
}

static void media_sink_reset_audio_pcm_buffer(void)
{
    taskENTER_CRITICAL(&s_sink_lock);
    s_audio_pcm_read_offset = 0;
    s_audio_pcm_used_bytes = 0;
    taskEXIT_CRITICAL(&s_sink_lock);
}

static size_t media_sink_get_audio_pcm_used_bytes(void)
{
    size_t used_bytes = 0;

    taskENTER_CRITICAL(&s_sink_lock);
    used_bytes = s_audio_pcm_used_bytes;
    taskEXIT_CRITICAL(&s_sink_lock);
    return used_bytes;
}

static void media_sink_note_trimmed_audio_ms(uint32_t dropped_ms, uint32_t buffered_ms)
{
    TickType_t now = 0;

    if (dropped_ms == 0) {
        return;
    }

    s_audio_trim_drop_ms_in_window += dropped_ms;
    s_audio_trim_drop_packets_in_window +=
        (dropped_ms + MEDIA_SINK_AUDIO_PLAY_CHUNK_MS - 1U) / MEDIA_SINK_AUDIO_PLAY_CHUNK_MS;

    now = xTaskGetTickCount();
    if (s_last_audio_trim_log_tick == 0 || now - s_last_audio_trim_log_tick >= pdMS_TO_TICKS(1000)) {
        s_last_audio_trim_log_tick = now;
        ESP_LOGW(TAG,
                 "remote audio buffer trimmed: dropped_ms=%u buffered_ms=%u queued=%u",
                 (unsigned)dropped_ms,
                 (unsigned)buffered_ms,
                 (unsigned)uxQueueMessagesWaiting(s_audio_queue));
    }
}

static uint32_t media_sink_drop_audio_pcm_head(size_t drop_bytes)
{
    const audio_format_t *playback_format = speaker_get_playback_format();

    if (playback_format == NULL || s_audio_pcm_buffer_size == 0 || s_audio_pcm_used_bytes == 0 || drop_bytes == 0) {
        return 0;
    }

    if (drop_bytes > s_audio_pcm_used_bytes) {
        drop_bytes = s_audio_pcm_used_bytes;
    }

    s_audio_pcm_read_offset = (s_audio_pcm_read_offset + drop_bytes) % s_audio_pcm_buffer_size;
    s_audio_pcm_used_bytes -= drop_bytes;
    return media_sink_audio_duration_ms_for_bytes(drop_bytes, playback_format);
}

static esp_err_t media_sink_append_audio_pcm(const uint8_t *data,
                                             size_t data_len,
                                             uint32_t source_packet_ms)
{
    const audio_format_t *playback_format = speaker_get_playback_format();
    const size_t play_chunk_bytes =
        media_sink_audio_bytes_for_duration_ms(MEDIA_SINK_AUDIO_PLAY_CHUNK_MS,
                                                         playback_format);
    const size_t latency_target_bytes =
        media_sink_audio_bytes_for_duration_ms(media_sink_audio_latency_target_ms(source_packet_ms),
                                                        playback_format);
    const uint8_t *write_data = data;
    uint32_t trimmed_ms = 0;
    uint32_t buffered_ms = 0;

    ESP_RETURN_ON_FALSE(data != NULL && data_len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid audio pcm append");
    ESP_RETURN_ON_ERROR(media_sink_ensure_audio_playback_buffers(), TAG, "audio pcm buffer prepare failed");

    taskENTER_CRITICAL(&s_sink_lock);

    if (data_len > s_audio_pcm_buffer_size) {
        size_t trim_bytes = data_len - s_audio_pcm_buffer_size;

        write_data += trim_bytes;
        data_len = s_audio_pcm_buffer_size;
        trimmed_ms += media_sink_audio_duration_ms_for_bytes(trim_bytes, playback_format);
    }

    if (data_len > (s_audio_pcm_buffer_size - s_audio_pcm_used_bytes)) {
        size_t drop_bytes = data_len - (s_audio_pcm_buffer_size - s_audio_pcm_used_bytes);
        if (play_chunk_bytes > 0 && (drop_bytes % play_chunk_bytes) != 0) {
            drop_bytes += play_chunk_bytes - (drop_bytes % play_chunk_bytes);
        }
        trimmed_ms += media_sink_drop_audio_pcm_head(drop_bytes);
    }

    size_t write_offset = (s_audio_pcm_read_offset + s_audio_pcm_used_bytes) % s_audio_pcm_buffer_size;
    size_t first_copy = s_audio_pcm_buffer_size - write_offset;
    if (first_copy > data_len) {
        first_copy = data_len;
    }
    memcpy(s_audio_pcm_buffer + write_offset, write_data, first_copy);
    if (data_len > first_copy) {
        memcpy(s_audio_pcm_buffer, write_data + first_copy, data_len - first_copy);
    }
    s_audio_pcm_used_bytes += data_len;
    if (source_packet_ms > 0U) {
        s_audio_last_source_packet_ms = source_packet_ms;
    }

    if (latency_target_bytes > 0U && s_audio_pcm_used_bytes > latency_target_bytes) {
        size_t drop_bytes = s_audio_pcm_used_bytes - latency_target_bytes;
        if (play_chunk_bytes > 0 && (drop_bytes % play_chunk_bytes) != 0) {
            drop_bytes += play_chunk_bytes - (drop_bytes % play_chunk_bytes);
        }
        trimmed_ms += media_sink_drop_audio_pcm_head(drop_bytes);
    }

    buffered_ms = media_sink_audio_duration_ms_for_bytes(s_audio_pcm_used_bytes, playback_format);

    taskEXIT_CRITICAL(&s_sink_lock);

    media_sink_note_trimmed_audio_ms(trimmed_ms, buffered_ms);
    return ESP_OK;
}

static esp_err_t media_sink_pop_audio_pcm_chunk(size_t chunk_bytes, uint8_t **chunk_data)
{
    ESP_RETURN_ON_FALSE(chunk_data != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid audio chunk output");
    ESP_RETURN_ON_ERROR(media_sink_ensure_audio_playback_buffers(), TAG, "audio chunk buffer prepare failed");
    ESP_RETURN_ON_FALSE(chunk_bytes <= s_audio_pcm_chunk_buffer_size,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "audio chunk too large");

    taskENTER_CRITICAL(&s_sink_lock);
    if (chunk_bytes > s_audio_pcm_used_bytes) {
        taskEXIT_CRITICAL(&s_sink_lock);
        return ESP_ERR_NOT_FOUND;
    }

    size_t first_copy = s_audio_pcm_buffer_size - s_audio_pcm_read_offset;
    if (first_copy > chunk_bytes) {
        first_copy = chunk_bytes;
    }
    memcpy(s_audio_pcm_chunk_buffer, s_audio_pcm_buffer + s_audio_pcm_read_offset, first_copy);
    if (chunk_bytes > first_copy) {
        memcpy(s_audio_pcm_chunk_buffer + first_copy, s_audio_pcm_buffer, chunk_bytes - first_copy);
    }

    s_audio_pcm_read_offset = (s_audio_pcm_read_offset + chunk_bytes) % s_audio_pcm_buffer_size;
    s_audio_pcm_used_bytes -= chunk_bytes;
    taskEXIT_CRITICAL(&s_sink_lock);

    *chunk_data = s_audio_pcm_chunk_buffer;
    return ESP_OK;
}

static uint32_t media_sink_audio_level_percent(const int16_t *samples, size_t data_len)
{
    uint32_t playback_peak = 0;
    size_t sample_count = data_len / sizeof(int16_t);

    if (samples == NULL || sample_count == 0) {
        return 0;
    }

    for (size_t index = 0; index < sample_count; ++index) {
        uint32_t abs_value = (uint32_t)abs(samples[index]);
        if (abs_value > playback_peak) {
            playback_peak = abs_value;
        }
    }

    return (playback_peak * 100U) / 32767U;
}

static uint32_t media_sink_get_generation(void)
{
    uint32_t generation = 0;

    taskENTER_CRITICAL(&s_sink_lock);
    generation = s_generation;
    taskEXIT_CRITICAL(&s_sink_lock);
    return generation;
}

static void media_sink_free_audio_packet(media_sink_audio_packet_t *packet)
{
    if (packet == NULL) {
        return;
    }

    free(packet->data);
    memset(packet, 0, sizeof(*packet));
}

static uint32_t media_sink_audio_packet_duration_us(const media_sink_audio_packet_t *packet)
{
    if (packet == NULL || packet->format.bits_per_sample == 0 || packet->format.channels == 0 ||
        packet->format.sample_rate_hz == 0) {
        return 0;
    }

    size_t bytes_per_frame = ((size_t)packet->format.bits_per_sample / 8U) * packet->format.channels;
    if (bytes_per_frame == 0) {
        return 0;
    }

    size_t frame_count = packet->data_len / bytes_per_frame;
    if (frame_count == 0) {
        return 0;
    }

    return (uint32_t)(((uint64_t)frame_count * 1000000ULL) / packet->format.sample_rate_hz);
}

static void media_sink_maybe_log_audio_enqueue(const media_sink_audio_packet_t *packet)
{
    TickType_t now = 0;
    bool should_log = false;
    bool first_log = false;
    uint32_t packet_ms = 0;
    UBaseType_t queued_packets = 0;

    if (packet == NULL || s_audio_queue == NULL) {
        return;
    }

    now = xTaskGetTickCount();
    queued_packets = uxQueueMessagesWaiting(s_audio_queue);
    packet_ms = media_sink_audio_packet_duration_us(packet) / 1000U;
    if (!s_remote_audio_enqueue_logged) {
        s_remote_audio_enqueue_logged = true;
        first_log = true;
        should_log = true;
    } else if (s_last_audio_enqueue_log_tick == 0 ||
               now - s_last_audio_enqueue_log_tick >= pdMS_TO_TICKS(1000)) {
        should_log = true;
    }

    if (!should_log) {
        return;
    }

    s_last_audio_enqueue_log_tick = now;
    if (first_log) {
        ESP_LOGI(TAG,
                 "remote audio queued: packet_ms=%u bytes=%u format=%luHz/%ubit/%uch queued=%u buffered_ms=%u",
                 (unsigned)packet_ms,
                 (unsigned)packet->data_len,
                 (unsigned long)packet->format.sample_rate_hz,
                 packet->format.bits_per_sample,
                 packet->format.channels,
                 (unsigned)queued_packets,
                 (unsigned)media_sink_audio_duration_ms_for_bytes(
                     media_sink_get_audio_pcm_used_bytes(), speaker_get_playback_format()));
    } else {
        ESP_LOGD(TAG,
                 "remote audio queued: packet_ms=%u bytes=%u format=%luHz/%ubit/%uch queued=%u buffered_ms=%u",
                 (unsigned)packet_ms,
                 (unsigned)packet->data_len,
                 (unsigned long)packet->format.sample_rate_hz,
                 packet->format.bits_per_sample,
                 packet->format.channels,
                 (unsigned)queued_packets,
                 (unsigned)media_sink_audio_duration_ms_for_bytes(
                     media_sink_get_audio_pcm_used_bytes(), speaker_get_playback_format()));
    }
}

static void media_sink_drop_oldest_audio(void)
{
    media_sink_audio_packet_t stale = {0};

    if (s_audio_queue != NULL && xQueueReceive(s_audio_queue, &stale, 0) == pdTRUE) {
        media_sink_free_audio_packet(&stale);
    }
}

static void media_sink_trim_audio_backlog(void)
{
    if (s_audio_queue == NULL) {
        return;
    }

    UBaseType_t queued_packets = uxQueueMessagesWaiting(s_audio_queue);
    if (queued_packets < MEDIA_SINK_AUDIO_BACKLOG_TRIM_THRESHOLD) {
        return;
    }

    uint32_t dropped_packets = 0;
    uint32_t dropped_ms = 0;
    while (queued_packets > MEDIA_SINK_AUDIO_BACKLOG_TARGET_PACKETS) {
        media_sink_audio_packet_t stale = {0};

        if (s_audio_queue != NULL && xQueueReceive(s_audio_queue, &stale, 0) == pdTRUE) {
            dropped_ms += media_sink_audio_packet_duration_us(&stale) / 1000U;
            media_sink_free_audio_packet(&stale);
            dropped_packets++;
        }
        queued_packets = uxQueueMessagesWaiting(s_audio_queue);
    }

    s_audio_trim_drop_packets_in_window += dropped_packets;
    s_audio_trim_drop_ms_in_window += dropped_ms;

    TickType_t now = xTaskGetTickCount();
    if (dropped_packets > 0 &&
        (s_last_audio_trim_log_tick == 0 || now - s_last_audio_trim_log_tick >= pdMS_TO_TICKS(1000))) {
        const audio_format_t *playback_format = speaker_get_playback_format();

        s_last_audio_trim_log_tick = now;
        ESP_LOGW(TAG,
                 "remote audio backlog trimmed: dropped_packets=%u queued=%u buffered_ms=%u",
                 (unsigned)dropped_packets,
                 (unsigned)queued_packets,
                 (unsigned)media_sink_audio_duration_ms_for_bytes(
                     media_sink_get_audio_pcm_used_bytes(), playback_format));
    }
}

static void media_sink_maybe_log_audio_rate(void)
{
    TickType_t now = xTaskGetTickCount();

    if (s_last_audio_rate_log_tick == 0) {
        s_last_audio_rate_log_tick = now;
        return;
    }
    if (now - s_last_audio_rate_log_tick < pdMS_TO_TICKS(1000)) {
        return;
    }

    if (s_audio_play_drop_packets_in_window > 0 || s_audio_trim_drop_packets_in_window > 0) {
        ESP_LOGW(TAG,
                 "remote audio pressure: rx=%up/%ums played=%up/%ums play_drop=%up/%ums trim_drop=%up/%ums queued=%u buffered_ms=%u jitter_boost_ms=%u",
                 (unsigned)s_audio_rx_packets_in_window,
                 (unsigned)s_audio_rx_ms_in_window,
                 (unsigned)s_audio_play_ok_packets_in_window,
                 (unsigned)s_audio_play_ok_ms_in_window,
                 (unsigned)s_audio_play_drop_packets_in_window,
                 (unsigned)s_audio_play_drop_ms_in_window,
                 (unsigned)s_audio_trim_drop_packets_in_window,
                 (unsigned)s_audio_trim_drop_ms_in_window,
                 (unsigned)uxQueueMessagesWaiting(s_audio_queue),
                 (unsigned)media_sink_audio_duration_ms_for_bytes(
                     media_sink_get_audio_pcm_used_bytes(), speaker_get_playback_format()),
                 (unsigned)s_audio_jitter_boost_ms);
        if (s_audio_play_drop_packets_in_window == 0) {
            media_sink_audio_decay_jitter_boost();
        }
    } else if (s_audio_rx_packets_in_window > 0 && s_audio_play_ok_packets_in_window == 0) {
        ESP_LOGI(TAG,
                 "remote audio buffering: rx=%up/%ums queued=%u buffered_ms=%u prebuffer_ms=%u",
                 (unsigned)s_audio_rx_packets_in_window,
                 (unsigned)s_audio_rx_ms_in_window,
                 (unsigned)uxQueueMessagesWaiting(s_audio_queue),
                 (unsigned)media_sink_audio_duration_ms_for_bytes(
                     media_sink_get_audio_pcm_used_bytes(), speaker_get_playback_format()),
                 (unsigned)media_sink_audio_prebuffer_ms(media_sink_get_last_source_packet_ms()));
    } else {
        media_sink_audio_decay_jitter_boost();
        ESP_LOGD(TAG,
                 "remote audio steady: rx=%up/%ums played=%up/%ums queued=%u buffered_ms=%u jitter_boost_ms=%u",
                 (unsigned)s_audio_rx_packets_in_window,
                 (unsigned)s_audio_rx_ms_in_window,
                 (unsigned)s_audio_play_ok_packets_in_window,
                 (unsigned)s_audio_play_ok_ms_in_window,
                 (unsigned)uxQueueMessagesWaiting(s_audio_queue),
                 (unsigned)media_sink_audio_duration_ms_for_bytes(
                     media_sink_get_audio_pcm_used_bytes(), speaker_get_playback_format()),
                 (unsigned)s_audio_jitter_boost_ms);
    }

    s_audio_rx_packets_in_window = 0;
    s_audio_rx_ms_in_window = 0;
    s_audio_play_ok_packets_in_window = 0;
    s_audio_play_ok_ms_in_window = 0;
    s_audio_play_drop_packets_in_window = 0;
    s_audio_play_drop_ms_in_window = 0;
    s_audio_trim_drop_packets_in_window = 0;
    s_audio_trim_drop_ms_in_window = 0;
    s_last_audio_rate_log_tick = now;
}

static esp_err_t media_sink_queue_audio(media_sink_audio_packet_t *packet)
{
    if (packet == NULL || s_audio_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    media_sink_trim_audio_backlog();

    if (xQueueSend(s_audio_queue, packet, 0) == pdTRUE) {
        memset(packet, 0, sizeof(*packet));
        return ESP_OK;
    }

    ESP_LOGD(TAG, "remote audio queue full: dropped oldest packet");
    media_sink_drop_oldest_audio();
    if (xQueueSend(s_audio_queue, packet, 0) == pdTRUE) {
        memset(packet, 0, sizeof(*packet));
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}

static void media_sink_buffer_audio_packet(media_sink_audio_packet_t *packet)
{
    if (packet == NULL) {
        return;
    }

    if (packet->generation != media_sink_get_generation()) {
        media_sink_free_audio_packet(packet);
        return;
    }

    uint32_t packet_duration_us = media_sink_audio_packet_duration_us(packet);
    int16_t *rendered_samples = NULL;
    size_t rendered_bytes = 0;
    uint32_t output_level = 0;
    esp_err_t render_ret = speaker_render_pcm(packet->data,
                                                              packet->data_len,
                                                              &packet->format,
                                                              &rendered_samples,
                                                              &rendered_bytes,
                                                              &output_level);
    if (render_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "remote audio render failed: ret=%d rate=%lu bits=%u ch=%u bytes=%u",
                 render_ret,
                 (unsigned long)packet->format.sample_rate_hz,
                 packet->format.bits_per_sample,
                 packet->format.channels,
                 (unsigned)packet->data_len);
        media_sink_free_audio_packet(packet);
        return;
    }

    while (uxQueueMessagesWaiting(s_audio_queue) >= MEDIA_SINK_AUDIO_BACKLOG_TRIM_THRESHOLD) {
        media_sink_trim_audio_backlog();
    }

    esp_err_t append_ret = media_sink_append_audio_pcm((const uint8_t *)rendered_samples,
                                                                rendered_bytes,
                                                                packet_duration_us / 1000U);
    TickType_t now = xTaskGetTickCount();
    if (!s_remote_audio_render_logged) {
        s_remote_audio_render_logged = true;
        s_last_audio_render_log_tick = now;
        ESP_LOGI(TAG,
                 "remote audio rendered: input_bytes=%u rendered_bytes=%u packet_ms=%lu level=%u queued=%u buffered_ms=%u",
                 (unsigned)packet->data_len,
                 (unsigned)rendered_bytes,
                 (unsigned long)(packet_duration_us / 1000U),
                 (unsigned)output_level,
                 (unsigned)uxQueueMessagesWaiting(s_audio_queue),
                 (unsigned)media_sink_audio_duration_ms_for_bytes(
                     media_sink_get_audio_pcm_used_bytes(), speaker_get_playback_format()));
    } else if (s_last_audio_render_log_tick == 0 ||
               now - s_last_audio_render_log_tick >= pdMS_TO_TICKS(1000)) {
        s_last_audio_render_log_tick = now;
        ESP_LOGD(TAG,
                 "remote audio render steady: input_bytes=%u rendered_bytes=%u packet_ms=%lu level=%u queued=%u buffered_ms=%u",
                 (unsigned)packet->data_len,
                 (unsigned)rendered_bytes,
                 (unsigned long)(packet_duration_us / 1000U),
                 (unsigned)output_level,
                 (unsigned)uxQueueMessagesWaiting(s_audio_queue),
                 (unsigned)media_sink_audio_duration_ms_for_bytes(
                     media_sink_get_audio_pcm_used_bytes(), speaker_get_playback_format()));
    }
    if (append_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "remote audio pcm append failed: ret=%d rendered_bytes=%u input_ms=%lu",
                 append_ret,
                 (unsigned)rendered_bytes,
                 (unsigned long)(packet_duration_us / 1000U));
    }

    media_sink_free_audio_packet(packet);
}

static void media_sink_audio_task(void *ctx)
{
    (void)ctx;
    const audio_format_t *playback_format = speaker_get_playback_format();
    const size_t play_chunk_bytes =
        media_sink_audio_bytes_for_duration_ms(MEDIA_SINK_AUDIO_PLAY_CHUNK_MS,
                                                        playback_format);
    bool playback_started = false;
    media_sink_audio_packet_t packet = {0};

    while (true) {
        BaseType_t received_packet = xQueueReceive(s_audio_queue,
                                                   &packet,
                                                   playback_started ? 0 : portMAX_DELAY);
        size_t buffered_bytes = 0;

        if (received_packet == pdTRUE) {
#if MEDIA_SINK_AUDIO_DRAIN_BURST_MAX > 0
            size_t drained_packets = 0;

            media_sink_buffer_audio_packet(&packet);
            while (drained_packets < MEDIA_SINK_AUDIO_DRAIN_BURST_MAX &&
                   xQueueReceive(s_audio_queue, &packet, 0) == pdTRUE) {
                media_sink_buffer_audio_packet(&packet);
                drained_packets++;
            }
#else
            media_sink_buffer_audio_packet(&packet);
#endif
        }

        buffered_bytes = media_sink_get_audio_pcm_used_bytes();
        uint32_t source_packet_ms = media_sink_get_last_source_packet_ms();
        uint32_t prebuffer_ms = media_sink_audio_prebuffer_ms(source_packet_ms);
        size_t prebuffer_bytes =
            media_sink_audio_bytes_for_duration_ms(prebuffer_ms, playback_format);
        if (prebuffer_bytes == 0U) {
            prebuffer_bytes = play_chunk_bytes;
        }

        if (!playback_started && buffered_bytes >= prebuffer_bytes) {
            playback_started = true;
        }

        if (playback_started && buffered_bytes < play_chunk_bytes) {
            playback_started = false;
            media_sink_audio_note_underflow();
        }

        if (playback_started && buffered_bytes >= play_chunk_bytes) {
            uint8_t *play_chunk = NULL;
            uint32_t buffered_ms_before_play =
                media_sink_audio_duration_ms_for_bytes(buffered_bytes, playback_format);
            uint32_t output_level = 0;
            int64_t play_start_us = 0;
            int64_t play_elapsed_us = 0;
            esp_err_t play_ret = media_sink_pop_audio_pcm_chunk(play_chunk_bytes, &play_chunk);

            if (play_ret != ESP_OK) {
                playback_started = false;
                continue;
            }

            output_level = media_sink_audio_level_percent((const int16_t *)play_chunk, play_chunk_bytes);
            play_start_us = esp_timer_get_time();
            play_ret = speaker_write_rendered_pcm((int16_t *)play_chunk,
                                                              play_chunk_bytes,
                                                              output_level);
            play_elapsed_us = esp_timer_get_time() - play_start_us;

            audio_playback_timing_t playback_timing = {0};
            speaker_get_last_playback_timing(&playback_timing);
            if (play_ret == ESP_ERR_TIMEOUT) {
                s_audio_play_drop_packets_in_window++;
                s_audio_play_drop_ms_in_window += MEDIA_SINK_AUDIO_PLAY_CHUNK_MS;
                TickType_t now = xTaskGetTickCount();
                if (s_last_audio_write_drop_log_tick == 0 ||
                    now - s_last_audio_write_drop_log_tick >= pdMS_TO_TICKS(1000)) {
                    s_last_audio_write_drop_log_tick = now;
                    ESP_LOGW(TAG,
                             "remote audio playback drop: i2s_busy chunk_ms=%u buffered_ms=%u queued=%u",
                             MEDIA_SINK_AUDIO_PLAY_CHUNK_MS,
                             (unsigned)buffered_ms_before_play,
                             (unsigned)uxQueueMessagesWaiting(s_audio_queue));
                }
            } else if (play_ret != ESP_OK) {
                ESP_LOGW(TAG,
                         "remote audio playback failed: ret=%d chunk_ms=%u buffered_ms=%u",
                         play_ret,
                         MEDIA_SINK_AUDIO_PLAY_CHUNK_MS,
                         (unsigned)buffered_ms_before_play);
                playback_started = false;
            } else {
                if (!s_remote_audio_playback_started_logged) {
                    s_remote_audio_playback_started_logged = true;
                    ESP_LOGI(TAG,
                             "remote audio playback started: prebuffer_ms=%u chunk_ms=%u buffered_ms=%u level=%u",
                             (unsigned)prebuffer_ms,
                             MEDIA_SINK_AUDIO_PLAY_CHUNK_MS,
                             (unsigned)buffered_ms_before_play,
                             (unsigned)output_level);
                }
                s_audio_play_ok_packets_in_window++;
                s_audio_play_ok_ms_in_window += MEDIA_SINK_AUDIO_PLAY_CHUNK_MS;
            }

            if (play_ret == ESP_OK && play_elapsed_us > MEDIA_SINK_AUDIO_SLOW_PLAY_US) {
                TickType_t now = xTaskGetTickCount();
                if (s_last_audio_slow_log_tick == 0 || now - s_last_audio_slow_log_tick >= pdMS_TO_TICKS(1000)) {
                    s_last_audio_slow_log_tick = now;
                    ESP_LOGD(TAG,
                             "remote audio playback slow elapsed_ms=%lu prepare_ms=%lu write_ms=%lu bytes=%lu chunk_ms=%u buffered_ms=%u queued=%u",
                             (unsigned long)(play_elapsed_us / 1000ULL),
                             (unsigned long)playback_timing.prepare_ms,
                             (unsigned long)playback_timing.write_ms,
                             (unsigned long)playback_timing.data_bytes,
                             MEDIA_SINK_AUDIO_PLAY_CHUNK_MS,
                             (unsigned)buffered_ms_before_play,
                             (unsigned)uxQueueMessagesWaiting(s_audio_queue));
                }
            }

            media_sink_maybe_log_audio_rate();
            continue;
        }

        media_sink_maybe_log_audio_rate();
    }
}

esp_err_t media_sink_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_audio_queue = xQueueCreateWithCaps(MEDIA_SINK_AUDIO_QUEUE_LEN,
                                         sizeof(media_sink_audio_packet_t),
                                         APP_QUEUE_CAPS_BACKGROUND);
    if (s_audio_queue == NULL) {
        s_audio_queue = xQueueCreate(MEDIA_SINK_AUDIO_QUEUE_LEN, sizeof(media_sink_audio_packet_t));
    }
    ESP_RETURN_ON_FALSE(s_audio_queue != NULL, ESP_ERR_NO_MEM, TAG, "audio queue alloc failed");

    BaseType_t audio_ok = xTaskCreatePinnedToCoreWithCaps(media_sink_audio_task,
                                                          "media_audio_rx",
                                                          MEDIA_SINK_AUDIO_TASK_STACK,
                                                          NULL,
                                                          MEDIA_SINK_AUDIO_TASK_PRIORITY,
                                                          &s_audio_task,
                                                          MEDIA_SINK_AUDIO_TASK_CORE,
                                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(audio_ok == pdPASS, ESP_ERR_NO_MEM, TAG, "audio task create failed");

    s_initialized = true;
    return ESP_OK;
}

void media_sink_get_stats(media_sink_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    memset(stats, 0, sizeof(*stats));
    stats->initialized = s_initialized;
    stats->audio_queue_capacity = MEDIA_SINK_AUDIO_QUEUE_LEN;
    stats->audio_queue_len = s_audio_queue != NULL ? (uint32_t)uxQueueMessagesWaiting(s_audio_queue) : 0U;

    const audio_format_t *playback_format = speaker_get_playback_format();
    taskENTER_CRITICAL(&s_sink_lock);
    stats->audio_pcm_used_bytes = s_audio_pcm_used_bytes;
    stats->audio_pcm_capacity = s_audio_pcm_buffer_size;
    stats->audio_jitter_boost_ms = s_audio_jitter_boost_ms;
    stats->audio_buffered_ms = media_sink_audio_duration_ms_for_bytes(s_audio_pcm_used_bytes, playback_format);
    taskEXIT_CRITICAL(&s_sink_lock);
}

static esp_err_t media_sink_submit_audio_packet(media_sink_audio_packet_t *packet)
{
    esp_err_t ret = media_sink_queue_audio(packet);
    if (ret != ESP_OK) {
        media_sink_free_audio_packet(packet);
    }
    return ret;
}

esp_err_t media_sink_submit_remote_audio(const uint8_t *data,
                                                  size_t data_len,
                                                  const audio_format_t *format)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "media sink not initialized");
    ESP_RETURN_ON_FALSE(data != NULL && data_len > 0 && format != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid audio packet");

    media_sink_audio_packet_t packet = {
        .generation = media_sink_get_generation(),
        .format = *format,
        .data_len = data_len,
    };
    packet.data = heap_caps_malloc(data_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (packet.data == NULL) {
        packet.data = heap_caps_malloc(data_len, MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(packet.data != NULL, ESP_ERR_NO_MEM, TAG, "audio packet alloc failed");
    memcpy(packet.data, data, data_len);

    s_audio_rx_packets_in_window++;
    s_audio_rx_ms_in_window += media_sink_audio_packet_duration_us(&packet) / 1000U;
    media_sink_maybe_log_audio_enqueue(&packet);

    return media_sink_submit_audio_packet(&packet);
}

esp_err_t media_sink_submit_remote_audio_owned(uint8_t *data,
                                                        size_t data_len,
                                                        const audio_format_t *format)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "media sink not initialized");
    ESP_RETURN_ON_FALSE(data != NULL && data_len > 0 && format != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid audio packet");

    media_sink_audio_packet_t packet = {
        .generation = media_sink_get_generation(),
        .format = *format,
        .data = data,
        .data_len = data_len,
    };

    s_audio_rx_packets_in_window++;
    s_audio_rx_ms_in_window += media_sink_audio_packet_duration_us(&packet) / 1000U;
    media_sink_maybe_log_audio_enqueue(&packet);

    return media_sink_submit_audio_packet(&packet);
}

esp_err_t media_sink_submit_remote_video_jpeg(const uint8_t *data, size_t data_len)
{
    (void)data;
    (void)data_len;
    return s_initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void media_sink_flush(void)
{
    if (!s_initialized) {
        return;
    }

    taskENTER_CRITICAL(&s_sink_lock);
    s_generation++;
    s_remote_audio_playback_started_logged = false;
    taskEXIT_CRITICAL(&s_sink_lock);

    s_remote_audio_enqueue_logged = false;
    s_remote_audio_render_logged = false;
    s_last_audio_enqueue_log_tick = 0;
    s_last_audio_render_log_tick = 0;
    s_audio_jitter_boost_ms = 0;
    media_sink_set_last_source_packet_ms(0);

    speaker_stop_playback();
    media_sink_reset_audio_pcm_buffer();

    media_sink_audio_packet_t audio_packet = {0};
    while (s_audio_queue != NULL && xQueueReceive(s_audio_queue, &audio_packet, 0) == pdTRUE) {
        media_sink_free_audio_packet(&audio_packet);
    }
}
