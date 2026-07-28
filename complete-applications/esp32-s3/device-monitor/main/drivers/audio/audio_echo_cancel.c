#include "audio_echo_cancel.h"

#include <stdbool.h>
#include <string.h>

#include "app_config.h"

#if APP_CONFIG_AUDIO_AEC_ENABLE

#include "esp_aec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

/*
 * ESP-SR AEC adapter for the app PCM path.
 *
 * The capture task uploads 16 kHz mono PCM in 20 ms frames, while ESP-SR AEC
 * consumes 32 ms frames. This adapter keeps that mismatch contained here:
 * playback is stored as a delayed reference, capture samples are accumulated
 * into ESP-SR sized frames, and processed output is returned through a small
 * PSRAM FIFO.
 */

#define AUDIO_AEC_SAMPLE_RATE_HZ              16000U
#define AUDIO_AEC_REF_RING_SAMPLES            8192U
#define AUDIO_AEC_REF_RING_MASK               (AUDIO_AEC_REF_RING_SAMPLES - 1U)
#define AUDIO_AEC_OUT_FIFO_SAMPLES            2048U
#define AUDIO_AEC_OUT_FIFO_MASK               (AUDIO_AEC_OUT_FIFO_SAMPLES - 1U)
#define AUDIO_AEC_REF_DELAY_MS                80U
#define AUDIO_AEC_REF_DELAY_SAMPLES           ((AUDIO_AEC_SAMPLE_RATE_HZ * AUDIO_AEC_REF_DELAY_MS) / 1000U)
#define AUDIO_AEC_REF_ACTIVE_US               500000LL
#define AUDIO_AEC_REF_ACTIVE_PEAK             64U
#define AUDIO_AEC_PROFILE_NAME                "max-fd-veryaggr"
#define AUDIO_AEC_FILTER_LENGTH               4
#define AUDIO_AEC_FALLBACK_FILTER_LENGTH      2
#define AUDIO_AEC_BUFFER_ALIGNMENT            16U
#define AUDIO_AEC_PSRAM_CAPS                  (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define AUDIO_AEC_DEINIT_WAIT_MS              60U

static const char *TAG = "audio_aec";

static portMUX_TYPE s_aec_lock = portMUX_INITIALIZER_UNLOCKED;

static aec_handle_t *s_aec;
static int s_aec_frame_size;
static int16_t *s_ref_ring;
static int16_t *s_mic_frame;
static int16_t *s_ref_frame;
static int16_t *s_out_frame;
static int16_t *s_out_fifo;
static uint32_t s_ref_write_pos;
static uint32_t s_ref_filled_samples;
static uint32_t s_last_ref_peak;
static int64_t s_last_playback_us;
static uint32_t s_frame_fill;
static uint32_t s_out_fifo_read_pos;
static uint32_t s_out_fifo_used;
static bool s_out_fifo_ready;
static bool s_initializing;
static bool s_deinit_requested;
static bool s_create_failed_logged;
static uint32_t s_active_users;

static inline uint32_t audio_aec_abs_i16(int32_t value)
{
    return (uint32_t)(value < 0 ? -value : value);
}

static inline uint32_t audio_aec_ring_prev(uint32_t pos, uint32_t back)
{
    return (pos + AUDIO_AEC_REF_RING_SAMPLES - (back & AUDIO_AEC_REF_RING_MASK)) & AUDIO_AEC_REF_RING_MASK;
}

static void *audio_aec_aligned_calloc(size_t count, size_t element_size)
{
    void *buffer = heap_caps_aligned_calloc(AUDIO_AEC_BUFFER_ALIGNMENT,
                                            count,
                                            element_size,
                                            AUDIO_AEC_PSRAM_CAPS);
    if (buffer == NULL) {
        ESP_LOGW(TAG,
                 "AEC PSRAM allocation failed: count=%u size=%u",
                 (unsigned)count,
                 (unsigned)element_size);
    }
    return buffer;
}

static void audio_aec_log_heap(const char *stage)
{
    ESP_LOGD(TAG,
             "%s: internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             stage != NULL ? stage : "heap",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static aec_handle_t *audio_aec_create_handle(aec_mode_t mode, int filter_length)
{
    aec_config_t config = {
        .mic_num = 1,
        .ref_num = 1,
        .out_num = 1,
        .filter_length = filter_length,
        .sample_rate = (int)AUDIO_AEC_SAMPLE_RATE_HZ,
        .caps = AUDIO_AEC_PSRAM_CAPS,
        .mode = mode,
        .nlp_level = AEC_NLP_LEVEL_VERYAGGR,
    };
    aec_handle_t *handle = aec_create_from_config(&config);
    if (handle != NULL) {
        (void)aec_set_nlp_level(handle, AEC_NLP_LEVEL_VERYAGGR);
    }
    return handle;
}

static void audio_aec_free_handle_and_buffers(aec_handle_t *handle,
                                              int16_t *ref_ring,
                                              int16_t *mic_frame,
                                              int16_t *ref_frame,
                                              int16_t *out_frame,
                                              int16_t *out_fifo)
{
    if (handle != NULL) {
        aec_destroy(handle);
    }
    heap_caps_free(ref_ring);
    heap_caps_free(mic_frame);
    heap_caps_free(ref_frame);
    heap_caps_free(out_frame);
    heap_caps_free(out_fifo);
}

static bool audio_aec_ensure_ready(void)
{
    taskENTER_CRITICAL(&s_aec_lock);
    if (s_aec != NULL) {
        taskEXIT_CRITICAL(&s_aec_lock);
        return true;
    }
    if (s_initializing || s_deinit_requested) {
        taskEXIT_CRITICAL(&s_aec_lock);
        return false;
    }
    s_initializing = true;
    taskEXIT_CRITICAL(&s_aec_lock);

    aec_handle_t *handle = audio_aec_create_handle(AEC_MODE_FD_HIGH_PERF, AUDIO_AEC_FILTER_LENGTH);
    if (handle == NULL) {
        handle = audio_aec_create_handle(AEC_MODE_VOIP_HIGH_PERF, AUDIO_AEC_FILTER_LENGTH);
    }
    if (handle == NULL) {
        handle = audio_aec_create_handle(AEC_MODE_FD_LOW_COST, AUDIO_AEC_FALLBACK_FILTER_LENGTH);
    }

    int frame_size = handle == NULL ? 0 : aec_get_chunksize(handle);
    int16_t *ref_ring = NULL;
    int16_t *mic_frame = NULL;
    int16_t *ref_frame = NULL;
    int16_t *out_frame = NULL;
    int16_t *out_fifo = NULL;

    if (handle != NULL && frame_size > 0) {
        ref_ring = audio_aec_aligned_calloc(AUDIO_AEC_REF_RING_SAMPLES, sizeof(int16_t));
        mic_frame = audio_aec_aligned_calloc((size_t)frame_size, sizeof(int16_t));
        ref_frame = audio_aec_aligned_calloc((size_t)frame_size, sizeof(int16_t));
        out_frame = audio_aec_aligned_calloc((size_t)frame_size, sizeof(int16_t));
        out_fifo = audio_aec_aligned_calloc(AUDIO_AEC_OUT_FIFO_SAMPLES, sizeof(int16_t));
    }

    if (handle == NULL || frame_size <= 0 || ref_ring == NULL || mic_frame == NULL ||
        ref_frame == NULL || out_frame == NULL || out_fifo == NULL) {
        audio_aec_free_handle_and_buffers(handle, ref_ring, mic_frame, ref_frame, out_frame, out_fifo);
        taskENTER_CRITICAL(&s_aec_lock);
        s_initializing = false;
        taskEXIT_CRITICAL(&s_aec_lock);
        if (!s_create_failed_logged) {
            s_create_failed_logged = true;
            ESP_LOGW(TAG, "official ESP-SR AEC unavailable, capture continues without AEC");
        }
        return false;
    }

    taskENTER_CRITICAL(&s_aec_lock);
    if (s_deinit_requested) {
        s_initializing = false;
        taskEXIT_CRITICAL(&s_aec_lock);
        audio_aec_free_handle_and_buffers(handle, ref_ring, mic_frame, ref_frame, out_frame, out_fifo);
        return false;
    }
    s_aec = handle;
    s_aec_frame_size = frame_size;
    s_ref_ring = ref_ring;
    s_mic_frame = mic_frame;
    s_ref_frame = ref_frame;
    s_out_frame = out_frame;
    s_out_fifo = out_fifo;
    s_ref_write_pos = 0;
    s_ref_filled_samples = 0;
    s_last_ref_peak = 0;
    s_last_playback_us = 0;
    s_frame_fill = 0;
    s_out_fifo_read_pos = 0;
    s_out_fifo_used = 0;
    s_out_fifo_ready = false;
    s_initializing = false;
    taskEXIT_CRITICAL(&s_aec_lock);

    ESP_LOGI(TAG,
             "official ESP-SR AEC ready: profile=%s mode=%s nlp=%s frame=%d filter=%d ref_delay=%ums psram=%uB",
             AUDIO_AEC_PROFILE_NAME,
             aec_get_mode_string(handle->config.mode),
             aec_get_nlp_string(handle->config.nlp_level),
             frame_size,
             handle->config.filter_length,
             (unsigned)AUDIO_AEC_REF_DELAY_MS,
             (unsigned)((AUDIO_AEC_REF_RING_SAMPLES + AUDIO_AEC_OUT_FIFO_SAMPLES +
                         (uint32_t)frame_size * 3U) * sizeof(int16_t)));
    audio_aec_log_heap("AEC ready");
    return true;
}

static bool audio_aec_enter(void)
{
    bool ready = false;

    taskENTER_CRITICAL(&s_aec_lock);
    if (s_aec != NULL && !s_deinit_requested) {
        s_active_users++;
        ready = true;
    }
    taskEXIT_CRITICAL(&s_aec_lock);
    return ready;
}

static void audio_aec_leave(void)
{
    taskENTER_CRITICAL(&s_aec_lock);
    if (s_active_users > 0U) {
        s_active_users--;
    }
    taskEXIT_CRITICAL(&s_aec_lock);
}

bool audio_echo_cancel_preload(void)
{
    bool ready = audio_aec_ensure_ready();
    if (ready) {
        audio_echo_cancel_reset();
    }
    return ready;
}

static bool audio_aec_ref_snapshot(size_t sample_count,
                                   int16_t **ring,
                                   uint32_t *read_start,
                                   uint32_t *ref_peak)
{
    int16_t *ref_ring = NULL;
    uint32_t write_pos = 0;
    uint32_t filled_samples = 0;
    uint32_t last_ref_peak = 0;
    int64_t last_playback_us = 0;

    taskENTER_CRITICAL(&s_aec_lock);
    ref_ring = s_ref_ring;
    write_pos = s_ref_write_pos;
    filled_samples = s_ref_filled_samples;
    last_ref_peak = s_last_ref_peak;
    last_playback_us = s_last_playback_us;
    taskEXIT_CRITICAL(&s_aec_lock);

    if (ref_ring == NULL ||
        filled_samples < AUDIO_AEC_REF_DELAY_SAMPLES + sample_count ||
        last_ref_peak < AUDIO_AEC_REF_ACTIVE_PEAK ||
        esp_timer_get_time() - last_playback_us > AUDIO_AEC_REF_ACTIVE_US) {
        return false;
    }

    *ring = ref_ring;
    *read_start = audio_aec_ring_prev(write_pos,
                                      AUDIO_AEC_REF_DELAY_SAMPLES + (uint32_t)sample_count);
    *ref_peak = last_ref_peak;
    return true;
}

static void audio_aec_fifo_clear(void)
{
    s_out_fifo_read_pos = 0;
    s_out_fifo_used = 0;
    s_out_fifo_ready = false;
}

static void audio_aec_fifo_push(const int16_t *samples, uint32_t count)
{
    if (s_out_fifo == NULL || count == 0U) {
        return;
    }
    if (count > AUDIO_AEC_OUT_FIFO_SAMPLES) {
        samples += count - AUDIO_AEC_OUT_FIFO_SAMPLES;
        count = AUDIO_AEC_OUT_FIFO_SAMPLES;
    }
    if (count > AUDIO_AEC_OUT_FIFO_SAMPLES - s_out_fifo_used) {
        uint32_t drop = count - (AUDIO_AEC_OUT_FIFO_SAMPLES - s_out_fifo_used);
        s_out_fifo_read_pos = (s_out_fifo_read_pos + drop) & AUDIO_AEC_OUT_FIFO_MASK;
        s_out_fifo_used -= drop;
    }

    uint32_t write_pos = (s_out_fifo_read_pos + s_out_fifo_used) & AUDIO_AEC_OUT_FIFO_MASK;
    for (uint32_t index = 0; index < count; ++index) {
        s_out_fifo[write_pos] = samples[index];
        write_pos = (write_pos + 1U) & AUDIO_AEC_OUT_FIFO_MASK;
    }
    s_out_fifo_used += count;
}

static bool audio_aec_fifo_pop(int16_t *samples, uint32_t count, uint32_t *out_peak)
{
    if (s_out_fifo == NULL || samples == NULL || count == 0U) {
        return false;
    }

    if (!s_out_fifo_ready) {
        uint32_t prefill = count + (uint32_t)(s_aec_frame_size > 0 ? s_aec_frame_size : 0);
        if (s_out_fifo_used < prefill) {
            return false;
        }
        s_out_fifo_ready = true;
    }
    if (s_out_fifo_used < count) {
        s_out_fifo_ready = false;
        return false;
    }

    uint32_t peak = 0;
    for (uint32_t index = 0; index < count; ++index) {
        int16_t sample = s_out_fifo[s_out_fifo_read_pos];
        samples[index] = sample;
        uint32_t abs_sample = audio_aec_abs_i16(sample);
        if (abs_sample > peak) {
            peak = abs_sample;
        }
        s_out_fifo_read_pos = (s_out_fifo_read_pos + 1U) & AUDIO_AEC_OUT_FIFO_MASK;
    }
    s_out_fifo_used -= count;
    if (out_peak != NULL) {
        *out_peak = peak;
    }
    return true;
}

void audio_echo_cancel_feed_playback(const int16_t *samples,
                                     size_t sample_count,
                                     uint8_t channels)
{
    if (samples == NULL || sample_count == 0U || channels == 0U || !audio_aec_ensure_ready()) {
        return;
    }
    if (!audio_aec_enter()) {
        return;
    }

    int16_t *ref_ring = NULL;
    uint32_t write_pos = 0;
    uint32_t filled_samples = 0;

    taskENTER_CRITICAL(&s_aec_lock);
    ref_ring = s_ref_ring;
    write_pos = s_ref_write_pos;
    filled_samples = s_ref_filled_samples;
    taskEXIT_CRITICAL(&s_aec_lock);

    if (ref_ring == NULL) {
        audio_aec_leave();
        return;
    }

    const size_t frame_count = sample_count / channels;
    uint32_t peak = 0;
    for (size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
        int32_t mono = 0;
        const size_t base = frame_index * channels;
        for (uint8_t channel = 0; channel < channels; ++channel) {
            mono += samples[base + channel];
        }
        mono /= channels;

        int16_t sample = (int16_t)mono;
        uint32_t abs_sample = audio_aec_abs_i16(sample);
        if (abs_sample > peak) {
            peak = abs_sample;
        }

        ref_ring[write_pos] = sample;
        write_pos = (write_pos + 1U) & AUDIO_AEC_REF_RING_MASK;
        if (filled_samples < AUDIO_AEC_REF_RING_SAMPLES) {
            filled_samples++;
        }
    }

    taskENTER_CRITICAL(&s_aec_lock);
    s_ref_write_pos = write_pos;
    s_ref_filled_samples = filled_samples;
    s_last_ref_peak = peak;
    if (peak >= AUDIO_AEC_REF_ACTIVE_PEAK) {
        s_last_playback_us = esp_timer_get_time();
    }
    taskEXIT_CRITICAL(&s_aec_lock);
    audio_aec_leave();
}

void audio_echo_cancel_process_capture(int16_t *samples,
                                       size_t sample_count,
                                       audio_echo_cancel_metrics_t *metrics)
{
    if (metrics != NULL) {
        memset(metrics, 0, sizeof(*metrics));
    }
    if (samples == NULL || sample_count == 0U || !audio_aec_ensure_ready()) {
        return;
    }
    if (!audio_aec_enter()) {
        return;
    }

    int16_t *ref_ring = NULL;
    uint32_t read_start = 0;
    uint32_t ref_peak = 0;
    if (!audio_aec_ref_snapshot(sample_count, &ref_ring, &read_start, &ref_peak)) {
        s_frame_fill = 0;
        audio_aec_fifo_clear();
        audio_aec_leave();
        return;
    }

    uint32_t mic_peak = 0;
    for (size_t index = 0; index < sample_count; ++index) {
        int16_t mic_sample = samples[index];
        int16_t ref_sample = ref_ring[(read_start + (uint32_t)index) & AUDIO_AEC_REF_RING_MASK];
        uint32_t mic_abs = audio_aec_abs_i16(mic_sample);
        if (mic_abs > mic_peak) {
            mic_peak = mic_abs;
        }

        s_mic_frame[s_frame_fill] = mic_sample;
        s_ref_frame[s_frame_fill] = ref_sample;
        s_frame_fill++;

        if (s_frame_fill >= (uint32_t)s_aec_frame_size) {
            aec_process(s_aec, s_mic_frame, s_ref_frame, s_out_frame);
            audio_aec_fifo_push(s_out_frame, (uint32_t)s_aec_frame_size);
            s_frame_fill = 0;
        }
    }

    uint32_t out_peak = 0;
    bool processed = audio_aec_fifo_pop(samples, (uint32_t)sample_count, &out_peak);
    if (metrics != NULL && processed) {
        metrics->active = true;
        metrics->ref_peak = ref_peak;
        metrics->mic_peak = mic_peak;
        metrics->out_peak = out_peak;
        metrics->delay_samples = AUDIO_AEC_REF_DELAY_SAMPLES;
        if (mic_peak > 0U && out_peak < mic_peak) {
            metrics->suppress_percent = (uint8_t)(((uint64_t)(mic_peak - out_peak) * 100U) / mic_peak);
        }
    }
    audio_aec_leave();
}

void audio_echo_cancel_process_capture_with_reference(int16_t *samples,
                                                      const int16_t *reference,
                                                      size_t sample_count,
                                                      audio_echo_cancel_metrics_t *metrics)
{
    if (metrics != NULL) {
        memset(metrics, 0, sizeof(*metrics));
    }
    if (samples == NULL || reference == NULL || sample_count == 0U || !audio_aec_ensure_ready()) {
        return;
    }
    if (!audio_aec_enter()) {
        return;
    }

    uint32_t ref_peak = 0;
    for (size_t index = 0; index < sample_count; ++index) {
        uint32_t abs_sample = audio_aec_abs_i16(reference[index]);
        if (abs_sample > ref_peak) {
            ref_peak = abs_sample;
        }
    }

    if (ref_peak < AUDIO_AEC_REF_ACTIVE_PEAK) {
        s_frame_fill = 0;
        audio_aec_fifo_clear();
        audio_aec_leave();
        return;
    }

    uint32_t mic_peak = 0;
    for (size_t index = 0; index < sample_count; ++index) {
        int16_t mic_sample = samples[index];
        int16_t ref_sample = reference[index];
        uint32_t mic_abs = audio_aec_abs_i16(mic_sample);
        if (mic_abs > mic_peak) {
            mic_peak = mic_abs;
        }

        s_mic_frame[s_frame_fill] = mic_sample;
        s_ref_frame[s_frame_fill] = ref_sample;
        s_frame_fill++;

        if (s_frame_fill >= (uint32_t)s_aec_frame_size) {
            aec_process(s_aec, s_mic_frame, s_ref_frame, s_out_frame);
            audio_aec_fifo_push(s_out_frame, (uint32_t)s_aec_frame_size);
            s_frame_fill = 0;
        }
    }

    uint32_t out_peak = 0;
    bool processed = audio_aec_fifo_pop(samples, (uint32_t)sample_count, &out_peak);
    if (metrics != NULL && processed) {
        metrics->active = true;
        metrics->ref_peak = ref_peak;
        metrics->mic_peak = mic_peak;
        metrics->out_peak = out_peak;
        metrics->delay_samples = 0;
        if (mic_peak > 0U && out_peak < mic_peak) {
            metrics->suppress_percent = (uint8_t)(((uint64_t)(mic_peak - out_peak) * 100U) / mic_peak);
        }
    }
    audio_aec_leave();
}

void audio_echo_cancel_reset(void)
{
    taskENTER_CRITICAL(&s_aec_lock);
    s_ref_write_pos = 0;
    s_ref_filled_samples = 0;
    s_last_ref_peak = 0;
    s_last_playback_us = 0;
    s_frame_fill = 0;
    s_out_fifo_read_pos = 0;
    s_out_fifo_used = 0;
    s_out_fifo_ready = false;
    taskEXIT_CRITICAL(&s_aec_lock);
}

void audio_echo_cancel_deinit(void)
{
    aec_handle_t *handle = NULL;
    int16_t *ref_ring = NULL;
    int16_t *mic_frame = NULL;
    int16_t *ref_frame = NULL;
    int16_t *out_frame = NULL;
    int16_t *out_fifo = NULL;

    taskENTER_CRITICAL(&s_aec_lock);
    if (s_aec == NULL && !s_initializing) {
        taskEXIT_CRITICAL(&s_aec_lock);
        return;
    }
    s_deinit_requested = true;
    taskEXIT_CRITICAL(&s_aec_lock);

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(AUDIO_AEC_DEINIT_WAIT_MS);
    while (true) {
        bool idle = false;

        taskENTER_CRITICAL(&s_aec_lock);
        idle = !s_initializing && s_active_users == 0U;
        taskEXIT_CRITICAL(&s_aec_lock);
        if (idle) {
            break;
        }
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            uint32_t active_users = 0;
            bool initializing = false;

            taskENTER_CRITICAL(&s_aec_lock);
            active_users = s_active_users;
            initializing = s_initializing;
            s_deinit_requested = false;
            taskEXIT_CRITICAL(&s_aec_lock);
            ESP_LOGW(TAG, "AEC deinit skipped: busy users=%lu initializing=%d",
                     (unsigned long)active_users,
                     initializing ? 1 : 0);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    taskENTER_CRITICAL(&s_aec_lock);
    handle = s_aec;
    ref_ring = s_ref_ring;
    mic_frame = s_mic_frame;
    ref_frame = s_ref_frame;
    out_frame = s_out_frame;
    out_fifo = s_out_fifo;
    s_aec = NULL;
    s_aec_frame_size = 0;
    s_ref_ring = NULL;
    s_mic_frame = NULL;
    s_ref_frame = NULL;
    s_out_frame = NULL;
    s_out_fifo = NULL;
    s_ref_write_pos = 0;
    s_ref_filled_samples = 0;
    s_last_ref_peak = 0;
    s_last_playback_us = 0;
    s_frame_fill = 0;
    s_out_fifo_read_pos = 0;
    s_out_fifo_used = 0;
    s_out_fifo_ready = false;
    s_deinit_requested = false;
    s_create_failed_logged = false;
    taskEXIT_CRITICAL(&s_aec_lock);

    audio_aec_free_handle_and_buffers(handle, ref_ring, mic_frame, ref_frame, out_frame, out_fifo);
    if (handle != NULL || ref_ring != NULL || mic_frame != NULL || ref_frame != NULL ||
        out_frame != NULL || out_fifo != NULL) {
        ESP_LOGI(TAG, "official ESP-SR AEC released");
        audio_aec_log_heap("AEC released");
    }
}

#else

bool audio_echo_cancel_preload(void)
{
    return false;
}

void audio_echo_cancel_feed_playback(const int16_t *samples,
                                     size_t sample_count,
                                     uint8_t channels)
{
    (void)samples;
    (void)sample_count;
    (void)channels;
}

void audio_echo_cancel_process_capture(int16_t *samples,
                                       size_t sample_count,
                                       audio_echo_cancel_metrics_t *metrics)
{
    (void)samples;
    (void)sample_count;
    if (metrics != NULL) {
        memset(metrics, 0, sizeof(*metrics));
    }
}

void audio_echo_cancel_process_capture_with_reference(int16_t *samples,
                                                      const int16_t *reference,
                                                      size_t sample_count,
                                                      audio_echo_cancel_metrics_t *metrics)
{
    (void)samples;
    (void)reference;
    (void)sample_count;
    if (metrics != NULL) {
        memset(metrics, 0, sizeof(*metrics));
    }
}

void audio_echo_cancel_reset(void)
{
}

void audio_echo_cancel_deinit(void)
{
}

#endif
