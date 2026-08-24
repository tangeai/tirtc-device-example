#include "audio_echo_cancel.h"

#include <stdbool.h>
#include <string.h>

#include "app_config.h"

#if APP_CONFIG_AUDIO_AEC_ENABLE

#include "esp_aec.h"
#include "esp_attr.h"
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
 * uses its own mode-specific frame size. This adapter keeps that mismatch here:
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
#define AUDIO_AEC_REF_ACTIVE_US               320000LL
#define AUDIO_AEC_REF_ACTIVE_PEAK             256U
#define AUDIO_AEC_PROFILE_NAME                "fd-low-linear-tail320"
#define AUDIO_AEC_MODE                        AEC_MODE_FD_LOW_COST
#define AUDIO_AEC_DEFAULT_NLP_LEVEL            AEC_NLP_LEVEL_NORMAL
#define AUDIO_AEC_FILTER_LENGTH               4
#define AUDIO_AEC_FALLBACK_FILTER_LENGTH      2
#define AUDIO_AEC_BUFFER_ALIGNMENT            16U
#define AUDIO_AEC_PSRAM_CAPS                  (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define AUDIO_AEC_DEINIT_WAIT_MS              60U
#define AUDIO_AEC_NEAR_END_MIN_LINEAR_PEAK        192U
/* FD-low measurements: quiet double talk retains about 1%; steady echo is lower. */
#define AUDIO_AEC_NEAR_END_MIN_RETAINED_ENERGY_PERCENT 1U
#define AUDIO_AEC_NEAR_END_MIN_NLP_REDUCTION_PERCENT  20U
/* Far-end echo in the raw microphone remains reference-correlated; local speech does not. */
#define AUDIO_AEC_ECHO_MIN_COHERENCE_SQ_PERCENT       20U
#define AUDIO_AEC_COHERENCE_MAX_LAG_SAMPLES           64
/*
 * Keep the full +/-4 ms acoustic search window, but do not run a dense
 * correlation grid in the 20 ms capture pipeline. A 0.25 ms lag step and
 * 4 kHz analysis stream retain voice-band discrimination while reducing the
 * double-talk detector from about 15k to 4k multiply-accumulates per frame.
 */
#define AUDIO_AEC_COHERENCE_LAG_STEP                  4
#define AUDIO_AEC_COHERENCE_SAMPLE_STEP               4U
/* Hold detected speech for 256 ms so syllables do not fall back into echo gain. */
#define AUDIO_AEC_NEAR_END_PRESENCE_HANGOVER_FRAMES    8U
#define AUDIO_AEC_NEAR_END_PROTECTION_HANGOVER_FRAMES  2U
#define AUDIO_AEC_NEAR_END_CONFIRM_FRAMES               2U
/* Let the adaptive filter see 512 ms of a new far-end talkspurt first. */
#define AUDIO_AEC_NEAR_END_ADAPT_FRAMES                 16U
#define AUDIO_AEC_NEAR_END_RAW_BLEND_Q8                48U
#define AUDIO_AEC_NEAR_END_BLEND_RELEASE_STEP_Q8       16U
#define AUDIO_AEC_VOIP_NEAR_END_PEAK                  512U
#define AUDIO_AEC_VOIP_NEAR_END_MIN_RETAINED_ENERGY_PERCENT 2U
#define AUDIO_AEC_VOIP_NEAR_END_HANGOVER_FRAMES         5U
#define AUDIO_AEC_ANALYSIS_SHIFT                  8U
#define AUDIO_AEC_DEBUG_CAPTURE_MIN_MS           250U
#define AUDIO_AEC_DEBUG_CAPTURE_MAX_MS          5000U
#define AUDIO_AEC_DEBUG_CHANNELS AUDIO_ECHO_CANCEL_DEBUG_CHANNEL_COUNT

static const char *TAG = "audio_aec";

static portMUX_TYPE s_aec_lock = portMUX_INITIALIZER_UNLOCKED;

static aec_handle_t *s_aec;
static int s_aec_frame_size;
static int16_t *s_ref_ring;
static int16_t *s_mic_frame;
static int16_t *s_ref_frame;
static int16_t *s_out_frame;
static int16_t *s_linear_frame;
static int16_t *s_analysis_mic_frame;
static int16_t *s_analysis_ref_frame;
static int16_t *s_raw_fifo;
static int16_t *s_out_fifo;
static uint32_t s_ref_write_pos;
static uint32_t s_ref_filled_samples;
static uint32_t s_last_ref_peak;
static int64_t s_last_playback_us;
static uint32_t s_frame_fill;
static uint32_t s_frame_ref_peak;
static bool s_frame_playback_active;
static uint32_t s_out_fifo_read_pos;
static uint32_t s_out_fifo_used;
static bool s_out_fifo_ready;
static bool s_analysis_frame_valid;
static uint32_t s_near_end_candidate_frames;
static uint32_t s_reference_active_frames;
static uint32_t s_near_end_presence_hangover_frames;
static uint32_t s_near_end_protection_hangover_frames;
static uint32_t s_near_end_blend_q8;
static uint32_t s_last_linear_peak;
static bool s_last_near_end_detected;
static bool s_last_near_end_protected;
static uint32_t s_near_decision_sequence;
static uint8_t s_last_near_retained_energy_percent;
static uint8_t s_last_near_nlp_reduction_percent;
static uint8_t s_last_near_reference_coherence_percent;
static audio_echo_near_reject_reason_t s_last_near_reject_reason;
static bool s_initializing;
static bool s_deinit_requested;
static bool s_create_failed_logged;
static uint32_t s_active_users;
static aec_nlp_level_t s_requested_nlp_level = AUDIO_AEC_DEFAULT_NLP_LEVEL;
static aec_nlp_level_t s_applied_nlp_level = AUDIO_AEC_DEFAULT_NLP_LEVEL;

typedef struct {
    int16_t *samples;
    int16_t *nlp_scratch;
    size_t capacity_frames;
    size_t captured_frames;
    bool recording;
    bool complete;
} audio_aec_debug_capture_t;

typedef struct {
    uint32_t frequency_hz;
    uint32_t phase;
    uint16_t amplitude;
} audio_aec_debug_injection_t;

/* Diagnostic PCM is cold-path data and must never consume scarce internal RAM. */
static EXT_RAM_BSS_ATTR audio_aec_debug_capture_t s_debug_capture;
static EXT_RAM_BSS_ATTR audio_aec_debug_injection_t s_debug_injection;

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

static bool audio_aec_mode_supports_runtime_nlp(aec_mode_t mode)
{
    return mode == AEC_MODE_FD_LOW_COST || mode == AEC_MODE_FD_HIGH_PERF;
}

static aec_handle_t *audio_aec_create_handle(aec_mode_t mode,
                                              int filter_length,
                                              aec_nlp_level_t nlp_level)
{
    aec_config_t config = {
        .mic_num = 1,
        .ref_num = 1,
        .out_num = 1,
        .filter_length = filter_length,
        .sample_rate = (int)AUDIO_AEC_SAMPLE_RATE_HZ,
        .caps = AUDIO_AEC_PSRAM_CAPS,
        .mode = mode,
        .nlp_level = nlp_level,
    };
    aec_handle_t *handle = aec_create_from_config(&config);
    if (handle != NULL && audio_aec_mode_supports_runtime_nlp(mode)) {
        (void)aec_set_nlp_level(handle, nlp_level);
    }
    return handle;
}

static void audio_aec_free_handle_and_buffers(aec_handle_t *handle,
                                              int16_t *ref_ring,
                                              int16_t *mic_frame,
                                              int16_t *ref_frame,
                                              int16_t *out_frame,
                                              int16_t *linear_frame,
                                              int16_t *analysis_mic_frame,
                                              int16_t *analysis_ref_frame,
                                              int16_t *raw_fifo,
                                              int16_t *out_fifo)
{
    if (handle != NULL) {
        aec_destroy(handle);
    }
    heap_caps_free(ref_ring);
    heap_caps_free(mic_frame);
    heap_caps_free(ref_frame);
    heap_caps_free(out_frame);
    heap_caps_free(linear_frame);
    heap_caps_free(analysis_mic_frame);
    heap_caps_free(analysis_ref_frame);
    heap_caps_free(raw_fifo);
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

    aec_nlp_level_t requested_nlp = AEC_NLP_LEVEL_AGGR;
    taskENTER_CRITICAL(&s_aec_lock);
    requested_nlp = s_requested_nlp_level;
    taskEXIT_CRITICAL(&s_aec_lock);

    /*
     * Device calls use the FD low-cost linear canceller. ESP-SR no longer
     * recommends the legacy VOIP modes, and the combined nonlinear path was
     * measured to damage quiet near-end speech. FD low-cost keeps sufficient
     * echo cancellation while preserving the microphone waveform. The app
     * deliberately leaves custom near-end blending disabled for calls, so the
     * result below is the official linear AEC output without a raw-echo mix.
     */
    aec_handle_t *handle = audio_aec_create_handle(AUDIO_AEC_MODE,
                                                    AUDIO_AEC_FILTER_LENGTH,
                                                    requested_nlp);
    if (handle == NULL) {
        handle = audio_aec_create_handle(AUDIO_AEC_MODE,
                                         AUDIO_AEC_FALLBACK_FILTER_LENGTH,
                                         requested_nlp);
    }

    int frame_size = handle == NULL ? 0 : aec_get_chunksize(handle);
    int16_t *ref_ring = NULL;
    int16_t *mic_frame = NULL;
    int16_t *ref_frame = NULL;
    int16_t *out_frame = NULL;
    int16_t *linear_frame = NULL;
    int16_t *analysis_mic_frame = NULL;
    int16_t *analysis_ref_frame = NULL;
    int16_t *raw_fifo = NULL;
    int16_t *out_fifo = NULL;

    if (handle != NULL && frame_size > 0) {
        ref_ring = audio_aec_aligned_calloc(AUDIO_AEC_REF_RING_SAMPLES, sizeof(int16_t));
        mic_frame = audio_aec_aligned_calloc((size_t)frame_size, sizeof(int16_t));
        ref_frame = audio_aec_aligned_calloc((size_t)frame_size, sizeof(int16_t));
        out_frame = audio_aec_aligned_calloc((size_t)frame_size, sizeof(int16_t));
        linear_frame = audio_aec_aligned_calloc((size_t)frame_size, sizeof(int16_t));
        analysis_mic_frame = audio_aec_aligned_calloc((size_t)frame_size, sizeof(int16_t));
        analysis_ref_frame = audio_aec_aligned_calloc((size_t)frame_size, sizeof(int16_t));
        raw_fifo = audio_aec_aligned_calloc(AUDIO_AEC_OUT_FIFO_SAMPLES, sizeof(int16_t));
        out_fifo = audio_aec_aligned_calloc(AUDIO_AEC_OUT_FIFO_SAMPLES, sizeof(int16_t));
    }

    if (handle == NULL || frame_size <= 0 || ref_ring == NULL || mic_frame == NULL ||
        ref_frame == NULL || out_frame == NULL || linear_frame == NULL || raw_fifo == NULL ||
        analysis_mic_frame == NULL || analysis_ref_frame == NULL || out_fifo == NULL) {
        audio_aec_free_handle_and_buffers(handle,
                                          ref_ring,
                                          mic_frame,
                                          ref_frame,
                                          out_frame,
                                          linear_frame,
                                          analysis_mic_frame,
                                          analysis_ref_frame,
                                          raw_fifo,
                                          out_fifo);
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
        audio_aec_free_handle_and_buffers(handle,
                                          ref_ring,
                                          mic_frame,
                                          ref_frame,
                                          out_frame,
                                          linear_frame,
                                          analysis_mic_frame,
                                          analysis_ref_frame,
                                          raw_fifo,
                                          out_fifo);
        return false;
    }
    s_aec = handle;
    s_aec_frame_size = frame_size;
    s_ref_ring = ref_ring;
    s_mic_frame = mic_frame;
    s_ref_frame = ref_frame;
    s_out_frame = out_frame;
    s_linear_frame = linear_frame;
    s_analysis_mic_frame = analysis_mic_frame;
    s_analysis_ref_frame = analysis_ref_frame;
    s_raw_fifo = raw_fifo;
    s_out_fifo = out_fifo;
    s_ref_write_pos = 0;
    s_ref_filled_samples = 0;
    s_last_ref_peak = 0;
    s_last_playback_us = 0;
    s_frame_fill = 0;
    s_frame_ref_peak = 0;
    s_frame_playback_active = false;
    s_out_fifo_read_pos = 0;
    s_out_fifo_used = 0;
    s_out_fifo_ready = false;
    s_analysis_frame_valid = false;
    s_near_end_candidate_frames = 0;
    s_reference_active_frames = 0;
    s_near_end_presence_hangover_frames = 0;
    s_near_end_protection_hangover_frames = 0;
    s_near_end_blend_q8 = 0;
    s_last_linear_peak = 0;
    s_last_near_end_detected = false;
    s_last_near_end_protected = false;
    s_near_decision_sequence = 0;
    s_last_near_retained_energy_percent = 0;
    s_last_near_nlp_reduction_percent = 0;
    s_last_near_reference_coherence_percent = 0;
    s_last_near_reject_reason = AUDIO_ECHO_NEAR_REJECT_NONE;
    s_applied_nlp_level = requested_nlp;
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
             (unsigned)((AUDIO_AEC_REF_RING_SAMPLES + AUDIO_AEC_OUT_FIFO_SAMPLES * 2U +
                         (uint32_t)frame_size * 6U) * sizeof(int16_t)));
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

static bool audio_aec_debug_capture_is_recording(void)
{
    bool recording = false;

    taskENTER_CRITICAL(&s_aec_lock);
    recording = s_debug_capture.recording;
    taskEXIT_CRITICAL(&s_aec_lock);
    return recording;
}

static int16_t *audio_aec_debug_capture_nlp_scratch(void)
{
    int16_t *scratch = NULL;

    taskENTER_CRITICAL(&s_aec_lock);
    if (s_debug_capture.recording) {
        scratch = s_debug_capture.nlp_scratch;
    }
    taskEXIT_CRITICAL(&s_aec_lock);
    return scratch;
}

static int16_t audio_aec_clip_i16(int32_t value)
{
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

static void audio_aec_debug_apply_injection(int16_t *samples, size_t sample_count)
{
    uint32_t frequency_hz = 0;
    uint32_t phase = 0;
    uint16_t amplitude = 0;

    taskENTER_CRITICAL(&s_aec_lock);
    frequency_hz = s_debug_injection.frequency_hz;
    phase = s_debug_injection.phase;
    amplitude = s_debug_injection.amplitude;
    taskEXIT_CRITICAL(&s_aec_lock);
    if (samples == NULL || sample_count == 0U || frequency_hz == 0U || amplitude == 0U) {
        return;
    }

    for (size_t index = 0; index < sample_count; ++index) {
        int32_t injected = 0;
        if (phase < AUDIO_AEC_SAMPLE_RATE_HZ / 2U) {
            injected = ((int32_t)phase * 4 * amplitude / AUDIO_AEC_SAMPLE_RATE_HZ) - amplitude;
        } else {
            injected = (3 * amplitude) -
                       ((int32_t)phase * 4 * amplitude / AUDIO_AEC_SAMPLE_RATE_HZ);
        }
        samples[index] = audio_aec_clip_i16((int32_t)samples[index] + injected);
        phase += frequency_hz;
        while (phase >= AUDIO_AEC_SAMPLE_RATE_HZ) {
            phase -= AUDIO_AEC_SAMPLE_RATE_HZ;
        }
    }

    taskENTER_CRITICAL(&s_aec_lock);
    if (s_debug_injection.frequency_hz == frequency_hz &&
        s_debug_injection.amplitude == amplitude) {
        s_debug_injection.phase = phase;
    }
    taskEXIT_CRITICAL(&s_aec_lock);
}

static void audio_aec_debug_capture_frame(const int16_t *mic_samples,
                                           const int16_t *reference_samples,
                                           const int16_t *linear_samples,
                                           const int16_t *nlp_samples,
                                           const int16_t *final_samples,
                                           uint16_t near_end_blend_q8,
                                           size_t sample_count)
{
    int16_t *capture = NULL;
    size_t captured_frames = 0;
    size_t copy_frames = 0;

    taskENTER_CRITICAL(&s_aec_lock);
    if (s_debug_capture.recording && s_debug_capture.samples != NULL &&
        s_debug_capture.captured_frames < s_debug_capture.capacity_frames) {
        capture = s_debug_capture.samples;
        captured_frames = s_debug_capture.captured_frames;
        copy_frames = s_debug_capture.capacity_frames - captured_frames;
        if (copy_frames > sample_count) {
            copy_frames = sample_count;
        }
    }
    taskEXIT_CRITICAL(&s_aec_lock);
    if (capture == NULL || copy_frames == 0U) {
        return;
    }

    int16_t *write = capture + captured_frames * AUDIO_AEC_DEBUG_CHANNELS;
    for (size_t index = 0; index < copy_frames; ++index) {
        *write++ = mic_samples[index];
        *write++ = reference_samples[index];
        *write++ = linear_samples[index];
        *write++ = nlp_samples[index];
        *write++ = final_samples[index];
        *write++ = (int16_t)near_end_blend_q8;
    }

    bool completed = false;
    taskENTER_CRITICAL(&s_aec_lock);
    if (s_debug_capture.samples == capture && s_debug_capture.recording &&
        s_debug_capture.captured_frames == captured_frames) {
        s_debug_capture.captured_frames += copy_frames;
        if (s_debug_capture.captured_frames >= s_debug_capture.capacity_frames) {
            s_debug_capture.recording = false;
            s_debug_capture.complete = true;
            memset(&s_debug_injection, 0, sizeof(s_debug_injection));
            completed = true;
        }
    }
    taskEXIT_CRITICAL(&s_aec_lock);

    if (completed) {
        ESP_LOGI(TAG,
                 "AEC debug capture ready: frames=%u channels=%u bytes=%u",
                 (unsigned)s_debug_capture.captured_frames,
                 (unsigned)AUDIO_AEC_DEBUG_CHANNELS,
                 (unsigned)(s_debug_capture.captured_frames *
                            AUDIO_AEC_DEBUG_CHANNELS * sizeof(int16_t)));
    }
}

static void audio_aec_apply_requested_suppression(void)
{
    aec_nlp_level_t requested = AEC_NLP_LEVEL_AGGR;
    aec_nlp_level_t applied = AEC_NLP_LEVEL_AGGR;

    taskENTER_CRITICAL(&s_aec_lock);
    requested = s_requested_nlp_level;
    applied = s_applied_nlp_level;
    taskEXIT_CRITICAL(&s_aec_lock);
    if (requested == applied || s_aec == NULL ||
        !audio_aec_mode_supports_runtime_nlp(s_aec->config.mode)) {
        return;
    }

    aec_nlp_level_t actual = aec_set_nlp_level(s_aec, requested);
    taskENTER_CRITICAL(&s_aec_lock);
    s_applied_nlp_level = actual;
    taskEXIT_CRITICAL(&s_aec_lock);
    ESP_LOGI(TAG,
             "AEC suppression changed: requested=%s applied=%s",
             aec_get_nlp_string(requested),
             aec_get_nlp_string(actual));
}

void audio_echo_cancel_set_suppression(audio_echo_suppression_t suppression)
{
    aec_nlp_level_t requested = suppression == AUDIO_ECHO_SUPPRESSION_STRONG ?
                                AEC_NLP_LEVEL_VERYAGGR :
                                AEC_NLP_LEVEL_NORMAL;

    taskENTER_CRITICAL(&s_aec_lock);
    s_requested_nlp_level = requested;
    taskEXIT_CRITICAL(&s_aec_lock);
}

bool audio_echo_cancel_preload(void)
{
    bool recreate_for_nlp = false;

    taskENTER_CRITICAL(&s_aec_lock);
    if (s_aec != NULL &&
        !audio_aec_mode_supports_runtime_nlp(s_aec->config.mode) &&
        s_aec->config.nlp_level != s_requested_nlp_level) {
        recreate_for_nlp = true;
    }
    taskEXIT_CRITICAL(&s_aec_lock);

    /*
     * ESP-SR VoIP modes accept NLP only when the handle is created. Calls enter
     * through preload before capture is enabled, so this is the one safe place
     * to rebuild a retained handle when the application profile changes.
     */
    if (recreate_for_nlp) {
        audio_echo_cancel_deinit();

        taskENTER_CRITICAL(&s_aec_lock);
        bool old_handle_still_active = s_aec != NULL;
        taskEXIT_CRITICAL(&s_aec_lock);
        if (old_handle_still_active) {
            ESP_LOGW(TAG, "AEC profile rebuild deferred because the retained handle is busy");
            return false;
        }
    }

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

static bool audio_aec_playback_is_active(void)
{
    uint32_t last_ref_peak = 0;
    int64_t last_playback_us = 0;

    taskENTER_CRITICAL(&s_aec_lock);
    last_ref_peak = s_last_ref_peak;
    last_playback_us = s_last_playback_us;
    taskEXIT_CRITICAL(&s_aec_lock);

    int64_t now_us = esp_timer_get_time();
    return last_ref_peak >= AUDIO_AEC_REF_ACTIVE_PEAK &&
           last_playback_us > 0 &&
           now_us >= last_playback_us &&
           now_us - last_playback_us <= AUDIO_AEC_REF_ACTIVE_US;
}

static uint32_t audio_aec_max_reference_coherence_sq_percent(
    const int16_t *signal_samples,
    const int16_t *reference_samples,
    uint32_t sample_count)
{
    uint32_t best_percent = 0;

    if (sample_count <= AUDIO_AEC_COHERENCE_MAX_LAG_SAMPLES) {
        return 0U;
    }

    /*
     * Echo is delayed and phase shifted by the codec, amplifier and acoustic
     * path. Search a bounded lag window between the caller-selected signal and
     * the hardware playback reference.
     */
    for (int lag = -AUDIO_AEC_COHERENCE_MAX_LAG_SAMPLES;
         lag <= AUDIO_AEC_COHERENCE_MAX_LAG_SAMPLES;
         lag += AUDIO_AEC_COHERENCE_LAG_STEP) {
        uint32_t signal_start = lag < 0 ? (uint32_t)(-lag) : 0U;
        uint32_t ref_start = lag > 0 ? (uint32_t)lag : 0U;
        uint32_t available = sample_count -
            (signal_start > ref_start ? signal_start : ref_start);
        uint64_t signal_energy = 0;
        uint64_t reference_energy = 0;
        int64_t dot = 0;

        for (uint32_t index = 0; index < available;
             index += AUDIO_AEC_COHERENCE_SAMPLE_STEP) {
            int32_t signal =
                signal_samples[signal_start + index] >> AUDIO_AEC_ANALYSIS_SHIFT;
            int32_t reference =
                reference_samples[ref_start + index] >> AUDIO_AEC_ANALYSIS_SHIFT;
            signal_energy += (uint64_t)((int64_t)signal * signal);
            reference_energy += (uint64_t)((int64_t)reference * reference);
            dot += (int64_t)signal * reference;
        }

        if (signal_energy == 0U || reference_energy == 0U) {
            continue;
        }
        uint64_t abs_dot = (uint64_t)(dot < 0 ? -dot : dot);
        uint64_t denominator = signal_energy * reference_energy;
        uint32_t percent = denominator == 0U ? 0U :
            (uint32_t)((abs_dot * abs_dot * 100U) / denominator);
        if (percent > 100U) {
            percent = 100U;
        }
        if (percent > best_percent) {
            best_percent = percent;
        }
    }

    return best_percent;
}

static bool audio_aec_detect_near_end(const int16_t *mic_samples,
                                      const int16_t *reference_samples,
                                      const int16_t *linear_samples,
                                      const int16_t *nlp_samples,
                                      uint32_t sample_count,
                                      uint32_t *linear_peak,
                                      uint8_t *retained_energy_percent,
                                      uint8_t *nlp_reduction_percent,
                                      uint8_t *reference_coherence_percent,
                                      bool *nlp_protection_needed,
                                      audio_echo_near_reject_reason_t *reject_reason)
{
    uint64_t mic_energy = 0;
    uint64_t linear_energy = 0;
    uint64_t nlp_energy = 0;
    uint32_t peak = 0;

    for (uint32_t index = 0; index < sample_count; ++index) {
        int32_t mic = mic_samples[index] >> AUDIO_AEC_ANALYSIS_SHIFT;
        int32_t linear = linear_samples[index] >> AUDIO_AEC_ANALYSIS_SHIFT;
        int32_t nlp = nlp_samples[index] >> AUDIO_AEC_ANALYSIS_SHIFT;
        uint32_t abs_linear = audio_aec_abs_i16(linear_samples[index]);

        mic_energy += (uint64_t)((int64_t)mic * mic);
        linear_energy += (uint64_t)((int64_t)linear * linear);
        nlp_energy += (uint64_t)((int64_t)nlp * nlp);
        if (abs_linear > peak) {
            peak = abs_linear;
        }
    }

    if (linear_peak != NULL) {
        *linear_peak = peak;
    }
    if (retained_energy_percent != NULL) {
        *retained_energy_percent = mic_energy == 0U ? 0U :
            (uint8_t)((linear_energy * 100U / mic_energy) > 100U ?
                100U : (linear_energy * 100U / mic_energy));
    }
    if (nlp_reduction_percent != NULL) {
        uint64_t reduction_percent =
            linear_energy == 0U || nlp_energy >= linear_energy ? 0U :
            ((linear_energy - nlp_energy) * 100U) / linear_energy;
        *nlp_reduction_percent = (uint8_t)(reduction_percent > 100U ?
            100U : reduction_percent);
    }
    if (reference_coherence_percent != NULL) {
        *reference_coherence_percent = 0U;
    }
    if (nlp_protection_needed != NULL) {
        *nlp_protection_needed = false;
    }
    if (reject_reason != NULL) {
        *reject_reason = AUDIO_ECHO_NEAR_REJECT_NONE;
    }
    if (peak < AUDIO_AEC_NEAR_END_MIN_LINEAR_PEAK || mic_energy == 0U ||
        linear_energy == 0U) {
        if (reject_reason != NULL) {
            *reject_reason = AUDIO_ECHO_NEAR_REJECT_LOW_LINEAR_PEAK;
        }
        return false;
    }
    /*
     * Classify double talk from the unprocessed microphone, not from the
     * adaptive filter error. A correctly adapting linear canceller changes the
     * phase and spectrum of its residual, so pure far-end echo can have very
     * low residual/reference coherence and be misclassified as local speech.
     * The aligned MIC1/reference pair retains the acoustic-path correlation;
     * independent near-end speech lowers that correlation without changing
     * either the capture gain or the speaker level.
     */
    uint32_t echo_coherence = audio_aec_max_reference_coherence_sq_percent(
        mic_samples,
        reference_samples,
        sample_count);
    if (reference_coherence_percent != NULL) {
        *reference_coherence_percent = (uint8_t)echo_coherence;
    }
    if (linear_energy * 100U <
        mic_energy * AUDIO_AEC_NEAR_END_MIN_RETAINED_ENERGY_PERCENT) {
        if (reject_reason != NULL) {
            *reject_reason = AUDIO_ECHO_NEAR_REJECT_LOW_RETAINED_ENERGY;
        }
        return false;
    }
    if (echo_coherence >= AUDIO_AEC_ECHO_MIN_COHERENCE_SQ_PERCENT) {
        if (reject_reason != NULL) {
            *reject_reason = AUDIO_ECHO_NEAR_REJECT_HIGH_REFERENCE_COHERENCE;
        }
        return false;
    }

    /*
     * Near-end presence and NLP damage are separate facts. A local talker can
     * survive NLP unchanged and still needs the normal uplink gain target. Only
     * blend the linear AEC output back when NLP actually attenuated that talker.
     */
    if (nlp_protection_needed != NULL) {
        *nlp_protection_needed = nlp_energy * 100U <
            linear_energy * (100U - AUDIO_AEC_NEAR_END_MIN_NLP_REDUCTION_PERCENT);
    }
    return true;
}

static void audio_aec_snapshot_input_frame(void)
{
    if (s_analysis_mic_frame == NULL || s_analysis_ref_frame == NULL ||
        s_mic_frame == NULL || s_ref_frame == NULL || s_aec_frame_size <= 0) {
        s_analysis_frame_valid = false;
        return;
    }

    memcpy(s_analysis_mic_frame,
           s_mic_frame,
           (size_t)s_aec_frame_size * sizeof(s_analysis_mic_frame[0]));
    memcpy(s_analysis_ref_frame,
           s_ref_frame,
           (size_t)s_aec_frame_size * sizeof(s_analysis_ref_frame[0]));
    s_analysis_frame_valid = true;
}

static const int16_t *audio_aec_process_frame(bool near_end_protection_enabled,
                                              bool reference_active)
{
    /*
     * ESP-SR accepts writable input buffers and does not promise to preserve
     * them. Snapshot the current aligned mic/reference pair before processing
     * so raw fallback, double-talk diagnostics, and FIFO accounting never use
     * samples that the algorithm may already have changed in place.
     */
    audio_aec_snapshot_input_frame();
    bool debug_recording = audio_aec_debug_capture_is_recording();
    int16_t *debug_nlp = debug_recording ? audio_aec_debug_capture_nlp_scratch() : NULL;
    const int16_t *analysis_mic = s_analysis_frame_valid ?
        s_analysis_mic_frame : s_mic_frame;
    const int16_t *analysis_ref = s_analysis_frame_valid ?
        s_analysis_ref_frame : s_ref_frame;

    if (s_aec->config.mode == AEC_MODE_VOIP_LOW_COST ||
        s_aec->config.mode == AEC_MODE_VOIP_HIGH_PERF) {
        /*
         * VOIP mode owns double-talk handling; configurable NLP is FD-only.
         * Do not classify near-end speech by residual peak alone. Physical
         * far-end-only captures regularly contain short residual peaks above
         * 512 even though their retained energy stays below 0.1%. Treating
         * those peaks as speech releases the far-end gain guard and amplifies
         * the residual echo back into a feedback loop. Real double talk keeps
         * at least a few percent of microphone energy after cancellation.
         */
        int16_t *voip_mic = s_mic_frame;
        int16_t *voip_ref = s_ref_frame;

        aec_process(s_aec, voip_mic, voip_ref, s_out_frame);
        if (!reference_active) {
            /*
             * VOIP mode can suppress a quiet physical near-end signal even
             * when the board-level MIC3 reference proves that the speaker is
             * silent. Keep running the AEC state machine, but select the raw
             * microphone frame for this equally delayed output slot. This
             * preserves the continuous 256-sample timeline across playback
             * transitions without treating distant speech as residual echo.
             */
            if (debug_nlp != NULL) {
                memcpy(debug_nlp,
                       s_out_frame,
                       (size_t)s_aec_frame_size * sizeof(debug_nlp[0]));
                audio_aec_debug_capture_frame(analysis_mic,
                                              analysis_ref,
                                              analysis_mic,
                                              debug_nlp,
                                              analysis_mic,
                                              0U,
                                              (size_t)s_aec_frame_size);
            }
            s_near_decision_sequence++;
            s_last_linear_peak = 0U;
            s_last_near_end_detected = false;
            s_last_near_end_protected = false;
            s_last_near_retained_energy_percent = 0U;
            s_last_near_nlp_reduction_percent = 0U;
            s_last_near_reference_coherence_percent = 0U;
            s_last_near_reject_reason = AUDIO_ECHO_NEAR_REJECT_NONE;
            s_near_end_candidate_frames = 0U;
            s_reference_active_frames = 0U;
            s_near_end_presence_hangover_frames = 0U;
            s_near_end_protection_hangover_frames = 0U;
            s_near_end_blend_q8 = 0U;
            memcpy(s_out_frame,
                   analysis_mic,
                   (size_t)s_aec_frame_size * sizeof(s_out_frame[0]));
            return s_out_frame;
        }
        uint64_t mic_energy = 0;
        uint64_t output_energy = 0;
        uint32_t output_peak = 0;
        for (int index = 0; index < s_aec_frame_size; ++index) {
            int32_t mic = analysis_mic[index] >> AUDIO_AEC_ANALYSIS_SHIFT;
            int32_t output = s_out_frame[index] >> AUDIO_AEC_ANALYSIS_SHIFT;
            uint32_t value = audio_aec_abs_i16(s_out_frame[index]);
            mic_energy += (uint64_t)((int64_t)mic * mic);
            output_energy += (uint64_t)((int64_t)output * output);
            if (value > output_peak) {
                output_peak = value;
            }
        }
        uint64_t retained_percent = mic_energy == 0U ? 0U :
            (output_energy * 100U) / mic_energy;
        if (retained_percent > 100U) {
            retained_percent = 100U;
        }
        bool near_end_present = reference_active &&
            output_peak >= AUDIO_AEC_VOIP_NEAR_END_PEAK &&
            output_energy * 100U >=
                mic_energy * AUDIO_AEC_VOIP_NEAR_END_MIN_RETAINED_ENERGY_PERCENT;
        if (near_end_present) {
            s_near_end_presence_hangover_frames =
                AUDIO_AEC_VOIP_NEAR_END_HANGOVER_FRAMES;
        } else if (s_near_end_presence_hangover_frames > 0U) {
            s_near_end_presence_hangover_frames--;
        }
        s_near_decision_sequence++;
        s_last_linear_peak = output_peak;
        s_last_near_end_detected = s_near_end_presence_hangover_frames > 0U;
        s_last_near_end_protected = false;
        s_last_near_retained_energy_percent = (uint8_t)retained_percent;
        s_last_near_nlp_reduction_percent = 0U;
        s_last_near_reference_coherence_percent = 0U;
        s_last_near_reject_reason = near_end_present ?
            AUDIO_ECHO_NEAR_REJECT_NONE :
            AUDIO_ECHO_NEAR_REJECT_LOW_RETAINED_ENERGY;
        s_near_end_protection_hangover_frames = 0;
        s_near_end_blend_q8 = 0;
        if (debug_nlp != NULL) {
            /*
             * VOIP mode exposes only the combined AEC output. Keep the debug
             * capture representative by recording that same output in the
             * linear/NLP/final slots instead of switching algorithms while a
             * measurement is active. Raw microphone and playback reference
             * remain independent channels for gain and echo-path diagnosis.
             */
            memcpy(debug_nlp,
                   s_out_frame,
                   (size_t)s_aec_frame_size * sizeof(debug_nlp[0]));
            audio_aec_debug_capture_frame(analysis_mic,
                                          analysis_ref,
                                          s_out_frame,
                                          debug_nlp,
                                          s_out_frame,
                                          0U,
                                          (size_t)s_aec_frame_size);
        }
        return s_out_frame;
    }

    if (!reference_active) {
        /*
         * ESP-SR returns one algorithm frame later than its input. Returning
         * the current raw microphone frame here, then switching to the delayed
         * AEC output when playback resumes, makes the uplink timeline jump by
         * 32 ms at every far-end speech pause. Keep the linear AEC path running
         * and return its delayed output for a continuous timeline. NLP still
         * runs only for diagnostics in this branch; near-end-only speech is
         * intentionally taken before nonlinear suppression.
         */
        aec_linear_process(s_aec, s_mic_frame, s_ref_frame, s_out_frame);
        memcpy(s_linear_frame,
               s_out_frame,
               (size_t)s_aec_frame_size * sizeof(s_linear_frame[0]));
        if (debug_nlp != NULL) {
            memcpy(debug_nlp,
                   s_linear_frame,
                   (size_t)s_aec_frame_size * sizeof(debug_nlp[0]));
            audio_aec_debug_capture_frame(analysis_mic,
                                          analysis_ref,
                                          s_linear_frame,
                                          debug_nlp,
                                          s_linear_frame,
                                          0U,
                                          (size_t)s_aec_frame_size);
        }
        s_last_linear_peak = 0;
        s_last_near_end_detected = false;
        s_last_near_end_protected = false;
        s_last_near_reject_reason = AUDIO_ECHO_NEAR_REJECT_NONE;
        s_near_end_candidate_frames = 0;
        s_reference_active_frames = 0;
        s_near_end_presence_hangover_frames = 0;
        s_near_end_protection_hangover_frames = 0;
        s_near_end_blend_q8 = 0;
        return s_linear_frame;
    }

    aec_linear_process(s_aec, s_mic_frame, s_ref_frame, s_out_frame);
    memcpy(s_linear_frame,
           s_out_frame,
           (size_t)s_aec_frame_size * sizeof(s_linear_frame[0]));
    if (debug_nlp != NULL) {
        memcpy(debug_nlp,
               s_linear_frame,
               (size_t)s_aec_frame_size * sizeof(debug_nlp[0]));
    }

    uint32_t linear_peak = 0;
    uint8_t retained_energy_percent = 0;
    uint8_t nlp_reduction_percent = 0;
    uint8_t reference_coherence_percent = 0;
    bool nlp_protection_needed = false;
    audio_echo_near_reject_reason_t reject_reason = AUDIO_ECHO_NEAR_REJECT_NONE;
    bool near_end_detected = audio_aec_detect_near_end(analysis_mic,
                                                       analysis_ref,
                                                       s_linear_frame,
                                                       s_linear_frame,
                                                       (uint32_t)s_aec_frame_size,
                                                       &linear_peak,
                                                       &retained_energy_percent,
                                                       &nlp_reduction_percent,
                                                       &reference_coherence_percent,
                                                       &nlp_protection_needed,
                                                       &reject_reason);
    if (s_reference_active_frames < UINT32_MAX) {
        s_reference_active_frames++;
    }
    if (s_reference_active_frames < AUDIO_AEC_NEAR_END_ADAPT_FRAMES) {
        /* Early linear residual is filter convergence, not local speech. */
        near_end_detected = false;
    }
    s_near_decision_sequence++;
    s_last_near_retained_energy_percent = retained_energy_percent;
    s_last_near_nlp_reduction_percent = nlp_reduction_percent;
    s_last_near_reference_coherence_percent = reference_coherence_percent;
    s_last_near_reject_reason = reject_reason;
    if (near_end_detected) {
        if (s_near_end_candidate_frames < AUDIO_AEC_NEAR_END_CONFIRM_FRAMES) {
            s_near_end_candidate_frames++;
        }
    } else {
        s_near_end_candidate_frames = 0U;
    }
    bool near_end_confirmed =
        s_near_end_candidate_frames >= AUDIO_AEC_NEAR_END_CONFIRM_FRAMES;
    if (near_end_confirmed) {
        s_near_end_presence_hangover_frames =
            AUDIO_AEC_NEAR_END_PRESENCE_HANGOVER_FRAMES;
    } else if (s_near_end_presence_hangover_frames > 0U) {
        s_near_end_presence_hangover_frames--;
    }
    if (near_end_confirmed && nlp_protection_needed) {
        s_near_end_protection_hangover_frames =
            AUDIO_AEC_NEAR_END_PROTECTION_HANGOVER_FRAMES;
    } else if (s_near_end_protection_hangover_frames > 0U) {
        s_near_end_protection_hangover_frames--;
    }

    /*
     * FD_HIGH's linear output is the measured-better far-end signal, but on
     * real double talk it retains only about 8-15% of near-end energy. AGC can
     * restore level, not consonants already removed by the adaptive filter.
     * Mix back a bounded 18.75% of the time-aligned microphone only after the
     * detector has proved low reference coherence and retained near-end energy.
     * Pure far-end echo never enters this branch, so the normal echo path stays
     * fully cancelled. Release over three 32 ms frames to avoid cutting word
     * endings without leaving a long raw-echo window after local speech.
     */
    /*
     * Near-end classification also drives the post-AEC far-end gain guard.
     * Keep that classification available even when raw-microphone blending is
     * disabled: otherwise every far-end residual and every real double-talk
     * frame look identical to the gain controller. Only the blend itself is
     * controlled by near_end_protection_enabled.
     */
    bool protect = near_end_protection_enabled &&
                   s_near_end_presence_hangover_frames > 0U;
    uint32_t blend_target_q8 = protect ? AUDIO_AEC_NEAR_END_RAW_BLEND_Q8 : 0U;
    if (s_near_end_blend_q8 < blend_target_q8) {
        s_near_end_blend_q8 = blend_target_q8;
    } else if (s_near_end_blend_q8 > blend_target_q8) {
        uint32_t release = AUDIO_AEC_NEAR_END_BLEND_RELEASE_STEP_Q8;
        s_near_end_blend_q8 = s_near_end_blend_q8 > release ?
            s_near_end_blend_q8 - release : 0U;
    }

    if (s_near_end_blend_q8 > 0U) {
        uint32_t linear_q8 = 256U - s_near_end_blend_q8;
        for (int index = 0; index < s_aec_frame_size; ++index) {
            int32_t mixed = ((int32_t)s_linear_frame[index] * (int32_t)linear_q8 +
                             (int32_t)analysis_mic[index] * (int32_t)s_near_end_blend_q8) /
                            256;
            s_out_frame[index] = mixed > INT16_MAX ? INT16_MAX :
                                 (mixed < INT16_MIN ? INT16_MIN : (int16_t)mixed);
        }
    } else {
        memcpy(s_out_frame,
               s_linear_frame,
               (size_t)s_aec_frame_size * sizeof(s_out_frame[0]));
    }

    if (debug_nlp != NULL) {
        audio_aec_debug_capture_frame(analysis_mic,
                                      analysis_ref,
                                      s_out_frame,
                                      debug_nlp,
                                      s_linear_frame,
                                      (uint16_t)s_near_end_blend_q8,
                                      (size_t)s_aec_frame_size);
    }
    s_last_linear_peak = linear_peak;
    s_last_near_end_detected = s_near_end_presence_hangover_frames > 0U;
    s_last_near_end_protected = protect;
    return s_out_frame;
}

static void audio_aec_fifo_clear(void)
{
    s_out_fifo_read_pos = 0;
    s_out_fifo_used = 0;
    s_out_fifo_ready = false;
    s_analysis_frame_valid = false;
}

static uint32_t audio_aec_gcd_u32(uint32_t lhs, uint32_t rhs)
{
    while (rhs != 0U) {
        uint32_t remainder = lhs % rhs;
        lhs = rhs;
        rhs = remainder;
    }
    return lhs;
}

static void audio_aec_fifo_push(const int16_t *raw_samples,
                                const int16_t *processed_samples,
                                uint32_t count)
{
    if (s_raw_fifo == NULL || s_out_fifo == NULL || raw_samples == NULL ||
        processed_samples == NULL || count == 0U) {
        return;
    }
    if (count > AUDIO_AEC_OUT_FIFO_SAMPLES) {
        raw_samples += count - AUDIO_AEC_OUT_FIFO_SAMPLES;
        processed_samples += count - AUDIO_AEC_OUT_FIFO_SAMPLES;
        count = AUDIO_AEC_OUT_FIFO_SAMPLES;
    }
    if (count > AUDIO_AEC_OUT_FIFO_SAMPLES - s_out_fifo_used) {
        uint32_t drop = count - (AUDIO_AEC_OUT_FIFO_SAMPLES - s_out_fifo_used);
        s_out_fifo_read_pos = (s_out_fifo_read_pos + drop) & AUDIO_AEC_OUT_FIFO_MASK;
        s_out_fifo_used -= drop;
    }

    uint32_t write_pos = (s_out_fifo_read_pos + s_out_fifo_used) & AUDIO_AEC_OUT_FIFO_MASK;
    for (uint32_t index = 0; index < count; ++index) {
        s_raw_fifo[write_pos] = raw_samples[index];
        s_out_fifo[write_pos] = processed_samples[index];
        write_pos = (write_pos + 1U) & AUDIO_AEC_OUT_FIFO_MASK;
    }
    s_out_fifo_used += count;
}

static bool audio_aec_fifo_pop(int16_t *raw_samples,
                               int16_t *processed_samples,
                               uint32_t count,
                               uint32_t *raw_peak,
                               uint32_t *out_peak)
{
    if (s_raw_fifo == NULL || s_out_fifo == NULL || count == 0U) {
        return false;
    }

    if (!s_out_fifo_ready) {
        uint32_t frame_size = (uint32_t)(s_aec_frame_size > 0 ? s_aec_frame_size : 0);
        uint32_t cadence_step = audio_aec_gcd_u32(count, frame_size);
        uint32_t prefill = count + frame_size - cadence_step;
        if (s_out_fifo_used < prefill) {
            return false;
        }
        s_out_fifo_ready = true;
    }
    if (s_out_fifo_used < count) {
        s_out_fifo_ready = false;
        return false;
    }

    uint32_t input_peak = 0;
    uint32_t output_peak = 0;
    for (uint32_t index = 0; index < count; ++index) {
        int16_t raw_sample = s_raw_fifo[s_out_fifo_read_pos];
        int16_t processed_sample = s_out_fifo[s_out_fifo_read_pos];
        if (raw_samples != NULL) {
            raw_samples[index] = raw_sample;
        }
        if (processed_samples != NULL) {
            processed_samples[index] = processed_sample;
        }
        uint32_t raw_abs = audio_aec_abs_i16(raw_sample);
        if (raw_abs > input_peak) {
            input_peak = raw_abs;
        }
        uint32_t processed_abs = audio_aec_abs_i16(processed_sample);
        if (processed_abs > output_peak) {
            output_peak = processed_abs;
        }
        s_out_fifo_read_pos = (s_out_fifo_read_pos + 1U) & AUDIO_AEC_OUT_FIFO_MASK;
    }
    s_out_fifo_used -= count;
    if (raw_peak != NULL) {
        *raw_peak = input_peak;
    }
    if (out_peak != NULL) {
        *out_peak = output_peak;
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

    const int64_t now_us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_aec_lock);
    s_ref_write_pos = write_pos;
    s_ref_filled_samples = filled_samples;
    if (peak >= AUDIO_AEC_REF_ACTIVE_PEAK) {
        /*
         * Keep the AEC path active across short silent frames. Speech and
         * packetized playback naturally contain brief zero/low-energy gaps;
         * overwriting the active peak with every silent packet defeated the
         * short acoustic-tail hangover and repeatedly exposed raw microphone
         * frames while the filter FIFO was warming up again.
         */
        s_last_ref_peak = peak;
        s_last_playback_us = now_us;
    } else if (s_last_playback_us > 0 &&
               now_us >= s_last_playback_us &&
               now_us - s_last_playback_us > AUDIO_AEC_REF_ACTIVE_US) {
        s_last_ref_peak = 0;
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
    audio_aec_apply_requested_suppression();
    audio_aec_debug_apply_injection(samples, sample_count);

    int16_t *ref_ring = NULL;
    uint32_t read_start = 0;
    uint32_t ref_peak = 0;
    if (!audio_aec_ref_snapshot(sample_count, &ref_ring, &read_start, &ref_peak)) {
        s_frame_fill = 0;
        audio_aec_fifo_clear();
        audio_aec_leave();
        return;
    }
    if (metrics != NULL) {
        metrics->reference_active = true;
        metrics->ref_peak = ref_peak;
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
            audio_aec_snapshot_input_frame();
            aec_process(s_aec, s_mic_frame, s_ref_frame, s_out_frame);
            audio_aec_fifo_push(s_analysis_mic_frame,
                                s_out_frame,
                                (uint32_t)s_aec_frame_size);
            s_frame_fill = 0;
        }
    }

    uint32_t raw_peak = 0;
    uint32_t out_peak = 0;
    bool processed = audio_aec_fifo_pop(NULL,
                                        samples,
                                        (uint32_t)sample_count,
                                        &raw_peak,
                                        &out_peak);
    if (metrics != NULL && processed) {
        metrics->active = true;
        metrics->ref_peak = ref_peak;
        metrics->mic_peak = raw_peak;
        metrics->out_peak = out_peak;
        metrics->delay_samples = AUDIO_AEC_REF_DELAY_SAMPLES;
        if (raw_peak > 0U && out_peak < raw_peak) {
            metrics->suppress_percent = (uint8_t)(((uint64_t)(raw_peak - out_peak) * 100U) / raw_peak);
        }
    }
    audio_aec_leave();
}

void audio_echo_cancel_process_capture_with_reference(int16_t *samples,
                                                      const int16_t *reference,
                                                      size_t sample_count,
                                                      bool continuous_processing,
                                                      bool near_end_protection_enabled,
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
    audio_aec_apply_requested_suppression();
    audio_aec_debug_apply_injection(samples, sample_count);

    bool playback_active = audio_aec_playback_is_active();
    /*
     * Non-call users keep the historical on-demand path. Full-duplex device
     * calls keep the ESP-SR adapter continuously primed, including quiet
     * speaker intervals. Restarting it for every talkspurt otherwise creates
     * missing output frames whenever the 20 ms capture cadence and the AEC
     * algorithm frame size are not equal.
     */
    if (!continuous_processing && !playback_active) {
        s_frame_fill = 0;
        s_frame_ref_peak = 0;
        s_frame_playback_active = false;
        s_near_end_candidate_frames = 0;
        s_reference_active_frames = 0;
        s_near_end_presence_hangover_frames = 0;
        s_near_end_protection_hangover_frames = 0;
        s_near_end_blend_q8 = 0;
        audio_aec_fifo_clear();
        audio_aec_leave();
        return;
    }

    uint32_t ref_peak = 0;
    for (size_t index = 0; index < sample_count; ++index) {
        uint32_t abs_sample = audio_aec_abs_i16(reference[index]);
        if (abs_sample > ref_peak) {
            ref_peak = abs_sample;
        }
    }

    /*
     * In a full-duplex call, committed speaker playback is the lifecycle
     * signal that echo cancellation is required. MIC3 supplies the physical
     * reference waveform, but a single low-energy MIC3 frame must not expose
     * the raw microphone while speech or its acoustic tail is still playing.
     * Volume zero and speaker write failures reset the playback state at the
     * audio driver boundary, so this does not keep AEC active for muted audio.
     */
    bool capture_reference_active = playback_active &&
                                    (continuous_processing ||
                                     ref_peak >= AUDIO_AEC_REF_ACTIVE_PEAK);
    if (metrics != NULL) {
        /* Keep the MIC3 peak visible for physical-reference diagnostics. */
        metrics->reference_active = capture_reference_active;
        metrics->output_bypassed = continuous_processing && !capture_reference_active;
        metrics->ref_peak = ref_peak;
    }

    uint32_t mic_peak = 0;
    if (playback_active) {
        s_frame_playback_active = true;
    }
    for (size_t index = 0; index < sample_count; ++index) {
        int16_t mic_sample = samples[index];
        int16_t ref_sample = reference[index];
        uint32_t mic_abs = audio_aec_abs_i16(mic_sample);
        if (mic_abs > mic_peak) {
            mic_peak = mic_abs;
        }

        s_mic_frame[s_frame_fill] = mic_sample;
        s_ref_frame[s_frame_fill] = ref_sample;
        uint32_t ref_abs = audio_aec_abs_i16(ref_sample);
        if (ref_abs > s_frame_ref_peak) {
            s_frame_ref_peak = ref_abs;
        }
        s_frame_fill++;

        if (s_frame_fill >= (uint32_t)s_aec_frame_size) {
            bool frame_reference_active = s_frame_playback_active &&
                                          (continuous_processing ||
                                           s_frame_ref_peak >= AUDIO_AEC_REF_ACTIVE_PEAK);
            const int16_t *selected_output =
                audio_aec_process_frame(near_end_protection_enabled,
                                        frame_reference_active);
            audio_aec_fifo_push(s_analysis_mic_frame,
                                selected_output,
                                (uint32_t)s_aec_frame_size);
            s_frame_fill = 0;
            s_frame_ref_peak = 0;
            /*
             * One 20 ms capture callback contains 320 samples while ESP-SR
             * VOIP consumes 256-sample frames. A callback can therefore finish
             * one AEC frame and immediately start another. The remaining
             * samples still belong to the same playback-active interval; clear
             * this flag here and every fifth AEC frame bypasses cancellation
             * (256 raw samples leak about once per 80 ms). Seed the next partial
             * frame with the current callback state instead.
             */
            s_frame_playback_active = playback_active;
        }
    }

    uint32_t raw_peak = 0;
    uint32_t out_peak = 0;
    /*
     * The output decision was made when each algorithm frame was produced.
     * Always consume that delayed decision here; using current playback state
     * to select adapter-delayed samples creates a discontinuity at every
     * playback transition.
     */
    bool processed = audio_aec_fifo_pop(NULL,
                                        samples,
                                        (uint32_t)sample_count,
                                        &raw_peak,
                                        &out_peak);
    if (!processed && continuous_processing) {
        /*
         * Keep the uplink time axis monotonic while the fixed-latency AEC FIFO
         * is filling. Returning the current raw frame and then older processed
         * samples duplicates the opening audio (for example, "123" becomes
         * repeated syllables). A short zero pre-roll is deterministic and only
         * occurs once when the continuous call pipeline is primed.
         */
        memset(samples, 0, sample_count * sizeof(samples[0]));
        if (metrics != NULL) {
            metrics->warming_up = true;
            metrics->warmup_near_end_passthrough = false;
            metrics->mic_peak = mic_peak;
            metrics->out_peak = 0U;
        }
    }
    if (metrics != NULL) {
        metrics->mic_peak = processed ? raw_peak : mic_peak;
        metrics->linear_peak = s_last_linear_peak;
        metrics->near_end_detected = s_last_near_end_detected;
        metrics->near_end_protected = s_last_near_end_protected;
        metrics->near_decision_sequence = s_near_decision_sequence;
        metrics->near_retained_energy_percent = s_last_near_retained_energy_percent;
        metrics->near_nlp_reduction_percent = s_last_near_nlp_reduction_percent;
        metrics->near_reference_coherence_percent = s_last_near_reference_coherence_percent;
        metrics->near_reject_reason = s_last_near_reject_reason;
    }
    if (metrics != NULL && processed && capture_reference_active) {
        metrics->active = true;
        metrics->ref_peak = ref_peak;
        metrics->out_peak = out_peak;
        metrics->delay_samples = 0;
        if (raw_peak > 0U && out_peak < raw_peak) {
            metrics->suppress_percent = (uint8_t)(((uint64_t)(raw_peak - out_peak) * 100U) / raw_peak);
        }
    }
    audio_aec_leave();
}

esp_err_t audio_echo_cancel_debug_capture_start(uint32_t duration_ms)
{
    if (duration_ms < AUDIO_AEC_DEBUG_CAPTURE_MIN_MS ||
        duration_ms > AUDIO_AEC_DEBUG_CAPTURE_MAX_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!audio_aec_ensure_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t frame_size = 0;
    taskENTER_CRITICAL(&s_aec_lock);
    if (s_debug_capture.samples != NULL || s_debug_capture.recording) {
        taskEXIT_CRITICAL(&s_aec_lock);
        return ESP_ERR_INVALID_STATE;
    }
    frame_size = (size_t)s_aec_frame_size;
    taskEXIT_CRITICAL(&s_aec_lock);
    if (frame_size == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t requested_frames =
        ((size_t)AUDIO_AEC_SAMPLE_RATE_HZ * duration_ms + 999U) / 1000U;
    size_t capacity_frames =
        ((requested_frames + frame_size - 1U) / frame_size) * frame_size;
    if (capacity_frames > SIZE_MAX / AUDIO_AEC_DEBUG_CHANNELS / sizeof(int16_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t capture_bytes = capacity_frames * AUDIO_AEC_DEBUG_CHANNELS * sizeof(int16_t);
    int16_t *capture = heap_caps_aligned_calloc(AUDIO_AEC_BUFFER_ALIGNMENT,
                                                1U,
                                                capture_bytes,
                                                AUDIO_AEC_PSRAM_CAPS);
    int16_t *nlp_scratch = heap_caps_aligned_calloc(AUDIO_AEC_BUFFER_ALIGNMENT,
                                                    frame_size,
                                                    sizeof(int16_t),
                                                    AUDIO_AEC_PSRAM_CAPS);
    if (capture == NULL || nlp_scratch == NULL) {
        heap_caps_free(capture);
        heap_caps_free(nlp_scratch);
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&s_aec_lock);
    if (s_debug_capture.samples != NULL || s_debug_capture.recording) {
        taskEXIT_CRITICAL(&s_aec_lock);
        heap_caps_free(capture);
        heap_caps_free(nlp_scratch);
        return ESP_ERR_INVALID_STATE;
    }
    s_debug_capture.samples = capture;
    s_debug_capture.nlp_scratch = nlp_scratch;
    s_debug_capture.capacity_frames = capacity_frames;
    s_debug_capture.captured_frames = 0;
    s_debug_capture.recording = true;
    s_debug_capture.complete = false;
    s_debug_injection.phase = 0;
    taskEXIT_CRITICAL(&s_aec_lock);

    ESP_LOGI(TAG,
             "AEC debug capture started: duration=%ums frames=%u channels=%u psram=%uB",
             (unsigned)duration_ms,
             (unsigned)capacity_frames,
             (unsigned)AUDIO_AEC_DEBUG_CHANNELS,
             (unsigned)(capture_bytes + frame_size * sizeof(int16_t)));
    return ESP_OK;
}

void audio_echo_cancel_debug_capture_get_status(
    audio_echo_cancel_debug_capture_status_t *status)
{
    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));
    taskENTER_CRITICAL(&s_aec_lock);
    status->allocated = s_debug_capture.samples != NULL;
    status->recording = s_debug_capture.recording;
    status->complete = s_debug_capture.complete;
    status->sample_rate_hz = AUDIO_AEC_SAMPLE_RATE_HZ;
    status->channel_count = AUDIO_AEC_DEBUG_CHANNELS;
    status->captured_frames = s_debug_capture.captured_frames;
    status->capacity_frames = s_debug_capture.capacity_frames;
    status->data_bytes = s_debug_capture.captured_frames *
                         AUDIO_AEC_DEBUG_CHANNELS * sizeof(int16_t);
    taskEXIT_CRITICAL(&s_aec_lock);
}

esp_err_t audio_echo_cancel_debug_capture_read(size_t offset,
                                               void *buffer,
                                               size_t buffer_size,
                                               size_t *bytes_read)
{
    if (buffer == NULL || buffer_size == 0U || bytes_read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *bytes_read = 0;

    const uint8_t *capture = NULL;
    size_t data_bytes = 0;
    taskENTER_CRITICAL(&s_aec_lock);
    if (s_debug_capture.complete && s_debug_capture.samples != NULL) {
        capture = (const uint8_t *)s_debug_capture.samples;
        data_bytes = s_debug_capture.captured_frames *
                     AUDIO_AEC_DEBUG_CHANNELS * sizeof(int16_t);
    }
    taskEXIT_CRITICAL(&s_aec_lock);
    if (capture == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (offset > data_bytes) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t copy_bytes = data_bytes - offset;
    if (copy_bytes > buffer_size) {
        copy_bytes = buffer_size;
    }
    memcpy(buffer, capture + offset, copy_bytes);
    *bytes_read = copy_bytes;
    return ESP_OK;
}

esp_err_t audio_echo_cancel_debug_capture_free(void)
{
    int16_t *capture = NULL;
    int16_t *nlp_scratch = NULL;

    taskENTER_CRITICAL(&s_aec_lock);
    if (s_debug_capture.recording) {
        taskEXIT_CRITICAL(&s_aec_lock);
        return ESP_ERR_INVALID_STATE;
    }
    capture = s_debug_capture.samples;
    nlp_scratch = s_debug_capture.nlp_scratch;
    memset(&s_debug_capture, 0, sizeof(s_debug_capture));
    memset(&s_debug_injection, 0, sizeof(s_debug_injection));
    taskEXIT_CRITICAL(&s_aec_lock);

    heap_caps_free(capture);
    heap_caps_free(nlp_scratch);
    return ESP_OK;
}

esp_err_t audio_echo_cancel_debug_injection_set(uint32_t frequency_hz,
                                                uint16_t amplitude)
{
    if ((frequency_hz == 0U && amplitude != 0U) ||
        (frequency_hz != 0U && (frequency_hz < 200U || frequency_hz > 3000U)) ||
        amplitude > 12000U) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_aec_lock);
    s_debug_injection.frequency_hz = frequency_hz;
    s_debug_injection.amplitude = amplitude;
    s_debug_injection.phase = 0;
    taskEXIT_CRITICAL(&s_aec_lock);
    return ESP_OK;
}

void audio_echo_cancel_reset(void)
{
    taskENTER_CRITICAL(&s_aec_lock);
    s_ref_write_pos = 0;
    s_ref_filled_samples = 0;
    s_last_ref_peak = 0;
    s_last_playback_us = 0;
    s_frame_fill = 0;
    s_frame_ref_peak = 0;
    s_frame_playback_active = false;
    s_out_fifo_read_pos = 0;
    s_out_fifo_used = 0;
    s_out_fifo_ready = false;
    s_near_end_candidate_frames = 0;
    s_reference_active_frames = 0;
    s_near_end_presence_hangover_frames = 0;
    s_near_end_protection_hangover_frames = 0;
    s_near_end_blend_q8 = 0;
    s_last_linear_peak = 0;
    s_last_near_end_detected = false;
    s_last_near_end_protected = false;
    s_near_decision_sequence = 0;
    s_last_near_retained_energy_percent = 0;
    s_last_near_nlp_reduction_percent = 0;
    s_last_near_reference_coherence_percent = 0;
    s_last_near_reject_reason = AUDIO_ECHO_NEAR_REJECT_NONE;
    memset(&s_debug_injection, 0, sizeof(s_debug_injection));
    taskEXIT_CRITICAL(&s_aec_lock);
}

void audio_echo_cancel_deinit(void)
{
    aec_handle_t *handle = NULL;
    int16_t *ref_ring = NULL;
    int16_t *mic_frame = NULL;
    int16_t *ref_frame = NULL;
    int16_t *out_frame = NULL;
    int16_t *linear_frame = NULL;
    int16_t *analysis_mic_frame = NULL;
    int16_t *analysis_ref_frame = NULL;
    int16_t *raw_fifo = NULL;
    int16_t *out_fifo = NULL;
    int16_t *debug_nlp_scratch = NULL;

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
    linear_frame = s_linear_frame;
    analysis_mic_frame = s_analysis_mic_frame;
    analysis_ref_frame = s_analysis_ref_frame;
    raw_fifo = s_raw_fifo;
    out_fifo = s_out_fifo;
    s_aec = NULL;
    s_aec_frame_size = 0;
    s_ref_ring = NULL;
    s_mic_frame = NULL;
    s_ref_frame = NULL;
    s_out_frame = NULL;
    s_linear_frame = NULL;
    s_analysis_mic_frame = NULL;
    s_analysis_ref_frame = NULL;
    s_raw_fifo = NULL;
    s_out_fifo = NULL;
    s_ref_write_pos = 0;
    s_ref_filled_samples = 0;
    s_last_ref_peak = 0;
    s_last_playback_us = 0;
    s_frame_fill = 0;
    s_frame_ref_peak = 0;
    s_frame_playback_active = false;
    s_out_fifo_read_pos = 0;
    s_out_fifo_used = 0;
    s_out_fifo_ready = false;
    s_analysis_frame_valid = false;
    s_near_end_candidate_frames = 0;
    s_reference_active_frames = 0;
    s_near_end_presence_hangover_frames = 0;
    s_near_end_protection_hangover_frames = 0;
    s_near_end_blend_q8 = 0;
    s_last_linear_peak = 0;
    s_last_near_end_detected = false;
    s_last_near_end_protected = false;
    s_near_decision_sequence = 0;
    s_last_near_retained_energy_percent = 0;
    s_last_near_nlp_reduction_percent = 0;
    s_last_near_reference_coherence_percent = 0;
    s_last_near_reject_reason = AUDIO_ECHO_NEAR_REJECT_NONE;
    debug_nlp_scratch = s_debug_capture.nlp_scratch;
    s_debug_capture.nlp_scratch = NULL;
    if (s_debug_capture.recording) {
        /* Keep any captured evidence readable after an app tears AEC down. */
        s_debug_capture.recording = false;
        s_debug_capture.complete = s_debug_capture.captured_frames > 0U;
    }
    s_deinit_requested = false;
    s_create_failed_logged = false;
    taskEXIT_CRITICAL(&s_aec_lock);

    audio_aec_free_handle_and_buffers(handle,
                                      ref_ring,
                                      mic_frame,
                                      ref_frame,
                                      out_frame,
                                      linear_frame,
                                      analysis_mic_frame,
                                      analysis_ref_frame,
                                      raw_fifo,
                                      out_fifo);
    heap_caps_free(debug_nlp_scratch);
    if (handle != NULL || ref_ring != NULL || mic_frame != NULL || ref_frame != NULL ||
        out_frame != NULL || linear_frame != NULL || analysis_mic_frame != NULL ||
        analysis_ref_frame != NULL || out_fifo != NULL) {
        ESP_LOGI(TAG, "official ESP-SR AEC released");
        audio_aec_log_heap("AEC released");
    }
}

#else

bool audio_echo_cancel_preload(void)
{
    return false;
}

void audio_echo_cancel_set_suppression(audio_echo_suppression_t suppression)
{
    (void)suppression;
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
                                                      bool continuous_processing,
                                                      bool near_end_protection_enabled,
                                                      audio_echo_cancel_metrics_t *metrics)
{
    (void)samples;
    (void)reference;
    (void)sample_count;
    (void)continuous_processing;
    (void)near_end_protection_enabled;
    if (metrics != NULL) {
        memset(metrics, 0, sizeof(*metrics));
    }
}

esp_err_t audio_echo_cancel_debug_capture_start(uint32_t duration_ms)
{
    (void)duration_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

void audio_echo_cancel_debug_capture_get_status(
    audio_echo_cancel_debug_capture_status_t *status)
{
    if (status != NULL) {
        memset(status, 0, sizeof(*status));
    }
}

esp_err_t audio_echo_cancel_debug_capture_read(size_t offset,
                                               void *buffer,
                                               size_t buffer_size,
                                               size_t *bytes_read)
{
    (void)offset;
    (void)buffer;
    (void)buffer_size;
    if (bytes_read != NULL) {
        *bytes_read = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_echo_cancel_debug_capture_free(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void audio_echo_cancel_reset(void)
{
}

void audio_echo_cancel_deinit(void)
{
}

#endif
