#include "device_call_ringtone.h"

#include <limits.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "audio_device.h"
#include "platform_task_reaper.h"

static const char *TAG = "call_ringtone";

#define DEVICE_CALL_RINGTONE_TASK_STACK_BYTES (12U * 1024U)
#define DEVICE_CALL_RINGTONE_TASK_PRIORITY    4
#define DEVICE_CALL_RINGTONE_TASK_CORE        1
#define DEVICE_CALL_RINGTONE_CHUNK_MS         20U
#define DEVICE_CALL_RINGTONE_TONE_A_HZ        440U
#define DEVICE_CALL_RINGTONE_TONE_B_HZ        480U
#define DEVICE_CALL_RINGTONE_BURST_MS         400U
#define DEVICE_CALL_RINGTONE_BURST_GAP_MS     200U
#define DEVICE_CALL_RINGTONE_REPEAT_GAP_MS    2000U
#define DEVICE_CALL_RINGTONE_PERIOD_MS        \
    ((2U * DEVICE_CALL_RINGTONE_BURST_MS) + \
     DEVICE_CALL_RINGTONE_BURST_GAP_MS + \
     DEVICE_CALL_RINGTONE_REPEAT_GAP_MS)
#define DEVICE_CALL_RINGTONE_FADE_MS          12U
#define DEVICE_CALL_RINGTONE_STOP_TIMEOUT_MS  500U
#define DEVICE_CALL_RINGTONE_MAX_SAMPLES      (16000U * 2U * DEVICE_CALL_RINGTONE_CHUNK_MS / 1000U)

static portMUX_TYPE s_ringtone_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_ringtone_task;
static bool s_ringtone_stop_requested;
static EXT_RAM_BSS_ATTR int16_t s_ringtone_pcm[DEVICE_CALL_RINGTONE_MAX_SAMPLES];

/* A compact sine table keeps the ringtone deterministic without a large WAV
 * asset or floating-point work in the playback loop. */
static const int16_t s_sine_lut[32] = {
    0, 6393, 12539, 18204, 23170, 27245, 30273, 32137,
    32767, 32137, 30273, 27245, 23170, 18204, 12539, 6393,
    0, -6393, -12539, -18204, -23170, -27245, -30273, -32137,
    -32767, -32137, -30273, -27245, -23170, -18204, -12539, -6393,
};

static bool device_call_ringtone_stop_requested(void)
{
    bool stop = false;

    taskENTER_CRITICAL(&s_ringtone_lock);
    stop = s_ringtone_stop_requested;
    taskEXIT_CRITICAL(&s_ringtone_lock);
    return stop;
}

static int16_t device_call_ringtone_sample(uint32_t phase_a,
                                           uint32_t phase_b,
                                           uint32_t envelope_q15)
{
    int32_t tone_a = s_sine_lut[phase_a >> 27];
    int32_t tone_b = s_sine_lut[phase_b >> 27];
    int32_t mixed = (tone_a * 9200 + tone_b * 7600) / 32767;

    mixed = (mixed * (int32_t)envelope_q15) / 32767;
    if (mixed > INT16_MAX) {
        mixed = INT16_MAX;
    } else if (mixed < INT16_MIN) {
        mixed = INT16_MIN;
    }
    return (int16_t)mixed;
}

static uint32_t device_call_ringtone_burst_envelope(uint32_t pattern_frame,
                                                     uint32_t burst_start_frame,
                                                     uint32_t burst_frames,
                                                     uint32_t fade_frames)
{
    if (pattern_frame < burst_start_frame ||
        pattern_frame >= burst_start_frame + burst_frames) {
        return 0U;
    }

    uint32_t burst_frame = pattern_frame - burst_start_frame;
    if (fade_frames > 0U && burst_frame < fade_frames) {
        return (burst_frame * 32767U) / fade_frames;
    }
    if (fade_frames > 0U && burst_frame + fade_frames > burst_frames) {
        return ((burst_frames - burst_frame) * 32767U) / fade_frames;
    }
    return 32767U;
}

static void device_call_ringtone_task(void *ctx)
{
    const audio_format_t *format = speaker_get_playback_format();
    uint32_t phase_a = 0U;
    uint32_t phase_b = 0U;
    uint64_t rendered_frames = 0U;
    esp_err_t result = ESP_OK;

    (void)ctx;
    if (format == NULL || format->bits_per_sample != 16U ||
        (format->channels != 1U && format->channels != 2U) ||
        format->sample_rate_hz == 0U) {
        result = ESP_ERR_NOT_SUPPORTED;
        goto done;
    }

    const size_t chunk_frames =
        ((size_t)format->sample_rate_hz * DEVICE_CALL_RINGTONE_CHUNK_MS) / 1000U;
    const size_t chunk_samples = chunk_frames * format->channels;
    if (chunk_frames == 0U || chunk_samples > DEVICE_CALL_RINGTONE_MAX_SAMPLES) {
        result = ESP_ERR_INVALID_SIZE;
        goto done;
    }

    result = speaker_prepare_playback_path();
    if (result != ESP_OK) {
        goto done;
    }

    const uint32_t phase_step_a =
        (uint32_t)(((uint64_t)DEVICE_CALL_RINGTONE_TONE_A_HZ << 32) /
                   format->sample_rate_hz);
    const uint32_t phase_step_b =
        (uint32_t)(((uint64_t)DEVICE_CALL_RINGTONE_TONE_B_HZ << 32) /
                   format->sample_rate_hz);
    const uint32_t fade_frames =
        (format->sample_rate_hz * DEVICE_CALL_RINGTONE_FADE_MS) / 1000U;
    const uint32_t burst_frames =
        (format->sample_rate_hz * DEVICE_CALL_RINGTONE_BURST_MS) / 1000U;
    const uint32_t second_burst_start_frame =
        (format->sample_rate_hz *
         (DEVICE_CALL_RINGTONE_BURST_MS + DEVICE_CALL_RINGTONE_BURST_GAP_MS)) /
        1000U;
    const uint32_t period_frames =
        (format->sample_rate_hz * DEVICE_CALL_RINGTONE_PERIOD_MS) / 1000U;

    ESP_LOGI(TAG,
             "ringtone started: rate=%luHz channels=%u tones=%u+%uHz pattern=%u/%u/%u/%ums stack=PSRAM",
             (unsigned long)format->sample_rate_hz,
             (unsigned)format->channels,
             (unsigned)DEVICE_CALL_RINGTONE_TONE_A_HZ,
             (unsigned)DEVICE_CALL_RINGTONE_TONE_B_HZ,
             (unsigned)DEVICE_CALL_RINGTONE_BURST_MS,
             (unsigned)DEVICE_CALL_RINGTONE_BURST_GAP_MS,
             (unsigned)DEVICE_CALL_RINGTONE_BURST_MS,
             (unsigned)DEVICE_CALL_RINGTONE_REPEAT_GAP_MS);

    while (!device_call_ringtone_stop_requested()) {
        for (size_t frame = 0; frame < chunk_frames; ++frame) {
            uint32_t pattern_frame = (uint32_t)((rendered_frames + frame) % period_frames);
            uint32_t envelope_q15 =
                device_call_ringtone_burst_envelope(pattern_frame,
                                                    0U,
                                                    burst_frames,
                                                    fade_frames);
            if (envelope_q15 == 0U) {
                envelope_q15 =
                    device_call_ringtone_burst_envelope(pattern_frame,
                                                        second_burst_start_frame,
                                                        burst_frames,
                                                        fade_frames);
            }
            int16_t sample = envelope_q15 > 0U ?
                             device_call_ringtone_sample(phase_a, phase_b, envelope_q15) :
                             0;

            for (uint8_t channel = 0U; channel < format->channels; ++channel) {
                s_ringtone_pcm[frame * format->channels + channel] = sample;
            }
            phase_a += phase_step_a;
            phase_b += phase_step_b;
        }

        result = speaker_play_pcm_frame((const uint8_t *)s_ringtone_pcm,
                                        chunk_samples * sizeof(int16_t),
                                        format);
        if (result != ESP_OK) {
            break;
        }
        rendered_frames += chunk_frames;
    }

done:
    speaker_stop_playback();
    taskENTER_CRITICAL(&s_ringtone_lock);
    if (s_ringtone_task == xTaskGetCurrentTaskHandle()) {
        s_ringtone_task = NULL;
    }
    s_ringtone_stop_requested = false;
    taskEXIT_CRITICAL(&s_ringtone_lock);

    if (result == ESP_OK) {
        ESP_LOGI(TAG, "ringtone stopped");
    } else {
        ESP_LOGW(TAG, "ringtone stopped after playback error: %s", esp_err_to_name(result));
    }
    platform_task_reaper_delete_current_with_caps(TAG);
}

esp_err_t device_call_ringtone_start(void)
{
    BaseType_t task_result = pdFAIL;

    taskENTER_CRITICAL(&s_ringtone_lock);
    if (s_ringtone_task != NULL) {
        taskEXIT_CRITICAL(&s_ringtone_lock);
        return ESP_OK;
    }
    s_ringtone_stop_requested = false;
    taskEXIT_CRITICAL(&s_ringtone_lock);

    task_result = xTaskCreatePinnedToCoreWithCaps(device_call_ringtone_task,
                                                  "call_ring",
                                                  DEVICE_CALL_RINGTONE_TASK_STACK_BYTES,
                                                  NULL,
                                                  DEVICE_CALL_RINGTONE_TASK_PRIORITY,
                                                  &s_ringtone_task,
                                                  DEVICE_CALL_RINGTONE_TASK_CORE,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_result != pdPASS) {
        taskENTER_CRITICAL(&s_ringtone_lock);
        s_ringtone_task = NULL;
        s_ringtone_stop_requested = false;
        taskEXIT_CRITICAL(&s_ringtone_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t device_call_ringtone_stop(void)
{
    TickType_t started = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(DEVICE_CALL_RINGTONE_STOP_TIMEOUT_MS);

    taskENTER_CRITICAL(&s_ringtone_lock);
    if (s_ringtone_task == NULL) {
        s_ringtone_stop_requested = false;
        taskEXIT_CRITICAL(&s_ringtone_lock);
        return ESP_OK;
    }
    s_ringtone_stop_requested = true;
    taskEXIT_CRITICAL(&s_ringtone_lock);

    while (device_call_ringtone_is_active()) {
        if ((xTaskGetTickCount() - started) >= timeout) {
            ESP_LOGE(TAG, "ringtone stop timed out");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

bool device_call_ringtone_is_active(void)
{
    bool active = false;

    taskENTER_CRITICAL(&s_ringtone_lock);
    active = s_ringtone_task != NULL;
    taskEXIT_CRITICAL(&s_ringtone_lock);
    return active;
}
