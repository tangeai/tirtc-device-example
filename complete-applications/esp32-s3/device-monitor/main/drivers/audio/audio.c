#include "audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "audio_echo_cancel.h"
#include "hardware_board.h"
#include "platform_task_reaper.h"
#include "platform/app_task_affinity.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "audio";

#define AUDIO_INPUT_LEVEL_DISPLAY_SCALE  10U
#define AUDIO_CAPTURE_CODEC_GAIN_MAX_PERCENT 80U
#define AUDIO_CAPTURE_UPLOAD_GAIN_MAX_Q8     384U
#define AUDIO_CAPTURE_AUTO_GAIN_TARGET_PEAK 4096U
#define AUDIO_CAPTURE_NEAR_END_AUTO_GAIN_TARGET_PEAK 8192U
#define AUDIO_CAPTURE_AUTO_GAIN_NOISE_FLOOR_PEAK 80U
/*
 * Two target boards measured a post-AEC idle peak of 231..319 samples.  Keep
 * that stationary residue below the call AGC knee so it is not lifted by the
 * 3x distant-speech gain.  This is deliberately an AGC floor, not a noise
 * gate: samples remain continuous and speech starts/word endings are not cut.
 */
#define AUDIO_CAPTURE_CALL_AUTO_GAIN_NOISE_FLOOR_PEAK 384U
#define AUDIO_CAPTURE_AUTO_GAIN_DEFAULT_MAX_PERCENT 200U
/*
 * This is a policy ceiling, not a fixed gain. The frame peak controller still
 * targets -12 dBFS and immediately backs gain down for normal/strong speech.
 * A larger ceiling is needed only for distant near-end speech after AEC; the
 * call profile keeps echo-only frames on its separate 200% far-end guard.
 */
#define AUDIO_CAPTURE_AUTO_GAIN_LIMIT_PERCENT 1600U
#define AUDIO_CAPTURE_AUTO_GAIN_MIN_Q8   256U
#define AUDIO_CAPTURE_AUTO_GAIN_ATTACK_DIV 2U
#define AUDIO_MAX_CAPTURE_GAIN_DB        36.0f
#define AUDIO_CAPTURE_NOISE_GATE_OPEN_PEAK 240U
#define AUDIO_CAPTURE_NOISE_GATE_CLOSE_PEAK 120U
#define AUDIO_CAPTURE_NOISE_GATE_ATTENUATION_PERCENT 20U
#define AUDIO_CAPTURE_HIGH_PASS_CUTOFF_HZ 100U
/* exp(-2*pi*100/16000) in Q15 for a low-cost first-order DC/rumble blocker. */
#define AUDIO_CAPTURE_HIGH_PASS_ALPHA_Q15 31506
#define AUDIO_CAPTURE_LEVEL_LOG_INTERVAL_MS 1000U
#define AUDIO_CAPTURE_LEVEL_DBFS_FLOOR_X10 (-960)
#define AUDIO_AEC_REFERENCE_CLIP_PEAK       30000U
#define AUDIO_ES7210_MIC1_GAIN_REG           0x43
#define AUDIO_ES7210_MIC3_GAIN_REG           0x45
#define AUDIO_CAPTURE_PRIMARY_CHANNEL HARDWARE_BOARD_AUDIO_ADC_PRIMARY_CHANNEL
#define AUDIO_DEFAULT_CAPTURE_GAIN_PERCENT \
    ((uint8_t)(((HARDWARE_BOARD_AUDIO_DEFAULT_ADC_GAIN_DB) * 100.0f / AUDIO_MAX_CAPTURE_GAIN_DB) + 0.5f))
/*
 * ESP-IDF uses the same DMA sizing for both sides of this duplex I2S channel.
 * Keep it conservative so TiRTC/WebRTC internal RAM pressure does not prevent
 * the microphone path from coming up when the speaker path is also prepared.
 */
#define AUDIO_I2S_DMA_DESC_NUM           6
#define AUDIO_I2S_DMA_FRAME_NUM          64
#define AUDIO_PLAYBACK_SAMPLE_RATE_HZ    HARDWARE_BOARD_AUDIO_SAMPLE_RATE_HZ
#define AUDIO_PLAYBACK_OUTPUT_CHANNELS   HARDWARE_BOARD_AUDIO_CHANNELS
#define AUDIO_CAPTURE_READER_TASK_STACK    (4 * 1024)
#define AUDIO_CAPTURE_READER_TASK_PRIORITY 14
#define AUDIO_CAPTURE_READER_TASK_CORE     APP_TASK_CORE_NETWORK
#define AUDIO_CAPTURE_PROCESS_TASK_STACK    (8 * 1024)
#define AUDIO_CAPTURE_PROCESS_TASK_PRIORITY 12
#define AUDIO_CAPTURE_PROCESS_TASK_CORE     APP_TASK_CORE_AUDIO
#define AUDIO_CAPTURE_PIPELINE_DEPTH         6U
#define AUDIO_CAPTURE_OBSERVER_MAX       4
#define AUDIO_CAPTURE_TASK_STOP_WAIT_MS  300
/*
 * AI Chat declares PCM/16 kHz/1 ch to the cloud. Keep the local capture stream
 * at the native board rate so ASR receives real 16 kHz audio instead of an
 * 8 kHz stream expanded back to 16 kHz later.
 */
#define AUDIO_CAPTURE_UPLOAD_SAMPLE_RATE_HZ HARDWARE_BOARD_AUDIO_SAMPLE_RATE_HZ
#define AUDIO_CAPTURE_HW_SAMPLE_RATE_HZ     HARDWARE_BOARD_AUDIO_SAMPLE_RATE_HZ
#define AUDIO_CAPTURE_HW_INPUT_CHANNELS     HARDWARE_BOARD_AUDIO_ADC_CHANNELS
#define AUDIO_CAPTURE_HW_TOTAL_CHANNELS     HARDWARE_BOARD_AUDIO_ADC_TDM_CHANNELS
#define AUDIO_CAPTURE_HW_CHANNEL_MASK       HARDWARE_BOARD_AUDIO_ADC_CHANNEL_MASK
#define AUDIO_CAPTURE_REFERENCE_TDM_SLOT    HARDWARE_BOARD_AUDIO_ADC_REFERENCE_TDM_SLOT
#define AUDIO_CAPTURE_REFERENCE_DMA_CHANNEL HARDWARE_BOARD_AUDIO_ADC_REFERENCE_DMA_CHANNEL
#define AUDIO_CAPTURE_REFERENCE_CODEC_CHANNEL HARDWARE_BOARD_AUDIO_ADC_REFERENCE_CODEC_CHANNEL
#define AUDIO_CAPTURE_REFERENCE_DIGITAL_GAIN_Q8 HARDWARE_BOARD_AUDIO_ADC_REFERENCE_DIGITAL_GAIN_Q8
#define AUDIO_CAPTURE_REFERENCE_ENABLED     HARDWARE_BOARD_AUDIO_ADC_REFERENCE_ENABLED
#define AUDIO_CAPTURE_OUTPUT_CHANNELS    1
#define AUDIO_CAPTURE_DOWNSAMPLE_RATIO \
    (AUDIO_CAPTURE_HW_SAMPLE_RATE_HZ / AUDIO_CAPTURE_UPLOAD_SAMPLE_RATE_HZ)

#if (AUDIO_CAPTURE_HW_SAMPLE_RATE_HZ % AUDIO_CAPTURE_UPLOAD_SAMPLE_RATE_HZ) != 0
#error "audio capture upload sample rate must evenly divide hardware sample rate"
#endif

#if AUDIO_CAPTURE_PRIMARY_CHANNEL >= AUDIO_CAPTURE_HW_INPUT_CHANNELS
#error "audio capture primary channel must be within hardware input channels"
#endif

#if AUDIO_CAPTURE_REFERENCE_ENABLED && (AUDIO_CAPTURE_REFERENCE_TDM_SLOT >= AUDIO_CAPTURE_HW_TOTAL_CHANNELS)
#error "audio capture reference TDM slot must be within the hardware TDM frame"
#endif

#if AUDIO_CAPTURE_REFERENCE_ENABLED && (AUDIO_CAPTURE_REFERENCE_DMA_CHANNEL >= AUDIO_CAPTURE_HW_INPUT_CHANNELS)
#error "audio capture reference DMA channel must be within packed input channels"
#endif

#if AUDIO_CAPTURE_REFERENCE_ENABLED && \
    (AUDIO_CAPTURE_REFERENCE_CODEC_CHANNEL >= HARDWARE_BOARD_AUDIO_ADC_CODEC_CHANNELS)
#error "audio capture reference codec channel must be within ES7210 inputs"
#endif

#if AUDIO_CAPTURE_REFERENCE_ENABLED && (AUDIO_CAPTURE_REFERENCE_DIGITAL_GAIN_Q8 == 0U)
#error "audio capture reference digital gain must be positive"
#endif

static const audio_format_t s_capture_format = {
    .sample_rate_hz = AUDIO_CAPTURE_UPLOAD_SAMPLE_RATE_HZ,
    .channels = AUDIO_CAPTURE_OUTPUT_CHANNELS,
    .bits_per_sample = HARDWARE_BOARD_AUDIO_BITS_PER_SAMPLE,
};

static const audio_format_t s_playback_format = {
    .sample_rate_hz = AUDIO_PLAYBACK_SAMPLE_RATE_HZ,
    .channels = AUDIO_PLAYBACK_OUTPUT_CHANNELS,
    .bits_per_sample = HARDWARE_BOARD_AUDIO_BITS_PER_SAMPLE,
};

typedef struct {
    audio_capture_frame_cb_t cb;
    void *ctx;
    bool enabled;
} audio_capture_observer_t;

typedef struct {
    int16_t *raw_samples;
    int64_t frame_start_us;
    uint32_t read_us;
} audio_capture_pipeline_slot_t;

static audio_capture_frame_cb_t s_capture_cb;
static void *s_capture_cb_ctx;
static bool s_capture_primary_enabled;
static audio_capture_observer_t s_capture_observers[AUDIO_CAPTURE_OBSERVER_MAX];
static TaskHandle_t s_capture_task;
static TaskHandle_t s_capture_process_task;
static TaskHandle_t s_tone_task;
static bool s_capture_task_stop_requested;
static bool s_audio_ready;
static bool s_audio_output_ready;
static bool s_audio_input_ready;
static bool s_audio_preparing;
static bool s_audio_output_preparing;
static bool s_audio_input_preparing;
static bool s_speaker_path_enabled;
static portMUX_TYPE s_audio_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_playback_mutex;
static audio_stats_t s_audio_stats = {
    .speaker_volume_percent = HARDWARE_BOARD_AUDIO_DEFAULT_VOLUME,
    .capture_gain_percent = AUDIO_DEFAULT_CAPTURE_GAIN_PERCENT,
    .capture_codec_gain_percent = AUDIO_CAPTURE_CODEC_GAIN_MAX_PERCENT,
    .capture_upload_gain_percent = AUDIO_DEFAULT_CAPTURE_GAIN_PERCENT,
    .capture_auto_gain_max_percent = AUDIO_CAPTURE_AUTO_GAIN_DEFAULT_MAX_PERCENT,
    .echo_suppression = AUDIO_ECHO_SUPPRESSION_BALANCED,
    .far_end_upload_gain_percent = AUDIO_DEFAULT_CAPTURE_GAIN_PERCENT,
    .far_end_auto_gain_max_percent = AUDIO_CAPTURE_AUTO_GAIN_DEFAULT_MAX_PERCENT,
    .capture_high_pass_filter_enabled = false,
    .capture_noise_gate_enabled = true,
};
static esp_err_t s_audio_output_prepare_last_err = ESP_OK;
static esp_err_t s_audio_input_prepare_last_err = ESP_OK;
static TickType_t s_audio_output_prepare_retry_after_ticks;
static TickType_t s_audio_input_prepare_retry_after_ticks;
static uint8_t s_speaker_volume_percent = HARDWARE_BOARD_AUDIO_DEFAULT_VOLUME;
static uint8_t s_capture_gain_percent = AUDIO_DEFAULT_CAPTURE_GAIN_PERCENT;
static audio_capture_processing_config_t s_capture_processing_config = {
    .send_volume_percent = AUDIO_DEFAULT_CAPTURE_GAIN_PERCENT,
    .codec_gain_percent = AUDIO_CAPTURE_CODEC_GAIN_MAX_PERCENT,
    .upload_gain_percent = AUDIO_DEFAULT_CAPTURE_GAIN_PERCENT,
    .auto_gain_max_percent = AUDIO_CAPTURE_AUTO_GAIN_DEFAULT_MAX_PERCENT,
    .echo_continuous_processing = false,
    .echo_near_end_protection_enabled = false,
    .far_end_gain_guard_enabled = false,
    .far_end_upload_gain_percent = AUDIO_DEFAULT_CAPTURE_GAIN_PERCENT,
    .far_end_auto_gain_max_percent = AUDIO_CAPTURE_AUTO_GAIN_DEFAULT_MAX_PERCENT,
    .echo_suppression = AUDIO_ECHO_SUPPRESSION_BALANCED,
    .echo_diagnostics_enabled = false,
    .high_pass_filter_enabled = false,
    .noise_gate_enabled = true,
    .noise_gate_open_peak = AUDIO_CAPTURE_NOISE_GATE_OPEN_PEAK,
    .noise_gate_close_peak = AUDIO_CAPTURE_NOISE_GATE_CLOSE_PEAK,
    .noise_gate_attenuation_percent = AUDIO_CAPTURE_NOISE_GATE_ATTENUATION_PERCENT,
};
static bool s_playback_muted_logged;
static bool s_playback_write_logged;
static TickType_t s_last_playback_write_log_tick;

static esp_codec_dev_handle_t s_play_dev_handle;
static esp_codec_dev_handle_t s_record_dev_handle;
static esp_codec_dev_handle_t s_shared_es8311_dev_handle;
static const audio_codec_if_t *s_play_codec_if;
static const audio_codec_ctrl_if_t *s_play_ctrl_if;
static const audio_codec_gpio_if_t *s_play_gpio_if;
static const audio_codec_if_t *s_record_codec_if;
static const audio_codec_ctrl_if_t *s_record_ctrl_if;
static const audio_codec_gpio_if_t *s_record_gpio_if;
static const audio_codec_if_t *s_shared_es8311_codec_if;
static const audio_codec_ctrl_if_t *s_shared_es8311_ctrl_if;
static const audio_codec_gpio_if_t *s_shared_es8311_gpio_if;
static i2s_chan_handle_t s_i2s_tx_chan;
static i2s_chan_handle_t s_i2s_rx_chan;
static const audio_codec_data_if_t *s_i2s_data_if;

static uint8_t *s_playback_scratch;
static size_t s_playback_scratch_size;
static QueueHandle_t s_capture_free_queue;
static QueueHandle_t s_capture_ready_queue;
static audio_capture_pipeline_slot_t s_capture_pipeline[AUDIO_CAPTURE_PIPELINE_DEPTH];
static int16_t *s_capture_mono_buffer;
static int16_t *s_capture_ref_buffer;
static audio_playback_timing_t s_last_playback_timing;
static bool s_playback_path_ready_logged;

static esp_codec_dev_handle_t audio_new_speaker(void);
static esp_codec_dev_handle_t audio_new_microphone(void);
static void audio_capture_task(void *ctx);
static void audio_capture_process_task(void *ctx);
static void audio_release_capture_buffers(void);

static esp_err_t audio_ensure_playback_mutex(void)
{
    if (s_playback_mutex != NULL) {
        return ESP_OK;
    }

    SemaphoreHandle_t mutex = xSemaphoreCreateRecursiveMutexWithCaps(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&s_audio_lock);
    if (s_playback_mutex == NULL) {
        s_playback_mutex = mutex;
        mutex = NULL;
    }
    taskEXIT_CRITICAL(&s_audio_lock);

    if (mutex != NULL) {
        vSemaphoreDeleteWithCaps(mutex);
    }
    return ESP_OK;
}

static esp_err_t audio_take_playback_mutex(TickType_t wait_ticks)
{
    ESP_RETURN_ON_ERROR(audio_ensure_playback_mutex(), TAG, "create playback mutex failed");
    return xSemaphoreTakeRecursive(s_playback_mutex, wait_ticks) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void audio_give_playback_mutex(void)
{
    if (s_playback_mutex != NULL) {
        xSemaphoreGiveRecursive(s_playback_mutex);
    }
}

static uint32_t audio_capture_peak_to_meter_percent(uint32_t peak)
{
    uint32_t meter_percent = (peak * 100U * AUDIO_INPUT_LEVEL_DISPLAY_SCALE) / 32767U;

    return meter_percent > 100U ? 100U : meter_percent;
}

static int16_t audio_clip_i16(int32_t sample)
{
    if (sample > 32767) {
        return 32767;
    }
    if (sample < -32768) {
        return -32768;
    }
    return (int16_t)sample;
}

static uint32_t audio_abs_i16(int16_t sample)
{
    return sample == INT16_MIN ? 32768U : (uint32_t)abs(sample);
}

static int audio_dbfs_x10_from_ratio(double ratio)
{
    if (ratio <= 0.0) {
        return AUDIO_CAPTURE_LEVEL_DBFS_FLOOR_X10;
    }

    double db = 20.0 * log10(ratio);
    int value = (int)(db * 10.0 + (db >= 0.0 ? 0.5 : -0.5));
    return value < AUDIO_CAPTURE_LEVEL_DBFS_FLOOR_X10 ? AUDIO_CAPTURE_LEVEL_DBFS_FLOOR_X10 : value;
}

static int audio_peak_dbfs_x10(uint32_t peak)
{
    if (peak == 0U) {
        return AUDIO_CAPTURE_LEVEL_DBFS_FLOOR_X10;
    }
    if (peak > 32767U) {
        peak = 32767U;
    }
    return audio_dbfs_x10_from_ratio((double)peak / 32767.0);
}

static int audio_rms_dbfs_x10(uint64_t square_sum, uint32_t sample_count)
{
    if (square_sum == 0U || sample_count == 0U) {
        return AUDIO_CAPTURE_LEVEL_DBFS_FLOOR_X10;
    }

    double rms = sqrt((double)square_sum / (double)sample_count);
    return audio_dbfs_x10_from_ratio(rms / 32767.0);
}

static void audio_format_dbfs_x10(int dbfs_x10, char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0U) {
        return;
    }

    int abs_value = dbfs_x10 < 0 ? -dbfs_x10 : dbfs_x10;
    snprintf(buffer,
             buffer_size,
             "%s%d.%01d",
             dbfs_x10 < 0 ? "-" : "",
             abs_value / 10,
             abs_value % 10);
}

static uint8_t audio_get_capture_gain_percent_locked(void)
{
    return s_capture_gain_percent > 100U ? 100U : s_capture_gain_percent;
}

static audio_capture_processing_config_t audio_get_capture_processing_config_locked(void)
{
    return s_capture_processing_config;
}

static uint8_t audio_get_speaker_volume_percent_locked(void)
{
    return s_speaker_volume_percent > 100U ? 100U : s_speaker_volume_percent;
}

static uint8_t audio_clamp_percent(uint8_t percent)
{
    return percent > 100U ? 100U : percent;
}

static uint16_t audio_clamp_auto_gain_percent(uint16_t percent)
{
    if (percent < 100U) {
        return 100U;
    }
    if (percent > AUDIO_CAPTURE_AUTO_GAIN_LIMIT_PERCENT) {
        return AUDIO_CAPTURE_AUTO_GAIN_LIMIT_PERCENT;
    }
    return percent;
}

static audio_capture_processing_config_t audio_capture_make_default_processing_config(uint8_t percent)
{
    percent = audio_clamp_percent(percent);

    audio_capture_processing_config_t config = {
        .send_volume_percent = percent,
        .codec_gain_percent = percent > AUDIO_CAPTURE_CODEC_GAIN_MAX_PERCENT ?
                              AUDIO_CAPTURE_CODEC_GAIN_MAX_PERCENT : percent,
        .upload_gain_percent = percent,
        .auto_gain_max_percent = AUDIO_CAPTURE_AUTO_GAIN_DEFAULT_MAX_PERCENT,
        .echo_continuous_processing = false,
        .echo_near_end_protection_enabled = false,
        .far_end_gain_guard_enabled = false,
        .far_end_upload_gain_percent = percent,
        .far_end_auto_gain_max_percent = AUDIO_CAPTURE_AUTO_GAIN_DEFAULT_MAX_PERCENT,
        .echo_suppression = AUDIO_ECHO_SUPPRESSION_BALANCED,
        .echo_diagnostics_enabled = false,
        .high_pass_filter_enabled = false,
        .noise_gate_enabled = true,
        .noise_gate_open_peak = AUDIO_CAPTURE_NOISE_GATE_OPEN_PEAK,
        .noise_gate_close_peak = AUDIO_CAPTURE_NOISE_GATE_CLOSE_PEAK,
        .noise_gate_attenuation_percent = AUDIO_CAPTURE_NOISE_GATE_ATTENUATION_PERCENT,
    };
    return config;
}

static void audio_capture_sanitize_processing_config(audio_capture_processing_config_t *config)
{
    if (config == NULL) {
        return;
    }

    config->send_volume_percent = audio_clamp_percent(config->send_volume_percent);
    config->codec_gain_percent = audio_clamp_percent(config->codec_gain_percent);
    if (config->codec_gain_percent > AUDIO_CAPTURE_CODEC_GAIN_MAX_PERCENT) {
        config->codec_gain_percent = AUDIO_CAPTURE_CODEC_GAIN_MAX_PERCENT;
    }
    config->upload_gain_percent = audio_clamp_percent(config->upload_gain_percent);
    config->auto_gain_max_percent = audio_clamp_auto_gain_percent(config->auto_gain_max_percent);
    config->far_end_upload_gain_percent = audio_clamp_percent(config->far_end_upload_gain_percent);
    if (config->far_end_upload_gain_percent > config->upload_gain_percent) {
        config->far_end_upload_gain_percent = config->upload_gain_percent;
    }
    config->far_end_auto_gain_max_percent =
        audio_clamp_auto_gain_percent(config->far_end_auto_gain_max_percent);
    if (config->far_end_auto_gain_max_percent > config->auto_gain_max_percent) {
        config->far_end_auto_gain_max_percent = config->auto_gain_max_percent;
    }
    if (config->echo_suppression != AUDIO_ECHO_SUPPRESSION_STRONG) {
        config->echo_suppression = AUDIO_ECHO_SUPPRESSION_BALANCED;
    }
    config->noise_gate_attenuation_percent = audio_clamp_percent(config->noise_gate_attenuation_percent);

    if (config->noise_gate_open_peak == 0U) {
        config->noise_gate_open_peak = AUDIO_CAPTURE_NOISE_GATE_OPEN_PEAK;
    }
    if (config->noise_gate_close_peak == 0U) {
        config->noise_gate_close_peak = AUDIO_CAPTURE_NOISE_GATE_CLOSE_PEAK;
    }
    if (config->noise_gate_close_peak > config->noise_gate_open_peak) {
        config->noise_gate_close_peak = config->noise_gate_open_peak;
    }
}

static uint32_t audio_capture_base_gain_q8(uint8_t upload_gain_percent)
{
    upload_gain_percent = audio_clamp_percent(upload_gain_percent);
    if (upload_gain_percent == 0U) {
        return 0;
    }

    return ((uint32_t)upload_gain_percent * AUDIO_CAPTURE_UPLOAD_GAIN_MAX_Q8) / 100U;
}

static uint32_t audio_capture_auto_gain_max_q8(uint16_t percent)
{
    return ((uint32_t)audio_clamp_auto_gain_percent(percent) * 256U) / 100U;
}

static uint32_t audio_capture_auto_gain_target_q8(uint32_t pre_peak,
                                                  uint32_t target_peak,
                                                  uint32_t base_gain_q8,
                                                  uint32_t auto_gain_max_q8,
                                                  uint32_t noise_floor_peak)
{
    if (noise_floor_peak == 0U) {
        noise_floor_peak = AUDIO_CAPTURE_AUTO_GAIN_NOISE_FLOOR_PEAK;
    }
    if (pre_peak < noise_floor_peak || base_gain_q8 == 0U) {
        return AUDIO_CAPTURE_AUTO_GAIN_MIN_Q8;
    }
    if (auto_gain_max_q8 < AUDIO_CAPTURE_AUTO_GAIN_MIN_Q8) {
        auto_gain_max_q8 = AUDIO_CAPTURE_AUTO_GAIN_MIN_Q8;
    }

    if (target_peak == 0U) {
        target_peak = AUDIO_CAPTURE_AUTO_GAIN_TARGET_PEAK;
    }

    uint32_t base_peak = (uint32_t)(((uint64_t)pre_peak * base_gain_q8) / 256ULL);
    if (base_peak == 0U || base_peak >= target_peak) {
        return AUDIO_CAPTURE_AUTO_GAIN_MIN_Q8;
    }

    uint32_t target_q8 =
        (uint32_t)(((uint64_t)target_peak * 256ULL) / base_peak);
    if (target_q8 < AUDIO_CAPTURE_AUTO_GAIN_MIN_Q8) {
        target_q8 = AUDIO_CAPTURE_AUTO_GAIN_MIN_Q8;
    }
    if (target_q8 > auto_gain_max_q8) {
        target_q8 = auto_gain_max_q8;
    }
    return target_q8;
}

static bool audio_capture_apply_noise_gate(int16_t *samples,
                                           size_t sample_count,
                                           uint32_t frame_peak,
                                           const audio_capture_processing_config_t *config,
                                           bool *gate_open)
{
    if (samples == NULL || sample_count == 0U || config == NULL || gate_open == NULL ||
        !config->noise_gate_enabled || config->send_volume_percent == 0U) {
        return true;
    }

    bool open = *gate_open;
    if (open) {
        if (frame_peak <= config->noise_gate_close_peak) {
            open = false;
        }
    } else if (frame_peak >= config->noise_gate_open_peak) {
        open = true;
    }
    *gate_open = open;

    if (open) {
        return true;
    }

    uint8_t attenuation = config->noise_gate_attenuation_percent;
    if (attenuation == 0U) {
        memset(samples, 0, sample_count * sizeof(int16_t));
        return false;
    }
    if (attenuation >= 100U) {
        return true;
    }

    for (size_t index = 0; index < sample_count; ++index) {
        samples[index] = (int16_t)(((int32_t)samples[index] * attenuation) / 100);
    }
    return false;
}

static uint32_t audio_capture_smooth_auto_gain_q8(uint32_t current_q8, uint32_t target_q8)
{
    if (current_q8 < AUDIO_CAPTURE_AUTO_GAIN_MIN_Q8) {
        current_q8 = AUDIO_CAPTURE_AUTO_GAIN_MIN_Q8;
    }
    if (target_q8 <= current_q8) {
        return target_q8;
    }

    uint32_t delta = target_q8 - current_q8;
    uint32_t step = delta / AUDIO_CAPTURE_AUTO_GAIN_ATTACK_DIV;
    if (step == 0U) {
        step = 1U;
    }
    return current_q8 + step;
}

static int16_t audio_apply_capture_upload_gain(int32_t sample, uint32_t base_gain_q8, uint32_t auto_gain_q8)
{
    if (base_gain_q8 == 0U || auto_gain_q8 == 0U) {
        return 0;
    }

    int64_t amplified = (int64_t)sample * (int64_t)base_gain_q8 * (int64_t)auto_gain_q8;
    amplified /= (256LL * 256LL);
    return audio_clip_i16((int32_t)amplified);
}

static void audio_capture_apply_high_pass(int16_t *samples,
                                          size_t sample_count,
                                          int32_t *previous_input,
                                          int32_t *previous_output,
                                          bool *state_valid)
{
    if (samples == NULL || previous_input == NULL || previous_output == NULL ||
        state_valid == NULL || sample_count == 0U) {
        return;
    }

    size_t index = 0U;
    if (!*state_valid) {
        *previous_input = samples[0];
        *previous_output = 0;
        *state_valid = true;
        samples[0] = 0;
        index = 1U;
    }

    for (; index < sample_count; ++index) {
        int32_t input = samples[index];
        int32_t state = *previous_output + input - *previous_input;
        int32_t output =
            (int32_t)(((int64_t)AUDIO_CAPTURE_HIGH_PASS_ALPHA_Q15 * state) >> 15);
        *previous_input = input;
        *previous_output = output;
        samples[index] = audio_clip_i16(output);
    }
}

static void audio_mute_playback_path_no_mutex(void)
{
    if (!s_speaker_path_enabled) {
        return;
    }
    (void)esp_codec_dev_set_out_mute(s_play_dev_handle, true);
    (void)hardware_board_set_audio_power(false);
    s_speaker_path_enabled = false;
    s_playback_path_ready_logged = false;
}

static float audio_capture_gain_percent_to_db(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    return ((float)percent * AUDIO_MAX_CAPTURE_GAIN_DB) / 100.0f;
}

static void audio_log_heap(const char *stage)
{
    size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGD(TAG,
             "%s heap dma_free=%u dma_largest=%u internal_free=%u psram_free=%u",
             stage != NULL ? stage : "audio",
             (unsigned)dma_free,
             (unsigned)dma_largest,
             (unsigned)internal_free,
             (unsigned)psram_free);
}

static void audio_update_ready_state(void)
{
    taskENTER_CRITICAL(&s_audio_lock);
    s_audio_ready = s_audio_output_ready || s_audio_input_ready;
    s_audio_stats.ready = s_audio_ready;
    if (!s_audio_input_ready) {
        s_audio_stats.capture_enabled = false;
        s_audio_stats.input_level = 0;
    }
    if (!s_audio_output_ready) {
        s_audio_stats.speaker_enabled = false;
        s_audio_stats.output_level = 0;
    }
    taskEXIT_CRITICAL(&s_audio_lock);
}

static bool audio_capture_has_active_consumer_locked(void)
{
    if (s_capture_primary_enabled && s_capture_cb != NULL) {
        return true;
    }

    for (size_t index = 0; index < AUDIO_CAPTURE_OBSERVER_MAX; ++index) {
        if (s_capture_observers[index].enabled && s_capture_observers[index].cb != NULL) {
            return true;
        }
    }
    return false;
}

static esp_err_t audio_ensure_scratch(size_t required_size)
{
    if (required_size <= s_playback_scratch_size) {
        return ESP_OK;
    }

    uint8_t *new_buffer = heap_caps_malloc(required_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (new_buffer == NULL) {
        new_buffer = heap_caps_malloc(required_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (new_buffer == NULL) {
        new_buffer = malloc(required_size);
    }
    if (new_buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (s_playback_scratch != NULL && s_playback_scratch_size > 0) {
        memcpy(new_buffer, s_playback_scratch, s_playback_scratch_size);
        free(s_playback_scratch);
    }

    s_playback_scratch = new_buffer;
    s_playback_scratch_size = required_size;
    return ESP_OK;
}

static void *audio_calloc_prefer_psram(size_t count, size_t size)
{
    void *buffer = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = calloc(count, size);
    }
    return buffer;
}

static esp_err_t audio_ensure_capture_buffers(void)
{
    const size_t samples_per_frame = AUDIO_CAPTURE_UPLOAD_SAMPLE_RATE_HZ / 50;
    const size_t raw_samples_per_frame = samples_per_frame * AUDIO_CAPTURE_DOWNSAMPLE_RATIO *
                                         AUDIO_CAPTURE_HW_INPUT_CHANNELS;

    bool complete = s_capture_free_queue != NULL && s_capture_ready_queue != NULL &&
                    s_capture_mono_buffer != NULL;
#if AUDIO_CAPTURE_REFERENCE_ENABLED
    complete = complete && s_capture_ref_buffer != NULL;
#endif
    for (size_t index = 0; index < AUDIO_CAPTURE_PIPELINE_DEPTH; ++index) {
        complete = complete && s_capture_pipeline[index].raw_samples != NULL;
    }
    if (complete) {
        return ESP_OK;
    }

    /* Capture tasks are not running while this function repairs a partial setup. */
    audio_release_capture_buffers();

    s_capture_free_queue = xQueueCreateWithCaps(AUDIO_CAPTURE_PIPELINE_DEPTH,
                                                sizeof(uint8_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_capture_ready_queue = xQueueCreateWithCaps(AUDIO_CAPTURE_PIPELINE_DEPTH,
                                                 sizeof(uint8_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_capture_free_queue == NULL || s_capture_ready_queue == NULL) {
        audio_release_capture_buffers();
        return ESP_ERR_NO_MEM;
    }

    for (uint8_t index = 0; index < AUDIO_CAPTURE_PIPELINE_DEPTH; ++index) {
        s_capture_pipeline[index].raw_samples =
            audio_calloc_prefer_psram(raw_samples_per_frame, sizeof(int16_t));
        if (s_capture_pipeline[index].raw_samples == NULL ||
            xQueueSend(s_capture_free_queue, &index, 0) != pdTRUE) {
            audio_release_capture_buffers();
            return ESP_ERR_NO_MEM;
        }
    }

    s_capture_mono_buffer = audio_calloc_prefer_psram(samples_per_frame, sizeof(int16_t));
    if (s_capture_mono_buffer == NULL) {
        audio_release_capture_buffers();
        return ESP_ERR_NO_MEM;
    }

#if AUDIO_CAPTURE_REFERENCE_ENABLED
    s_capture_ref_buffer = audio_calloc_prefer_psram(samples_per_frame, sizeof(int16_t));
    if (s_capture_ref_buffer == NULL) {
        audio_release_capture_buffers();
        return ESP_ERR_NO_MEM;
    }
#endif
    return ESP_OK;
}

static void audio_release_capture_buffers(void)
{
    if (s_capture_free_queue != NULL) {
        vQueueDeleteWithCaps(s_capture_free_queue);
        s_capture_free_queue = NULL;
    }
    if (s_capture_ready_queue != NULL) {
        vQueueDeleteWithCaps(s_capture_ready_queue);
        s_capture_ready_queue = NULL;
    }
    for (size_t index = 0; index < AUDIO_CAPTURE_PIPELINE_DEPTH; ++index) {
        free(s_capture_pipeline[index].raw_samples);
        memset(&s_capture_pipeline[index], 0, sizeof(s_capture_pipeline[index]));
    }
    free(s_capture_mono_buffer);
    free(s_capture_ref_buffer);
    s_capture_mono_buffer = NULL;
    s_capture_ref_buffer = NULL;
}

static void audio_delete_codec_interfaces(const audio_codec_if_t **codec_if,
                                          const audio_codec_ctrl_if_t **ctrl_if,
                                          const audio_codec_gpio_if_t **gpio_if)
{
    if (codec_if != NULL && *codec_if != NULL) {
        (void)audio_codec_delete_codec_if(*codec_if);
        *codec_if = NULL;
    }
    if (ctrl_if != NULL && *ctrl_if != NULL) {
        (void)audio_codec_delete_ctrl_if(*ctrl_if);
        *ctrl_if = NULL;
    }
    if (gpio_if != NULL && *gpio_if != NULL) {
        (void)audio_codec_delete_gpio_if(*gpio_if);
        *gpio_if = NULL;
    }
}

static void audio_release_codec_instance(esp_codec_dev_handle_t *dev_handle,
                                         const audio_codec_if_t **codec_if,
                                         const audio_codec_ctrl_if_t **ctrl_if,
                                         const audio_codec_gpio_if_t **gpio_if)
{
    if (dev_handle != NULL && *dev_handle != NULL) {
        esp_codec_dev_close(*dev_handle);
        esp_codec_dev_delete(*dev_handle);
        *dev_handle = NULL;
    }

    audio_delete_codec_interfaces(codec_if, ctrl_if, gpio_if);
}

static void audio_release_shared_es8311_device(void)
{
    esp_codec_dev_handle_t shared_handle = s_shared_es8311_dev_handle;

    if (shared_handle != NULL) {
        esp_codec_dev_close(shared_handle);
        esp_codec_dev_delete(shared_handle);
    }
    if (s_play_dev_handle == shared_handle) {
        s_play_dev_handle = NULL;
    }
    if (s_record_dev_handle == shared_handle) {
        s_record_dev_handle = NULL;
    }
    s_shared_es8311_dev_handle = NULL;

    audio_delete_codec_interfaces(&s_shared_es8311_codec_if,
                                  &s_shared_es8311_ctrl_if,
                                  &s_shared_es8311_gpio_if);
}

static void audio_release_speaker_device(void)
{
    if (s_play_dev_handle == NULL) {
        return;
    }

    if (s_play_dev_handle == s_shared_es8311_dev_handle) {
        if (s_audio_input_ready) {
            return;
        }
        audio_release_shared_es8311_device();
        return;
    }

    audio_release_codec_instance(&s_play_dev_handle,
                                 &s_play_codec_if,
                                 &s_play_ctrl_if,
                                 &s_play_gpio_if);
}

static void audio_release_microphone_device(void)
{
    if (s_record_dev_handle == NULL) {
        return;
    }

    if (s_record_dev_handle == s_shared_es8311_dev_handle) {
        if (s_audio_output_ready) {
            return;
        }
        audio_release_shared_es8311_device();
        return;
    }

    audio_release_codec_instance(&s_record_dev_handle,
                                 &s_record_codec_if,
                                 &s_record_ctrl_if,
                                 &s_record_gpio_if);
}

static void audio_release_i2s_channel(i2s_chan_handle_t *channel)
{
    if (channel == NULL || *channel == NULL) {
        return;
    }

    i2s_chan_info_t chan_info = {0};
    if (i2s_channel_get_info(*channel, &chan_info) == ESP_OK && chan_info.is_enabled) {
        (void)i2s_channel_disable(*channel);
    }
    (void)i2s_del_channel(*channel);
    *channel = NULL;
}

static void audio_release_i2s_bus(void)
{
    if (s_i2s_data_if != NULL) {
        (void)audio_codec_delete_data_if(s_i2s_data_if);
        s_i2s_data_if = NULL;
    }
    audio_release_i2s_channel(&s_i2s_rx_chan);
    audio_release_i2s_channel(&s_i2s_tx_chan);
}

static void audio_cleanup_output_prepare_failure(void)
{
    s_audio_output_ready = false;
    s_speaker_path_enabled = false;
    audio_release_speaker_device();
    if (!s_audio_input_ready) {
        audio_release_i2s_bus();
    }
    audio_update_ready_state();
}

static void audio_cleanup_input_prepare_failure(void)
{
    s_audio_input_ready = false;
    s_capture_primary_enabled = false;
    audio_release_microphone_device();
    if (!s_audio_output_ready) {
        audio_release_i2s_bus();
    }
    audio_release_capture_buffers();
    audio_update_ready_state();
}

static esp_err_t audio_open_speaker(uint32_t sample_rate_hz, uint32_t bits_per_sample, uint32_t channels)
{
    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = sample_rate_hz,
        .bits_per_sample = bits_per_sample,
        .channel = channels,
    };

    ESP_RETURN_ON_FALSE(s_play_dev_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "speaker codec handle missing");
    return esp_codec_dev_open(s_play_dev_handle, &sample_info);
}

static esp_err_t audio_apply_primary_microphone_gain(uint8_t percent)
{
    const hardware_audio_config_t *audio_config = hardware_board_get_audio_config();
    float db = audio_capture_gain_percent_to_db(percent);

    ESP_RETURN_ON_FALSE(s_record_dev_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "mic codec handle missing");

    if (audio_config != NULL && audio_config->microphone_codec == HARDWARE_AUDIO_CODEC_ES7210) {
        return esp_codec_dev_set_in_channel_gain(
            s_record_dev_handle,
            ESP_CODEC_DEV_MAKE_CHANNEL_MASK(AUDIO_CAPTURE_PRIMARY_CHANNEL),
            db);
    }

    return esp_codec_dev_set_in_gain(s_record_dev_handle, db);
}

static esp_err_t audio_configure_microphone_gains(uint8_t primary_percent)
{
    const hardware_audio_config_t *audio_config = hardware_board_get_audio_config();

    ESP_RETURN_ON_ERROR(audio_apply_primary_microphone_gain(primary_percent),
                        TAG,
                        "set primary mic gain failed");

#if AUDIO_CAPTURE_REFERENCE_ENABLED
    if (audio_config != NULL && audio_config->microphone_codec == HARDWARE_AUDIO_CODEC_ES7210) {
        /*
         * DMA channel indexes are not ES7210 physical input indexes. The I2S
         * driver packs wire slots 0 and 1 as MIC1 and physical MIC3. User gain
         * owns only MIC1; MIC3 is an electrical playback reference and is
         * calibrated once when the codec opens. Runtime volume changes must not
         * rewrite or read the reference channel while I2S capture is active.
         */
        ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_channel_gain(
                                s_record_dev_handle,
                                ESP_CODEC_DEV_MAKE_CHANNEL_MASK(AUDIO_CAPTURE_REFERENCE_CODEC_CHANNEL),
                                HARDWARE_BOARD_AUDIO_ADC_REFERENCE_GAIN_DB),
                            TAG,
                            "set AEC reference gain failed");

        int primary_gain_reg = 0;
        int reference_gain_reg = 0;
        ESP_RETURN_ON_ERROR(esp_codec_dev_read_reg(s_record_dev_handle,
                                                    AUDIO_ES7210_MIC1_GAIN_REG,
                                                    &primary_gain_reg),
                            TAG,
                            "read primary mic gain failed");
        ESP_RETURN_ON_ERROR(esp_codec_dev_read_reg(s_record_dev_handle,
                                                    AUDIO_ES7210_MIC3_GAIN_REG,
                                                    &reference_gain_reg),
                            TAG,
                            "read AEC reference gain failed");
        ESP_LOGD(TAG,
                 "ES7210 gain verified: mic1_reg=0x%02x mic3_ref_reg=0x%02x",
                 primary_gain_reg & 0xff,
                 reference_gain_reg & 0xff);
    }
#else
    (void)audio_config;
#endif
    return ESP_OK;
}

static esp_err_t audio_open_microphone(uint32_t sample_rate_hz, uint32_t bits_per_sample, uint32_t channels)
{
    const hardware_audio_config_t *audio_config = hardware_board_get_audio_config();
    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = sample_rate_hz,
        .bits_per_sample = bits_per_sample,
        .channel = channels,
    };

    if (audio_config != NULL && audio_config->microphone_codec == HARDWARE_AUDIO_CODEC_ES7210) {
        sample_info.channel = AUDIO_CAPTURE_HW_TOTAL_CHANNELS;
        sample_info.channel_mask = AUDIO_CAPTURE_HW_CHANNEL_MASK;
    }

    ESP_RETURN_ON_FALSE(s_record_dev_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "mic codec handle missing");
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_record_dev_handle, &sample_info), TAG, "open mic codec failed");
    return audio_configure_microphone_gains(s_capture_processing_config.codec_gain_percent);
}

static esp_err_t audio_bus_init(void)
{
    const hardware_audio_config_t *audio_config = hardware_board_get_audio_config();

    if (s_i2s_data_if != NULL && s_i2s_tx_chan != NULL && s_i2s_rx_chan != NULL) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(HARDWARE_BOARD_AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = AUDIO_I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = AUDIO_I2S_DMA_FRAME_NUM;

    if (s_i2s_tx_chan == NULL && s_i2s_rx_chan == NULL) {
        audio_log_heap("i2s before duplex new_channel");
        ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx_chan, &s_i2s_rx_chan),
                            TAG,
                            "create i2s duplex channel failed");
    } else if (s_i2s_tx_chan == NULL || s_i2s_rx_chan == NULL) {
        audio_release_i2s_bus();
        return ESP_ERR_INVALID_STATE;
    }

    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_PLAYBACK_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = HARDWARE_BOARD_AUDIO_MCLK,
            .bclk = HARDWARE_BOARD_AUDIO_BCLK,
            .ws = HARDWARE_BOARD_AUDIO_LRCK,
            .dout = HARDWARE_BOARD_AUDIO_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };

    i2s_std_slot_config_t slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    i2s_std_config_t rx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_CAPTURE_HW_SAMPLE_RATE_HZ),
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = HARDWARE_BOARD_AUDIO_MCLK,
            .bclk = HARDWARE_BOARD_AUDIO_BCLK,
            .ws = HARDWARE_BOARD_AUDIO_LRCK,
            .dout = I2S_GPIO_UNUSED,
            .din = HARDWARE_BOARD_AUDIO_DIN,
        },
    };

    i2s_tdm_config_t rx_tdm_cfg = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(AUDIO_CAPTURE_HW_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO,
                                                        I2S_TDM_SLOT0 | I2S_TDM_SLOT1 |
                                                            I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {
            .mclk = HARDWARE_BOARD_AUDIO_MCLK,
            .bclk = HARDWARE_BOARD_AUDIO_BCLK,
            .ws = HARDWARE_BOARD_AUDIO_LRCK,
            .dout = I2S_GPIO_UNUSED,
            .din = HARDWARE_BOARD_AUDIO_DIN,
        },
    };

    if (s_i2s_data_if == NULL) {
        esp_err_t ret = i2s_channel_init_std_mode(s_i2s_tx_chan, &tx_std_cfg);
        if (ret != ESP_OK) {
            audio_log_heap("i2s tx init failed");
            audio_release_i2s_bus();
            return ret;
        }
        ret = audio_config != NULL && audio_config->microphone_codec == HARDWARE_AUDIO_CODEC_ES7210 ?
                  i2s_channel_init_tdm_mode(s_i2s_rx_chan, &rx_tdm_cfg) :
                  i2s_channel_init_std_mode(s_i2s_rx_chan, &rx_std_cfg);
        if (ret != ESP_OK) {
            audio_log_heap("i2s rx init failed");
            audio_release_i2s_bus();
            return ret;
        }

        audio_codec_i2s_cfg_t i2s_cfg = {
            .port = HARDWARE_BOARD_AUDIO_I2S_PORT,
            .rx_handle = s_i2s_rx_chan,
            .tx_handle = s_i2s_tx_chan,
        };
        s_i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    }

    return s_i2s_data_if == NULL ? ESP_FAIL : ESP_OK;
}

static esp_err_t audio_do_prepare_output(void)
{
    esp_err_t ret = hardware_board_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = audio_bus_init();
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_play_dev_handle == NULL) {
        s_play_dev_handle = audio_new_speaker();
    }
    if (s_play_dev_handle == NULL) {
        return ESP_FAIL;
    }

    ret = audio_open_speaker(s_playback_format.sample_rate_hz,
                                      s_playback_format.bits_per_sample,
                                      s_playback_format.channels);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_codec_dev_set_out_vol(s_play_dev_handle, s_speaker_volume_percent);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_codec_dev_set_out_mute(s_play_dev_handle, true);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = hardware_board_set_audio_power(false);
    if (ret != ESP_OK) {
        return ret;
    }
    s_speaker_path_enabled = false;

    return ESP_OK;
}

static esp_err_t audio_do_prepare_input(void)
{
    esp_err_t ret = hardware_board_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = audio_bus_init();
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_record_dev_handle == NULL) {
        s_record_dev_handle = audio_new_microphone();
    }
    if (s_record_dev_handle == NULL) {
        return ESP_FAIL;
    }

    ret = audio_open_microphone(AUDIO_CAPTURE_HW_SAMPLE_RATE_HZ,
                                         HARDWARE_BOARD_AUDIO_BITS_PER_SAMPLE,
                                         AUDIO_CAPTURE_HW_INPUT_CHANNELS);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = audio_ensure_capture_buffers();
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_capture_process_task == NULL) {
        BaseType_t task_ok = xTaskCreatePinnedToCoreWithCaps(audio_capture_process_task,
                                                             "audio_process",
                                                             AUDIO_CAPTURE_PROCESS_TASK_STACK,
                                                             NULL,
                                                             AUDIO_CAPTURE_PROCESS_TASK_PRIORITY,
                                                             &s_capture_process_task,
                                                             AUDIO_CAPTURE_PROCESS_TASK_CORE,
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (task_ok != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_capture_task == NULL) {
        BaseType_t task_ok = xTaskCreatePinnedToCoreWithCaps(audio_capture_task,
                                                             "audio_capture",
                                                             AUDIO_CAPTURE_READER_TASK_STACK,
                                                             NULL,
                                                             AUDIO_CAPTURE_READER_TASK_PRIORITY,
                                                             &s_capture_task,
                                                             AUDIO_CAPTURE_READER_TASK_CORE,
                                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (task_ok != pdPASS) {
            taskENTER_CRITICAL(&s_audio_lock);
            s_capture_task_stop_requested = true;
            taskEXIT_CRITICAL(&s_audio_lock);
            TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(AUDIO_CAPTURE_TASK_STOP_WAIT_MS);
            while (s_capture_process_task != NULL && xTaskGetTickCount() < deadline) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

static bool audio_uses_shared_es8311_codec(const hardware_audio_config_t *audio_config)
{
    return audio_config != NULL &&
           audio_config->speaker_codec == HARDWARE_AUDIO_CODEC_ES8311 &&
           audio_config->microphone_codec == HARDWARE_AUDIO_CODEC_ES8311 &&
           audio_config->speaker_codec_i2c_addr == audio_config->microphone_codec_i2c_addr;
}

static const audio_codec_if_t *audio_create_es8311_codec_if(const hardware_audio_config_t *audio_config,
                                                            uint8_t i2c_addr,
                                                            esp_codec_dec_work_mode_t codec_mode,
                                                            const audio_codec_ctrl_if_t **ctrl_if_slot,
                                                            const audio_codec_gpio_if_t **gpio_if_slot)
{
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (gpio_if == NULL) {
        return NULL;
    }
    if (gpio_if_slot != NULL) {
        *gpio_if_slot = gpio_if;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = hardware_board_get_i2c_config()->port,
        .addr = i2c_addr,
        .bus_handle = hardware_board_get_i2c_bus_handle(),
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (i2c_ctrl_if == NULL) {
        return NULL;
    }
    if (ctrl_if_slot != NULL) {
        *ctrl_if_slot = i2c_ctrl_if;
    }

    esp_codec_dev_hw_gain_t hw_gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };
    es8311_codec_cfg_t codec_cfg = {
        .ctrl_if = i2c_ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = codec_mode,
        .pa_pin = audio_config->pa_gpio,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = hw_gain,
    };

    return es8311_codec_new(&codec_cfg);
}

static esp_codec_dev_handle_t audio_new_shared_es8311_device(void)
{
    const hardware_audio_config_t *audio_config = hardware_board_get_audio_config();

    if (s_shared_es8311_dev_handle != NULL) {
        s_play_dev_handle = s_shared_es8311_dev_handle;
        s_record_dev_handle = s_shared_es8311_dev_handle;
        return s_shared_es8311_dev_handle;
    }

    if (s_i2s_data_if == NULL) {
        return NULL;
    }

    s_shared_es8311_codec_if = audio_create_es8311_codec_if(audio_config,
                                                            audio_config->speaker_codec_i2c_addr,
                                                            ESP_CODEC_DEV_WORK_MODE_BOTH,
                                                            &s_shared_es8311_ctrl_if,
                                                            &s_shared_es8311_gpio_if);
    if (s_shared_es8311_codec_if == NULL) {
        audio_release_shared_es8311_device();
        return NULL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = s_shared_es8311_codec_if,
        .data_if = s_i2s_data_if,
    };
    s_shared_es8311_dev_handle = esp_codec_dev_new(&dev_cfg);
    if (s_shared_es8311_dev_handle == NULL) {
        audio_release_shared_es8311_device();
        return NULL;
    }

    s_play_dev_handle = s_shared_es8311_dev_handle;
    s_record_dev_handle = s_shared_es8311_dev_handle;
    return s_shared_es8311_dev_handle;
}

static esp_codec_dev_handle_t audio_new_speaker(void)
{
    const hardware_audio_config_t *audio_config = hardware_board_get_audio_config();

    if (s_i2s_data_if == NULL) {
        return NULL;
    }
    if (audio_uses_shared_es8311_codec(audio_config)) {
        return audio_new_shared_es8311_device();
    }
    if (audio_config->speaker_codec != HARDWARE_AUDIO_CODEC_ES8311) {
        ESP_LOGE(TAG, "unsupported speaker codec: %d", (int)audio_config->speaker_codec);
        return NULL;
    }

    s_play_codec_if = audio_create_es8311_codec_if(audio_config,
                                                   audio_config->speaker_codec_i2c_addr,
                                                   ESP_CODEC_DEV_WORK_MODE_DAC,
                                                   &s_play_ctrl_if,
                                                   &s_play_gpio_if);
    if (s_play_codec_if == NULL) {
        audio_release_codec_instance(&s_play_dev_handle,
                                     &s_play_codec_if,
                                     &s_play_ctrl_if,
                                     &s_play_gpio_if);
        return NULL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_play_codec_if,
        .data_if = s_i2s_data_if,
    };
    s_play_dev_handle = esp_codec_dev_new(&dev_cfg);
    if (s_play_dev_handle == NULL) {
        audio_release_codec_instance(&s_play_dev_handle,
                                     &s_play_codec_if,
                                     &s_play_ctrl_if,
                                     &s_play_gpio_if);
    }
    return s_play_dev_handle;
}

static esp_codec_dev_handle_t audio_new_microphone(void)
{
    const hardware_audio_config_t *audio_config = hardware_board_get_audio_config();

    if (s_i2s_data_if == NULL) {
        return NULL;
    }
    if (audio_uses_shared_es8311_codec(audio_config)) {
        return audio_new_shared_es8311_device();
    }

    if (audio_config->microphone_codec == HARDWARE_AUDIO_CODEC_ES8311) {
        s_record_codec_if = audio_create_es8311_codec_if(audio_config,
                                                         audio_config->microphone_codec_i2c_addr,
                                                         ESP_CODEC_DEV_WORK_MODE_ADC,
                                                         &s_record_ctrl_if,
                                                         &s_record_gpio_if);
        if (s_record_codec_if == NULL) {
            audio_release_codec_instance(&s_record_dev_handle,
                                         &s_record_codec_if,
                                         &s_record_ctrl_if,
                                         &s_record_gpio_if);
            return NULL;
        }

        esp_codec_dev_cfg_t dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN,
            .codec_if = s_record_codec_if,
            .data_if = s_i2s_data_if,
        };
        s_record_dev_handle = esp_codec_dev_new(&dev_cfg);
        if (s_record_dev_handle == NULL) {
            audio_release_codec_instance(&s_record_dev_handle,
                                         &s_record_codec_if,
                                         &s_record_ctrl_if,
                                         &s_record_gpio_if);
        }
        return s_record_dev_handle;
    }

    if (audio_config->microphone_codec != HARDWARE_AUDIO_CODEC_ES7210) {
        ESP_LOGE(TAG, "unsupported microphone codec: %d", (int)audio_config->microphone_codec);
        return NULL;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = hardware_board_get_i2c_config()->port,
        .addr = audio_config->microphone_codec_i2c_addr,
        .bus_handle = hardware_board_get_i2c_bus_handle(),
    };
    s_record_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (s_record_ctrl_if == NULL) {
        return NULL;
    }

    es7210_codec_cfg_t codec_cfg = {
        .ctrl_if = s_record_ctrl_if,
        .mic_selected = audio_config->microphone_select_mask,
    };
    s_record_codec_if = es7210_codec_new(&codec_cfg);
    if (s_record_codec_if == NULL) {
        audio_release_codec_instance(&s_record_dev_handle,
                                     &s_record_codec_if,
                                     &s_record_ctrl_if,
                                     &s_record_gpio_if);
        return NULL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_record_codec_if,
        .data_if = s_i2s_data_if,
    };
    s_record_dev_handle = esp_codec_dev_new(&dev_cfg);
    if (s_record_dev_handle == NULL) {
        audio_release_codec_instance(&s_record_dev_handle,
                                     &s_record_codec_if,
                                     &s_record_ctrl_if,
                                     &s_record_gpio_if);
    }
    return s_record_dev_handle;
}

static void audio_capture_task(void *ctx)
{
    (void)ctx;
    const size_t samples_per_frame = AUDIO_CAPTURE_UPLOAD_SAMPLE_RATE_HZ / 50;
    const size_t raw_samples_per_frame = samples_per_frame * AUDIO_CAPTURE_DOWNSAMPLE_RATIO *
                                         AUDIO_CAPTURE_HW_INPUT_CHANNELS;
    const size_t raw_frame_bytes = raw_samples_per_frame * sizeof(int16_t);
    int64_t last_frame_start_us = 0;
    uint64_t timing_interval_sum_us = 0;
    uint64_t timing_read_sum_us = 0;
    uint32_t timing_interval_max_us = 0;
    uint32_t timing_interval_count = 0;
    uint32_t timing_window_frames = 0;

    while (true) {
        bool stop_requested = false;
        bool capture_enabled = false;

        taskENTER_CRITICAL(&s_audio_lock);
        stop_requested = s_capture_task_stop_requested;
        capture_enabled = audio_capture_has_active_consumer_locked();
        taskEXIT_CRITICAL(&s_audio_lock);

        if (stop_requested) {
            taskENTER_CRITICAL(&s_audio_lock);
            s_capture_task = NULL;
            s_audio_input_ready = false;
            s_capture_primary_enabled = false;
            s_audio_stats.capture_enabled = false;
            s_audio_stats.input_level = 0;
            if (s_capture_process_task == NULL) {
                s_capture_task_stop_requested = false;
            }
            taskEXIT_CRITICAL(&s_audio_lock);
            audio_update_ready_state();
            platform_task_reaper_delete_current_with_caps(TAG);
            return;
        }

        if (!capture_enabled) {
            last_frame_start_us = 0;
            timing_interval_sum_us = 0;
            timing_read_sum_us = 0;
            timing_interval_max_us = 0;
            timing_interval_count = 0;
            timing_window_frames = 0;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        uint8_t slot_index = 0;
        if (xQueueReceive(s_capture_free_queue, &slot_index, pdMS_TO_TICKS(20)) != pdTRUE ||
            slot_index >= AUDIO_CAPTURE_PIPELINE_DEPTH ||
            s_capture_pipeline[slot_index].raw_samples == NULL) {
            taskENTER_CRITICAL(&s_audio_lock);
            s_audio_stats.capture_pipeline_overruns++;
            taskEXIT_CRITICAL(&s_audio_lock);
            continue;
        }

        int64_t frame_start_us = esp_timer_get_time();
        esp_err_t ret = esp_codec_dev_read(s_record_dev_handle,
                                           s_capture_pipeline[slot_index].raw_samples,
                                           raw_frame_bytes);
        int64_t read_done_us = esp_timer_get_time();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "audio capture read failed: %s", esp_err_to_name(ret));
            taskENTER_CRITICAL(&s_audio_lock);
            s_audio_stats.capture_read_errors++;
            taskEXIT_CRITICAL(&s_audio_lock);
            (void)xQueueSend(s_capture_free_queue, &slot_index, 0);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        taskENTER_CRITICAL(&s_audio_lock);
        capture_enabled = audio_capture_has_active_consumer_locked();
        taskEXIT_CRITICAL(&s_audio_lock);
        if (!capture_enabled) {
            (void)xQueueSend(s_capture_free_queue, &slot_index, 0);
            continue;
        }

        s_capture_pipeline[slot_index].frame_start_us = frame_start_us;
        s_capture_pipeline[slot_index].read_us = (uint32_t)(read_done_us - frame_start_us);
        if (xQueueSend(s_capture_ready_queue, &slot_index, 0) != pdTRUE) {
            taskENTER_CRITICAL(&s_audio_lock);
            s_audio_stats.capture_pipeline_overruns++;
            taskEXIT_CRITICAL(&s_audio_lock);
            (void)xQueueSend(s_capture_free_queue, &slot_index, 0);
            continue;
        }

        UBaseType_t ready_depth = uxQueueMessagesWaiting(s_capture_ready_queue);
        taskENTER_CRITICAL(&s_audio_lock);
        if (ready_depth > s_audio_stats.capture_pipeline_high_water) {
            s_audio_stats.capture_pipeline_high_water = ready_depth;
        }
        taskEXIT_CRITICAL(&s_audio_lock);

        uint32_t interval_us = 0;
        if (last_frame_start_us > 0 && frame_start_us > last_frame_start_us) {
            interval_us = (uint32_t)(frame_start_us - last_frame_start_us);
            timing_interval_sum_us += interval_us;
            timing_interval_count++;
            if (interval_us > timing_interval_max_us) {
                timing_interval_max_us = interval_us;
            }
        }
        last_frame_start_us = frame_start_us;
        timing_read_sum_us += (uint32_t)(read_done_us - frame_start_us);
        timing_window_frames++;
        if (timing_window_frames >= 50U) {
            taskENTER_CRITICAL(&s_audio_lock);
            s_audio_stats.capture_interval_us = timing_interval_count > 0U ?
                (uint32_t)(timing_interval_sum_us / timing_interval_count) : 0U;
            s_audio_stats.capture_interval_max_us = timing_interval_max_us;
            s_audio_stats.capture_read_us = (uint32_t)(timing_read_sum_us / timing_window_frames);
            taskEXIT_CRITICAL(&s_audio_lock);
            timing_interval_sum_us = 0;
            timing_read_sum_us = 0;
            timing_interval_max_us = 0;
            timing_interval_count = 0;
            timing_window_frames = 0;
        }
    }
}

static void audio_capture_process_task(void *ctx)
{
    (void)ctx;
    const size_t samples_per_frame = AUDIO_CAPTURE_UPLOAD_SAMPLE_RATE_HZ / 50;
    int16_t *mono_buffer = s_capture_mono_buffer;
    int16_t *ref_buffer = s_capture_ref_buffer;
    TickType_t last_level_log_tick = 0;
    bool reference_path_logged = false;
    uint32_t log_raw_channel_peak[AUDIO_CAPTURE_HW_INPUT_CHANNELS] = {0};
    uint32_t log_scaled_reference_peak = 0;
    uint32_t log_pre_gain_peak = 0;
    uint32_t log_post_gain_peak = 0;
    uint64_t log_pre_gain_square_sum = 0;
    uint64_t log_post_gain_square_sum = 0;
    uint32_t log_echo_ref_peak = 0;
    uint32_t log_echo_mic_peak = 0;
    uint32_t log_echo_out_peak = 0;
    uint32_t log_echo_reference_frames = 0;
    uint32_t log_echo_active_frames = 0;
    uint32_t log_echo_bypass_frames = 0;
    uint32_t log_echo_warmup_frames = 0;
    uint32_t log_echo_warmup_passthrough_frames = 0;
    uint32_t log_echo_near_end_detected_frames = 0;
    uint32_t log_echo_near_end_frames = 0;
    uint32_t log_echo_near_last_sequence = 0;
    uint32_t log_echo_near_decisions = 0;
    uint32_t log_echo_near_reject_low_peak = 0;
    uint32_t log_echo_near_reject_low_retained = 0;
    uint32_t log_echo_near_reject_low_nlp = 0;
    uint32_t log_echo_near_reject_high_coherence = 0;
    uint8_t log_echo_near_retained_energy_percent = 0;
    uint8_t log_echo_near_nlp_reduction_percent = 0;
    uint8_t log_echo_near_reference_coherence_percent = 0;
    uint32_t log_echo_linear_peak = 0;
    uint32_t log_far_end_guard_frames = 0;
    uint32_t log_echo_suppress_percent = 0;
    uint32_t log_noise_gate_closed_frames = 0;
    uint32_t log_sample_count = 0;
    uint32_t log_frame_count = 0;
    uint32_t log_auto_gain_q8 = AUDIO_CAPTURE_AUTO_GAIN_MIN_Q8;
    uint32_t auto_gain_q8 = AUDIO_CAPTURE_AUTO_GAIN_MIN_Q8;
    bool noise_gate_open = false;
    int32_t high_pass_previous_input = 0;
    int32_t high_pass_previous_output = 0;
    bool high_pass_state_valid = false;
    bool high_pass_was_enabled = false;
    uint64_t timing_process_sum_us = 0;
    uint64_t timing_dispatch_sum_us = 0;
    uint64_t timing_pipeline_wait_sum_us = 0;
    uint32_t timing_pipeline_wait_max_us = 0;
    uint32_t timing_window_frames = 0;

    if (mono_buffer == NULL
#if AUDIO_CAPTURE_REFERENCE_ENABLED
        || ref_buffer == NULL
#endif
    ) {
        ESP_LOGE(TAG, "audio capture buffers alloc failed");
        taskENTER_CRITICAL(&s_audio_lock);
        s_capture_process_task = NULL;
        s_capture_task_stop_requested = true;
        taskEXIT_CRITICAL(&s_audio_lock);
        platform_task_reaper_delete_current_with_caps(TAG);
        return;
    }

    while (true) {
        bool stop_requested = false;
        bool capture_primary_enabled = false;
        bool capture_enabled = false;
        audio_capture_frame_cb_t capture_cb = NULL;
        void *capture_cb_ctx = NULL;
        audio_capture_observer_t observers[AUDIO_CAPTURE_OBSERVER_MAX] = {0};
        size_t observer_count = 0;

        taskENTER_CRITICAL(&s_audio_lock);
        stop_requested = s_capture_task_stop_requested;
        taskEXIT_CRITICAL(&s_audio_lock);
        if (stop_requested) {
            taskENTER_CRITICAL(&s_audio_lock);
            s_capture_process_task = NULL;
            if (s_capture_task == NULL) {
                s_capture_task_stop_requested = false;
            }
            taskEXIT_CRITICAL(&s_audio_lock);
            platform_task_reaper_delete_current_with_caps(TAG);
            return;
        }

        uint8_t slot_index = 0;
        if (xQueueReceive(s_capture_ready_queue, &slot_index, pdMS_TO_TICKS(20)) != pdTRUE) {
            continue;
        }
        if (slot_index >= AUDIO_CAPTURE_PIPELINE_DEPTH ||
            s_capture_pipeline[slot_index].raw_samples == NULL) {
            continue;
        }

        int16_t *raw_buffer = s_capture_pipeline[slot_index].raw_samples;
        int64_t frame_start_us = s_capture_pipeline[slot_index].frame_start_us;
        int64_t process_start_us = esp_timer_get_time();
        uint32_t pipeline_wait_us = 0;
        int64_t read_done_us = frame_start_us + s_capture_pipeline[slot_index].read_us;
        if (process_start_us > read_done_us) {
            pipeline_wait_us = (uint32_t)(process_start_us - read_done_us);
        }

        taskENTER_CRITICAL(&s_audio_lock);
        capture_primary_enabled = s_capture_primary_enabled && s_capture_cb != NULL;
        capture_cb = s_capture_cb;
        capture_cb_ctx = s_capture_cb_ctx;
        for (size_t index = 0; index < AUDIO_CAPTURE_OBSERVER_MAX; ++index) {
            if (s_capture_observers[index].enabled && s_capture_observers[index].cb != NULL) {
                observers[observer_count++] = s_capture_observers[index];
            }
        }
        capture_enabled = capture_primary_enabled || observer_count > 0;
        if (!capture_enabled) {
            s_audio_stats.input_level = 0;
        }
        taskEXIT_CRITICAL(&s_audio_lock);

        if (!capture_enabled) {
            audio_echo_cancel_reset();
            high_pass_previous_input = 0;
            high_pass_previous_output = 0;
            high_pass_state_valid = false;
            high_pass_was_enabled = false;
            (void)xQueueSend(s_capture_free_queue, &slot_index, 0);
            continue;
        }

        uint32_t peak = 0;
        uint8_t capture_gain_percent = 0;
        uint32_t pre_frame_peak = 0;
        uint32_t base_gain_q8 = 0;
        uint32_t auto_gain_max_q8 = AUDIO_CAPTURE_AUTO_GAIN_MIN_Q8;
        bool far_end_guard_active = false;
        uint32_t reference_gain_q8 = AUDIO_CAPTURE_REFERENCE_DIGITAL_GAIN_Q8;
        audio_capture_processing_config_t processing_config = {0};
        audio_echo_cancel_metrics_t echo_metrics = {0};

        taskENTER_CRITICAL(&s_audio_lock);
        capture_gain_percent = audio_get_capture_gain_percent_locked();
        processing_config = audio_get_capture_processing_config_locked();
        taskEXIT_CRITICAL(&s_audio_lock);
        base_gain_q8 = audio_capture_base_gain_q8(processing_config.upload_gain_percent);
        auto_gain_max_q8 = audio_capture_auto_gain_max_q8(processing_config.auto_gain_max_percent);
        for (size_t frame_index = 0; frame_index < samples_per_frame; ++frame_index) {
            int32_t primary_sum = 0;
            size_t primary_sample_count = 0;
            for (size_t downsample_index = 0; downsample_index < AUDIO_CAPTURE_DOWNSAMPLE_RATIO; ++downsample_index) {
                size_t raw_base_index = (frame_index * AUDIO_CAPTURE_DOWNSAMPLE_RATIO + downsample_index) *
                                        AUDIO_CAPTURE_HW_INPUT_CHANNELS;
                for (size_t channel_index = 0; channel_index < AUDIO_CAPTURE_HW_INPUT_CHANNELS; ++channel_index) {
                    int16_t raw_sample = raw_buffer[raw_base_index + channel_index];
                    uint32_t raw_abs = audio_abs_i16(raw_sample);
                    if (raw_abs > log_raw_channel_peak[channel_index]) {
                        log_raw_channel_peak[channel_index] = raw_abs;
                    }
                    if (channel_index == AUDIO_CAPTURE_PRIMARY_CHANNEL) {
                        primary_sum += raw_sample;
                        primary_sample_count++;
                    }
                }
            }
            if (primary_sample_count > 0) {
                primary_sum /= (int32_t)primary_sample_count;
            }

            mono_buffer[frame_index] = (int16_t)primary_sum;
#if AUDIO_CAPTURE_REFERENCE_ENABLED
            int32_t reference_sum = 0;
            size_t reference_sample_count = 0;
            for (size_t downsample_index = 0; downsample_index < AUDIO_CAPTURE_DOWNSAMPLE_RATIO; ++downsample_index) {
                size_t raw_base_index = (frame_index * AUDIO_CAPTURE_DOWNSAMPLE_RATIO + downsample_index) *
                                        AUDIO_CAPTURE_HW_INPUT_CHANNELS;
                reference_sum += raw_buffer[raw_base_index + AUDIO_CAPTURE_REFERENCE_DMA_CHANNEL];
                reference_sample_count++;
            }
            if (reference_sample_count > 0) {
                reference_sum /= (int32_t)reference_sample_count;
            }
            /*
             * MIC3 is a dedicated analog speaker loopback, not user audio.
             * Keep this electrical reference linear. Gain belongs to the codec
             * calibration above; application-specific digital boosts would
             * distort the reference-to-echo relationship seen by the adaptive
             * filter and can make double-talk suppression unstable.
             */
            int16_t scaled_reference = audio_clip_i16(
                (reference_sum * (int32_t)reference_gain_q8) / 256);
            uint32_t scaled_reference_abs = audio_abs_i16(scaled_reference);
            if (scaled_reference_abs > log_scaled_reference_peak) {
                log_scaled_reference_peak = scaled_reference_abs;
            }
            ref_buffer[frame_index] = scaled_reference;
#endif
        }

#if AUDIO_CAPTURE_REFERENCE_ENABLED
        if (!reference_path_logged) {
            reference_path_logged = true;
            ESP_LOGI(TAG,
                     "audio AEC hardware reference: mic_dma=%u ref_dma=%u ref_tdm_slot=%u ref_codec_ch=%u input_mask=0x%x tdm_channels=%u",
                     AUDIO_CAPTURE_PRIMARY_CHANNEL,
                     AUDIO_CAPTURE_REFERENCE_DMA_CHANNEL,
                     AUDIO_CAPTURE_REFERENCE_TDM_SLOT,
                     AUDIO_CAPTURE_REFERENCE_CODEC_CHANNEL,
                     AUDIO_CAPTURE_HW_CHANNEL_MASK,
                     AUDIO_CAPTURE_HW_TOTAL_CHANNELS);
        }
        audio_echo_cancel_process_capture_with_reference(
            mono_buffer,
            ref_buffer,
            samples_per_frame,
            processing_config.echo_continuous_processing,
            processing_config.echo_near_end_protection_enabled,
            &echo_metrics);
#else
        audio_echo_cancel_process_capture(mono_buffer, samples_per_frame, &echo_metrics);
#endif
        /*
         * Keep the conservative gain cap while the microphone contains only
         * far-end playback. Once AEC has positively identified near-end
         * speech, retaining that cap attenuates consecutive words even though
         * the echo path is already being protected by the AEC near-end blend.
         */
        far_end_guard_active = processing_config.far_end_gain_guard_enabled &&
                               echo_metrics.reference_active &&
                               !echo_metrics.near_end_detected;
        if (far_end_guard_active) {
            base_gain_q8 = audio_capture_base_gain_q8(
                processing_config.far_end_upload_gain_percent);
            auto_gain_max_q8 = audio_capture_auto_gain_max_q8(
                processing_config.far_end_auto_gain_max_percent);
            log_far_end_guard_frames++;
        }
        if (echo_metrics.reference_active) {
            log_echo_reference_frames++;
            if (echo_metrics.ref_peak > log_echo_ref_peak) {
                log_echo_ref_peak = echo_metrics.ref_peak;
            }
        }
        if (echo_metrics.active) {
            log_echo_active_frames++;
            if (echo_metrics.mic_peak > log_echo_mic_peak) {
                log_echo_mic_peak = echo_metrics.mic_peak;
            }
            if (echo_metrics.out_peak > log_echo_out_peak) {
                log_echo_out_peak = echo_metrics.out_peak;
            }
            log_echo_suppress_percent = echo_metrics.suppress_percent;
        }
        if (echo_metrics.output_bypassed) {
            log_echo_bypass_frames++;
        }
        if (echo_metrics.warming_up) {
            log_echo_warmup_frames++;
            if (echo_metrics.warmup_near_end_passthrough) {
                log_echo_warmup_passthrough_frames++;
            }
        }
        if (echo_metrics.near_end_protected) {
            log_echo_near_end_frames++;
        }
        if (echo_metrics.near_end_detected) {
            log_echo_near_end_detected_frames++;
        }
        if (echo_metrics.near_decision_sequence != 0U &&
            echo_metrics.near_decision_sequence != log_echo_near_last_sequence) {
            log_echo_near_last_sequence = echo_metrics.near_decision_sequence;
            log_echo_near_decisions++;
            log_echo_near_retained_energy_percent =
                echo_metrics.near_retained_energy_percent;
            log_echo_near_nlp_reduction_percent =
                echo_metrics.near_nlp_reduction_percent;
            log_echo_near_reference_coherence_percent =
                echo_metrics.near_reference_coherence_percent;
            switch (echo_metrics.near_reject_reason) {
            case AUDIO_ECHO_NEAR_REJECT_LOW_LINEAR_PEAK:
                log_echo_near_reject_low_peak++;
                break;
            case AUDIO_ECHO_NEAR_REJECT_LOW_RETAINED_ENERGY:
                log_echo_near_reject_low_retained++;
                break;
            case AUDIO_ECHO_NEAR_REJECT_LOW_NLP_REDUCTION:
                log_echo_near_reject_low_nlp++;
                break;
            case AUDIO_ECHO_NEAR_REJECT_HIGH_REFERENCE_COHERENCE:
                log_echo_near_reject_high_coherence++;
                break;
            case AUDIO_ECHO_NEAR_REJECT_NONE:
            default:
                break;
            }
        }
        if (echo_metrics.linear_peak > log_echo_linear_peak) {
            log_echo_linear_peak = echo_metrics.linear_peak;
        }

        if (processing_config.high_pass_filter_enabled) {
            audio_capture_apply_high_pass(mono_buffer,
                                          samples_per_frame,
                                          &high_pass_previous_input,
                                          &high_pass_previous_output,
                                          &high_pass_state_valid);
            high_pass_was_enabled = true;
        } else if (high_pass_was_enabled) {
            high_pass_previous_input = 0;
            high_pass_previous_output = 0;
            high_pass_state_valid = false;
            high_pass_was_enabled = false;
        }

        uint32_t noise_gate_input_peak = 0;
        for (size_t frame_index = 0; frame_index < samples_per_frame; ++frame_index) {
            uint32_t gate_abs_value = audio_abs_i16(mono_buffer[frame_index]);
            if (gate_abs_value > noise_gate_input_peak) {
                noise_gate_input_peak = gate_abs_value;
            }
        }
        if (!audio_capture_apply_noise_gate(mono_buffer,
                                            samples_per_frame,
                                            noise_gate_input_peak,
                                            &processing_config,
                                            &noise_gate_open)) {
            log_noise_gate_closed_frames++;
        }

        for (size_t frame_index = 0; frame_index < samples_per_frame; ++frame_index) {
            uint32_t pre_abs_value = audio_abs_i16(mono_buffer[frame_index]);
            if (pre_abs_value > log_pre_gain_peak) {
                log_pre_gain_peak = pre_abs_value;
            }
            if (pre_abs_value > pre_frame_peak) {
                pre_frame_peak = pre_abs_value;
            }
            log_pre_gain_square_sum += (uint64_t)((int64_t)mono_buffer[frame_index] *
                                                  (int64_t)mono_buffer[frame_index]);
        }

        /*
         * Device-call capture keeps AEC on continuously. With no active
         * playback reference there is no echo to guard, so local speech must
         * use the near-end target as well. Tying this only to the AEC
         * double-talk detector left single-talk speech at the conservative
         * -18 dBFS target and made a distant talker unnecessarily quiet.
         */
        bool call_near_gain_active = processing_config.echo_continuous_processing &&
                                     !far_end_guard_active;
        uint32_t target_peak = call_near_gain_active ?
            AUDIO_CAPTURE_NEAR_END_AUTO_GAIN_TARGET_PEAK :
            AUDIO_CAPTURE_AUTO_GAIN_TARGET_PEAK;
        /*
         * Continuous full-duplex calls use a higher AGC floor than the generic
         * capture path. This keeps the measured AEC idle residue out of the
         * gain loop while still lifting a quiet, distant talker.
         */
        uint32_t auto_gain_noise_floor_peak =
            processing_config.echo_continuous_processing ?
                AUDIO_CAPTURE_CALL_AUTO_GAIN_NOISE_FLOOR_PEAK :
                AUDIO_CAPTURE_AUTO_GAIN_NOISE_FLOOR_PEAK;
        uint32_t target_auto_gain_q8 = audio_capture_auto_gain_target_q8(
            pre_frame_peak,
            target_peak,
            base_gain_q8,
            auto_gain_max_q8,
            auto_gain_noise_floor_peak);
        auto_gain_q8 = audio_capture_smooth_auto_gain_q8(auto_gain_q8, target_auto_gain_q8);
        log_auto_gain_q8 = auto_gain_q8;

        for (size_t frame_index = 0; frame_index < samples_per_frame; ++frame_index) {
            mono_buffer[frame_index] =
                audio_apply_capture_upload_gain(mono_buffer[frame_index], base_gain_q8, auto_gain_q8);
            uint32_t post_abs_value = audio_abs_i16(mono_buffer[frame_index]);
            if (post_abs_value > peak) {
                peak = post_abs_value;
            }
            if (post_abs_value > log_post_gain_peak) {
                log_post_gain_peak = post_abs_value;
            }
            log_post_gain_square_sum += (uint64_t)((int64_t)mono_buffer[frame_index] *
                                                   (int64_t)mono_buffer[frame_index]);
            log_sample_count++;
        }

        taskENTER_CRITICAL(&s_audio_lock);
        s_audio_stats.capture_frames++;
        s_audio_stats.input_level = audio_capture_peak_to_meter_percent(peak);
        taskEXIT_CRITICAL(&s_audio_lock);

        int64_t process_done_us = esp_timer_get_time();
        if (capture_primary_enabled) {
            capture_cb((const uint8_t *)mono_buffer,
                       samples_per_frame * sizeof(int16_t),
                       &s_capture_format,
                       capture_cb_ctx);
        }
        for (size_t index = 0; index < observer_count; ++index) {
            observers[index].cb((const uint8_t *)mono_buffer,
                                samples_per_frame * sizeof(int16_t),
                                &s_capture_format,
                                observers[index].ctx);
        }
        int64_t dispatch_done_us = esp_timer_get_time();
        (void)xQueueSend(s_capture_free_queue, &slot_index, 0);

        timing_process_sum_us += (uint32_t)(process_done_us - process_start_us);
        timing_dispatch_sum_us += (uint32_t)(dispatch_done_us - process_done_us);
        timing_pipeline_wait_sum_us += pipeline_wait_us;
        if (pipeline_wait_us > timing_pipeline_wait_max_us) {
            timing_pipeline_wait_max_us = pipeline_wait_us;
        }
        timing_window_frames++;
        if (timing_window_frames >= 50U) {
            taskENTER_CRITICAL(&s_audio_lock);
            s_audio_stats.capture_process_us =
                (uint32_t)(timing_process_sum_us / timing_window_frames);
            s_audio_stats.capture_dispatch_us =
                (uint32_t)(timing_dispatch_sum_us / timing_window_frames);
            s_audio_stats.capture_pipeline_wait_us =
                (uint32_t)(timing_pipeline_wait_sum_us / timing_window_frames);
            s_audio_stats.capture_pipeline_wait_max_us = timing_pipeline_wait_max_us;
            taskEXIT_CRITICAL(&s_audio_lock);
            timing_process_sum_us = 0;
            timing_dispatch_sum_us = 0;
            timing_pipeline_wait_sum_us = 0;
            timing_pipeline_wait_max_us = 0;
            timing_window_frames = 0;
        }

        log_frame_count++;
        TickType_t now_tick = xTaskGetTickCount();
        if (last_level_log_tick == 0 ||
            now_tick - last_level_log_tick >= pdMS_TO_TICKS(AUDIO_CAPTURE_LEVEL_LOG_INTERVAL_MS)) {
            char pre_peak_db[12] = {0};
            char pre_rms_db[12] = {0};
            char post_peak_db[12] = {0};
            char post_rms_db[12] = {0};
            char ch0_peak_db[12] = {0};
            char ch1_peak_db[12] = {0};
            int codec_gain_x10 =
                (int)(audio_capture_gain_percent_to_db(processing_config.codec_gain_percent) * 10.0f + 0.5f);
            uint32_t sw_gain_x10 = (base_gain_q8 * 10U) / 256U;
            uint32_t auto_gain_x10 = (log_auto_gain_q8 * 10U) / 256U;
            int raw_ch0_dbfs_x10 = audio_peak_dbfs_x10(log_raw_channel_peak[0]);
            int raw_ch1_dbfs_x10 = audio_peak_dbfs_x10(
                AUDIO_CAPTURE_HW_INPUT_CHANNELS > 1U ? log_raw_channel_peak[1] : 0U);
            int pre_peak_dbfs_x10 = audio_peak_dbfs_x10(log_pre_gain_peak);
            int pre_rms_dbfs_x10 = audio_rms_dbfs_x10(log_pre_gain_square_sum, log_sample_count);
            int post_peak_dbfs_x10 = audio_peak_dbfs_x10(log_post_gain_peak);
            int post_rms_dbfs_x10 = audio_rms_dbfs_x10(log_post_gain_square_sum, log_sample_count);

            taskENTER_CRITICAL(&s_audio_lock);
            s_audio_stats.capture_window_valid = true;
            s_audio_stats.capture_window_updated_ms =
                (uint32_t)(esp_timer_get_time() / 1000ULL);
            s_audio_stats.capture_raw_ch0_dbfs_x10 = raw_ch0_dbfs_x10;
            s_audio_stats.capture_raw_ch1_dbfs_x10 = raw_ch1_dbfs_x10;
            s_audio_stats.capture_pre_peak_dbfs_x10 = pre_peak_dbfs_x10;
            s_audio_stats.capture_pre_rms_dbfs_x10 = pre_rms_dbfs_x10;
            s_audio_stats.capture_post_peak_dbfs_x10 = post_peak_dbfs_x10;
            s_audio_stats.capture_post_rms_dbfs_x10 = post_rms_dbfs_x10;
            s_audio_stats.capture_base_gain_q8 = (uint16_t)base_gain_q8;
            s_audio_stats.capture_auto_gain_q8 = (uint16_t)log_auto_gain_q8;
            s_audio_stats.capture_auto_gain_target_peak = (uint16_t)target_peak;
            s_audio_stats.capture_effective_auto_gain_max_percent =
                (uint16_t)((auto_gain_max_q8 * 100U) / 256U);
            s_audio_stats.echo_reference_frames = log_echo_reference_frames;
            s_audio_stats.echo_active_frames = log_echo_active_frames;
            s_audio_stats.echo_bypass_frames = log_echo_bypass_frames;
            s_audio_stats.echo_warmup_frames = log_echo_warmup_frames;
            s_audio_stats.echo_warmup_passthrough_frames = log_echo_warmup_passthrough_frames;
            s_audio_stats.echo_near_end_detected_frames = log_echo_near_end_detected_frames;
            s_audio_stats.echo_near_end_frames = log_echo_near_end_frames;
            s_audio_stats.echo_near_decisions = log_echo_near_decisions;
            s_audio_stats.echo_near_reject_low_peak = log_echo_near_reject_low_peak;
            s_audio_stats.echo_near_reject_low_retained = log_echo_near_reject_low_retained;
            s_audio_stats.echo_near_reject_low_nlp = log_echo_near_reject_low_nlp;
            s_audio_stats.echo_near_reject_high_coherence =
                log_echo_near_reject_high_coherence;
            s_audio_stats.echo_near_retained_energy_percent =
                log_echo_near_retained_energy_percent;
            s_audio_stats.echo_near_nlp_reduction_percent =
                log_echo_near_nlp_reduction_percent;
            s_audio_stats.echo_near_reference_coherence_percent =
                log_echo_near_reference_coherence_percent;
            s_audio_stats.echo_far_end_guard_frames = log_far_end_guard_frames;
            s_audio_stats.echo_ref_peak = log_echo_ref_peak;
            s_audio_stats.echo_mic_peak = log_echo_mic_peak;
            s_audio_stats.echo_linear_peak = log_echo_linear_peak;
            s_audio_stats.echo_out_peak = log_echo_out_peak;
            s_audio_stats.echo_suppress_percent = (uint8_t)log_echo_suppress_percent;
            taskEXIT_CRITICAL(&s_audio_lock);

            audio_format_dbfs_x10(pre_peak_dbfs_x10,
                                  pre_peak_db,
                                  sizeof(pre_peak_db));
            audio_format_dbfs_x10(pre_rms_dbfs_x10,
                                  pre_rms_db,
                                  sizeof(pre_rms_db));
            audio_format_dbfs_x10(post_peak_dbfs_x10,
                                  post_peak_db,
                                  sizeof(post_peak_db));
            audio_format_dbfs_x10(post_rms_dbfs_x10,
                                  post_rms_db,
                                  sizeof(post_rms_db));
            audio_format_dbfs_x10(raw_ch0_dbfs_x10,
                                  ch0_peak_db,
                                  sizeof(ch0_peak_db));
            audio_format_dbfs_x10(raw_ch1_dbfs_x10,
                                  ch1_peak_db,
                                  sizeof(ch1_peak_db));

            if (processing_config.echo_diagnostics_enabled) {
                ESP_LOGD(TAG,
                         "call echo probe: frames=%lu ref_active=%lu aec_out=%lu bypass=%lu warmup=%lu/%lu near_end=%lu linear_peak=%lu guard=%lu ref_peak=%lu mic_peak=%lu out_peak=%lu suppress=%lu%% ref_gain=%lu.%02lux ref_scaled=%lu clip=%d codec=%d.%01ddB upload=%lu.%lux auto=%lu.%lux pre=%s/%s post=%s/%s",
                         (unsigned long)log_frame_count,
                         (unsigned long)log_echo_reference_frames,
                         (unsigned long)log_echo_active_frames,
                         (unsigned long)log_echo_bypass_frames,
                         (unsigned long)log_echo_warmup_frames,
                         (unsigned long)log_echo_warmup_passthrough_frames,
                         (unsigned long)log_echo_near_end_frames,
                         (unsigned long)log_echo_linear_peak,
                         (unsigned long)log_far_end_guard_frames,
                         (unsigned long)log_echo_ref_peak,
                         (unsigned long)log_echo_mic_peak,
                         (unsigned long)log_echo_out_peak,
                         (unsigned long)log_echo_suppress_percent,
                         (unsigned long)(reference_gain_q8 / 256U),
                         (unsigned long)(((reference_gain_q8 % 256U) * 100U) / 256U),
                         (unsigned long)log_scaled_reference_peak,
                         log_scaled_reference_peak >= AUDIO_AEC_REFERENCE_CLIP_PEAK ? 1 : 0,
                         codec_gain_x10 / 10,
                         codec_gain_x10 % 10,
                         (unsigned long)(sw_gain_x10 / 10U),
                         (unsigned long)(sw_gain_x10 % 10U),
                         (unsigned long)(auto_gain_x10 / 10U),
                         (unsigned long)(auto_gain_x10 % 10U),
                         pre_peak_db,
                         pre_rms_db,
                         post_peak_db,
                         post_rms_db);
            }

            ESP_LOGD(TAG,
                     "mic capture level: frames=%lu send=%u codec_gain=%d.%01ddB upload_gain=%lu.%lux auto_gain=%lu.%lux auto_max=%u%% noise_gate=%u/%lu primary_ch=%u ch0_peak=%sdBFS ch1_peak=%sdBFS aec_frames=%lu aec_ref=%lu aec_out=%lu aec_suppress=%lu%% pre_peak=%sdBFS pre_rms=%sdBFS post_peak=%sdBFS post_rms=%sdBFS meter=%lu",
                     (unsigned long)log_frame_count,
                     (unsigned)capture_gain_percent,
                     codec_gain_x10 / 10,
                     codec_gain_x10 % 10,
                     (unsigned long)(sw_gain_x10 / 10U),
                     (unsigned long)(sw_gain_x10 % 10U),
                     (unsigned long)(auto_gain_x10 / 10U),
                     (unsigned long)(auto_gain_x10 % 10U),
                     (unsigned)processing_config.auto_gain_max_percent,
                     processing_config.noise_gate_enabled ? 1U : 0U,
                     (unsigned long)log_noise_gate_closed_frames,
                     (unsigned)AUDIO_CAPTURE_PRIMARY_CHANNEL,
                     ch0_peak_db,
                     ch1_peak_db,
                     (unsigned long)log_echo_active_frames,
                     (unsigned long)log_echo_ref_peak,
                     (unsigned long)log_echo_out_peak,
                     (unsigned long)log_echo_suppress_percent,
                     pre_peak_db,
                     pre_rms_db,
                     post_peak_db,
                     post_rms_db,
                     (unsigned long)audio_capture_peak_to_meter_percent(log_post_gain_peak));

#if AUDIO_CAPTURE_REFERENCE_ENABLED
            if (log_scaled_reference_peak >= AUDIO_AEC_REFERENCE_CLIP_PEAK) {
                ESP_LOGW(TAG,
                         "AEC reference clipping: ref_dma=%u ref_tdm_slot=%u ref_codec_ch=%u raw_peak=%lu scaled_peak=%lu",
                         AUDIO_CAPTURE_REFERENCE_DMA_CHANNEL,
                         AUDIO_CAPTURE_REFERENCE_TDM_SLOT,
                         AUDIO_CAPTURE_REFERENCE_CODEC_CHANNEL,
                         (unsigned long)log_raw_channel_peak[AUDIO_CAPTURE_REFERENCE_DMA_CHANNEL],
                         (unsigned long)log_scaled_reference_peak);
            }
#endif

            memset(log_raw_channel_peak, 0, sizeof(log_raw_channel_peak));
            log_scaled_reference_peak = 0;
            log_pre_gain_peak = 0;
            log_post_gain_peak = 0;
            log_pre_gain_square_sum = 0;
            log_post_gain_square_sum = 0;
            log_echo_ref_peak = 0;
            log_echo_mic_peak = 0;
            log_echo_out_peak = 0;
            log_echo_reference_frames = 0;
            log_echo_active_frames = 0;
            log_echo_bypass_frames = 0;
            log_echo_warmup_frames = 0;
            log_echo_warmup_passthrough_frames = 0;
            log_echo_near_end_detected_frames = 0;
            log_echo_near_end_frames = 0;
            log_echo_near_decisions = 0;
            log_echo_near_reject_low_peak = 0;
            log_echo_near_reject_low_retained = 0;
            log_echo_near_reject_low_nlp = 0;
            log_echo_near_reject_high_coherence = 0;
            log_echo_linear_peak = 0;
            log_far_end_guard_frames = 0;
            log_echo_suppress_percent = 0;
            log_noise_gate_closed_frames = 0;
            log_sample_count = 0;
            log_frame_count = 0;
            last_level_log_tick = now_tick;
        }
    }
}

static void audio_tone_task(void *ctx)
{
    uint32_t tone_hz = ((uint32_t *)ctx)[0];
    uint32_t duration_ms = ((uint32_t *)ctx)[1];
    bool stop_after_tone = false;
    free(ctx);

    taskENTER_CRITICAL(&s_audio_lock);
    stop_after_tone = !s_speaker_path_enabled;
    taskEXIT_CRITICAL(&s_audio_lock);

    const uint32_t chunk_ms = 20U;
    size_t frame_count = ((size_t)s_playback_format.sample_rate_hz * duration_ms) / 1000U;
    size_t chunk_frames = ((size_t)s_playback_format.sample_rate_hz * chunk_ms) / 1000U;
    size_t chunk_samples = chunk_frames * s_playback_format.channels;
    int16_t *tone_buffer = heap_caps_calloc(chunk_samples,
                                           sizeof(int16_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (tone_buffer == NULL) {
        s_tone_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    audio_format_t tone_format = s_playback_format;
    for (size_t first_frame = 0; first_frame < frame_count; first_frame += chunk_frames) {
        size_t frames_this_chunk = frame_count - first_frame;
        if (frames_this_chunk > chunk_frames) {
            frames_this_chunk = chunk_frames;
        }
        for (size_t frame = 0; frame < frames_this_chunk; ++frame) {
            size_t absolute_frame = first_frame + frame;
            float phase = (6.2831853f * (float)tone_hz * (float)absolute_frame) /
                          (float)s_playback_format.sample_rate_hz;
            int16_t sample = (int16_t)(sinf(phase) * 14000.0f);
            for (uint8_t channel = 0; channel < s_playback_format.channels; ++channel) {
                tone_buffer[frame * s_playback_format.channels + channel] = sample;
            }
        }
        audio_play_pcm_frame_with_format(
            (const uint8_t *)tone_buffer,
            frames_this_chunk * s_playback_format.channels * sizeof(int16_t),
            &tone_format);
    }
    /* A diagnostic pulse inside a live call must not reset its AEC/playback path. */
    if (stop_after_tone) {
        audio_stop_playback();
    }

    heap_caps_free(tone_buffer);
    s_tone_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t audio_prepare_output(void)
{
    const TickType_t retry_delay = pdMS_TO_TICKS(200);

    while (true) {
        TickType_t now = xTaskGetTickCount();

        taskENTER_CRITICAL(&s_audio_lock);
        if (s_audio_output_ready) {
            taskEXIT_CRITICAL(&s_audio_lock);
            return ESP_OK;
        }
        if (!s_audio_preparing) {
            if (s_audio_output_prepare_last_err != ESP_OK && now < s_audio_output_prepare_retry_after_ticks) {
                esp_err_t last_err = s_audio_output_prepare_last_err;
                taskEXIT_CRITICAL(&s_audio_lock);
                return last_err;
            }
            s_audio_preparing = true;
            s_audio_output_preparing = true;
            taskEXIT_CRITICAL(&s_audio_lock);
            break;
        }
        taskEXIT_CRITICAL(&s_audio_lock);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    esp_err_t ret = audio_do_prepare_output();
    if (ret == ESP_OK) {
        s_audio_output_ready = true;
        audio_update_ready_state();
        taskENTER_CRITICAL(&s_audio_lock);
        s_audio_output_prepare_last_err = ESP_OK;
        s_audio_output_prepare_retry_after_ticks = 0;
        s_audio_output_preparing = false;
        s_audio_preparing = false;
        taskEXIT_CRITICAL(&s_audio_lock);
        return ESP_OK;
    }

    audio_cleanup_output_prepare_failure();
    taskENTER_CRITICAL(&s_audio_lock);
    s_audio_output_prepare_last_err = ret;
    s_audio_output_prepare_retry_after_ticks = xTaskGetTickCount() + retry_delay;
    s_audio_output_preparing = false;
    s_audio_preparing = false;
    taskEXIT_CRITICAL(&s_audio_lock);
    return ret;
}

static esp_err_t audio_prepare_input(void)
{
    const TickType_t retry_delay = pdMS_TO_TICKS(200);

    while (true) {
        TickType_t now = xTaskGetTickCount();

        taskENTER_CRITICAL(&s_audio_lock);
        if (s_audio_input_ready) {
            taskEXIT_CRITICAL(&s_audio_lock);
            return ESP_OK;
        }
        if (!s_audio_preparing) {
            if (s_audio_input_prepare_last_err != ESP_OK && now < s_audio_input_prepare_retry_after_ticks) {
                esp_err_t last_err = s_audio_input_prepare_last_err;
                taskEXIT_CRITICAL(&s_audio_lock);
                return last_err;
            }
            s_audio_preparing = true;
            s_audio_input_preparing = true;
            taskEXIT_CRITICAL(&s_audio_lock);
            break;
        }
        taskEXIT_CRITICAL(&s_audio_lock);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    esp_err_t ret = audio_do_prepare_input();
    if (ret == ESP_OK) {
        s_audio_input_ready = true;
        audio_update_ready_state();
        taskENTER_CRITICAL(&s_audio_lock);
        s_audio_input_prepare_last_err = ESP_OK;
        s_audio_input_prepare_retry_after_ticks = 0;
        s_audio_input_preparing = false;
        s_audio_preparing = false;
        taskEXIT_CRITICAL(&s_audio_lock);
        return ESP_OK;
    }

    audio_cleanup_input_prepare_failure();
    taskENTER_CRITICAL(&s_audio_lock);
    s_audio_input_prepare_last_err = ret;
    s_audio_input_prepare_retry_after_ticks = xTaskGetTickCount() + retry_delay;
    s_audio_input_preparing = false;
    s_audio_preparing = false;
    taskEXIT_CRITICAL(&s_audio_lock);
    return ret;
}

esp_err_t audio_prepare(void)
{
    esp_err_t input_ret = audio_prepare_input();
    esp_err_t output_ret = audio_prepare_output();

    if (input_ret == ESP_OK && output_ret == ESP_OK) {
        return ESP_OK;
    }

    if (input_ret == ESP_OK) {
        ESP_LOGW(TAG,
                 "speaker output not ready, microphone capture remains available: %s",
                 esp_err_to_name(output_ret));
        return ESP_OK;
    }

    if (output_ret == ESP_OK) {
        ESP_LOGW(TAG,
                 "microphone capture not ready, speaker output remains available: %s",
                 esp_err_to_name(input_ret));
        return ESP_OK;
    }

    ESP_LOGE(TAG,
             "audio prepare failed: microphone=%s speaker=%s",
             esp_err_to_name(input_ret),
             esp_err_to_name(output_ret));
    return input_ret;
}

esp_err_t audio_prepare_input_path(void)
{
    return audio_prepare_input();
}

bool audio_preload_echo_cancel(void)
{
    return audio_echo_cancel_preload();
}

void audio_release(void)
{
    bool retained_input = false;
    bool retained_output = false;
    TaskHandle_t capture_task = NULL;
    const size_t before_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t before_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    audio_log_heap("audio suspend begin");
    taskENTER_CRITICAL(&s_audio_lock);
    s_capture_cb = NULL;
    s_capture_cb_ctx = NULL;
    s_capture_primary_enabled = false;
    s_capture_task_stop_requested = false;
    for (size_t index = 0; index < AUDIO_CAPTURE_OBSERVER_MAX; ++index) {
        s_capture_observers[index].enabled = false;
    }
    s_audio_stats.capture_enabled = false;
    s_audio_stats.input_level = 0;
    retained_input = s_audio_input_ready;
    retained_output = s_audio_output_ready;
    capture_task = s_capture_task;
    taskEXIT_CRITICAL(&s_audio_lock);

    audio_stop_playback();
#if APP_CONFIG_AUDIO_AEC_RETAIN_ON_RELEASE
    audio_echo_cancel_reset();
#else
    audio_echo_cancel_deinit();
#endif

    taskENTER_CRITICAL(&s_audio_lock);
    s_audio_input_prepare_last_err = ESP_OK;
    s_audio_input_prepare_retry_after_ticks = 0;
    s_audio_output_prepare_last_err = ESP_OK;
    s_audio_output_prepare_retry_after_ticks = 0;
    s_audio_preparing = false;
    s_audio_output_preparing = false;
    s_audio_input_preparing = false;
    s_speaker_path_enabled = false;
    memset(&s_last_playback_timing, 0, sizeof(s_last_playback_timing));
    taskEXIT_CRITICAL(&s_audio_lock);

    audio_update_ready_state();
    ESP_LOGI(TAG,
             "audio suspend done: retained_input=%u retained_output=%u capture_task=%p internal=%u->%u largest=%u->%u",
             retained_input ? 1U : 0U,
             retained_output ? 1U : 0U,
             capture_task,
             (unsigned)before_internal,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)before_largest,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    audio_log_heap("audio suspend done");
}

const audio_format_t *audio_get_format(void)
{
    return &s_capture_format;
}

const audio_format_t *audio_get_playback_format(void)
{
    return &s_playback_format;
}

void audio_set_capture_frame_cb(audio_capture_frame_cb_t cb, void *ctx)
{
    taskENTER_CRITICAL(&s_audio_lock);
    s_capture_cb = cb;
    s_capture_cb_ctx = ctx;
    s_audio_stats.capture_enabled = s_audio_input_ready &&
                                    audio_capture_has_active_consumer_locked();
    taskEXIT_CRITICAL(&s_audio_lock);
}

esp_err_t audio_register_capture_observer(audio_capture_frame_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(cb != NULL, ESP_ERR_INVALID_ARG, TAG, "capture observer callback is null");

    taskENTER_CRITICAL(&s_audio_lock);
    int free_slot = -1;
    for (size_t index = 0; index < AUDIO_CAPTURE_OBSERVER_MAX; ++index) {
        if (s_capture_observers[index].cb == cb && s_capture_observers[index].ctx == ctx) {
            taskEXIT_CRITICAL(&s_audio_lock);
            return ESP_OK;
        }
        if (free_slot < 0 && s_capture_observers[index].cb == NULL) {
            free_slot = (int)index;
        }
    }
    if (free_slot < 0) {
        taskEXIT_CRITICAL(&s_audio_lock);
        return ESP_ERR_NO_MEM;
    }
    s_capture_observers[free_slot] = (audio_capture_observer_t){
        .cb = cb,
        .ctx = ctx,
        .enabled = false,
    };
    s_audio_stats.capture_enabled = s_audio_input_ready &&
                                    audio_capture_has_active_consumer_locked();
    taskEXIT_CRITICAL(&s_audio_lock);
    return ESP_OK;
}

void audio_unregister_capture_observer(audio_capture_frame_cb_t cb, void *ctx)
{
    taskENTER_CRITICAL(&s_audio_lock);
    for (size_t index = 0; index < AUDIO_CAPTURE_OBSERVER_MAX; ++index) {
        if (s_capture_observers[index].cb == cb && s_capture_observers[index].ctx == ctx) {
            memset(&s_capture_observers[index], 0, sizeof(s_capture_observers[index]));
            break;
        }
    }
    s_audio_stats.capture_enabled = s_audio_input_ready &&
                                    audio_capture_has_active_consumer_locked();
    if (!s_audio_stats.capture_enabled) {
        s_audio_stats.input_level = 0;
    }
    taskEXIT_CRITICAL(&s_audio_lock);
}

esp_err_t audio_set_capture_observer_enabled(audio_capture_frame_cb_t cb, void *ctx, bool enabled)
{
    ESP_RETURN_ON_FALSE(cb != NULL, ESP_ERR_INVALID_ARG, TAG, "capture observer callback is null");

    if (enabled && !s_audio_input_ready) {
        ESP_RETURN_ON_ERROR(audio_prepare_input(), TAG, "audio input prepare failed");
    }

    taskENTER_CRITICAL(&s_audio_lock);
    for (size_t index = 0; index < AUDIO_CAPTURE_OBSERVER_MAX; ++index) {
        if (s_capture_observers[index].cb == cb && s_capture_observers[index].ctx == ctx) {
            s_capture_observers[index].enabled = enabled;
            s_audio_stats.capture_enabled = s_audio_input_ready &&
                                            audio_capture_has_active_consumer_locked();
            if (!s_audio_stats.capture_enabled) {
                s_audio_stats.input_level = 0;
            }
            taskEXIT_CRITICAL(&s_audio_lock);
            return ESP_OK;
        }
    }
    taskEXIT_CRITICAL(&s_audio_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t audio_set_capture_enabled(bool enabled)
{
    if (!s_audio_input_ready && enabled) {
        ESP_RETURN_ON_ERROR(audio_prepare_input(), TAG, "audio input prepare failed");
    }

    if (!s_audio_input_ready) {
        taskENTER_CRITICAL(&s_audio_lock);
        s_audio_stats.capture_enabled = false;
        s_audio_stats.input_level = 0;
        taskEXIT_CRITICAL(&s_audio_lock);
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_audio_lock);
    s_capture_primary_enabled = enabled;
    s_audio_stats.capture_enabled = audio_capture_has_active_consumer_locked();
    if (!s_audio_stats.capture_enabled) {
        s_audio_stats.input_level = 0;
    }
    taskEXIT_CRITICAL(&s_audio_lock);
    return ESP_OK;
}

esp_err_t audio_set_speaker_volume(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    bool update_codec_volume = false;
    taskENTER_CRITICAL(&s_audio_lock);
    s_speaker_volume_percent = percent;
    s_audio_stats.speaker_volume_percent = percent;
    if (percent > 0U) {
        s_playback_muted_logged = false;
    }
    update_codec_volume = s_audio_output_ready;
    taskEXIT_CRITICAL(&s_audio_lock);

    if (!update_codec_volume) {
        return ESP_OK;
    }

    esp_err_t lock_ret = audio_take_playback_mutex(pdMS_TO_TICKS(20));
    if (lock_ret != ESP_OK) {
        ESP_LOGD(TAG, "defer codec volume update: volume=%u ret=%s", (unsigned)percent, esp_err_to_name(lock_ret));
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_audio_lock);
    update_codec_volume = s_audio_output_ready;
    taskEXIT_CRITICAL(&s_audio_lock);
    if (update_codec_volume && s_play_dev_handle != NULL) {
        esp_err_t codec_ret = esp_codec_dev_set_out_vol(s_play_dev_handle, percent);
        if (codec_ret != ESP_OK) {
            ESP_LOGD(TAG,
                     "codec volume update deferred: volume=%u ret=%s",
                     (unsigned)percent,
                     esp_err_to_name(codec_ret));
        }
    }
    audio_give_playback_mutex();
    return ESP_OK;
}

esp_err_t audio_set_capture_gain_percent(uint8_t percent)
{
    audio_capture_processing_config_t config = audio_capture_make_default_processing_config(percent);
    return audio_set_capture_processing_config(&config);
}

esp_err_t audio_set_capture_processing_config(const audio_capture_processing_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "capture processing config is null");

    audio_capture_processing_config_t next = *config;
    audio_capture_sanitize_processing_config(&next);

    bool input_ready = false;
    taskENTER_CRITICAL(&s_audio_lock);
    s_capture_processing_config = next;
    s_capture_gain_percent = next.send_volume_percent;
    s_audio_stats.capture_gain_percent = next.send_volume_percent;
    s_audio_stats.capture_codec_gain_percent = next.codec_gain_percent;
    s_audio_stats.capture_upload_gain_percent = next.upload_gain_percent;
    s_audio_stats.capture_auto_gain_max_percent = next.auto_gain_max_percent;
    s_audio_stats.echo_continuous_processing = next.echo_continuous_processing;
    s_audio_stats.echo_near_end_protection_enabled = next.echo_near_end_protection_enabled;
    s_audio_stats.far_end_gain_guard_enabled = next.far_end_gain_guard_enabled;
    s_audio_stats.far_end_upload_gain_percent = next.far_end_upload_gain_percent;
    s_audio_stats.far_end_auto_gain_max_percent = next.far_end_auto_gain_max_percent;
    s_audio_stats.echo_suppression = next.echo_suppression;
    s_audio_stats.capture_high_pass_filter_enabled = next.high_pass_filter_enabled;
    s_audio_stats.capture_noise_gate_enabled = next.noise_gate_enabled;
    input_ready = s_audio_input_ready;
    taskEXIT_CRITICAL(&s_audio_lock);
    audio_echo_cancel_set_suppression(next.echo_suppression);

    int codec_gain_x10 = (int)(audio_capture_gain_percent_to_db(next.codec_gain_percent) * 10.0f + 0.5f);
    uint32_t upload_gain_x10 = (audio_capture_base_gain_q8(next.upload_gain_percent) * 10U) / 256U;
    uint32_t far_end_upload_gain_x10 =
        (audio_capture_base_gain_q8(next.far_end_upload_gain_percent) * 10U) / 256U;
    ESP_LOGI(TAG,
             "capture processing configured: send=%u codec=%u(%d.%01ddB) upload=%u(%lu.%lux) auto_max=%u%% echo_continuous=%u near_end_protect=%u far_end_guard=%u upload=%u(%lu.%lux) auto_max=%u%% suppression=%s diagnostics=%u hpf=%u/%uHz noise_gate=%u open=%u close=%u atten=%u%%",
             (unsigned)next.send_volume_percent,
             (unsigned)next.codec_gain_percent,
             codec_gain_x10 / 10,
             codec_gain_x10 % 10,
             (unsigned)next.upload_gain_percent,
             (unsigned long)(upload_gain_x10 / 10U),
             (unsigned long)(upload_gain_x10 % 10U),
             (unsigned)next.auto_gain_max_percent,
             next.echo_continuous_processing ? 1U : 0U,
             next.echo_near_end_protection_enabled ? 1U : 0U,
             next.far_end_gain_guard_enabled ? 1U : 0U,
             (unsigned)next.far_end_upload_gain_percent,
             (unsigned long)(far_end_upload_gain_x10 / 10U),
             (unsigned long)(far_end_upload_gain_x10 % 10U),
             (unsigned)next.far_end_auto_gain_max_percent,
             next.echo_suppression == AUDIO_ECHO_SUPPRESSION_STRONG ? "strong" : "balanced",
             next.echo_diagnostics_enabled ? 1U : 0U,
             next.high_pass_filter_enabled ? 1U : 0U,
             AUDIO_CAPTURE_HIGH_PASS_CUTOFF_HZ,
             next.noise_gate_enabled ? 1U : 0U,
             (unsigned)next.noise_gate_open_peak,
             (unsigned)next.noise_gate_close_peak,
             (unsigned)next.noise_gate_attenuation_percent);

    if (input_ready) {
        esp_err_t codec_ret = audio_apply_primary_microphone_gain(next.codec_gain_percent);
        if (codec_ret != ESP_OK) {
            ESP_LOGD(TAG,
                     "codec mic gain update deferred: codec_gain=%u ret=%s",
                     (unsigned)next.codec_gain_percent,
                     esp_err_to_name(codec_ret));
        }
    }
    return ESP_OK;
}

esp_err_t audio_prepare_playback_path(void)
{
    esp_err_t ret = audio_take_playback_mutex(portMAX_DELAY);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = audio_prepare_output();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "audio output prepare failed: %s", esp_err_to_name(ret));
        audio_give_playback_mutex();
        return ret;
    }

    if (!s_speaker_path_enabled) {
        ret = hardware_board_set_audio_power(true);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "speaker power on failed: %s", esp_err_to_name(ret));
            audio_give_playback_mutex();
            return ret;
        }
        esp_err_t unmute_ret = esp_codec_dev_set_out_mute(s_play_dev_handle, false);
        if (unmute_ret != ESP_OK) {
            (void)hardware_board_set_audio_power(false);
            audio_give_playback_mutex();
            return unmute_ret;
        }
        s_speaker_path_enabled = true;
        s_playback_path_ready_logged = false;
    }

    taskENTER_CRITICAL(&s_audio_lock);
    s_audio_stats.speaker_enabled = true;
    taskEXIT_CRITICAL(&s_audio_lock);
    if (!s_playback_path_ready_logged) {
        s_playback_path_ready_logged = true;
        ESP_LOGI(TAG,
                 "speaker playback path ready: rate=%luHz channels=%u volume=%u",
                 (unsigned long)s_playback_format.sample_rate_hz,
                 (unsigned)s_playback_format.channels,
                 (unsigned)s_speaker_volume_percent);
    }
    audio_give_playback_mutex();
    return ESP_OK;
}

esp_err_t audio_play_pcm_frame_with_format(const uint8_t *data,
                                                     size_t data_len,
                                                     const audio_format_t *format)
{
    int16_t *output_samples = NULL;
    size_t output_bytes = 0;
    uint32_t output_level = 0;
    esp_err_t ret = audio_take_playback_mutex(portMAX_DELAY);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = audio_render_playback_pcm(data,
                                    data_len,
                                    format,
                                    &output_samples,
                                    &output_bytes,
                                    &output_level);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "render playback pcm failed: %s", esp_err_to_name(ret));
        audio_give_playback_mutex();
        return ret;
    }

    ret = audio_write_rendered_playback(output_samples, output_bytes, output_level);
    audio_give_playback_mutex();
    return ret;
}

static int16_t audio_playback_input_mono_sample(const int16_t *samples,
                                                size_t frame_index,
                                                uint8_t channels)
{
    int16_t left = samples[frame_index * channels];
    int16_t right = channels == 2U ?
                        samples[frame_index * channels + 1U] : left;
    return (int16_t)(((int32_t)left + right) / 2);
}

esp_err_t audio_render_playback_pcm(const uint8_t *data,
                                              size_t data_len,
                                              const audio_format_t *format,
                                              int16_t **output_data,
                                              size_t *output_bytes,
                                              uint32_t *output_level)
{
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(data != NULL && format != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid pcm input");
    ESP_RETURN_ON_FALSE(output_data != NULL && output_bytes != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid pcm output");
    *output_data = NULL;
    *output_bytes = 0;

    ret = audio_take_playback_mutex(portMAX_DELAY);
    if (ret != ESP_OK) {
        return ret;
    }

    if (format->bits_per_sample != 16) {
        ret = ESP_ERR_NOT_SUPPORTED;
        ESP_LOGW(TAG, "only 16-bit pcm is supported");
        goto out;
    }
    if (format->channels != 1 && format->channels != 2) {
        ret = ESP_ERR_NOT_SUPPORTED;
        ESP_LOGW(TAG, "unsupported channel count");
        goto out;
    }
    if (format->sample_rate_hz != 8000 && format->sample_rate_hz != 16000) {
        ret = ESP_ERR_NOT_SUPPORTED;
        ESP_LOGW(TAG, "unsupported sample rate");
        goto out;
    }

    size_t input_frames = data_len / (sizeof(int16_t) * format->channels);
    if (input_frames == 0 || data_len != input_frames * sizeof(int16_t) * format->channels) {
        ret = ESP_ERR_INVALID_SIZE;
        ESP_LOGW(TAG, "invalid playback pcm length: bytes=%u ch=%u", (unsigned)data_len, format->channels);
        goto out;
    }
    if (s_playback_format.sample_rate_hz < format->sample_rate_hz) {
        ret = ESP_ERR_NOT_SUPPORTED;
        ESP_LOGW(TAG, "playback downsample is not supported");
        goto out;
    }
    if ((s_playback_format.sample_rate_hz % format->sample_rate_hz) != 0) {
        ret = ESP_ERR_NOT_SUPPORTED;
        ESP_LOGW(TAG, "playback sample rate must be an integer multiple of input");
        goto out;
    }
    size_t upsample_ratio = s_playback_format.sample_rate_hz / format->sample_rate_hz;
    size_t output_frames = input_frames * upsample_ratio;
    size_t rendered_bytes = output_frames * s_playback_format.channels * sizeof(int16_t);
    ret = audio_ensure_scratch(rendered_bytes);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "playback scratch alloc failed: %s", esp_err_to_name(ret));
        goto out;
    }

    const int16_t *input_samples = (const int16_t *)data;
    int16_t *output_samples = (int16_t *)s_playback_scratch;
    size_t out_index = 0;
    uint32_t playback_peak = 0;
    for (size_t frame_index = 0; frame_index < input_frames; ++frame_index) {
        int16_t left = input_samples[frame_index * format->channels];
        int16_t right = (format->channels == 2) ? input_samples[frame_index * format->channels + 1] : left;
        int16_t mono = audio_playback_input_mono_sample(input_samples,
                                                        frame_index,
                                                        format->channels);
        uint32_t abs_left = (uint32_t)abs(left);
        uint32_t abs_right = (uint32_t)abs(right);
        if (abs_left > playback_peak) {
            playback_peak = abs_left;
        }
        if (abs_right > playback_peak) {
            playback_peak = abs_right;
        }
        for (uint8_t channel = 0; channel < s_playback_format.channels; ++channel) {
            output_samples[out_index++] = mono;
        }
        if (upsample_ratio == 2U) {
            size_t previous_frame = frame_index > 0U ? frame_index - 1U : frame_index;
            size_t next_frame = frame_index + 1U < input_frames ?
                                    frame_index + 1U : frame_index;
            size_t next_next_frame = frame_index + 2U < input_frames ?
                                         frame_index + 2U : next_frame;
            int16_t previous_mono = audio_playback_input_mono_sample(
                input_samples,
                previous_frame,
                format->channels);
            int16_t next_mono = audio_playback_input_mono_sample(
                input_samples,
                next_frame,
                format->channels);
            int16_t next_next_mono = audio_playback_input_mono_sample(
                input_samples,
                next_next_frame,
                format->channels);
            /*
             * Four-point half-sample interpolation preserves noticeably more
             * of the 2-3.4 kHz speech-presence band than a simple midpoint,
             * without adding state or changing packet timing.
             */
            int32_t interpolation_sum = -(int32_t)previous_mono +
                                        9 * (int32_t)mono +
                                        9 * (int32_t)next_mono -
                                        (int32_t)next_next_mono;
            int32_t interpolated =
                (interpolation_sum + (interpolation_sum >= 0 ? 8 : -8)) / 16;
            int16_t midpoint = audio_clip_i16(interpolated);
            for (uint8_t channel = 0; channel < s_playback_format.channels; ++channel) {
                output_samples[out_index++] = midpoint;
            }
        }
    }

    *output_data = output_samples;
    *output_bytes = rendered_bytes;
    if (output_level != NULL) {
        *output_level = (playback_peak * 100U) / 32767U;
    }

out:
    audio_give_playback_mutex();
    return ret;
}

esp_err_t audio_write_rendered_playback(int16_t *data,
                                                   size_t data_len,
                                                   uint32_t output_level)
{
    int64_t prepare_start_us = 0;
    int64_t write_start_us = 0;
    uint32_t prepare_us = 0;
    uint32_t write_us = 0;
    uint32_t prepare_ms = 0;
    uint32_t write_ms = 0;
    esp_err_t ret = ESP_OK;
    bool playback_path_ready = false;

    ESP_RETURN_ON_FALSE(data != NULL && data_len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid rendered pcm input");

    ret = audio_take_playback_mutex(portMAX_DELAY);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t speaker_volume_percent = 0;
    bool log_muted_playback = false;
    taskENTER_CRITICAL(&s_audio_lock);
    speaker_volume_percent = audio_get_speaker_volume_percent_locked();
    if (speaker_volume_percent == 0U && !s_playback_muted_logged) {
        s_playback_muted_logged = true;
        log_muted_playback = true;
    }
    taskEXIT_CRITICAL(&s_audio_lock);

    if (!s_audio_output_ready) {
        ret = audio_prepare_output();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "audio output prepare failed: %s", esp_err_to_name(ret));
            goto out;
        }
    }

    if (log_muted_playback) {
        ESP_LOGD(TAG, "speaker volume is 0: remote playback and AEC reference are muted");
    }

    if (speaker_volume_percent == 0U) {
        audio_echo_cancel_reset();
        audio_mute_playback_path_no_mutex();

        taskENTER_CRITICAL(&s_audio_lock);
        s_audio_stats.speaker_enabled = false;
        s_audio_stats.output_level = 0;
        memset(&s_last_playback_timing, 0, sizeof(s_last_playback_timing));
        s_last_playback_timing.data_bytes = (uint32_t)data_len;
        taskEXIT_CRITICAL(&s_audio_lock);
        goto out;
    }

    taskENTER_CRITICAL(&s_audio_lock);
    playback_path_ready = s_speaker_path_enabled;
    taskEXIT_CRITICAL(&s_audio_lock);
    if (!playback_path_ready) {
        prepare_start_us = esp_timer_get_time();
        ret = audio_prepare_playback_path();
        if (ret != ESP_OK) {
            taskENTER_CRITICAL(&s_audio_lock);
            s_audio_stats.speaker_prepare_errors++;
            taskEXIT_CRITICAL(&s_audio_lock);
            ESP_LOGW(TAG, "prepare playback path failed: %s", esp_err_to_name(ret));
            goto out;
        }
        prepare_us = (uint32_t)(esp_timer_get_time() - prepare_start_us);
        prepare_ms = prepare_us / 1000U;
    }

    /*
     * The ES8311 codec is the sole owner of user playback volume. Applying the
     * same percentage to PCM here as well caused two independent attenuations:
     * volume 80 was scaled by 0.8 in PCM and then mapped to -10 dB by
     * esp_codec_dev. Keep the media samples and AEC reference at their original
     * level; speaker_set_volume_percent() controls the real acoustic gain.
     */
    audio_echo_cancel_feed_playback(data,
                                    data_len / sizeof(int16_t),
                                    s_playback_format.channels);

    write_start_us = esp_timer_get_time();
    ret = esp_codec_dev_write(s_play_dev_handle, (void *)data, (int)data_len);
    if (ret != ESP_OK) {
        audio_echo_cancel_reset();
        taskENTER_CRITICAL(&s_audio_lock);
        s_audio_stats.speaker_write_errors++;
        taskEXIT_CRITICAL(&s_audio_lock);
        ESP_LOGW(TAG, "speaker write failed: %s", esp_err_to_name(ret));
        goto out;
    }
    write_us = (uint32_t)(esp_timer_get_time() - write_start_us);
    write_ms = write_us / 1000U;

    taskENTER_CRITICAL(&s_audio_lock);
    s_audio_stats.speaker_enabled = true;
    s_audio_stats.output_level = output_level > 100U ? 100U : output_level;
    s_audio_stats.speaker_write_frames++;
    s_audio_stats.speaker_prepare_last_us = prepare_us;
    if (prepare_us > s_audio_stats.speaker_prepare_max_us) {
        s_audio_stats.speaker_prepare_max_us = prepare_us;
    }
    s_audio_stats.speaker_write_last_us = write_us;
    if (write_us > s_audio_stats.speaker_write_max_us) {
        s_audio_stats.speaker_write_max_us = write_us;
    }
    s_last_playback_timing.prepare_ms = prepare_ms;
    s_last_playback_timing.write_ms = write_ms;
    s_last_playback_timing.data_bytes = (uint32_t)data_len;
    taskEXIT_CRITICAL(&s_audio_lock);

    TickType_t now = xTaskGetTickCount();
    if (!s_playback_write_logged) {
        s_playback_write_logged = true;
        s_last_playback_write_log_tick = now;
        ESP_LOGD(TAG,
                 "speaker write committed: bytes=%u level=%u volume=%u prepare_ms=%lu write_ms=%lu path_enabled=%d output_ready=%d",
                 (unsigned)data_len,
                 (unsigned)output_level,
                 (unsigned)speaker_volume_percent,
                 (unsigned long)prepare_ms,
                 (unsigned long)write_ms,
                 s_speaker_path_enabled,
                 s_audio_output_ready);
    } else if (s_last_playback_write_log_tick == 0 ||
               now - s_last_playback_write_log_tick >= pdMS_TO_TICKS(1000)) {
        s_last_playback_write_log_tick = now;
        ESP_LOGD(TAG,
                 "speaker write steady: bytes=%u level=%u volume=%u prepare_ms=%lu write_ms=%lu path_enabled=%d output_ready=%d",
                 (unsigned)data_len,
                 (unsigned)output_level,
                 (unsigned)speaker_volume_percent,
                 (unsigned long)prepare_ms,
                 (unsigned long)write_ms,
                 s_speaker_path_enabled,
                 s_audio_output_ready);
    }
out:
    audio_give_playback_mutex();
    return ret;
}

void audio_get_last_playback_timing(audio_playback_timing_t *timing)
{
    if (timing == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_audio_lock);
    *timing = s_last_playback_timing;
    taskEXIT_CRITICAL(&s_audio_lock);
}

void audio_stop_playback(void)
{
    audio_echo_cancel_reset();

    esp_err_t ret = audio_take_playback_mutex(portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "audio stop playback skipped: %s", esp_err_to_name(ret));
        return;
    }

    if (!s_audio_output_ready) {
        audio_give_playback_mutex();
        return;
    }

    if (s_speaker_path_enabled) {
        esp_codec_dev_set_out_mute(s_play_dev_handle, true);
        hardware_board_set_audio_power(false);
        s_speaker_path_enabled = false;
        s_playback_path_ready_logged = false;
    }

    taskENTER_CRITICAL(&s_audio_lock);
    s_audio_stats.speaker_enabled = false;
    s_audio_stats.output_level = 0;
    taskEXIT_CRITICAL(&s_audio_lock);
    s_playback_write_logged = false;
    s_last_playback_write_log_tick = 0;
    audio_give_playback_mutex();
}

esp_err_t audio_play_test_tone(uint32_t tone_hz, uint32_t duration_ms)
{
    if (!s_audio_output_ready) {
        ESP_RETURN_ON_ERROR(audio_prepare_output(), TAG, "audio output prepare failed");
    }
    if (s_tone_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t *args = calloc(2, sizeof(uint32_t));
    if (args == NULL) {
        return ESP_ERR_NO_MEM;
    }
    args[0] = tone_hz;
    args[1] = duration_ms;

    BaseType_t task_ok = xTaskCreatePinnedToCore(audio_tone_task,
                                                 "audio_tone",
                                                 4 * 1024,
                                                 args,
                                                 5,
                                                 &s_tone_task,
                                                 0);
    if (task_ok != pdPASS) {
        free(args);
        s_tone_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void audio_get_stats(audio_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_audio_lock);
    *stats = s_audio_stats;
    taskEXIT_CRITICAL(&s_audio_lock);
}
