#include "camera_pipeline.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "sdkconfig.h"
#include "tiRTC.h"

#include "app_task_affinity.h"
#include "camera_driver.h"
#include "media_dma_reserve.h"
#include "media_governor.h"

static const char *TAG = "camera_pipeline";

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

#define CAMERA_PIPELINE_TASK_STACK       (9U * 1024U)
#define CAMERA_PIPELINE_TASK_PRIORITY    10U
#define CAMERA_PIPELINE_RETRY_DELAY_MS   200U
#define CAMERA_PIPELINE_START_DELAY_MS   40U
#define CAMERA_PIPELINE_LOG_INTERVAL_MS  10000U
#define CAMERA_PIPELINE_H264_OPEN_RETRY_MS 2000U
#define CAMERA_PIPELINE_H264_BUFFER_CNT  1U
#ifndef CONFIG_APP_RTC_H264_BITRATE
#define CONFIG_APP_RTC_H264_BITRATE 6000000
#endif
#ifndef CONFIG_APP_RTC_H264_FPS
#define CONFIG_APP_RTC_H264_FPS 20
#endif
#ifndef CONFIG_APP_RTC_H264_DIRECT_HW_ENCODER
#define CONFIG_APP_RTC_H264_DIRECT_HW_ENCODER 0
#endif
#ifndef CONFIG_APP_RTC_H264_GOP
#define CONFIG_APP_RTC_H264_GOP CONFIG_APP_RTC_H264_FPS
#endif
#ifndef CONFIG_APP_RTC_H264_MIN_QP
#define CONFIG_APP_RTC_H264_MIN_QP 34
#endif
#ifndef CONFIG_APP_RTC_H264_MAX_QP
#define CONFIG_APP_RTC_H264_MAX_QP 45
#endif
#ifndef CONFIG_APP_RTC_H264_OUTPUT_BUFFER_BYTES
#define CONFIG_APP_RTC_H264_OUTPUT_BUFFER_BYTES (3U * 1024U * 1024U)
#endif
#ifndef CONFIG_APP_RTC_H264_MAX_DELTA_PAYLOAD_BYTES
#define CONFIG_APP_RTC_H264_MAX_DELTA_PAYLOAD_BYTES (128U * 1024U)
#endif
#ifndef CONFIG_APP_RTC_H264_STARTUP_GUARD_MS
#define CONFIG_APP_RTC_H264_STARTUP_GUARD_MS 2500
#endif
#ifndef CONFIG_APP_RTC_H264_STARTUP_MAX_DELTA_PAYLOAD_BYTES
#define CONFIG_APP_RTC_H264_STARTUP_MAX_DELTA_PAYLOAD_BYTES (128U * 1024U)
#endif
#ifndef CONFIG_APP_CAMERA_FRAME_TRACE_INITIAL_COUNT
#define CONFIG_APP_CAMERA_FRAME_TRACE_INITIAL_COUNT 0
#endif
#ifndef CONFIG_APP_CAMERA_FRAME_TRACE_INTERVAL_MS
#define CONFIG_APP_CAMERA_FRAME_TRACE_INTERVAL_MS 10000
#endif
#ifndef CONFIG_APP_RTC_H264_KEY_FRAME_REQUEST_MIN_INTERVAL_MS
#define CONFIG_APP_RTC_H264_KEY_FRAME_REQUEST_MIN_INTERVAL_MS 100
#endif
#define CAMERA_PIPELINE_H264_BITRATE     CONFIG_APP_RTC_H264_BITRATE
#define CAMERA_PIPELINE_H264_MIN_QP      CONFIG_APP_RTC_H264_MIN_QP
#define CAMERA_PIPELINE_H264_MAX_QP      CONFIG_APP_RTC_H264_MAX_QP
#define CAMERA_PIPELINE_H264_STRICT_TARGET 1
#define CAMERA_PIPELINE_H264_DIM_ALIGN   16U
#define CAMERA_PIPELINE_H264_MIN_WIDTH   320U
#define CAMERA_PIPELINE_H264_MIN_HEIGHT  240U
#define CAMERA_PIPELINE_H264_INTERNAL_MARGIN 2048U
#define CAMERA_PIPELINE_H264_FALLBACK_OUTPUT_BUFFER_BYTES CONFIG_APP_RTC_H264_OUTPUT_BUFFER_BYTES
#define CAMERA_PIPELINE_H264_FALLBACK_MAX_DELTA_PAYLOAD CONFIG_APP_RTC_H264_MAX_DELTA_PAYLOAD_BYTES
#define CAMERA_PIPELINE_FALLBACK_DMA_FREE_MIN_BYTES     (8U * 1024U)
#define CAMERA_PIPELINE_FALLBACK_DMA_LARGEST_MIN_BYTES  (4U * 1024U)
#define CAMERA_PIPELINE_TRANSPORT_GUARD_LOG_INTERVAL_MS 1000U
#define CAMERA_PIPELINE_FRAME_TRACE_INITIAL_COUNT       CONFIG_APP_CAMERA_FRAME_TRACE_INITIAL_COUNT
#define CAMERA_PIPELINE_FRAME_TRACE_INTERVAL_MS         CONFIG_APP_CAMERA_FRAME_TRACE_INTERVAL_MS
#define CAMERA_PIPELINE_FRAME_TRACE_SLOW_STAGE_US       75000U
#define CAMERA_PIPELINE_FRAME_TRACE_SLOW_LOOP_US        100000U
#define CAMERA_PIPELINE_FRAME_TRACE_LARGE_PAYLOAD_BYTES (224U * 1024U)
#define CAMERA_PIPELINE_KEY_FRAME_REQUEST_MIN_INTERVAL_US \
    ((uint64_t)CONFIG_APP_RTC_H264_KEY_FRAME_REQUEST_MIN_INTERVAL_MS * 1000ULL)
#if CONFIG_CACHE_L2_CACHE_LINE_SIZE > CONFIG_CACHE_L1_CACHE_LINE_SIZE
#define CAMERA_PIPELINE_CACHE_LINE_SIZE CONFIG_CACHE_L2_CACHE_LINE_SIZE
#else
#define CAMERA_PIPELINE_CACHE_LINE_SIZE CONFIG_CACHE_L1_CACHE_LINE_SIZE
#endif

typedef struct {
    bool rtc_enabled;
    camera_pipeline_video_cb_t video_cb;
    void *video_ctx;
} camera_pipeline_runtime_t;

typedef struct {
    int fd;
    esp_h264_enc_handle_t direct_handle;
    esp_h264_enc_param_hw_handle_t direct_param;
    uint8_t *capture_buffer;
    size_t capture_buffer_size;
    size_t output_buffer_bytes;
    uint16_t width;
    uint16_t height;
    uint32_t bitrate_bps;
    uint8_t fps;
    uint8_t gop;
    uint8_t direct_active_gop;
    uint8_t v4l2_active_gop;
    bool direct_encoder;
    bool capture_streaming;
    bool output_streaming;
} camera_pipeline_h264_encoder_t;

typedef struct {
    int64_t sync_in_us;
    int64_t hw_us;
    int64_t sync_out_us;
} camera_pipeline_h264_timing_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static bool s_rtc_enabled;
static bool s_starting;
static TaskHandle_t s_task;
static camera_pipeline_video_cb_t s_video_cb;
static void *s_video_ctx;
static TickType_t s_last_rtc_fail_log_tick;
static TickType_t s_last_transport_guard_log_tick;
static TickType_t s_last_frame_trace_log_tick;
static bool s_h264_output_sync_noncacheable_logged;
static bool s_h264_input_sync_noncacheable_logged;
static bool s_h264_reserve_in_progress;
static bool s_key_frame_request_pending;
static uint64_t s_last_key_frame_request_us;
static uint64_t s_last_key_frame_us;
static uint64_t s_last_key_frame_request_drop_log_us;
static camera_pipeline_h264_encoder_t s_reserved_h264 = {
    .fd = -1,
};
static camera_pipeline_metrics_t s_metrics = {
    .configured_bitrate_bps = CAMERA_PIPELINE_H264_BITRATE,
};

static uint32_t camera_pipeline_interval_ms(uint8_t fps)
{
    if (fps == 0U) {
        return 1000U;
    }
    uint32_t interval = 1000U / fps;
    return interval == 0U ? 1U : interval;
}

static uint64_t camera_pipeline_abs_delta_us(uint64_t a, uint64_t b)
{
    return a >= b ? a - b : b - a;
}

static bool camera_pipeline_time_due(TickType_t now, TickType_t *last_tick, uint32_t interval_ms)
{
    TickType_t interval_ticks = pdMS_TO_TICKS(interval_ms == 0U ? 1U : interval_ms);
    if (*last_tick == 0 || (now - *last_tick) >= interval_ticks) {
        *last_tick = now;
        return true;
    }
    return false;
}

static uint32_t camera_pipeline_wait_until_due_ms(TickType_t now, TickType_t last_tick, uint32_t interval_ms)
{
    if (last_tick == 0) {
        return 1U;
    }

    TickType_t interval_ticks = pdMS_TO_TICKS(interval_ms == 0U ? 1U : interval_ms);
    TickType_t elapsed_ticks = now - last_tick;
    if (elapsed_ticks >= interval_ticks) {
        return 1U;
    }

    TickType_t wait_ticks = interval_ticks - elapsed_ticks;
    uint32_t wait_ms = pdTICKS_TO_MS(wait_ticks);
    return wait_ms == 0U ? 1U : wait_ms;
}

static bool camera_pipeline_tick_reached(TickType_t now, TickType_t due)
{
    return (int32_t)(now - due) >= 0;
}

static camera_pipeline_runtime_t camera_pipeline_snapshot(void)
{
    camera_pipeline_runtime_t runtime = {0};

    taskENTER_CRITICAL(&s_lock);
    runtime.rtc_enabled = s_rtc_enabled;
    runtime.video_cb = s_video_cb;
    runtime.video_ctx = s_video_ctx;
    taskEXIT_CRITICAL(&s_lock);
    return runtime;
}

static void camera_pipeline_reset_metrics(void)
{
    media_governor_camera_policy_t policy = {0};
    media_governor_get_camera_policy(&policy);

    taskENTER_CRITICAL(&s_lock);
    s_metrics = (camera_pipeline_metrics_t) {
        .running = true,
        .rtc_enabled = s_rtc_enabled,
        .width = policy.rtc_width,
        .height = policy.rtc_height,
        .target_fps = policy.rtc_video_fps,
        .configured_bitrate_bps = policy.h264_bitrate_bps,
    };
    s_last_transport_guard_log_tick = 0;
    s_last_frame_trace_log_tick = 0;
    taskEXIT_CRITICAL(&s_lock);
}

static void camera_pipeline_note_frame_metrics(uint16_t width,
                                               uint16_t height,
                                               uint8_t target_fps,
                                               uint32_t configured_bitrate_bps,
                                               bool direct_input)
{
    taskENTER_CRITICAL(&s_lock);
    s_metrics.running = true;
    s_metrics.rtc_enabled = s_rtc_enabled;
    s_metrics.width = width;
    s_metrics.height = height;
    s_metrics.target_fps = target_fps;
    s_metrics.configured_bitrate_bps = configured_bitrate_bps;
    s_metrics.direct_input = direct_input;
    taskEXIT_CRITICAL(&s_lock);
}

static void camera_pipeline_note_interval_metrics(uint32_t measured_fps_x10,
                                                  uint32_t measured_bitrate_kbps,
                                                  uint32_t avg_payload_bytes,
                                                  uint32_t dropped_frames,
                                                  uint32_t capture_failures,
                                                  uint32_t encode_failures)
{
    taskENTER_CRITICAL(&s_lock);
    s_metrics.measured_fps_x10 = measured_fps_x10;
    s_metrics.measured_bitrate_kbps = measured_bitrate_kbps;
    s_metrics.avg_payload_bytes = avg_payload_bytes;
    s_metrics.dropped_frames = dropped_frames;
    s_metrics.capture_failures = capture_failures;
    s_metrics.encode_failures = encode_failures;
    taskEXIT_CRITICAL(&s_lock);
}

static bool camera_pipeline_should_run(void)
{
    bool should_run = false;

    taskENTER_CRITICAL(&s_lock);
    should_run = s_rtc_enabled;
    taskEXIT_CRITICAL(&s_lock);
    return should_run;
}

static void camera_pipeline_mark_task_started(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_task = xTaskGetCurrentTaskHandle();
    s_starting = false;
    taskEXIT_CRITICAL(&s_lock);
}

static void camera_pipeline_mark_task_stopped(void)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_task == xTaskGetCurrentTaskHandle()) {
        s_task = NULL;
    }
    s_starting = false;
    s_metrics.running = false;
    s_metrics.rtc_enabled = s_rtc_enabled;
    taskEXIT_CRITICAL(&s_lock);
}

static void camera_pipeline_apply_profile(bool rtc_enabled)
{
    media_governor_set_profile(rtc_enabled ?
                               MEDIA_GOVERNOR_PROFILE_RTC_AV_SAFE :
                               MEDIA_GOVERNOR_PROFILE_IDLE);
}

static bool camera_pipeline_h264_is_key_frame(const uint8_t *data, size_t len)
{
    if (data == NULL || len < 5U) {
        return false;
    }

    for (size_t i = 0; i + 4U < len; ++i) {
        size_t nal_offset = 0;
        if (data[i] == 0x00 && data[i + 1U] == 0x00 && data[i + 2U] == 0x01) {
            nal_offset = i + 3U;
        } else if (i + 5U < len &&
                   data[i] == 0x00 && data[i + 1U] == 0x00 &&
                   data[i + 2U] == 0x00 && data[i + 3U] == 0x01) {
            nal_offset = i + 4U;
        } else {
            continue;
        }

        if (nal_offset >= len) {
            break;
        }
        uint8_t nal_type = data[nal_offset] & 0x1fU;
        if (nal_type == 5U || nal_type == 7U) {
            return true;
        }
    }
    return false;
}

static void camera_pipeline_get_policy(media_governor_camera_policy_t *policy)
{
    media_governor_get_camera_policy(policy);
    if (policy->h264_output_buffer_bytes == 0U) {
        policy->h264_output_buffer_bytes = CAMERA_PIPELINE_H264_FALLBACK_OUTPUT_BUFFER_BYTES;
    }
    if (policy->h264_max_delta_payload_bytes == 0U) {
        policy->h264_max_delta_payload_bytes = CAMERA_PIPELINE_H264_FALLBACK_MAX_DELTA_PAYLOAD;
    }
    if (policy->dma_free_min_bytes == 0U) {
        policy->dma_free_min_bytes = CAMERA_PIPELINE_FALLBACK_DMA_FREE_MIN_BYTES;
    }
    if (policy->dma_largest_min_bytes == 0U) {
        policy->dma_largest_min_bytes = CAMERA_PIPELINE_FALLBACK_DMA_LARGEST_MIN_BYTES;
    }
}

static bool camera_pipeline_should_hold_video_for_transport(size_t payload_len,
                                                            bool key_frame,
                                                            const media_governor_camera_policy_t *policy,
                                                            uint32_t stream_age_ms,
                                                            size_t *effective_max_delta,
                                                            const char **reason)
{
    if (reason != NULL) {
        *reason = NULL;
    }

    size_t max_delta = policy != NULL ? policy->h264_max_delta_payload_bytes :
                       CAMERA_PIPELINE_H264_FALLBACK_MAX_DELTA_PAYLOAD;
#if CONFIG_APP_RTC_H264_STARTUP_GUARD_MS > 0
    if (stream_age_ms < CONFIG_APP_RTC_H264_STARTUP_GUARD_MS &&
        max_delta > CONFIG_APP_RTC_H264_STARTUP_MAX_DELTA_PAYLOAD_BYTES) {
        max_delta = CONFIG_APP_RTC_H264_STARTUP_MAX_DELTA_PAYLOAD_BYTES;
    }
#endif
    if (effective_max_delta != NULL) {
        *effective_max_delta = max_delta;
    }

    size_t dma_free_min = policy != NULL ? policy->dma_free_min_bytes :
                          CAMERA_PIPELINE_FALLBACK_DMA_FREE_MIN_BYTES;
    size_t dma_largest_min = policy != NULL ? policy->dma_largest_min_bytes :
                             CAMERA_PIPELINE_FALLBACK_DMA_LARGEST_MIN_BYTES;

    if (!key_frame && payload_len > max_delta) {
        if (reason != NULL) {
            *reason = "delta payload burst";
        }
        return true;
    }

    size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!key_frame &&
        (dma_free < dma_free_min ||
         dma_largest < dma_largest_min)) {
        if (reason != NULL) {
            *reason = "dma waterline";
        }
        return true;
    }

    return false;
}

static bool camera_pipeline_transport_guard_is_network_backpressure(const char *reason)
{
    return reason != NULL && strcmp(reason, "dma waterline") == 0;
}

static bool camera_pipeline_transport_guard_needs_key_frame(const char *reason)
{
    return reason != NULL && strcmp(reason, "delta payload burst") == 0;
}

static void camera_pipeline_log_transport_guard(const char *reason,
                                                size_t payload_len,
                                                bool key_frame,
                                                const media_governor_camera_policy_t *policy,
                                                size_t effective_max_delta,
                                                uint32_t stream_age_ms)
{
    TickType_t now_tick = xTaskGetTickCount();
    if (s_last_transport_guard_log_tick != 0 &&
        now_tick - s_last_transport_guard_log_tick <
            pdMS_TO_TICKS(CAMERA_PIPELINE_TRANSPORT_GUARD_LOG_INTERVAL_MS)) {
        return;
    }
    s_last_transport_guard_log_tick = now_tick;

    ESP_LOGW(TAG,
             "camera video transport guard: reason=%s payload=%u key=%d max_delta=%u policy_delta=%u stream_age_ms=%u target=%ux%u@%u dma_free=%u dma_largest=%u",
             reason != NULL ? reason : "unknown",
             (unsigned)payload_len,
             key_frame ? 1 : 0,
             (unsigned)effective_max_delta,
             (unsigned)(policy != NULL ? policy->h264_max_delta_payload_bytes : 0U),
             (unsigned)stream_age_ms,
             (unsigned)(policy != NULL ? policy->rtc_width : 0U),
             (unsigned)(policy != NULL ? policy->rtc_height : 0U),
             (unsigned)(policy != NULL ? policy->rtc_video_fps : 0U),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
}

static uint16_t camera_pipeline_even_dimension(uint16_t value)
{
    return (uint16_t)(value & (uint16_t)~1U);
}

static uint16_t camera_pipeline_align_down_u16(uint16_t value, uint16_t align)
{
    if (align == 0U) {
        return value;
    }
    return (uint16_t)(value - (value % align));
}

static size_t camera_pipeline_h264_input_size(uint16_t width, uint16_t height)
{
    return ((size_t)width * height * 3U) / 2U;
}

static size_t camera_pipeline_h264_ref_internal_estimate(uint16_t width)
{
    uint16_t mb_width = (uint16_t)((width + 15U) / 16U);
    return ((size_t)3U * 16U * (16U + 8U) * mb_width) + 7U;
}

static size_t camera_pipeline_align_up_size(size_t value, size_t align)
{
    if (align == 0U) {
        return value;
    }
    return (value + align - 1U) & ~(align - 1U);
}

static uintptr_t camera_pipeline_align_down_ptr(uintptr_t value, size_t align)
{
    if (align == 0U) {
        return value;
    }
    return value & ~((uintptr_t)align - 1U);
}

static esp_err_t camera_pipeline_h264_error_to_esp(esp_h264_err_t err)
{
    switch (err) {
    case ESP_H264_ERR_OK:
        return ESP_OK;
    case ESP_H264_ERR_ARG:
        return ESP_ERR_INVALID_ARG;
    case ESP_H264_ERR_MEM:
        return ESP_ERR_NO_MEM;
    case ESP_H264_ERR_TIMEOUT:
        return ESP_ERR_TIMEOUT;
    case ESP_H264_ERR_OVERFLOW:
        return ESP_ERR_INVALID_SIZE;
    case ESP_H264_ERR_UNSUPPORTED:
        return ESP_ERR_NOT_SUPPORTED;
    case ESP_H264_ERR_FAIL:
    default:
        return ESP_FAIL;
    }
}

static esp_err_t camera_pipeline_h264_set_control(int fd, uint32_t id, int32_t value, const char *name);

static bool camera_pipeline_h264_is_open(const camera_pipeline_h264_encoder_t *enc)
{
    if (enc == NULL || enc->capture_buffer == NULL) {
        return false;
    }
    if (enc->direct_encoder) {
        return enc->direct_handle != NULL;
    }
    return enc->fd >= 0;
}

static bool camera_pipeline_take_key_frame_request(void)
{
    bool requested = false;

    taskENTER_CRITICAL(&s_lock);
    if (s_key_frame_request_pending) {
        s_key_frame_request_pending = false;
        requested = true;
    }
    taskEXIT_CRITICAL(&s_lock);

    return requested;
}

static void camera_pipeline_note_key_frame_emitted(uint64_t now_us)
{
    taskENTER_CRITICAL(&s_lock);
    s_last_key_frame_us = now_us;
    s_last_key_frame_request_us = 0U;
    s_key_frame_request_pending = false;
    taskEXIT_CRITICAL(&s_lock);
}

static bool camera_pipeline_h264_force_next_idr(camera_pipeline_h264_encoder_t *enc,
                                                const char *reason)
{
    if (enc == NULL || !camera_pipeline_h264_is_open(enc)) {
        return false;
    }

    uint8_t base_gop = enc->gop != 0U ? enc->gop : enc->fps;
    if (base_gop == 0U) {
        return false;
    }

    uint8_t old_gop = enc->direct_encoder ?
                      (enc->direct_active_gop != 0U ? enc->direct_active_gop : base_gop) :
                      (enc->v4l2_active_gop != 0U ? enc->v4l2_active_gop : base_gop);
    uint8_t alternate_gop = base_gop > 1U ? (uint8_t)(base_gop - 1U) : 2U;
    uint8_t next_gop = old_gop == base_gop ? alternate_gop : base_gop;

    if (enc->direct_encoder) {
        if (enc->direct_handle == NULL || enc->direct_param == NULL) {
            return false;
        }
        esp_h264_err_t ret = esp_h264_enc_set_gop(&enc->direct_param->base, next_gop);
        if (ret != ESP_H264_ERR_OK) {
            ESP_LOGW(TAG,
                     "direct H264 key-frame request failed: reason=%s err=%d gop=%u",
                     reason != NULL ? reason : "unknown",
                     ret,
                     (unsigned)next_gop);
            return false;
        }
        enc->direct_active_gop = next_gop;
    } else {
        if (camera_pipeline_h264_set_control(enc->fd,
                                             V4L2_CID_MPEG_VIDEO_H264_I_PERIOD,
                                             next_gop,
                                             "gop-key-request") != ESP_OK) {
            return false;
        }
        enc->v4l2_active_gop = next_gop;
    }

    if (reason != NULL && strcmp(reason, "stream-start") == 0) {
        ESP_LOGI(TAG,
                 "H264 key-frame requested: reason=%s mode=%s gop_switch=%u->%u",
                 reason,
                 enc->direct_encoder ? "direct_hw" : "v4l2_m2m",
                 (unsigned)old_gop,
                 (unsigned)next_gop);
    } else {
        ESP_LOGD(TAG,
                 "H264 key-frame requested: reason=%s mode=%s gop_switch=%u->%u",
                 reason != NULL ? reason : "unknown",
                 enc->direct_encoder ? "direct_hw" : "v4l2_m2m",
                 (unsigned)old_gop,
                 (unsigned)next_gop);
    }
    return true;
}

static bool camera_pipeline_select_h264_internal_fit(uint16_t requested_width,
                                                     uint16_t requested_height,
                                                     uint16_t *width,
                                                     uint16_t *height)
{
    if (width == NULL || height == NULL ||
        requested_width < CAMERA_PIPELINE_H264_MIN_WIDTH ||
        requested_height < CAMERA_PIPELINE_H264_MIN_HEIGHT) {
        return false;
    }

    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t budget = largest_internal > CAMERA_PIPELINE_H264_INTERNAL_MARGIN ?
                    largest_internal - CAMERA_PIPELINE_H264_INTERNAL_MARGIN :
                    largest_internal;
    uint16_t candidate_width = camera_pipeline_align_down_u16(requested_width,
                                                             CAMERA_PIPELINE_H264_DIM_ALIGN);

    while (candidate_width >= CAMERA_PIPELINE_H264_MIN_WIDTH &&
           camera_pipeline_h264_ref_internal_estimate(candidate_width) > budget) {
        candidate_width = (uint16_t)(candidate_width - CAMERA_PIPELINE_H264_DIM_ALIGN);
    }

    if (candidate_width < CAMERA_PIPELINE_H264_MIN_WIDTH) {
        return false;
    }

    uint32_t candidate_height = ((uint32_t)candidate_width * requested_height) / requested_width;
    candidate_height = camera_pipeline_align_down_u16((uint16_t)candidate_height,
                                                      CAMERA_PIPELINE_H264_DIM_ALIGN);
    if (candidate_height < CAMERA_PIPELINE_H264_MIN_HEIGHT) {
        return false;
    }

    *width = candidate_width;
    *height = (uint16_t)candidate_height;
    return candidate_width != requested_width || candidate_height != requested_height;
}

static esp_err_t camera_pipeline_sync_h264_output_for_cpu(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    uintptr_t sync_start = camera_pipeline_align_down_ptr((uintptr_t)data,
                                                          CAMERA_PIPELINE_CACHE_LINE_SIZE);
    uintptr_t sync_end = (uintptr_t)data + len;
    size_t sync_len = camera_pipeline_align_up_size((size_t)(sync_end - sync_start),
                                                    CAMERA_PIPELINE_CACHE_LINE_SIZE);
    const void *sync_ptr = (const void *)sync_start;
    const void *sync_last = (const void *)(sync_start + sync_len - 1U);
    bool internal_range = esp_ptr_internal(sync_ptr) && esp_ptr_internal(sync_last);
    bool external_range = esp_ptr_external_ram(sync_ptr) && esp_ptr_external_ram(sync_last);
    if (!internal_range && !external_range) {
        if (!s_h264_output_sync_noncacheable_logged) {
            s_h264_output_sync_noncacheable_logged = true;
            ESP_LOGD(TAG, "H264 output cache sync skipped for non-cacheable memory");
        }
        return ESP_OK;
    }

    esp_err_t ret = esp_cache_msync((void *)sync_start,
                                    sync_len,
                                    ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                                        ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (ret == ESP_ERR_INVALID_ARG) {
        if (!s_h264_output_sync_noncacheable_logged) {
            s_h264_output_sync_noncacheable_logged = true;
            ESP_LOGD(TAG, "H264 output cache sync skipped for non-cacheable memory");
        }
        return ESP_OK;
    }
    return ret;
}

static esp_err_t camera_pipeline_sync_h264_input_for_device(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    uintptr_t sync_start = camera_pipeline_align_down_ptr((uintptr_t)data,
                                                          CAMERA_PIPELINE_CACHE_LINE_SIZE);
    uintptr_t sync_end = (uintptr_t)data + len;
    size_t sync_len = camera_pipeline_align_up_size((size_t)(sync_end - sync_start),
                                                    CAMERA_PIPELINE_CACHE_LINE_SIZE);
    const void *sync_ptr = (const void *)sync_start;
    const void *sync_last = (const void *)(sync_start + sync_len - 1U);
    bool internal_range = esp_ptr_internal(sync_ptr) && esp_ptr_internal(sync_last);
    bool external_range = esp_ptr_external_ram(sync_ptr) && esp_ptr_external_ram(sync_last);
    if (!internal_range && !external_range) {
        if (!s_h264_input_sync_noncacheable_logged) {
            s_h264_input_sync_noncacheable_logged = true;
            ESP_LOGD(TAG, "H264 input cache sync skipped for non-cacheable memory");
        }
        return ESP_OK;
    }

    esp_err_t ret = esp_cache_msync((void *)sync_start,
                                    sync_len,
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                        ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (ret == ESP_ERR_INVALID_ARG) {
        if (!s_h264_input_sync_noncacheable_logged) {
            s_h264_input_sync_noncacheable_logged = true;
            ESP_LOGD(TAG, "H264 input cache sync skipped for non-cacheable memory");
        }
        return ESP_OK;
    }
    return ret;
}

static esp_err_t camera_pipeline_h264_set_control(int fd, uint32_t id, int32_t value, const char *name)
{
    struct v4l2_ext_control control = {
        .id = id,
        .value = value,
    };
    struct v4l2_ext_controls controls = {
        .ctrl_class = V4L2_CID_CODEC_CLASS,
        .count = 1,
        .controls = &control,
    };

    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(TAG, "set H264 control failed: %s=%ld errno=%d", name, (long)value, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool camera_pipeline_h264_get_control(int fd, uint32_t id, int32_t *value, const char *name)
{
    struct v4l2_ext_control control = {
        .id = id,
    };
    struct v4l2_ext_controls controls = {
        .ctrl_class = V4L2_CID_CODEC_CLASS,
        .count = 1,
        .controls = &control,
    };

    if (value == NULL) {
        return false;
    }
    if (ioctl(fd, VIDIOC_G_EXT_CTRLS, &controls) != 0) {
        ESP_LOGD(TAG, "get H264 control failed: %s errno=%d", name != NULL ? name : "unknown", errno);
        return false;
    }
    *value = control.value;
    return true;
}

static void camera_pipeline_h264_close(camera_pipeline_h264_encoder_t *enc)
{
    if (enc == NULL) {
        return;
    }
    if (!camera_pipeline_h264_is_open(enc) &&
        enc->fd < 0 &&
        enc->direct_handle == NULL &&
        enc->capture_buffer == NULL) {
        return;
    }

    if (enc->direct_encoder || enc->direct_handle != NULL) {
        if (enc->direct_handle != NULL) {
            esp_h264_err_t close_ret = esp_h264_enc_close(enc->direct_handle);
            if (close_ret != ESP_H264_ERR_OK) {
                ESP_LOGW(TAG, "direct H264 encoder close failed err=%d", close_ret);
            }
            esp_h264_err_t del_ret = esp_h264_enc_del(enc->direct_handle);
            if (del_ret != ESP_H264_ERR_OK) {
                ESP_LOGW(TAG, "direct H264 encoder delete failed err=%d", del_ret);
            }
            enc->direct_handle = NULL;
        }
        if (enc->capture_buffer != NULL) {
            heap_caps_free(enc->capture_buffer);
            enc->capture_buffer = NULL;
        }
        *enc = (camera_pipeline_h264_encoder_t) {
            .fd = -1,
        };
        return;
    }

    if (enc->output_streaming) {
        int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        if (ioctl(enc->fd, VIDIOC_STREAMOFF, &type) != 0) {
            ESP_LOGW(TAG, "H264 output stream off failed errno=%d", errno);
        }
        enc->output_streaming = false;
    }
    if (enc->capture_streaming) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(enc->fd, VIDIOC_STREAMOFF, &type) != 0) {
            ESP_LOGW(TAG, "H264 capture stream off failed errno=%d", errno);
        }
        enc->capture_streaming = false;
    }
    if (enc->capture_buffer != NULL) {
        (void)munmap(enc->capture_buffer, enc->capture_buffer_size);
        enc->capture_buffer = NULL;
        enc->capture_buffer_size = 0;
    }
    close(enc->fd);
    enc->fd = -1;
    enc->width = 0;
    enc->height = 0;
}

static esp_err_t camera_pipeline_h264_open(camera_pipeline_h264_encoder_t *enc,
                                           uint16_t width,
                                           uint16_t height,
                                           uint8_t fps,
                                           uint32_t bitrate_bps,
                                           size_t output_buffer_bytes)
{
    ESP_RETURN_ON_FALSE(enc != NULL, ESP_ERR_INVALID_ARG, TAG, "h264 encoder is null");

    memset(enc, 0, sizeof(*enc));
    enc->fd = -1;

    uint8_t safe_fps = fps == 0U ? 12U : fps;
    uint32_t safe_bitrate_bps = bitrate_bps == 0U ? CAMERA_PIPELINE_H264_BITRATE : bitrate_bps;
    uint8_t safe_gop = CONFIG_APP_RTC_H264_GOP > 0 ? CONFIG_APP_RTC_H264_GOP : safe_fps;
    size_t safe_output_buffer_bytes =
        output_buffer_bytes == 0U ? CONFIG_APP_RTC_H264_OUTPUT_BUFFER_BYTES : output_buffer_bytes;

    ESP_LOGI(TAG,
             "H264 encoder open request: mode=%s size=%ux%u fps=%u gop=%u bitrate=%u ref_internal_est=%u internal_free=%u internal_largest=%u dma_largest=%u psram_free=%u psram_largest=%u",
             CONFIG_APP_RTC_H264_DIRECT_HW_ENCODER ? "direct_hw" : "v4l2_m2m",
             width,
             height,
             safe_fps,
             safe_gop,
             (unsigned)safe_bitrate_bps,
             (unsigned)camera_pipeline_h264_ref_internal_estimate(width),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    if (CONFIG_APP_RTC_H264_DIRECT_HW_ENCODER) {
        enc->direct_encoder = true;
        enc->capture_buffer =
            heap_caps_aligned_alloc(CAMERA_PIPELINE_CACHE_LINE_SIZE,
                                    safe_output_buffer_bytes,
                                    MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        ESP_RETURN_ON_FALSE(enc->capture_buffer != NULL,
                            ESP_ERR_NO_MEM,
                            TAG,
                            "alloc direct H264 output buffer failed size=%u psram_largest=%u",
                            (unsigned)safe_output_buffer_bytes,
                            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        enc->capture_buffer_size = safe_output_buffer_bytes;

        esp_h264_enc_cfg_hw_t config = {
            .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
            .gop = safe_gop,
            .fps = safe_fps,
            .res = {
                .width = width,
                .height = height,
            },
            .rc = {
                .bitrate = safe_bitrate_bps,
                .qp_min = CAMERA_PIPELINE_H264_MIN_QP,
                .qp_max = CAMERA_PIPELINE_H264_MAX_QP,
            },
        };

        esp_h264_err_t h264_ret = esp_h264_enc_hw_new(&config, &enc->direct_handle);
        if (h264_ret != ESP_H264_ERR_OK) {
            ESP_LOGE(TAG, "create direct H264 encoder failed err=%d", h264_ret);
            camera_pipeline_h264_close(enc);
            return camera_pipeline_h264_error_to_esp(h264_ret);
        }
        h264_ret = esp_h264_enc_hw_get_param_hd(enc->direct_handle, &enc->direct_param);
        if (h264_ret != ESP_H264_ERR_OK) {
            ESP_LOGW(TAG, "get direct H264 parameter handle failed err=%d; key-frame requests disabled", h264_ret);
            enc->direct_param = NULL;
        }
        h264_ret = esp_h264_enc_open(enc->direct_handle);
        if (h264_ret != ESP_H264_ERR_OK) {
            ESP_LOGE(TAG, "open direct H264 encoder failed err=%d", h264_ret);
            camera_pipeline_h264_close(enc);
            return camera_pipeline_h264_error_to_esp(h264_ret);
        }

        enc->fd = -1;
        enc->direct_encoder = true;
        enc->width = width;
        enc->height = height;
        enc->fps = safe_fps;
        enc->gop = safe_gop;
        enc->direct_active_gop = safe_gop;
        enc->bitrate_bps = safe_bitrate_bps;
        enc->output_buffer_bytes = safe_output_buffer_bytes;

        ESP_LOGI(TAG,
                 "H264 encoder ready: input=O_UYY_E_VYY output=H264 mode=direct_hw size=%ux%u fps=%u gop=%u bitrate=%u qp=%u-%u out_buf=%u",
                 width,
                 height,
                 safe_fps,
                 safe_gop,
                 safe_bitrate_bps,
                 CAMERA_PIPELINE_H264_MIN_QP,
                 CAMERA_PIPELINE_H264_MAX_QP,
                 (unsigned)enc->capture_buffer_size);
        return ESP_OK;
    }

    int fd = open(ESP_VIDEO_H264_DEVICE_NAME, O_RDONLY);
    ESP_RETURN_ON_FALSE(fd >= 0, ESP_FAIL, TAG, "open %s failed errno=%d", ESP_VIDEO_H264_DEVICE_NAME, errno);
    enc->fd = fd;

    struct v4l2_capability capability = {0};
    if (ioctl(fd, VIDIOC_QUERYCAP, &capability) == 0) {
        ESP_LOGI(TAG,
                 "H264 encoder device: driver=%s card=%s caps=0x%08" PRIx32,
                 capability.driver,
                 capability.card,
                 capability.capabilities);
    }

    (void)camera_pipeline_h264_set_control(fd,
                                           V4L2_CID_MPEG_VIDEO_H264_I_PERIOD,
                                           safe_gop,
                                           "gop");
    (void)camera_pipeline_h264_set_control(fd,
                                           V4L2_CID_MPEG_VIDEO_BITRATE,
                                           safe_bitrate_bps,
                                           "bitrate");
    (void)camera_pipeline_h264_set_control(fd,
                                           V4L2_CID_MPEG_VIDEO_H264_MIN_QP,
                                           CAMERA_PIPELINE_H264_MIN_QP,
                                           "min_qp");
    (void)camera_pipeline_h264_set_control(fd,
                                           V4L2_CID_MPEG_VIDEO_H264_MAX_QP,
                                           CAMERA_PIPELINE_H264_MAX_QP,
                                           "max_qp");

    int32_t actual_gop = -1;
    int32_t actual_bitrate = -1;
    int32_t actual_min_qp = -1;
    int32_t actual_max_qp = -1;
    bool have_gop = camera_pipeline_h264_get_control(fd,
                                                     V4L2_CID_MPEG_VIDEO_H264_I_PERIOD,
                                                     &actual_gop,
                                                     "gop");
    bool have_bitrate = camera_pipeline_h264_get_control(fd,
                                                         V4L2_CID_MPEG_VIDEO_BITRATE,
                                                         &actual_bitrate,
                                                         "bitrate");
    bool have_min_qp = camera_pipeline_h264_get_control(fd,
                                                        V4L2_CID_MPEG_VIDEO_H264_MIN_QP,
                                                        &actual_min_qp,
                                                        "min_qp");
    bool have_max_qp = camera_pipeline_h264_get_control(fd,
                                                        V4L2_CID_MPEG_VIDEO_H264_MAX_QP,
                                                        &actual_max_qp,
                                                        "max_qp");
    ESP_LOGI(TAG,
             "H264 encoder controls: requested bitrate=%u gop=%u qp=%u-%u actual bitrate=%ld gop=%ld qp=%ld-%ld valid=%d%d%d%d",
             (unsigned)safe_bitrate_bps,
             (unsigned)safe_gop,
             (unsigned)CAMERA_PIPELINE_H264_MIN_QP,
             (unsigned)CAMERA_PIPELINE_H264_MAX_QP,
             (long)actual_bitrate,
             (long)actual_gop,
             (long)actual_min_qp,
             (long)actual_max_qp,
             have_bitrate ? 1 : 0,
             have_gop ? 1 : 0,
             have_min_qp ? 1 : 0,
             have_max_qp ? 1 : 0);

    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
    };
    format.fmt.pix.width = width;
    format.fmt.pix.height = height;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    if (ioctl(fd, VIDIOC_S_FMT, &format) != 0) {
        ESP_LOGE(TAG, "set H264 input format failed errno=%d", errno);
        camera_pipeline_h264_close(enc);
        return ESP_FAIL;
    }

    struct v4l2_requestbuffers req = {
        .count = 1,
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_USERPTR,
    };
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "request H264 input buffers failed errno=%d", errno);
        camera_pipeline_h264_close(enc);
        return ESP_FAIL;
    }

    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = width;
    format.fmt.pix.height = height;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
    format.fmt.pix.sizeimage = (uint32_t)safe_output_buffer_bytes;
    if (ioctl(fd, VIDIOC_S_FMT, &format) != 0) {
        ESP_LOGE(TAG, "set H264 output format failed errno=%d", errno);
        camera_pipeline_h264_close(enc);
        return ESP_FAIL;
    }

    memset(&req, 0, sizeof(req));
    req.count = CAMERA_PIPELINE_H264_BUFFER_CNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "request H264 output buffers failed errno=%d", errno);
        camera_pipeline_h264_close(enc);
        return ESP_FAIL;
    }

    struct v4l2_buffer buf = {
        .index = 0,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) {
        ESP_LOGE(TAG, "query H264 output buffer failed errno=%d", errno);
        camera_pipeline_h264_close(enc);
        return ESP_FAIL;
    }

    enc->capture_buffer = (uint8_t *)mmap(NULL,
                                          buf.length,
                                          PROT_READ | PROT_WRITE,
                                          MAP_SHARED,
                                          fd,
                                          buf.m.offset);
    if (enc->capture_buffer == NULL || enc->capture_buffer == MAP_FAILED) {
        enc->capture_buffer = NULL;
        ESP_LOGE(TAG, "map H264 output buffer failed length=%u", (unsigned)buf.length);
        camera_pipeline_h264_close(enc);
        return ESP_FAIL;
    }
    enc->capture_buffer_size = buf.length;

    if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
        ESP_LOGE(TAG, "queue H264 output buffer failed errno=%d", errno);
        camera_pipeline_h264_close(enc);
        return ESP_FAIL;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "H264 capture stream on failed errno=%d", errno);
        camera_pipeline_h264_close(enc);
        return ESP_FAIL;
    }
    enc->capture_streaming = true;

    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "H264 output stream on failed errno=%d", errno);
        camera_pipeline_h264_close(enc);
        return ESP_FAIL;
    }
    enc->output_streaming = true;
    enc->width = width;
    enc->height = height;
    enc->fps = safe_fps;
    enc->gop = safe_gop;
    enc->v4l2_active_gop = safe_gop;
    enc->bitrate_bps = safe_bitrate_bps;
    enc->output_buffer_bytes = safe_output_buffer_bytes;

    ESP_LOGI(TAG,
             "H264 encoder ready: input=YUV420 output=H264 mode=v4l2_m2m size=%ux%u fps=%u gop=%u bitrate=%u qp=%u-%u out_buf=%u",
             width,
             height,
             safe_fps,
             safe_gop,
             safe_bitrate_bps,
             CAMERA_PIPELINE_H264_MIN_QP,
             CAMERA_PIPELINE_H264_MAX_QP,
             (unsigned)enc->capture_buffer_size);
    return ESP_OK;
}

static esp_err_t camera_pipeline_h264_open_with_dma_escrow(camera_pipeline_h264_encoder_t *enc,
                                                           uint16_t width,
                                                           uint16_t height,
                                                           uint8_t fps,
                                                           uint32_t bitrate_bps,
                                                           size_t output_buffer_bytes,
                                                           const char *stage)
{
    esp_err_t ret = camera_pipeline_h264_open(enc,
                                              width,
                                              height,
                                              fps,
                                              bitrate_bps,
                                              output_buffer_bytes);
    if (ret == ESP_OK || !media_dma_reserve_is_reserved()) {
        return ret;
    }

    ESP_LOGW(TAG,
             "H264 encoder open failed before DMA escrow lend: stage=%s ret=%s size=%ux%u fps=%u bitrate=%u",
             stage != NULL ? stage : "unknown",
             esp_err_to_name(ret),
             width,
             height,
             fps,
             (unsigned)bitrate_bps);

    media_dma_reserve_release("h264-open-retry");
    ret = camera_pipeline_h264_open(enc,
                                    width,
                                    height,
                                    fps,
                                    bitrate_bps,
                                    output_buffer_bytes);

    esp_err_t reclaim_ret =
        media_dma_reserve_reclaim(ret == ESP_OK ? "h264-open-retry-success" : "h264-open-retry-failed");
    if (reclaim_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "DMA escrow reclaim after H264 retry failed: open=%s reclaim=%s",
                 esp_err_to_name(ret),
                 esp_err_to_name(reclaim_ret));
    }

    return ret;
}

static bool camera_pipeline_h264_matches(const camera_pipeline_h264_encoder_t *enc,
                                         uint16_t width,
                                         uint16_t height,
                                         uint8_t fps,
                                         uint32_t bitrate_bps,
                                         size_t output_buffer_bytes)
{
    uint8_t safe_fps = fps == 0U ? 12U : fps;
    uint32_t safe_bitrate_bps = bitrate_bps == 0U ? CAMERA_PIPELINE_H264_BITRATE : bitrate_bps;
    uint8_t safe_gop = CONFIG_APP_RTC_H264_GOP > 0 ? CONFIG_APP_RTC_H264_GOP : safe_fps;
    size_t safe_output_buffer_bytes =
        output_buffer_bytes == 0U ? CONFIG_APP_RTC_H264_OUTPUT_BUFFER_BYTES : output_buffer_bytes;

    return enc != NULL &&
           camera_pipeline_h264_is_open(enc) &&
           enc->width == width &&
           enc->height == height &&
           enc->fps == safe_fps &&
           enc->gop == safe_gop &&
           enc->bitrate_bps == safe_bitrate_bps &&
           enc->output_buffer_bytes == safe_output_buffer_bytes;
}

static bool camera_pipeline_h264_take_reserved(camera_pipeline_h264_encoder_t *enc,
                                               uint16_t width,
                                               uint16_t height,
                                               uint8_t fps,
                                               uint32_t bitrate_bps,
                                               size_t output_buffer_bytes)
{
    bool taken = false;

    if (enc == NULL) {
        return false;
    }

    taskENTER_CRITICAL(&s_lock);
    if (camera_pipeline_h264_matches(&s_reserved_h264,
                                     width,
                                     height,
                                     fps,
                                     bitrate_bps,
                                     output_buffer_bytes)) {
        *enc = s_reserved_h264;
        s_reserved_h264 = (camera_pipeline_h264_encoder_t) {
            .fd = -1,
        };
        taken = true;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (taken) {
        ESP_LOGI(TAG,
                 "H264 encoder reserved resource adopted: mode=%s size=%ux%u fps=%u gop=%u bitrate=%u out_buf=%u internal_largest=%u dma_largest=%u",
                 enc->direct_encoder ? "direct_hw" : "v4l2_m2m",
                 width,
                 height,
                 fps,
                 (unsigned)enc->gop,
                 (unsigned)bitrate_bps,
                 (unsigned)output_buffer_bytes,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    }
    return taken;
}

static bool camera_pipeline_h264_store_reserved(camera_pipeline_h264_encoder_t *enc,
                                                const char *reason)
{
    bool stored = false;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t fps = 0;
    uint32_t bitrate_bps = 0;
    size_t output_buffer_bytes = 0;

    if (enc == NULL || !camera_pipeline_h264_is_open(enc)) {
        return false;
    }

    width = enc->width;
    height = enc->height;
    fps = enc->fps;
    bitrate_bps = enc->bitrate_bps;
    output_buffer_bytes = enc->output_buffer_bytes;

    taskENTER_CRITICAL(&s_lock);
    if (!camera_pipeline_h264_is_open(&s_reserved_h264)) {
        s_reserved_h264 = *enc;
        *enc = (camera_pipeline_h264_encoder_t) {
            .fd = -1,
        };
        stored = true;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (stored) {
        ESP_LOGI(TAG,
                 "H264 encoder reserved resource stored: reason=%s mode=%s size=%ux%u fps=%u gop=%u bitrate=%u out_buf=%u internal_largest=%u dma_largest=%u",
                 reason != NULL ? reason : "unknown",
                 s_reserved_h264.direct_encoder ? "direct_hw" : "v4l2_m2m",
                 width,
                 height,
                 fps,
                 (unsigned)s_reserved_h264.gop,
                 (unsigned)bitrate_bps,
                 (unsigned)output_buffer_bytes,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    }
    return stored;
}

static esp_err_t camera_pipeline_h264_encode(camera_pipeline_h264_encoder_t *enc,
                                             const uint8_t *input_data,
                                             size_t input_len,
                                             const uint8_t **data,
                                             size_t *data_len,
                                             bool *key_frame,
                                             camera_pipeline_h264_timing_t *timing)
{
    ESP_RETURN_ON_FALSE(enc != NULL && input_data != NULL && data != NULL && data_len != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid H264 encode args");
    ESP_RETURN_ON_FALSE(camera_pipeline_h264_is_open(enc),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "H264 encoder not open");

    if (enc->direct_encoder) {
        if (timing != NULL) {
            *timing = (camera_pipeline_h264_timing_t) {0};
        }

        if (camera_pipeline_take_key_frame_request()) {
            (void)camera_pipeline_h264_force_next_idr(enc, "peer-request");
        }

        int64_t sync_start_us = esp_timer_get_time();
        esp_err_t sync_ret = camera_pipeline_sync_h264_input_for_device(input_data, input_len);
        if (timing != NULL) {
            timing->sync_in_us = esp_timer_get_time() - sync_start_us;
        }
        if (sync_ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "sync direct H264 input cache failed: len=%u ret=%s",
                     (unsigned)input_len,
                     esp_err_to_name(sync_ret));
            return sync_ret;
        }

        esp_h264_enc_in_frame_t in_frame = {
            .raw_data = {
                .buffer = (uint8_t *)input_data,
                .len = (uint32_t)input_len,
            },
            .pts = (uint32_t)(esp_timer_get_time() / 1000LL),
        };
        esp_h264_enc_out_frame_t out_frame = {
            .raw_data = {
                .buffer = enc->capture_buffer,
                .len = (uint32_t)enc->capture_buffer_size,
            },
        };

        int64_t hw_start_us = esp_timer_get_time();
        esp_h264_err_t h264_ret = esp_h264_enc_process(enc->direct_handle, &in_frame, &out_frame);
        if (timing != NULL) {
            timing->hw_us = esp_timer_get_time() - hw_start_us;
        }
        if (h264_ret != ESP_H264_ERR_OK) {
            ESP_LOGW(TAG,
                     "direct H264 encode failed err=%d input=%u out_cap=%u",
                     h264_ret,
                     (unsigned)input_len,
                     (unsigned)enc->capture_buffer_size);
            return camera_pipeline_h264_error_to_esp(h264_ret);
        }

        if (out_frame.length > 0U) {
            sync_start_us = esp_timer_get_time();
            sync_ret = camera_pipeline_sync_h264_output_for_cpu(enc->capture_buffer, out_frame.length);
            if (timing != NULL) {
                timing->sync_out_us = esp_timer_get_time() - sync_start_us;
            }
            if (sync_ret != ESP_OK) {
                ESP_LOGW(TAG,
                         "sync direct H264 output cache failed: len=%u ret=%s",
                         (unsigned)out_frame.length,
                         esp_err_to_name(sync_ret));
            }
        }

        *data = enc->capture_buffer;
        *data_len = out_frame.length;
        if (key_frame != NULL) {
            *key_frame = out_frame.frame_type == ESP_H264_FRAME_TYPE_IDR ||
                         out_frame.frame_type == ESP_H264_FRAME_TYPE_I ||
                         camera_pipeline_h264_is_key_frame(enc->capture_buffer, out_frame.length);
        }
        return ESP_OK;
    }

    struct v4l2_buffer out_buf = {
        .index = 0,
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_USERPTR,
    };
    out_buf.m.userptr = (unsigned long)input_data;
    out_buf.length = (uint32_t)input_len;
    out_buf.bytesused = (uint32_t)input_len;

    if (timing != NULL) {
        *timing = (camera_pipeline_h264_timing_t) {0};
    }

    if (camera_pipeline_take_key_frame_request()) {
        (void)camera_pipeline_h264_force_next_idr(enc, "peer-request");
    }

    int64_t sync_start_us = esp_timer_get_time();
    esp_err_t sync_ret = camera_pipeline_sync_h264_input_for_device(input_data, input_len);
    if (timing != NULL) {
        timing->sync_in_us = esp_timer_get_time() - sync_start_us;
    }
    if (sync_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "sync H264 input cache failed: len=%u ret=%s",
                 (unsigned)input_len,
                 esp_err_to_name(sync_ret));
        return sync_ret;
    }

    int64_t hw_start_us = esp_timer_get_time();
    if (ioctl(enc->fd, VIDIOC_QBUF, &out_buf) != 0) {
        ESP_LOGW(TAG, "queue H264 input frame failed errno=%d", errno);
        return ESP_FAIL;
    }

    struct v4l2_buffer cap_buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(enc->fd, VIDIOC_DQBUF, &cap_buf) != 0) {
        ESP_LOGW(TAG, "dequeue H264 output frame failed errno=%d", errno);
        (void)ioctl(enc->fd, VIDIOC_DQBUF, &out_buf);
        return ESP_FAIL;
    }
    if (timing != NULL) {
        timing->hw_us = esp_timer_get_time() - hw_start_us;
    }
    if (cap_buf.bytesused > 0U) {
        sync_start_us = esp_timer_get_time();
        esp_err_t sync_ret =
            camera_pipeline_sync_h264_output_for_cpu(enc->capture_buffer, cap_buf.bytesused);
        if (timing != NULL) {
            timing->sync_out_us = esp_timer_get_time() - sync_start_us;
        }
        if (sync_ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "sync H264 output cache failed: len=%u ret=%s",
                     (unsigned)cap_buf.bytesused,
                     esp_err_to_name(sync_ret));
        }
    }

    struct v4l2_buffer done_out = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_USERPTR,
    };
    if (ioctl(enc->fd, VIDIOC_DQBUF, &done_out) != 0) {
        ESP_LOGW(TAG, "dequeue H264 input frame failed errno=%d", errno);
    }

    *data = enc->capture_buffer;
    *data_len = cap_buf.bytesused;
    if (key_frame != NULL) {
        *key_frame = (cap_buf.flags & V4L2_BUF_FLAG_KEYFRAME) != 0 ||
                     camera_pipeline_h264_is_key_frame(enc->capture_buffer, cap_buf.bytesused);
    }
    return ESP_OK;
}

static void camera_pipeline_h264_return_output(camera_pipeline_h264_encoder_t *enc)
{
    if (enc == NULL || enc->fd < 0 || enc->capture_buffer == NULL) {
        if (enc != NULL && enc->direct_encoder) {
            return;
        }
        return;
    }

    struct v4l2_buffer cap_buf = {
        .index = 0,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(enc->fd, VIDIOC_QBUF, &cap_buf) != 0) {
        ESP_LOGW(TAG, "requeue H264 output buffer failed errno=%d", errno);
    }
}

static void camera_pipeline_task(void *arg)
{
    (void)arg;

    camera_pipeline_mark_task_started();

    camera_pipeline_h264_encoder_t h264 = {
        .fd = -1,
    };
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t last_rtc_tick = 0;
    TickType_t last_stats_tick = start_tick;
    uint32_t upstream_count = 0;
    uint32_t drop_count = 0;
    uint32_t capture_fail_count = 0;
    uint32_t encode_fail_count = 0;
    uint32_t total_payload_bytes = 0;
    uint32_t backpressure_skip_count = 0;
    uint32_t transport_guard_drop_count = 0;
    uint32_t key_wait_drop_count = 0;
    uint32_t encoded_frame_count = 0;
    uint32_t key_frame_count = 0;
    uint32_t slow_capture_count = 0;
    uint32_t slow_encode_count = 0;
    uint32_t slow_callback_count = 0;
    uint32_t slow_loop_count = 0;
    uint32_t large_frame_count = 0;
    uint32_t min_payload_bytes = UINT32_MAX;
    uint32_t max_payload_bytes = 0;
    uint64_t frame_gap_us_total = 0;
    uint64_t max_frame_gap_us = 0;
    uint64_t last_frame_start_us = 0;
    uint64_t stream_start_us = 0;
    uint64_t capture_us_total = 0;
    uint64_t encode_us_total = 0;
    uint64_t h264_sync_in_us_total = 0;
    uint64_t h264_hw_us_total = 0;
    uint64_t h264_sync_out_us_total = 0;
    uint64_t callback_us_total = 0;
    uint64_t loop_us_total = 0;
    uint64_t media_timestamp_lag_us_total = 0;
    uint64_t max_media_timestamp_lag_us = 0;
    uint64_t camera_sequence_delta_total_x10 = 0;
    uint32_t capture_sample_count = 0;
    uint32_t encode_sample_count = 0;
    uint32_t callback_sample_count = 0;
    uint32_t loop_sample_count = 0;
    uint32_t media_timestamp_sample_count = 0;
    uint32_t camera_sequence_sample_count = 0;
    uint32_t camera_stale_frame_drain_count = 0;
    uint32_t max_camera_sequence_delta = 0;
    uint32_t last_camera_sequence = 0;
    uint32_t trace_frame_count = 0;
    bool first_frame_logged = false;
    bool last_camera_sequence_valid = false;
    bool stream_start_key_frame_requested = false;
    bool video_subsystem_prepared = false;
    bool camera_acquired = false;
    TickType_t next_h264_open_tick = 0;
    TickType_t last_source_mismatch_log_tick = 0;
    uint16_t h264_fallback_width = 0;
    uint16_t h264_fallback_height = 0;
    bool key_frame_required_after_drop = false;
    bool backpressure_key_request_pending = false;

    ESP_LOGI(TAG, "camera pipeline starting: mode=h264_upstream_only");
    camera_pipeline_reset_metrics();
    vTaskDelay(pdMS_TO_TICKS(CAMERA_PIPELINE_START_DELAY_MS));

    esp_err_t ret = camera_driver_prepare_video_subsystem();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "camera video subsystem prepare failed: %s", esp_err_to_name(ret));
        goto exit_task;
    }
    video_subsystem_prepared = true;

    media_governor_camera_policy_t preopen_policy = {0};
    camera_pipeline_get_policy(&preopen_policy);
    uint16_t preopen_width = camera_pipeline_even_dimension(preopen_policy.rtc_width);
    uint16_t preopen_height = camera_pipeline_even_dimension(preopen_policy.rtc_height);
    if (preopen_width > 0U && preopen_height > 0U && preopen_policy.rtc_video_fps > 0U) {
        esp_err_t camera_target_ret =
            camera_driver_set_stream_target(preopen_width, preopen_height, preopen_policy.capture_fps);
        if (camera_target_ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "camera stream target preopen failed: %ux%u@%u %s",
                     preopen_width,
                     preopen_height,
                     preopen_policy.capture_fps,
                     esp_err_to_name(camera_target_ret));
        }
        ESP_LOGI(TAG,
                 "H264 encoder preopen before camera buffers: size=%ux%u fps=%u bitrate=%u strict=%d",
                 preopen_width,
                 preopen_height,
                 preopen_policy.rtc_video_fps,
                 (unsigned)preopen_policy.h264_bitrate_bps,
                 CAMERA_PIPELINE_H264_STRICT_TARGET);
        if (camera_pipeline_h264_take_reserved(&h264,
                                               preopen_width,
                                               preopen_height,
                                               preopen_policy.rtc_video_fps,
                                               preopen_policy.h264_bitrate_bps,
                                               preopen_policy.h264_output_buffer_bytes)) {
            ret = ESP_OK;
        } else {
            ret = camera_pipeline_h264_open_with_dma_escrow(&h264,
                                                            preopen_width,
                                                            preopen_height,
                                                            preopen_policy.rtc_video_fps,
                                                            preopen_policy.h264_bitrate_bps,
                                                            preopen_policy.h264_output_buffer_bytes,
                                                            "pipeline-preopen");
        }
        if (ret != ESP_OK) {
            encode_fail_count++;
            if (!CAMERA_PIPELINE_H264_STRICT_TARGET &&
                camera_pipeline_select_h264_internal_fit(preopen_width,
                                                         preopen_height,
                                                         &h264_fallback_width,
                                                         &h264_fallback_height)) {
                ESP_LOGW(TAG,
                         "H264 encoder preopen failed at %ux%u: %s, fallback=%ux%u internal_largest=%u ref_est=%u",
                         preopen_width,
                         preopen_height,
                         esp_err_to_name(ret),
                         h264_fallback_width,
                         h264_fallback_height,
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                         (unsigned)camera_pipeline_h264_ref_internal_estimate(h264_fallback_width));
                ret = camera_pipeline_h264_open_with_dma_escrow(&h264,
                                                                h264_fallback_width,
                                                                h264_fallback_height,
                                                                preopen_policy.rtc_video_fps,
                                                                preopen_policy.h264_bitrate_bps,
                                                                preopen_policy.h264_output_buffer_bytes,
                                                                "pipeline-preopen-fallback");
            }
            if (ret != ESP_OK) {
                next_h264_open_tick = xTaskGetTickCount() + pdMS_TO_TICKS(CAMERA_PIPELINE_H264_OPEN_RETRY_MS);
                ESP_LOGW(TAG,
                         "H264 encoder preopen failed at strict target %ux%u: %s, retry after %ums",
                         preopen_width,
                         preopen_height,
                         esp_err_to_name(ret),
                         (unsigned)CAMERA_PIPELINE_H264_OPEN_RETRY_MS);
            }
        }
    }

    ret = camera_driver_acquire();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "camera pipeline acquire failed: %s", esp_err_to_name(ret));
        goto exit_task;
    }
    camera_acquired = true;

    while (camera_pipeline_should_run()) {
        camera_pipeline_runtime_t runtime = camera_pipeline_snapshot();
        media_governor_camera_policy_t policy = {0};
        camera_pipeline_get_policy(&policy);

        uint32_t rtc_interval_ms = camera_pipeline_interval_ms(policy.rtc_video_fps);
        TickType_t now_tick = xTaskGetTickCount();
        bool do_rtc = runtime.rtc_enabled &&
                      runtime.video_cb != NULL &&
                      policy.rtc_video_fps > 0U &&
                      camera_pipeline_time_due(now_tick, &last_rtc_tick, rtc_interval_ms);

        if (!do_rtc) {
            uint32_t wait_ms = 5U;
            if (runtime.rtc_enabled && runtime.video_cb != NULL && policy.rtc_video_fps > 0U) {
                wait_ms = camera_pipeline_wait_until_due_ms(now_tick, last_rtc_tick, rtc_interval_ms);
            }
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
            continue;
        }
        if (media_governor_is_network_backpressured()) {
            drop_count++;
            backpressure_skip_count++;
            key_frame_required_after_drop = true;
            backpressure_key_request_pending = true;
            continue;
        }
        uint16_t policy_width = camera_pipeline_even_dimension(policy.rtc_width);
        uint16_t policy_height = camera_pipeline_even_dimension(policy.rtc_height);
        if (policy_width > 0U && policy_height > 0U && policy.capture_fps > 0U) {
            esp_err_t camera_target_ret =
                camera_driver_set_stream_target(policy_width, policy_height, policy.capture_fps);
            if (camera_target_ret != ESP_OK && camera_target_ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG,
                         "camera stream target update failed: %ux%u@%u %s",
                         policy_width,
                         policy_height,
                         policy.capture_fps,
                         esp_err_to_name(camera_target_ret));
            }
        }

        camera_driver_frame_t frame = {0};
        int64_t loop_start_us = esp_timer_get_time();
        if (last_frame_start_us != 0U && loop_start_us > (int64_t)last_frame_start_us) {
            uint64_t gap_us = (uint64_t)loop_start_us - last_frame_start_us;
            frame_gap_us_total += gap_us;
            if (gap_us > max_frame_gap_us) {
                max_frame_gap_us = gap_us;
            }
        }
        last_frame_start_us = (uint64_t)loop_start_us;
        int64_t capture_start_us = loop_start_us;
        ret = camera_driver_capture(&frame);
        int64_t capture_us = esp_timer_get_time() - capture_start_us;
        if (ret != ESP_OK) {
            capture_fail_count++;
            ESP_LOGW(TAG, "camera capture failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(CAMERA_PIPELINE_RETRY_DELAY_MS));
            continue;
        }
        capture_us_total += (uint64_t)capture_us;
        capture_sample_count++;
        camera_stale_frame_drain_count += frame.stale_frames_dropped;
        if (last_camera_sequence_valid) {
            uint32_t sequence_delta = frame.sequence - last_camera_sequence;
            if (sequence_delta > 0U) {
                camera_sequence_delta_total_x10 += (uint64_t)sequence_delta * 10ULL;
                camera_sequence_sample_count++;
                if (sequence_delta > max_camera_sequence_delta) {
                    max_camera_sequence_delta = sequence_delta;
                }
            }
        }
        last_camera_sequence = frame.sequence;
        last_camera_sequence_valid = true;

        uint16_t source_width = frame.width;
        uint16_t source_height = frame.height;
        camera_driver_pixel_format_t source_format = frame.pixel_format;
        size_t source_data_len = frame.data_len;
        uint32_t source_sequence = frame.sequence;
        uint32_t source_stale_frames_dropped = frame.stale_frames_dropped;

        uint16_t target_width = camera_pipeline_even_dimension(policy.rtc_width);
        uint16_t target_height = camera_pipeline_even_dimension(policy.rtc_height);
        if (target_width == 0U || target_width > source_width) {
            target_width = camera_pipeline_even_dimension(source_width);
        }
        if (target_height == 0U || target_height > source_height) {
            target_height = camera_pipeline_even_dimension(source_height);
        }
        if (camera_pipeline_h264_is_open(&h264) &&
            h264.width <= target_width &&
            h264.height <= target_height) {
            target_width = h264.width;
            target_height = h264.height;
        } else if (h264_fallback_width > 0U &&
                   h264_fallback_height > 0U &&
                   h264_fallback_width <= target_width &&
                   h264_fallback_height <= target_height) {
            target_width = h264_fallback_width;
            target_height = h264_fallback_height;
        }
        if (source_format == CAMERA_DRIVER_PIXEL_FORMAT_YUV420 &&
            (source_width != target_width || source_height != target_height)) {
            TickType_t mismatch_tick = xTaskGetTickCount();
            if (last_source_mismatch_log_tick == 0 ||
                mismatch_tick - last_source_mismatch_log_tick >= pdMS_TO_TICKS(5000)) {
                last_source_mismatch_log_tick = mismatch_tick;
                ESP_LOGW(TAG,
                         "camera source size differs from RTC target, keep direct YUV420 source: source=%ux%u target=%ux%u",
                         source_width,
                         source_height,
                         target_width,
                         target_height);
            }
            target_width = camera_pipeline_even_dimension(source_width);
            target_height = camera_pipeline_even_dimension(source_height);
        }

        size_t h264_input_len = camera_pipeline_h264_input_size(target_width, target_height);
        const uint8_t *h264_input_data = NULL;
        bool frame_released = false;
        int64_t convert_us = 0;
        const char *h264_input_path = "unknown";
        bool h264_direct_input = false;

        if (source_format == CAMERA_DRIVER_PIXEL_FORMAT_YUV420 &&
            frame.data != NULL &&
            source_width == target_width &&
            source_height == target_height &&
            source_data_len >= h264_input_len) {
            h264_input_data = frame.data;
            h264_input_path = "yuv420-direct";
            h264_direct_input = true;
        } else if (source_format == CAMERA_DRIVER_PIXEL_FORMAT_RGB565) {
            camera_driver_release(&frame);
            frame_released = true;
            capture_fail_count++;
            ESP_LOGW(TAG,
                     "RTC video rejects RGB565 main path: source=%ux%u target=%ux%u bytes=%u",
                     source_width,
                     source_height,
                     target_width,
                     target_height,
                     (unsigned)source_data_len);
            vTaskDelay(pdMS_TO_TICKS(CAMERA_PIPELINE_RETRY_DELAY_MS));
            continue;
        } else if (source_format == CAMERA_DRIVER_PIXEL_FORMAT_YUV420 && frame.data != NULL) {
            camera_driver_release(&frame);
            frame_released = true;
            capture_fail_count++;
            ESP_LOGW(TAG,
                     "RTC video requires direct YUV420: source=%ux%u target=%ux%u bytes=%u expected=%u",
                     source_width,
                     source_height,
                     target_width,
                     target_height,
                     (unsigned)source_data_len,
                     (unsigned)h264_input_len);
            vTaskDelay(pdMS_TO_TICKS(CAMERA_PIPELINE_RETRY_DELAY_MS));
            continue;
        } else {
            camera_driver_release(&frame);
            frame_released = true;
            capture_fail_count++;
            ESP_LOGW(TAG,
                     "unsupported camera frame for H264: format=%d bytes=%u expected=%u size=%ux%u target=%ux%u",
                     source_format,
                     (unsigned)source_data_len,
                     (unsigned)h264_input_len,
                     source_width,
                     source_height,
                     target_width,
                     target_height);
            vTaskDelay(pdMS_TO_TICKS(CAMERA_PIPELINE_RETRY_DELAY_MS));
            continue;
        }

        camera_pipeline_note_frame_metrics(target_width,
                                           target_height,
                                           policy.rtc_video_fps,
                                           policy.h264_bitrate_bps,
                                           h264_direct_input);

        if (!camera_pipeline_h264_matches(&h264,
                                          target_width,
                                          target_height,
                                          policy.rtc_video_fps,
                                          policy.h264_bitrate_bps,
                                          policy.h264_output_buffer_bytes)) {
            camera_pipeline_h264_close(&h264);
            TickType_t open_tick = xTaskGetTickCount();
            if (next_h264_open_tick != 0 && !camera_pipeline_tick_reached(open_tick, next_h264_open_tick)) {
                encode_fail_count++;
                if (!frame_released) {
                    camera_driver_release(&frame);
                    frame_released = true;
                }
                uint32_t wait_ms = pdTICKS_TO_MS(next_h264_open_tick - open_tick);
                ESP_LOGD(TAG, "H264 encoder open backoff: wait=%ums", (unsigned)wait_ms);
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            ret = camera_pipeline_h264_open_with_dma_escrow(&h264,
                                                            target_width,
                                                            target_height,
                                                            policy.rtc_video_fps,
                                                            policy.h264_bitrate_bps,
                                                            policy.h264_output_buffer_bytes,
                                                            "pipeline-runtime");
            if (ret != ESP_OK) {
                encode_fail_count++;
                if (!CAMERA_PIPELINE_H264_STRICT_TARGET &&
                    camera_pipeline_select_h264_internal_fit(target_width,
                                                             target_height,
                                                             &h264_fallback_width,
                                                             &h264_fallback_height)) {
                    ESP_LOGW(TAG,
                             "H264 encoder fallback selected after open fail: requested=%ux%u fallback=%ux%u internal_largest=%u ref_est=%u",
                             target_width,
                             target_height,
                             h264_fallback_width,
                             h264_fallback_height,
                             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                             (unsigned)camera_pipeline_h264_ref_internal_estimate(h264_fallback_width));
                }
                next_h264_open_tick = xTaskGetTickCount() + pdMS_TO_TICKS(CAMERA_PIPELINE_H264_OPEN_RETRY_MS);
                if (!frame_released) {
                    camera_driver_release(&frame);
                    frame_released = true;
                }
                ESP_LOGW(TAG,
                         "H264 encoder open failed: %s, retry after %ums",
                         esp_err_to_name(ret),
                         (unsigned)CAMERA_PIPELINE_H264_OPEN_RETRY_MS);
                vTaskDelay(pdMS_TO_TICKS(CAMERA_PIPELINE_RETRY_DELAY_MS));
                continue;
            }
            next_h264_open_tick = 0;
        }

        const uint8_t *h264_data = NULL;
        size_t h264_len = 0;
        bool key_frame = false;
        camera_pipeline_h264_timing_t h264_timing = {0};
        if (!stream_start_key_frame_requested) {
            (void)camera_pipeline_h264_force_next_idr(&h264, "stream-start");
            (void)camera_pipeline_take_key_frame_request();
            stream_start_key_frame_requested = true;
            stream_start_us = (uint64_t)esp_timer_get_time();
        }
        if (backpressure_key_request_pending) {
            if (camera_pipeline_h264_force_next_idr(&h264, "backpressure-resume")) {
                (void)camera_pipeline_take_key_frame_request();
                backpressure_key_request_pending = false;
            }
        }
        int64_t encode_start_us = esp_timer_get_time();
        ret = camera_pipeline_h264_encode(&h264,
                                          h264_input_data,
                                          h264_input_len,
                                          &h264_data,
                                          &h264_len,
                                          &key_frame,
                                          &h264_timing);
        int64_t encode_us = esp_timer_get_time() - encode_start_us;
        if (!frame_released) {
            camera_driver_release(&frame);
            frame_released = true;
        }

        if (ret != ESP_OK || h264_data == NULL || h264_len == 0U) {
            encode_fail_count++;
            drop_count++;
            camera_pipeline_h264_return_output(&h264);
            vTaskDelay(pdMS_TO_TICKS(CAMERA_PIPELINE_RETRY_DELAY_MS));
            continue;
        }
        encode_us_total += (uint64_t)encode_us;
        h264_sync_in_us_total += (uint64_t)h264_timing.sync_in_us;
        h264_hw_us_total += (uint64_t)h264_timing.hw_us;
        h264_sync_out_us_total += (uint64_t)h264_timing.sync_out_us;
        encode_sample_count++;
        encoded_frame_count++;
        trace_frame_count++;
        if (key_frame) {
            key_frame_count++;
            key_frame_required_after_drop = false;
            camera_pipeline_note_key_frame_emitted((uint64_t)esp_timer_get_time());
        }
        if (h264_len > max_payload_bytes) {
            max_payload_bytes = (uint32_t)h264_len;
        }
        if (h264_len < min_payload_bytes) {
            min_payload_bytes = (uint32_t)h264_len;
        }
        if ((uint32_t)h264_len >= CAMERA_PIPELINE_FRAME_TRACE_LARGE_PAYLOAD_BYTES) {
            large_frame_count++;
        }
        if (capture_us > CAMERA_PIPELINE_FRAME_TRACE_SLOW_STAGE_US) {
            slow_capture_count++;
        }
        if (encode_us > CAMERA_PIPELINE_FRAME_TRACE_SLOW_STAGE_US) {
            slow_encode_count++;
        }

        camera_pipeline_note_frame_metrics(h264.width,
                                           h264.height,
                                           policy.rtc_video_fps,
                                           policy.h264_bitrate_bps,
                                           h264_direct_input);

        const char *transport_guard_reason = NULL;
        int64_t now_for_guard_us = esp_timer_get_time();
        uint32_t stream_age_ms = 0U;
        if (stream_start_us != 0U && now_for_guard_us > (int64_t)stream_start_us) {
            uint64_t elapsed_us = (uint64_t)now_for_guard_us - stream_start_us;
            stream_age_ms = (uint32_t)(elapsed_us / 1000ULL);
        }
        size_t effective_max_delta = policy.h264_max_delta_payload_bytes;
        if (camera_pipeline_should_hold_video_for_transport(h264_len,
                                                            key_frame,
                                                            &policy,
                                                            stream_age_ms,
                                                            &effective_max_delta,
                                                            &transport_guard_reason)) {
            drop_count++;
            transport_guard_drop_count++;
            if (camera_pipeline_transport_guard_is_network_backpressure(transport_guard_reason)) {
                media_governor_note_network_backpressure();
            }
            if (camera_pipeline_transport_guard_needs_key_frame(transport_guard_reason)) {
                key_frame_required_after_drop = true;
                (void)camera_pipeline_h264_force_next_idr(&h264, "transport-guard");
                (void)camera_pipeline_take_key_frame_request();
            }
            camera_pipeline_log_transport_guard(transport_guard_reason,
                                                h264_len,
                                                key_frame,
                                                &policy,
                                                effective_max_delta,
                                                stream_age_ms);
            camera_pipeline_h264_return_output(&h264);
            int64_t loop_us = esp_timer_get_time() - loop_start_us;
            loop_us_total += (uint64_t)loop_us;
            loop_sample_count++;
            if (loop_us > CAMERA_PIPELINE_FRAME_TRACE_SLOW_LOOP_US) {
                slow_loop_count++;
            }
            continue;
        }

        if (key_frame_required_after_drop && !key_frame) {
            drop_count++;
            key_wait_drop_count++;
            camera_pipeline_h264_return_output(&h264);
            int64_t loop_us = esp_timer_get_time() - loop_start_us;
            loop_us_total += (uint64_t)loop_us;
            loop_sample_count++;
            if (loop_us > CAMERA_PIPELINE_FRAME_TRACE_SLOW_LOOP_US) {
                slow_loop_count++;
            }
            continue;
        }

        /*
         * Keep the media timestamp on the capture cadence.  Using the time
         * after H264 encode makes hardware-encoder jitter look like frame-time
         * jitter to the RTC stack and can show up as tiny visual stalls.
         */
        uint64_t pts_us = (uint64_t)capture_start_us;
        int64_t callback_start_us = esp_timer_get_time();
        uint64_t media_timestamp_lag_us =
            callback_start_us > capture_start_us ?
                (uint64_t)(callback_start_us - capture_start_us) :
                camera_pipeline_abs_delta_us((uint64_t)callback_start_us, pts_us);
        media_timestamp_lag_us_total += media_timestamp_lag_us;
        media_timestamp_sample_count++;
        if (media_timestamp_lag_us > max_media_timestamp_lag_us) {
            max_media_timestamp_lag_us = media_timestamp_lag_us;
        }
        ret = runtime.video_cb(h264_data,
                               h264_len,
                               h264.width,
                               h264.height,
                               pts_us,
                               TIRTC_VIDEO_H264,
                               key_frame,
                               runtime.video_ctx);
        int64_t callback_us = esp_timer_get_time() - callback_start_us;
        camera_pipeline_h264_return_output(&h264);
        int64_t loop_us = esp_timer_get_time() - loop_start_us;
        callback_us_total += (uint64_t)callback_us;
        callback_sample_count++;
        loop_us_total += (uint64_t)loop_us;
        loop_sample_count++;
        if (callback_us > CAMERA_PIPELINE_FRAME_TRACE_SLOW_STAGE_US) {
            slow_callback_count++;
        }
        if (loop_us > CAMERA_PIPELINE_FRAME_TRACE_SLOW_LOOP_US) {
            slow_loop_count++;
        }

        bool trace_initial = trace_frame_count <= CAMERA_PIPELINE_FRAME_TRACE_INITIAL_COUNT;
        bool trace_large = (uint32_t)h264_len >= CAMERA_PIPELINE_FRAME_TRACE_LARGE_PAYLOAD_BYTES;
        bool trace_slow = capture_us > CAMERA_PIPELINE_FRAME_TRACE_SLOW_STAGE_US ||
                          encode_us > CAMERA_PIPELINE_FRAME_TRACE_SLOW_STAGE_US ||
                          callback_us > CAMERA_PIPELINE_FRAME_TRACE_SLOW_STAGE_US ||
                          loop_us > CAMERA_PIPELINE_FRAME_TRACE_SLOW_LOOP_US;
        TickType_t trace_tick = xTaskGetTickCount();
        bool trace_period_due = s_last_frame_trace_log_tick == 0 ||
                                trace_tick - s_last_frame_trace_log_tick >=
                                    pdMS_TO_TICKS(CAMERA_PIPELINE_FRAME_TRACE_INTERVAL_MS);
        if (trace_initial || ((trace_large || trace_slow || ret != ESP_OK) && trace_period_due)) {
            s_last_frame_trace_log_tick = trace_tick;
            ESP_LOGI(TAG,
                     "camera frame trace: idx=%lu ret=%s key=%d payload=%u seq=%lu drain=%lu cap=%lldus enc=%lldus sync_in=%lldus hw=%lldus sync_out=%lldus cb=%lldus loop=%lldus target=%ux%u@%u bitrate_cfg=%u max_delta=%u",
                     (unsigned long)trace_frame_count,
                     esp_err_to_name(ret),
                     key_frame ? 1 : 0,
                     (unsigned)h264_len,
                     (unsigned long)source_sequence,
                     (unsigned long)source_stale_frames_dropped,
                     (long long)capture_us,
                     (long long)encode_us,
                     (long long)h264_timing.sync_in_us,
                     (long long)h264_timing.hw_us,
                     (long long)h264_timing.sync_out_us,
                     (long long)callback_us,
                     (long long)loop_us,
                     (unsigned)h264.width,
                     (unsigned)h264.height,
                     (unsigned)policy.rtc_video_fps,
                     (unsigned)policy.h264_bitrate_bps,
                     (unsigned)policy.h264_max_delta_payload_bytes);
        }

        if (ret == ESP_OK) {
            upstream_count++;
            total_payload_bytes += h264_len;
            if (!first_frame_logged) {
                first_frame_logged = true;
                uint8_t head0 = h264_len > 0U ? h264_data[0] : 0;
                uint8_t head1 = h264_len > 1U ? h264_data[1] : 0;
                uint8_t head2 = h264_len > 2U ? h264_data[2] : 0;
                uint8_t head3 = h264_len > 3U ? h264_data[3] : 0;
                ESP_LOGI(TAG,
                         "camera pipeline first upstream frame: source=%ux%u input=%s output=%ux%u media=h264 key=%d payload=%u seq=%lu drain=%lu head=%02X%02X%02X%02X capture=%lldus convert=%lldus encode=%lldus sync_in=%lldus hw=%lldus sync_out=%lldus cb=%lldus loop=%lldus",
                         source_width,
                         source_height,
                         h264_input_path,
                         h264.width,
                         h264.height,
                         key_frame ? 1 : 0,
                         (unsigned)h264_len,
                         (unsigned long)source_sequence,
                         (unsigned long)source_stale_frames_dropped,
                         head0,
                         head1,
                         head2,
                         head3,
                         (long long)capture_us,
                         (long long)convert_us,
                         (long long)encode_us,
                         (long long)h264_timing.sync_in_us,
                         (long long)h264_timing.hw_us,
                         (long long)h264_timing.sync_out_us,
                         (long long)callback_us,
                         (long long)loop_us);
            }
        } else {
            drop_count++;
            if (ret != ESP_ERR_INVALID_STATE) {
                media_governor_note_network_backpressure();
            }
            TickType_t fail_tick = xTaskGetTickCount();
            if (s_last_rtc_fail_log_tick == 0 ||
                fail_tick - s_last_rtc_fail_log_tick >= pdMS_TO_TICKS(1000)) {
                s_last_rtc_fail_log_tick = fail_tick;
                ESP_LOG_LEVEL_LOCAL(ret == ESP_ERR_INVALID_STATE ? ESP_LOG_DEBUG : ESP_LOG_WARN,
                                    TAG,
                                    "camera pipeline upstream frame dropped: %s",
                                    esp_err_to_name(ret));
            }
        }

        now_tick = xTaskGetTickCount();
        if (now_tick - last_stats_tick >= pdMS_TO_TICKS(CAMERA_PIPELINE_LOG_INTERVAL_MS)) {
            uint32_t elapsed_ms = pdTICKS_TO_MS(now_tick - last_stats_tick);
            if (elapsed_ms == 0U) {
                elapsed_ms = 1U;
            }
            uint32_t avg_payload = upstream_count > 0U ? total_payload_bytes / upstream_count : 0U;
            uint32_t avg_capture_us = capture_sample_count > 0U ? (uint32_t)(capture_us_total / capture_sample_count) : 0U;
            uint32_t avg_encode_us = encode_sample_count > 0U ? (uint32_t)(encode_us_total / encode_sample_count) : 0U;
            uint32_t avg_h264_sync_in_us = encode_sample_count > 0U ?
                                           (uint32_t)(h264_sync_in_us_total / encode_sample_count) :
                                           0U;
            uint32_t avg_h264_hw_us = encode_sample_count > 0U ?
                                      (uint32_t)(h264_hw_us_total / encode_sample_count) :
                                      0U;
            uint32_t avg_h264_sync_out_us = encode_sample_count > 0U ?
                                            (uint32_t)(h264_sync_out_us_total / encode_sample_count) :
                                            0U;
            uint32_t avg_callback_us = callback_sample_count > 0U ? (uint32_t)(callback_us_total / callback_sample_count) : 0U;
            uint32_t avg_loop_us = loop_sample_count > 0U ? (uint32_t)(loop_us_total / loop_sample_count) : 0U;
            uint32_t avg_media_timestamp_lag_us =
                media_timestamp_sample_count > 0U ?
                    (uint32_t)(media_timestamp_lag_us_total / media_timestamp_sample_count) :
                    0U;
            uint32_t avg_camera_sequence_delta_x10 =
                camera_sequence_sample_count > 0U ?
                    (uint32_t)(camera_sequence_delta_total_x10 / camera_sequence_sample_count) :
                    0U;
            uint32_t avg_gap_us = encoded_frame_count > 1U ?
                                  (uint32_t)(frame_gap_us_total / (encoded_frame_count - 1U)) :
                                  0U;
            uint32_t min_payload = min_payload_bytes == UINT32_MAX ? 0U : min_payload_bytes;
            uint32_t measured_fps_x10 = (uint32_t)(((uint64_t)upstream_count * 10000ULL) / elapsed_ms);
            uint32_t measured_bitrate_kbps = (uint32_t)(((uint64_t)total_payload_bytes * 8ULL) / elapsed_ms);
            size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            camera_pipeline_note_interval_metrics(measured_fps_x10,
                                                  measured_bitrate_kbps,
                                                  avg_payload,
                                                  drop_count,
                                                  capture_fail_count,
                                                  encode_fail_count);
            ESP_LOGI(TAG,
                     "camera pipeline stats: target=%ux%u@%u cfg_bitrate=%ukbps encoded=%lu upstream=%lu fps=%lu.%lu bitrate=%lukbps drop=%lu bp_skip=%lu guard_drop=%lu keywait_drop=%lu cap_fail=%lu enc_fail=%lu key=%lu large=%lu slow_cap=%lu slow_enc=%lu slow_cb=%lu slow_loop=%lu payload[min/avg/max]=%lu/%lu/%lu avg_gap_us=%lu max_gap_us=%llu cam_drain=%lu seq_delta_avg=%lu.%lu seq_delta_max=%lu avg_cap_us=%lu avg_enc_us=%lu avg_h264[sync_in/hw/sync_out]=%lu/%lu/%lu avg_cb_us=%lu avg_loop_us=%lu avg_ts_lag_us=%lu max_ts_lag_us=%llu internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u psram_free=%u psram_largest=%u h264_out_buf=%u",
                     (unsigned)h264.width,
                     (unsigned)h264.height,
                     (unsigned)policy.rtc_video_fps,
                     (unsigned)(policy.h264_bitrate_bps / 1000U),
                     (unsigned long)encoded_frame_count,
                     (unsigned long)upstream_count,
                     (unsigned long)(measured_fps_x10 / 10U),
                     (unsigned long)(measured_fps_x10 % 10U),
                     (unsigned long)measured_bitrate_kbps,
                     (unsigned long)drop_count,
                     (unsigned long)backpressure_skip_count,
                     (unsigned long)transport_guard_drop_count,
                     (unsigned long)key_wait_drop_count,
                     (unsigned long)capture_fail_count,
                     (unsigned long)encode_fail_count,
                     (unsigned long)key_frame_count,
                     (unsigned long)large_frame_count,
                     (unsigned long)slow_capture_count,
                     (unsigned long)slow_encode_count,
                     (unsigned long)slow_callback_count,
                     (unsigned long)slow_loop_count,
                     (unsigned long)min_payload,
                     (unsigned long)avg_payload,
                     (unsigned long)max_payload_bytes,
                     (unsigned long)avg_gap_us,
                     (unsigned long long)max_frame_gap_us,
                     (unsigned long)camera_stale_frame_drain_count,
                     (unsigned long)(avg_camera_sequence_delta_x10 / 10U),
                     (unsigned long)(avg_camera_sequence_delta_x10 % 10U),
                     (unsigned long)max_camera_sequence_delta,
                     (unsigned long)avg_capture_us,
                     (unsigned long)avg_encode_us,
                     (unsigned long)avg_h264_sync_in_us,
                     (unsigned long)avg_h264_hw_us,
                     (unsigned long)avg_h264_sync_out_us,
                     (unsigned long)avg_callback_us,
                     (unsigned long)avg_loop_us,
                     (unsigned long)avg_media_timestamp_lag_us,
                     (unsigned long long)max_media_timestamp_lag_us,
                     (unsigned)internal_free,
                     (unsigned)internal_largest,
                     (unsigned)dma_free,
                     (unsigned)dma_largest,
                     (unsigned)psram_free,
                     (unsigned)psram_largest,
                     (unsigned)h264.capture_buffer_size);
            last_stats_tick = now_tick;
            upstream_count = 0;
            drop_count = 0;
            capture_fail_count = 0;
            encode_fail_count = 0;
            total_payload_bytes = 0;
            backpressure_skip_count = 0;
            transport_guard_drop_count = 0;
            key_wait_drop_count = 0;
            encoded_frame_count = 0;
            key_frame_count = 0;
            slow_capture_count = 0;
            slow_encode_count = 0;
            slow_callback_count = 0;
            slow_loop_count = 0;
            large_frame_count = 0;
            min_payload_bytes = UINT32_MAX;
            max_payload_bytes = 0;
            frame_gap_us_total = 0;
            max_frame_gap_us = 0;
            last_frame_start_us = 0;
            capture_us_total = 0;
            encode_us_total = 0;
            h264_sync_in_us_total = 0;
            h264_hw_us_total = 0;
            h264_sync_out_us_total = 0;
            callback_us_total = 0;
            loop_us_total = 0;
            media_timestamp_lag_us_total = 0;
            max_media_timestamp_lag_us = 0;
            camera_sequence_delta_total_x10 = 0;
            camera_stale_frame_drain_count = 0;
            camera_sequence_sample_count = 0;
            max_camera_sequence_delta = 0;
            capture_sample_count = 0;
            encode_sample_count = 0;
            callback_sample_count = 0;
            loop_sample_count = 0;
            media_timestamp_sample_count = 0;
        }

        /* The next loop iteration waits until the next RTC frame deadline. */
    }

exit_task:
    if (!camera_pipeline_h264_store_reserved(&h264, "pipeline-stop")) {
        camera_pipeline_h264_close(&h264);
    }
    if (camera_acquired) {
        camera_driver_release_device();
    } else if (video_subsystem_prepared) {
        (void)camera_driver_deinit();
    }

    ESP_LOGI(TAG, "camera pipeline stopped");
    camera_pipeline_mark_task_stopped();
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t camera_pipeline_ensure_task_started(void)
{
    bool create_task = false;

    taskENTER_CRITICAL(&s_lock);
    if (s_rtc_enabled && s_task == NULL && !s_starting) {
        s_starting = true;
        create_task = true;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (!create_task) {
        return ESP_OK;
    }

    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(camera_pipeline_task,
                                                          "camera_pipe",
                                                          CAMERA_PIPELINE_TASK_STACK,
                                                          NULL,
                                                          CAMERA_PIPELINE_TASK_PRIORITY,
                                                          NULL,
                                                          APP_TASK_CORE_CAMERA,
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ret != pdPASS) {
        task_ret = xTaskCreatePinnedToCoreWithCaps(camera_pipeline_task,
                                                   "camera_pipe",
                                                   CAMERA_PIPELINE_TASK_STACK,
                                                   NULL,
                                                   CAMERA_PIPELINE_TASK_PRIORITY,
                                                   NULL,
                                                   APP_TASK_CORE_CAMERA,
                                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (task_ret != pdPASS) {
        taskENTER_CRITICAL(&s_lock);
        s_starting = false;
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t camera_pipeline_init(void)
{
    ESP_RETURN_ON_ERROR(media_governor_init(), TAG, "media governor init failed");

    taskENTER_CRITICAL(&s_lock);
    s_initialized = true;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t camera_pipeline_prewarm_h264(void)
{
    ESP_RETURN_ON_ERROR(camera_pipeline_init(), TAG, "camera pipeline init failed");
    ESP_RETURN_ON_FALSE(camera_driver_is_configured(), ESP_ERR_NOT_SUPPORTED, TAG, "camera not configured");

    media_governor_camera_policy_t policy = {0};
    media_governor_get_rtc_av_camera_policy(&policy);
    if (policy.h264_output_buffer_bytes == 0U) {
        policy.h264_output_buffer_bytes = CAMERA_PIPELINE_H264_FALLBACK_OUTPUT_BUFFER_BYTES;
    }

    uint16_t width = camera_pipeline_even_dimension(policy.rtc_width);
    uint16_t height = camera_pipeline_even_dimension(policy.rtc_height);
    ESP_RETURN_ON_FALSE(width >= CAMERA_PIPELINE_H264_MIN_WIDTH &&
                            height >= CAMERA_PIPELINE_H264_MIN_HEIGHT &&
                            policy.rtc_video_fps > 0U,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "invalid H264 prewarm target: %ux%u@%u",
                        width,
                        height,
                        policy.rtc_video_fps);

    taskENTER_CRITICAL(&s_lock);
    if (camera_pipeline_h264_matches(&s_reserved_h264,
                                     width,
                                     height,
                                     policy.rtc_video_fps,
                                     policy.h264_bitrate_bps,
                                     policy.h264_output_buffer_bytes)) {
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG,
                 "H264 encoder prewarm already reserved: size=%ux%u fps=%u bitrate=%u out_buf=%u",
                 width,
                 height,
                 policy.rtc_video_fps,
                 (unsigned)policy.h264_bitrate_bps,
                 (unsigned)policy.h264_output_buffer_bytes);
        return ESP_OK;
    }
    if (s_rtc_enabled || s_task != NULL || s_starting || s_h264_reserve_in_progress) {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_h264_reserve_in_progress = true;
    taskEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG,
             "H264 encoder early prewarm begin: size=%ux%u fps=%u bitrate=%u out_buf=%u ref_internal_est=%u internal_free=%u internal_largest=%u dma_largest=%u psram_free=%u psram_largest=%u",
             width,
             height,
             policy.rtc_video_fps,
             (unsigned)policy.h264_bitrate_bps,
             (unsigned)policy.h264_output_buffer_bytes,
             (unsigned)camera_pipeline_h264_ref_internal_estimate(width),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    bool dma_escrow_lent = media_dma_reserve_is_reserved();
    if (dma_escrow_lent) {
        media_dma_reserve_release("h264-early-prewarm");
        ESP_LOGI(TAG, "DMA escrow lent to H264 early prewarm");
    }

    camera_pipeline_h264_encoder_t enc = {
        .fd = -1,
    };
    esp_err_t ret = camera_driver_prepare_video_subsystem();
    if (ret == ESP_OK) {
        ret = camera_pipeline_h264_open(&enc,
                                        width,
                                        height,
                                        policy.rtc_video_fps,
                                        policy.h264_bitrate_bps,
                                        policy.h264_output_buffer_bytes);
    }
    if (ret != ESP_OK && dma_escrow_lent) {
        esp_err_t reclaim_ret = media_dma_reserve_reclaim("h264-early-prewarm-failed");
        if (reclaim_ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "DMA escrow reclaim after failed H264 prewarm failed: open=%s reclaim=%s",
                     esp_err_to_name(ret),
                     esp_err_to_name(reclaim_ret));
        }
    }

    bool stored = false;
    if (ret == ESP_OK) {
        stored = camera_pipeline_h264_store_reserved(&enc, "early-prewarm");
        if (!stored) {
            ret = ESP_ERR_INVALID_STATE;
        }
    }
    if (camera_pipeline_h264_is_open(&enc)) {
        camera_pipeline_h264_close(&enc);
    }

    taskENTER_CRITICAL(&s_lock);
    s_h264_reserve_in_progress = false;
    taskEXIT_CRITICAL(&s_lock);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "H264 encoder early prewarm done: internal_free=%u internal_largest=%u dma_largest=%u psram_free=%u psram_largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    } else {
        ESP_LOGW(TAG,
                 "H264 encoder early prewarm failed: %s internal_free=%u internal_largest=%u dma_largest=%u psram_free=%u psram_largest=%u",
                 esp_err_to_name(ret),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    return ret;
}

void camera_pipeline_on_rtc_video_config_changed(void)
{
    camera_pipeline_h264_encoder_t stale = {
        .fd = -1,
    };
    media_governor_camera_policy_t policy = {0};

    media_governor_get_rtc_av_camera_policy(&policy);
    uint16_t width = camera_pipeline_even_dimension(policy.rtc_width);
    uint16_t height = camera_pipeline_even_dimension(policy.rtc_height);

    taskENTER_CRITICAL(&s_lock);
    if (camera_pipeline_h264_is_open(&s_reserved_h264) &&
        !camera_pipeline_h264_matches(&s_reserved_h264,
                                      width,
                                      height,
                                      policy.rtc_video_fps,
                                      policy.h264_bitrate_bps,
                                      policy.h264_output_buffer_bytes)) {
        stale = s_reserved_h264;
        s_reserved_h264 = (camera_pipeline_h264_encoder_t) {
            .fd = -1,
        };
    }
    taskEXIT_CRITICAL(&s_lock);

    if (camera_pipeline_h264_is_open(&stale)) {
        ESP_LOGI(TAG,
                 "H264 reserved resource released after video config change: old=%ux%u@%u %ukbps new=%ux%u@%u %ukbps",
                 (unsigned)stale.width,
                 (unsigned)stale.height,
                 (unsigned)stale.fps,
                 (unsigned)(stale.bitrate_bps / 1000U),
                 (unsigned)width,
                 (unsigned)height,
                 (unsigned)policy.rtc_video_fps,
                 (unsigned)(policy.h264_bitrate_bps / 1000U));
        camera_pipeline_h264_close(&stale);
    }
}

void camera_pipeline_request_key_frame(void)
{
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    bool accept = true;
    bool log_drop = false;
    uint64_t drop_age_us = 0U;
    const char *drop_reason = "unknown";

    taskENTER_CRITICAL(&s_lock);
    uint64_t last_request_us = s_last_key_frame_request_us;
    uint64_t last_key_us = s_last_key_frame_us;
    if (last_request_us != 0U && now_us >= last_request_us &&
        now_us - last_request_us < CAMERA_PIPELINE_KEY_FRAME_REQUEST_MIN_INTERVAL_US) {
        accept = false;
        drop_reason = "recent-request";
        drop_age_us = now_us - last_request_us;
    }
    if (accept && last_key_us != 0U && now_us >= last_key_us &&
        now_us - last_key_us < CAMERA_PIPELINE_KEY_FRAME_REQUEST_MIN_INTERVAL_US) {
        accept = false;
        drop_reason = "recent-key";
        drop_age_us = now_us - last_key_us;
    }
    if (accept) {
        s_key_frame_request_pending = true;
        s_last_key_frame_request_us = now_us;
    } else if (s_last_key_frame_request_drop_log_us == 0U ||
               now_us - s_last_key_frame_request_drop_log_us >= 2000000ULL) {
        s_last_key_frame_request_drop_log_us = now_us;
        log_drop = true;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (!accept && log_drop) {
        ESP_LOGI(TAG,
                 "H264 key-frame request coalesced: reason=%s age_ms=%" PRIu64 " min_ms=%u",
                 drop_reason,
                 drop_age_us / 1000ULL,
                 (unsigned)CONFIG_APP_RTC_H264_KEY_FRAME_REQUEST_MIN_INTERVAL_MS);
    }
}

esp_err_t camera_pipeline_set_rtc_video_sink(camera_pipeline_video_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_ERROR(camera_pipeline_init(), TAG, "camera pipeline init failed");

    taskENTER_CRITICAL(&s_lock);
    s_video_cb = cb;
    s_video_ctx = ctx;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t camera_pipeline_set_rtc_video_enabled(bool enabled)
{
    if (!s_initialized) {
        ESP_RETURN_ON_ERROR(camera_pipeline_init(), TAG, "camera pipeline init failed");
    }
    if (enabled && !camera_driver_is_configured()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (enabled && s_video_cb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    bool rtc_enabled = false;
    taskENTER_CRITICAL(&s_lock);
    s_rtc_enabled = enabled;
    rtc_enabled = s_rtc_enabled;
    taskEXIT_CRITICAL(&s_lock);

    camera_pipeline_apply_profile(rtc_enabled);
    esp_err_t ret = camera_pipeline_ensure_task_started();
    if (ret != ESP_OK) {
        taskENTER_CRITICAL(&s_lock);
        s_rtc_enabled = false;
        rtc_enabled = s_rtc_enabled;
        taskEXIT_CRITICAL(&s_lock);
        camera_pipeline_apply_profile(rtc_enabled);
    }
    return ret;
}

bool camera_pipeline_is_running(void)
{
    bool running = false;

    taskENTER_CRITICAL(&s_lock);
    running = s_task != NULL || s_starting;
    taskEXIT_CRITICAL(&s_lock);
    return running;
}

bool camera_pipeline_is_rtc_video_active(void)
{
    bool active = false;

    taskENTER_CRITICAL(&s_lock);
    active = s_rtc_enabled;
    taskEXIT_CRITICAL(&s_lock);
    return active;
}

void camera_pipeline_get_metrics(camera_pipeline_metrics_t *metrics)
{
    if (metrics == NULL) {
        return;
    }

    bool running = false;
    bool rtc_enabled = false;

    taskENTER_CRITICAL(&s_lock);
    *metrics = s_metrics;
    running = s_task != NULL || s_starting;
    rtc_enabled = s_rtc_enabled;
    metrics->running = running;
    metrics->rtc_enabled = rtc_enabled;
    taskEXIT_CRITICAL(&s_lock);

    if (!rtc_enabled) {
        memset(metrics, 0, sizeof(*metrics));
        metrics->running = running;
    }
}
