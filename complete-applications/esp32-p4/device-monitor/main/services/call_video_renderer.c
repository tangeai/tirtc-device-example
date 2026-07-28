#include "call_video_renderer.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_h264_dec_param.h"
#include "esp_h264_dec_sw.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "app_memory_policy.h"
#include "app_task_affinity.h"
#include "media_dma_reserve.h"
#include "video_frame_converter.h"

static const char *TAG = "call_video";

#define CALL_VIDEO_INPUT_SLOT_COUNT       8U
#define CALL_VIDEO_INPUT_SLOT_CAPACITY    (256U * 1024U)
#define CALL_VIDEO_DECODED_SLOT_COUNT     3U
#define CALL_VIDEO_DECODED_SLOT_CAPACITY  \
    (CALL_VIDEO_RENDER_WIDTH * CALL_VIDEO_RENDER_HEIGHT * 3U / 2U)
#define CALL_VIDEO_OUTPUT_SLOT_COUNT      3U
#define CALL_VIDEO_OUTPUT_SLOT_INVALID    UINT8_MAX
#define CALL_VIDEO_TASK_STACK_SIZE        (16U * 1024U)
#define CALL_VIDEO_CONVERT_TASK_STACK_SIZE (8U * 1024U)
/* Keep control/network and audio ahead of video, but do not let the decoder
 * fall behind the compressed input stream. A late decoder forces a full GOP
 * resync and is much more expensive than delaying one uplink frame. */
#define CALL_VIDEO_TASK_PRIORITY          16U
#define CALL_VIDEO_CONVERT_TASK_PRIORITY  14U
#define CALL_VIDEO_START_TIMEOUT_MS       5000U
#define CALL_VIDEO_STOP_TIMEOUT_MS        3000U
#define CALL_VIDEO_STATS_INTERVAL_US      (5LL * 1000LL * 1000LL)
#define CALL_VIDEO_FRAME_PIXELS           (CALL_VIDEO_RENDER_WIDTH * CALL_VIDEO_RENDER_HEIGHT)
#define CALL_VIDEO_FRAME_BYTES            (CALL_VIDEO_FRAME_PIXELS * sizeof(uint16_t))
#define CALL_VIDEO_PSRAM_POOL_BYTES        \
    ((CALL_VIDEO_INPUT_SLOT_COUNT * CALL_VIDEO_INPUT_SLOT_CAPACITY) + \
     (CALL_VIDEO_DECODED_SLOT_COUNT * CALL_VIDEO_DECODED_SLOT_CAPACITY) + \
     (CALL_VIDEO_OUTPUT_SLOT_COUNT * CALL_VIDEO_FRAME_BYTES))

#if !CONFIG_FREERTOS_UNICORE
_Static_assert(APP_TASK_CORE_VIDEO_DECODE != CONFIG_ESP_H264_DUAL_TASK_CORE,
               "TinyH264 helper must run on the core opposite the decoder caller");
#endif

#if CONFIG_CACHE_L2_CACHE_LINE_SIZE > CONFIG_CACHE_L1_CACHE_LINE_SIZE
#define CALL_VIDEO_CACHE_LINE_SIZE CONFIG_CACHE_L2_CACHE_LINE_SIZE
#else
#define CALL_VIDEO_CACHE_LINE_SIZE CONFIG_CACHE_L1_CACHE_LINE_SIZE
#endif

typedef struct {
    uint8_t *data;
    size_t data_len;
    bool key_frame;
    uint32_t pts;
    uint32_t generation;
    int64_t queued_at_us;
} call_video_input_slot_t;

typedef struct {
    uint8_t *data;
    size_t data_len;
    uint16_t width;
    uint16_t height;
    uint32_t generation;
} call_video_decoded_slot_t;

typedef enum {
    CALL_VIDEO_OUTPUT_FREE = 0,
    CALL_VIDEO_OUTPUT_WRITING,
    CALL_VIDEO_OUTPUT_READY,
    CALL_VIDEO_OUTPUT_PRESENTED,
} call_video_output_state_t;

typedef struct {
    uint16_t *pixels;
    call_video_output_state_t state;
    uint32_t sequence;
} call_video_output_slot_t;

typedef struct {
    portMUX_TYPE lock;
    QueueHandle_t free_slots;
    QueueHandle_t ready_slots;
    QueueHandle_t decoded_free_slots;
    QueueHandle_t decoded_ready_slots;
    SemaphoreHandle_t frame_mutex;
    SemaphoreHandle_t submit_mutex;
    SemaphoreHandle_t start_done;
    SemaphoreHandle_t stop_done;
    SemaphoreHandle_t convert_stop_done;
    TaskHandle_t task;
    TaskHandle_t convert_task;
    call_video_input_slot_t slots[CALL_VIDEO_INPUT_SLOT_COUNT];
    call_video_decoded_slot_t decoded_slots[CALL_VIDEO_DECODED_SLOT_COUNT];
    call_video_output_slot_t output_slots[CALL_VIDEO_OUTPUT_SLOT_COUNT];
    uint8_t ready_output_slots[CALL_VIDEO_OUTPUT_SLOT_COUNT];
    uint8_t ready_output_head;
    uint8_t ready_output_count;
    uint8_t presented_output_slot;
    bool start_pending;
    bool running;
    bool stop_requested;
    bool waiting_for_key_frame;
    bool frame_ready;
    esp_err_t start_result;
    uint32_t generation;
    uint16_t source_width;
    uint16_t source_height;
    uint32_t received_frames;
    uint64_t received_bytes;
    uint32_t submitted_frames;
    uint32_t decoded_frames;
    uint32_t converted_frames;
    uint32_t conversion_dropped_frames;
    uint32_t conversion_failures;
    uint32_t dropped_frames;
    uint32_t decode_failures;
    uint32_t latest_sequence;
    uint32_t presented_frames;
    uint32_t decode_process_calls;
    uint64_t decode_time_us;
    uint32_t decode_access_units;
    uint64_t decode_access_unit_time_us;
    uint32_t decode_access_unit_max_us;
    uint32_t decode_key_access_units;
    uint64_t decode_key_time_us;
    uint32_t decode_delta_access_units;
    uint64_t decode_delta_time_us;
    uint64_t decode_copy_time_us;
    uint32_t decode_copy_max_us;
    uint64_t convert_time_us;
    uint32_t convert_max_us;
    uint64_t present_copy_time_us;
    uint32_t present_copy_max_us;
    uint32_t input_queue_age_samples;
    uint64_t input_queue_age_us;
    uint32_t input_queue_age_max_us;
    uint32_t decoder_creations;
    uint32_t decoder_restarts;
    uint32_t discontinuities;
    uint32_t input_overflows;
    bool resources_preparing;
    bool resources_ready;
} call_video_renderer_t;

static call_video_renderer_t s_renderer = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
    .presented_output_slot = CALL_VIDEO_OUTPUT_SLOT_INVALID,
};

static bool call_video_stop_requested(void)
{
    bool requested = false;

    taskENTER_CRITICAL(&s_renderer.lock);
    requested = s_renderer.stop_requested;
    taskEXIT_CRITICAL(&s_renderer.lock);
    return requested;
}

static void call_video_return_slot(uint8_t index)
{
    if (s_renderer.free_slots != NULL) {
        (void)xQueueSend(s_renderer.free_slots, &index, 0);
    }
}

static void call_video_drain_ready_queue(void)
{
    uint8_t index = 0;

    while (s_renderer.ready_slots != NULL &&
           xQueueReceive(s_renderer.ready_slots, &index, 0) == pdTRUE) {
        call_video_return_slot(index);
    }
}

static void call_video_return_decoded_slot(uint8_t index)
{
    if (s_renderer.decoded_free_slots != NULL) {
        (void)xQueueSend(s_renderer.decoded_free_slots, &index, 0);
    }
}

static void call_video_drain_decoded_ready_queue(void)
{
    uint8_t index = 0;

    while (s_renderer.decoded_ready_slots != NULL &&
           xQueueReceive(s_renderer.decoded_ready_slots, &index, 0) == pdTRUE) {
        call_video_return_decoded_slot(index);
    }
}

static void call_video_drain_binary_semaphore(SemaphoreHandle_t semaphore)
{
    if (semaphore == NULL) {
        return;
    }
    while (xSemaphoreTake(semaphore, 0) == pdTRUE) {
    }
}

/* The RGB pool is a shallow presentation jitter buffer. Keeping converted
 * frames in FIFO order absorbs short network/decode bursts without allowing
 * latency to grow beyond the fixed three-slot pool. Callers hold frame_mutex. */
static bool call_video_output_pop_ready_locked(uint8_t *slot_index)
{
    if (slot_index == NULL || s_renderer.ready_output_count == 0U) {
        return false;
    }

    *slot_index = s_renderer.ready_output_slots[s_renderer.ready_output_head];
    s_renderer.ready_output_head =
        (uint8_t)((s_renderer.ready_output_head + 1U) % CALL_VIDEO_OUTPUT_SLOT_COUNT);
    s_renderer.ready_output_count--;
    return true;
}

static void call_video_output_push_ready_locked(uint8_t slot_index)
{
    uint8_t tail = (uint8_t)((s_renderer.ready_output_head +
                              s_renderer.ready_output_count) %
                             CALL_VIDEO_OUTPUT_SLOT_COUNT);
    s_renderer.ready_output_slots[tail] = slot_index;
    s_renderer.ready_output_count++;
}

static void call_video_output_release_ready_locked(void)
{
    uint8_t slot_index = CALL_VIDEO_OUTPUT_SLOT_INVALID;

    while (call_video_output_pop_ready_locked(&slot_index)) {
        if (slot_index < CALL_VIDEO_OUTPUT_SLOT_COUNT) {
            s_renderer.output_slots[slot_index].state = CALL_VIDEO_OUTPUT_FREE;
        }
    }
    s_renderer.ready_output_head = 0U;
}

static void call_video_mark_discontinuity(void)
{
    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.generation++;
    s_renderer.waiting_for_key_frame = true;
    s_renderer.discontinuities++;
    taskEXIT_CRITICAL(&s_renderer.lock);
    call_video_drain_ready_queue();
    call_video_drain_decoded_ready_queue();
}

static esp_err_t call_video_decoder_create(esp_h264_dec_handle_t *decoder,
                                           esp_h264_dec_param_sw_handle_t *parameters)
{
    esp_h264_dec_cfg_sw_t config = {
        .pic_type = ESP_H264_RAW_FMT_I420,
    };

    if (esp_h264_dec_sw_new(&config, decoder) != ESP_H264_ERR_OK) {
        return ESP_ERR_NO_MEM;
    }
    if (esp_h264_dec_sw_get_param_hd(*decoder, parameters) != ESP_H264_ERR_OK ||
        esp_h264_dec_open(*decoder) != ESP_H264_ERR_OK) {
        (void)esp_h264_dec_del(*decoder);
        *decoder = NULL;
        *parameters = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void call_video_decoder_destroy(esp_h264_dec_handle_t *decoder)
{
    if (decoder == NULL || *decoder == NULL) {
        return;
    }
    (void)esp_h264_dec_close(*decoder);
    (void)esp_h264_dec_del(*decoder);
    *decoder = NULL;
}

static esp_err_t call_video_decoder_dma_guard_begin(bool *escrow_lent)
{
    ESP_RETURN_ON_FALSE(escrow_lent != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid DMA guard state");
    *escrow_lent = false;

    /* The escrow protects a contiguous DMA block by occupying it while media
     * drivers are idle. Decoder bootstrap must borrow that block, not allocate
     * another escrow block before it is allowed to run. An absent escrow is
     * valid: the early H264 encoder reservation may already own that memory. */
    if (!media_dma_reserve_is_reserved()) {
        return ESP_OK;
    }

    media_dma_reserve_release("h264-downlink-bootstrap");
    *escrow_lent = true;
    return ESP_OK;
}

static void call_video_decoder_dma_guard_end(bool escrow_lent, esp_err_t decoder_ret)
{
    if (!escrow_lent) {
        return;
    }

    esp_err_t reclaim_ret = media_dma_reserve_reclaim(
        decoder_ret == ESP_OK ? "h264-downlink-bootstrap-done" :
                                "h264-downlink-bootstrap-failed");
    if (reclaim_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "H264 downlink DMA escrow remains lent: decoder=%s reclaim=%s",
                 esp_err_to_name(decoder_ret),
                 esp_err_to_name(reclaim_ret));
    }
    ESP_LOGI(TAG,
             "H264 downlink decoder bootstrap: ret=%s escrow_restore=%s internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u psram_free=%u",
             esp_err_to_name(decoder_ret),
             esp_err_to_name(reclaim_ret),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

typedef struct {
    int64_t last_log_us;
    uint32_t received_frames;
    uint64_t received_bytes;
    uint32_t submitted_frames;
    uint32_t decoded_frames;
    uint32_t converted_frames;
    uint32_t presented_frames;
    uint32_t dropped_frames;
    uint32_t conversion_dropped_frames;
    uint32_t decode_failures;
    uint32_t conversion_failures;
    uint32_t decode_process_calls;
    uint64_t decode_time_us;
    uint32_t decode_access_units;
    uint64_t decode_access_unit_time_us;
    uint32_t decode_access_unit_max_us;
    uint32_t decode_key_access_units;
    uint64_t decode_key_time_us;
    uint32_t decode_delta_access_units;
    uint64_t decode_delta_time_us;
    uint64_t decode_copy_time_us;
    uint32_t decode_copy_max_us;
    uint64_t convert_time_us;
    uint32_t convert_max_us;
    uint64_t present_copy_time_us;
    uint32_t present_copy_max_us;
    uint32_t input_queue_age_samples;
    uint64_t input_queue_age_us;
    uint32_t input_queue_age_max_us;
    uint32_t decoder_creations;
    uint32_t decoder_restarts;
    uint32_t discontinuities;
    uint32_t input_overflows;
    video_frame_converter_stats_t converter;
} call_video_log_window_t;

static void call_video_log_stats_if_due(call_video_log_window_t *window,
                                        video_frame_converter_handle_t converter)
{
#if !CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
    (void)window;
    (void)converter;
    return;
#else
    int64_t now_us = esp_timer_get_time();
    if (window->last_log_us == 0) {
        window->last_log_us = now_us;
        return;
    }
    int64_t elapsed_us = now_us - window->last_log_us;
    if (elapsed_us < CALL_VIDEO_STATS_INTERVAL_US) {
        return;
    }

    call_video_log_window_t current = {
        .last_log_us = now_us,
    };
    uint16_t source_width = 0;
    uint16_t source_height = 0;
    taskENTER_CRITICAL(&s_renderer.lock);
    current.received_frames = s_renderer.received_frames;
    current.received_bytes = s_renderer.received_bytes;
    current.submitted_frames = s_renderer.submitted_frames;
    current.decoded_frames = s_renderer.decoded_frames;
    current.converted_frames = s_renderer.converted_frames;
    current.presented_frames = s_renderer.presented_frames;
    current.dropped_frames = s_renderer.dropped_frames;
    current.conversion_dropped_frames = s_renderer.conversion_dropped_frames;
    current.decode_failures = s_renderer.decode_failures;
    current.conversion_failures = s_renderer.conversion_failures;
    current.decode_process_calls = s_renderer.decode_process_calls;
    current.decode_time_us = s_renderer.decode_time_us;
    current.decode_access_units = s_renderer.decode_access_units;
    current.decode_access_unit_time_us = s_renderer.decode_access_unit_time_us;
    current.decode_access_unit_max_us = s_renderer.decode_access_unit_max_us;
    current.decode_key_access_units = s_renderer.decode_key_access_units;
    current.decode_key_time_us = s_renderer.decode_key_time_us;
    current.decode_delta_access_units = s_renderer.decode_delta_access_units;
    current.decode_delta_time_us = s_renderer.decode_delta_time_us;
    current.decode_copy_time_us = s_renderer.decode_copy_time_us;
    current.decode_copy_max_us = s_renderer.decode_copy_max_us;
    current.convert_time_us = s_renderer.convert_time_us;
    current.convert_max_us = s_renderer.convert_max_us;
    current.present_copy_time_us = s_renderer.present_copy_time_us;
    current.present_copy_max_us = s_renderer.present_copy_max_us;
    current.input_queue_age_samples = s_renderer.input_queue_age_samples;
    current.input_queue_age_us = s_renderer.input_queue_age_us;
    current.input_queue_age_max_us = s_renderer.input_queue_age_max_us;
    current.decoder_creations = s_renderer.decoder_creations;
    current.decoder_restarts = s_renderer.decoder_restarts;
    current.discontinuities = s_renderer.discontinuities;
    current.input_overflows = s_renderer.input_overflows;
    source_width = s_renderer.source_width;
    source_height = s_renderer.source_height;
    taskEXIT_CRITICAL(&s_renderer.lock);
    video_frame_converter_get_stats(converter, &current.converter);

    uint32_t received_delta = current.received_frames - window->received_frames;
    uint64_t received_bytes_delta = current.received_bytes - window->received_bytes;
    uint32_t submitted_delta = current.submitted_frames - window->submitted_frames;
    uint32_t decoded_delta = current.decoded_frames - window->decoded_frames;
    uint32_t converted_delta = current.converted_frames - window->converted_frames;
    uint32_t presented_delta = current.presented_frames - window->presented_frames;
    uint32_t dropped_delta = current.dropped_frames - window->dropped_frames;
    uint32_t conversion_dropped_delta =
        current.conversion_dropped_frames - window->conversion_dropped_frames;
    uint32_t failure_delta = current.decode_failures - window->decode_failures;
    uint32_t conversion_failure_delta =
        current.conversion_failures - window->conversion_failures;
    uint32_t process_delta = current.decode_process_calls - window->decode_process_calls;
    uint64_t decode_time_delta = current.decode_time_us - window->decode_time_us;
    uint32_t access_unit_delta = current.decode_access_units - window->decode_access_units;
    uint64_t access_unit_time_delta =
        current.decode_access_unit_time_us - window->decode_access_unit_time_us;
    uint32_t key_access_unit_delta =
        current.decode_key_access_units - window->decode_key_access_units;
    uint64_t key_time_delta = current.decode_key_time_us - window->decode_key_time_us;
    uint32_t delta_access_unit_delta =
        current.decode_delta_access_units - window->decode_delta_access_units;
    uint64_t delta_time_delta = current.decode_delta_time_us - window->decode_delta_time_us;
    uint64_t decode_copy_time_delta = current.decode_copy_time_us - window->decode_copy_time_us;
    uint64_t convert_time_delta = current.convert_time_us - window->convert_time_us;
    uint64_t present_copy_time_delta =
        current.present_copy_time_us - window->present_copy_time_us;
    uint32_t queue_age_samples_delta =
        current.input_queue_age_samples - window->input_queue_age_samples;
    uint64_t queue_age_time_delta = current.input_queue_age_us - window->input_queue_age_us;
    uint32_t decoder_creations_delta = current.decoder_creations - window->decoder_creations;
    uint32_t decoder_restarts_delta = current.decoder_restarts - window->decoder_restarts;
    uint32_t discontinuities_delta = current.discontinuities - window->discontinuities;
    uint32_t input_overflows_delta = current.input_overflows - window->input_overflows;
    uint32_t ppa_frames_delta = current.converter.ppa_frames - window->converter.ppa_frames;
    uint32_t software_frames_delta =
        current.converter.software_frames - window->converter.software_frames;
    uint64_t pack_time_delta = current.converter.pack_time_us - window->converter.pack_time_us;
    uint64_t ppa_time_delta = current.converter.ppa_time_us - window->converter.ppa_time_us;
    uint64_t swap_time_delta = current.converter.swap_time_us - window->converter.swap_time_us;
    uint64_t software_time_delta =
        current.converter.software_time_us - window->converter.software_time_us;
    uint32_t received_fps_x10 = (uint32_t)(((uint64_t)received_delta * 10000000ULL) /
                                           (uint64_t)elapsed_us);
    uint32_t received_kbps = (uint32_t)((received_bytes_delta * 8ULL * 1000ULL) /
                                        (uint64_t)elapsed_us);
    uint32_t queued_fps_x10 = (uint32_t)(((uint64_t)submitted_delta * 10000000ULL) /
                                         (uint64_t)elapsed_us);
    uint32_t decoded_fps_x10 = (uint32_t)(((uint64_t)decoded_delta * 10000000ULL) /
                                           (uint64_t)elapsed_us);
    uint32_t converted_fps_x10 = (uint32_t)(((uint64_t)converted_delta * 10000000ULL) /
                                             (uint64_t)elapsed_us);
    uint32_t presented_fps_x10 = (uint32_t)(((uint64_t)presented_delta * 10000000ULL) /
                                            (uint64_t)elapsed_us);
    uint32_t average_decode_us = process_delta > 0U ?
                                 (uint32_t)(decode_time_delta / process_delta) : 0U;
    uint32_t average_access_unit_us = access_unit_delta > 0U ?
                                      (uint32_t)(access_unit_time_delta / access_unit_delta) : 0U;
    uint32_t average_key_us = key_access_unit_delta > 0U ?
                              (uint32_t)(key_time_delta / key_access_unit_delta) : 0U;
    uint32_t average_delta_us = delta_access_unit_delta > 0U ?
                                (uint32_t)(delta_time_delta / delta_access_unit_delta) : 0U;
    uint32_t average_copy_us = decoded_delta > 0U ?
                               (uint32_t)(decode_copy_time_delta / decoded_delta) : 0U;
    uint32_t average_convert_us = converted_delta > 0U ?
                                   (uint32_t)(convert_time_delta / converted_delta) : 0U;
    uint32_t average_present_copy_us = presented_delta > 0U ?
                                        (uint32_t)(present_copy_time_delta / presented_delta) : 0U;
    uint32_t average_queue_age_us = queue_age_samples_delta > 0U ?
                                     (uint32_t)(queue_age_time_delta / queue_age_samples_delta) : 0U;
    uint32_t average_pack_us = ppa_frames_delta > 0U ?
                                (uint32_t)(pack_time_delta / ppa_frames_delta) : 0U;
    uint32_t average_ppa_us = ppa_frames_delta > 0U ?
                               (uint32_t)(ppa_time_delta / ppa_frames_delta) : 0U;
    uint32_t average_swap_us = ppa_frames_delta > 0U ?
                                (uint32_t)(swap_time_delta / ppa_frames_delta) : 0U;
    uint32_t average_software_us = software_frames_delta > 0U ?
                                    (uint32_t)(software_time_delta / software_frames_delta) : 0U;
    uint32_t input_queue_depth = s_renderer.ready_slots != NULL ?
                                 (uint32_t)uxQueueMessagesWaiting(s_renderer.ready_slots) : 0U;
    uint32_t decoded_queue_depth = s_renderer.decoded_ready_slots != NULL ?
                                   (uint32_t)uxQueueMessagesWaiting(s_renderer.decoded_ready_slots) : 0U;

    ESP_LOGI(TAG,
             "H264 downlink stats: src=%ux%u rx=%u.%ufps/%ukbps queued=%u.%ufps "
             "decoded=%u.%ufps converted=%u.%ufps presented=%u.%ufps "
             "drop=input:%u display:%u fail=decode:%u convert:%u q=input:%u display:%u "
             "mode=%s decode_us=au:%u max:%u proc:%u key:%u delta:%u copy:%u/%u "
             "convert_us=%u/%u phases=%u/%u/%u sw:%u queue_age_us=%u/%u "
             "sync=create:%u restart:%u reset:%u overflow:%u ui_handoff_us=%u/%u",
             source_width,
             source_height,
             received_fps_x10 / 10U,
             received_fps_x10 % 10U,
             received_kbps,
             queued_fps_x10 / 10U,
             queued_fps_x10 % 10U,
             decoded_fps_x10 / 10U,
             decoded_fps_x10 % 10U,
             converted_fps_x10 / 10U,
             converted_fps_x10 % 10U,
             presented_fps_x10 / 10U,
             presented_fps_x10 % 10U,
             dropped_delta,
             conversion_dropped_delta,
             failure_delta,
             conversion_failure_delta,
             input_queue_depth,
             decoded_queue_depth,
             video_frame_converter_mode_name(video_frame_converter_get_mode(converter)),
             average_access_unit_us,
             current.decode_access_unit_max_us,
             average_decode_us,
             average_key_us,
             average_delta_us,
             average_copy_us,
             current.decode_copy_max_us,
             average_convert_us,
             current.convert_max_us,
             average_pack_us,
             average_ppa_us,
             average_swap_us,
             average_software_us,
             average_queue_age_us,
             current.input_queue_age_max_us,
             decoder_creations_delta,
             decoder_restarts_delta,
             discontinuities_delta,
             input_overflows_delta,
             average_present_copy_us,
             current.present_copy_max_us);
    *window = current;
#endif
}

static esp_err_t call_video_copy_display_i420(const uint8_t *source,
                                              uint16_t source_width,
                                              uint16_t source_height,
                                              uint8_t *output)
{
    uint16_t crop_x = CALL_VIDEO_RENDER_SOURCE_X;
    uint16_t crop_y = CALL_VIDEO_RENDER_SOURCE_Y;
    const uint16_t crop_width = CALL_VIDEO_RENDER_WIDTH;
    const uint16_t crop_height = CALL_VIDEO_RENDER_HEIGHT;

    ESP_RETURN_ON_FALSE(source != NULL && output != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid I420 crop buffer");
    if (source_width == crop_width && source_height == crop_height) {
        crop_x = 0U;
        crop_y = 0U;
    }
    ESP_RETURN_ON_FALSE(((crop_x | crop_y | crop_width | crop_height) & 1U) == 0U &&
                            (uint32_t)crop_x + crop_width <= source_width &&
                            (uint32_t)crop_y + crop_height <= source_height,
                        ESP_ERR_NOT_SUPPORTED,
                        TAG,
                        "decoded resolution does not contain display crop");

    const size_t source_luma_size = (size_t)source_width * source_height;
    const size_t source_chroma_stride = source_width / 2U;
    const uint8_t *source_y = source;
    const uint8_t *source_u = source_y + source_luma_size;
    const uint8_t *source_v = source_u + (source_luma_size / 4U);
    const size_t output_luma_size = (size_t)crop_width * crop_height;
    uint8_t *output_y = output;
    uint8_t *output_u = output_y + output_luma_size;
    uint8_t *output_v = output_u + (output_luma_size / 4U);

    if (crop_x == 0U && crop_width == source_width) {
        size_t chroma_copy_size = output_luma_size / 4U;
        size_t source_chroma_offset = (size_t)(crop_y / 2U) * source_chroma_stride;
        memcpy(output_y,
               source_y + ((size_t)crop_y * source_width),
               output_luma_size);
        memcpy(output_u, source_u + source_chroma_offset, chroma_copy_size);
        memcpy(output_v, source_v + source_chroma_offset, chroma_copy_size);
        return ESP_OK;
    }

    for (uint16_t row = 0; row < crop_height; ++row) {
        memcpy(output_y + ((size_t)row * crop_width),
               source_y + ((size_t)(crop_y + row) * source_width) + crop_x,
               crop_width);
    }
    for (uint16_t row = 0; row < crop_height / 2U; ++row) {
        size_t source_offset =
            ((size_t)((crop_y / 2U) + row) * source_chroma_stride) + (crop_x / 2U);
        size_t output_offset = (size_t)row * (crop_width / 2U);
        memcpy(output_u + output_offset, source_u + source_offset, crop_width / 2U);
        memcpy(output_v + output_offset, source_v + source_offset, crop_width / 2U);
    }
    return ESP_OK;
}

static esp_err_t call_video_queue_decoded_frame(const esp_h264_dec_out_frame_t *frame,
                                                 esp_h264_dec_param_sw_handle_t parameters,
                                                 uint32_t generation,
                                                 bool key_frame)
{
    esp_h264_resolution_t resolution = {0};
    uint8_t index = 0;

    ESP_RETURN_ON_FALSE(frame != NULL && frame->outbuf != NULL && frame->out_size > 0U,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid decoded frame");
    ESP_RETURN_ON_FALSE(esp_h264_dec_get_resolution(parameters, &resolution) == ESP_H264_ERR_OK,
                        ESP_FAIL,
                        TAG,
                        "read decoded resolution failed");
    size_t required = (size_t)resolution.width * resolution.height * 3U / 2U;
    ESP_RETURN_ON_FALSE(resolution.width >= 16U && resolution.height >= 16U &&
                            (resolution.width & 1U) == 0U && (resolution.height & 1U) == 0U &&
                            frame->out_size >= required,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "invalid I420 frame size");

    bool generation_matches = false;
    bool waiting_for_key_frame = false;
    taskENTER_CRITICAL(&s_renderer.lock);
    generation_matches = generation == s_renderer.generation;
    waiting_for_key_frame = s_renderer.waiting_for_key_frame;
    taskEXIT_CRITICAL(&s_renderer.lock);
    if (!generation_matches) {
        return ESP_ERR_INVALID_STATE;
    }
    if (waiting_for_key_frame && !key_frame) {
        return ESP_ERR_NOT_FINISHED;
    }

    if (xQueueReceive(s_renderer.decoded_free_slots, &index, 0) != pdTRUE) {
        /* The decoder must keep consuming every reference frame. If display
         * conversion falls behind, replace its oldest already-decoded frame. */
        if (xQueueReceive(s_renderer.decoded_ready_slots, &index, 0) != pdTRUE) {
            taskENTER_CRITICAL(&s_renderer.lock);
            s_renderer.conversion_dropped_frames++;
            taskEXIT_CRITICAL(&s_renderer.lock);
            return ESP_ERR_NOT_FINISHED;
        }
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.conversion_dropped_frames++;
        taskEXIT_CRITICAL(&s_renderer.lock);
    }

    call_video_decoded_slot_t *slot = &s_renderer.decoded_slots[index];
    int64_t copy_started_us = esp_timer_get_time();
    esp_err_t copy_ret = call_video_copy_display_i420(frame->outbuf,
                                                       resolution.width,
                                                       resolution.height,
                                                       slot->data);
    uint32_t copy_elapsed_us = (uint32_t)(esp_timer_get_time() - copy_started_us);
    if (copy_ret != ESP_OK) {
        call_video_return_decoded_slot(index);
        return copy_ret;
    }
    slot->data_len = CALL_VIDEO_DECODED_SLOT_CAPACITY;
    slot->width = CALL_VIDEO_RENDER_WIDTH;
    slot->height = CALL_VIDEO_RENDER_HEIGHT;
    slot->generation = generation;

    taskENTER_CRITICAL(&s_renderer.lock);
    generation_matches = generation == s_renderer.generation;
    s_renderer.decode_copy_time_us += copy_elapsed_us;
    if (copy_elapsed_us > s_renderer.decode_copy_max_us) {
        s_renderer.decode_copy_max_us = copy_elapsed_us;
    }
    taskEXIT_CRITICAL(&s_renderer.lock);
    if (!generation_matches) {
        call_video_return_decoded_slot(index);
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_renderer.decoded_ready_slots, &index, 0) != pdTRUE) {
        call_video_return_decoded_slot(index);
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.conversion_dropped_frames++;
        taskEXIT_CRITICAL(&s_renderer.lock);
        return ESP_ERR_NOT_FINISHED;
    }

    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.source_width = resolution.width;
    s_renderer.source_height = resolution.height;
    s_renderer.decoded_frames++;
    if (s_renderer.generation == generation) {
        s_renderer.waiting_for_key_frame = false;
    }
    taskEXIT_CRITICAL(&s_renderer.lock);
    return ESP_OK;
}

static esp_err_t call_video_decode_slot(esp_h264_dec_handle_t decoder,
                                        esp_h264_dec_param_sw_handle_t parameters,
                                        call_video_input_slot_t *slot)
{
    esp_h264_dec_in_frame_t input = {
        .raw_data = {
            .buffer = slot->data,
            .len = (uint32_t)slot->data_len,
        },
        .pts = slot->pts,
        .dts = slot->pts,
    };

    while (input.raw_data.len > 0U) {
        esp_h264_dec_out_frame_t output = {0};
        int64_t decode_started_us = esp_timer_get_time();
        esp_h264_err_t decode_ret = esp_h264_dec_process(decoder, &input, &output);
        uint32_t decode_elapsed_us = (uint32_t)(esp_timer_get_time() - decode_started_us);

        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.decode_process_calls++;
        s_renderer.decode_time_us += decode_elapsed_us;
        taskEXIT_CRITICAL(&s_renderer.lock);

        if (decode_ret != ESP_H264_ERR_OK || input.consume == 0U) {
            return ESP_FAIL;
        }
        input.raw_data.buffer += input.consume;
        input.raw_data.len -= input.consume;
        if (output.out_size > 0U) {
            esp_err_t queue_ret = call_video_queue_decoded_frame(&output,
                                                                  parameters,
                                                                  slot->generation,
                                                                  slot->key_frame);
            if (queue_ret == ESP_ERR_NOT_FINISHED) {
                /* Display pressure is not a decoder failure. The decoded
                 * reference state is already valid, so skip presentation and
                 * continue consuming the bitstream. */
                continue;
            }
            if (queue_ret != ESP_OK) {
                return queue_ret;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t call_video_output_begin_write(uint8_t *slot_index, uint16_t **pixels)
{
    ESP_RETURN_ON_FALSE(slot_index != NULL && pixels != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid output slot request");
    if (s_renderer.frame_mutex == NULL ||
        xSemaphoreTake(s_renderer.frame_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t selected = CALL_VIDEO_OUTPUT_SLOT_INVALID;
    for (uint8_t index = 0; index < CALL_VIDEO_OUTPUT_SLOT_COUNT; ++index) {
        if (s_renderer.output_slots[index].state == CALL_VIDEO_OUTPUT_FREE) {
            selected = index;
            break;
        }
    }
    if (selected == CALL_VIDEO_OUTPUT_SLOT_INVALID) {
        /* Bound latency when the renderer outruns the LCD: reuse the oldest
         * frame that has not been presented, never the frame in flight. */
        if (!call_video_output_pop_ready_locked(&selected) ||
            selected >= CALL_VIDEO_OUTPUT_SLOT_COUNT) {
            xSemaphoreGive(s_renderer.frame_mutex);
            return ESP_ERR_NOT_FOUND;
        }
        s_renderer.output_slots[selected].state = CALL_VIDEO_OUTPUT_FREE;
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.conversion_dropped_frames++;
        taskEXIT_CRITICAL(&s_renderer.lock);
    }

    s_renderer.output_slots[selected].state = CALL_VIDEO_OUTPUT_WRITING;
    *slot_index = selected;
    *pixels = s_renderer.output_slots[selected].pixels;
    xSemaphoreGive(s_renderer.frame_mutex);
    return ESP_OK;
}

static void call_video_output_finish_write(uint8_t slot_index,
                                           bool publish,
                                           uint32_t convert_elapsed_us)
{
    if (slot_index >= CALL_VIDEO_OUTPUT_SLOT_COUNT || s_renderer.frame_mutex == NULL ||
        xSemaphoreTake(s_renderer.frame_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    call_video_output_slot_t *slot = &s_renderer.output_slots[slot_index];
    if (!publish || slot->state != CALL_VIDEO_OUTPUT_WRITING) {
        slot->state = CALL_VIDEO_OUTPUT_FREE;
        xSemaphoreGive(s_renderer.frame_mutex);
        return;
    }

    if (s_renderer.ready_output_count >= CALL_VIDEO_OUTPUT_SLOT_COUNT) {
        uint8_t dropped_index = CALL_VIDEO_OUTPUT_SLOT_INVALID;
        if (call_video_output_pop_ready_locked(&dropped_index) &&
            dropped_index < CALL_VIDEO_OUTPUT_SLOT_COUNT) {
            s_renderer.output_slots[dropped_index].state = CALL_VIDEO_OUTPUT_FREE;
        }
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.conversion_dropped_frames++;
        taskEXIT_CRITICAL(&s_renderer.lock);
    }
    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.latest_sequence++;
    slot->sequence = s_renderer.latest_sequence;
    s_renderer.frame_ready = true;
    s_renderer.converted_frames++;
    s_renderer.convert_time_us += convert_elapsed_us;
    if (convert_elapsed_us > s_renderer.convert_max_us) {
        s_renderer.convert_max_us = convert_elapsed_us;
    }
    taskEXIT_CRITICAL(&s_renderer.lock);
    slot->state = CALL_VIDEO_OUTPUT_READY;
    call_video_output_push_ready_locked(slot_index);
    xSemaphoreGive(s_renderer.frame_mutex);
}

static void call_video_converter_task(void *arg)
{
    video_frame_converter_handle_t converter = (video_frame_converter_handle_t)arg;

    while (!call_video_stop_requested()) {
        uint8_t index = 0;
        if (xQueueReceive(s_renderer.decoded_ready_slots,
                          &index,
                          pdMS_TO_TICKS(50)) != pdTRUE) {
            continue;
        }

        if (index >= CALL_VIDEO_DECODED_SLOT_COUNT) {
            continue;
        }

        call_video_decoded_slot_t *slot = &s_renderer.decoded_slots[index];
        uint32_t generation = 0;
        taskENTER_CRITICAL(&s_renderer.lock);
        generation = s_renderer.generation;
        taskEXIT_CRITICAL(&s_renderer.lock);
        if (slot->generation != generation) {
            call_video_return_decoded_slot(index);
            continue;
        }

        uint8_t output_index = CALL_VIDEO_OUTPUT_SLOT_INVALID;
        uint16_t *output_pixels = NULL;
        esp_err_t output_ret = call_video_output_begin_write(&output_index, &output_pixels);
        if (output_ret != ESP_OK) {
            taskENTER_CRITICAL(&s_renderer.lock);
            s_renderer.conversion_dropped_frames++;
            taskEXIT_CRITICAL(&s_renderer.lock);
            call_video_return_decoded_slot(index);
            continue;
        }

        int64_t convert_started_us = esp_timer_get_time();
        esp_err_t convert_ret = video_frame_converter_i420_to_rgb565(converter,
                                                                     slot->data,
                                                                     slot->width,
                                                                     slot->height,
                                                                     output_pixels,
                                                                     NULL);
        uint32_t convert_elapsed_us =
            (uint32_t)(esp_timer_get_time() - convert_started_us);
        taskENTER_CRITICAL(&s_renderer.lock);
        generation = s_renderer.generation;
        taskEXIT_CRITICAL(&s_renderer.lock);
        bool generation_matches = slot->generation == generation;
        call_video_output_finish_write(output_index,
                                       convert_ret == ESP_OK && generation_matches,
                                       convert_elapsed_us);
        if (convert_ret != ESP_OK && generation_matches) {
            taskENTER_CRITICAL(&s_renderer.lock);
            s_renderer.conversion_failures++;
            taskEXIT_CRITICAL(&s_renderer.lock);
            ESP_LOGW(TAG,
                     "H264 downlink display conversion failed: %s",
                     esp_err_to_name(convert_ret));
        }
        call_video_return_decoded_slot(index);
    }

    call_video_drain_decoded_ready_queue();
    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.convert_task = NULL;
    taskEXIT_CRITICAL(&s_renderer.lock);
    xSemaphoreGive(s_renderer.convert_stop_done);
    vTaskDeleteWithCaps(NULL);
}

static void call_video_renderer_task(void *arg)
{
    (void)arg;
    esp_h264_dec_handle_t decoder = NULL;
    esp_h264_dec_param_sw_handle_t parameters = NULL;
    video_frame_converter_handle_t converter = NULL;
    call_video_log_window_t log_window = {0};
    const video_frame_converter_config_t converter_config = {
        .output_width = CALL_VIDEO_RENDER_WIDTH,
        .output_height = CALL_VIDEO_RENDER_HEIGHT,
        /* The decoder handoff already contains only the visible viewport. */
        .source_crop_x = 0,
        .source_crop_y = 0,
        .source_crop_width = CALL_VIDEO_RENDER_WIDTH,
        .source_crop_height = CALL_VIDEO_RENDER_HEIGHT,
        .output_rgb565_byte_swap = true,
    };

    /* Reserve the decoder's internal state before the call profile releases
     * the larger IPC encoder reference block. This keeps first-frame work out
     * of the RTC callback and prevents late allocations from fragmenting that
     * block during a profile transition. */
    bool dma_escrow_lent = false;
    esp_err_t startup_ret = call_video_decoder_dma_guard_begin(&dma_escrow_lent);
    if (startup_ret == ESP_OK) {
        startup_ret = call_video_decoder_create(&decoder, &parameters);
        if (startup_ret == ESP_OK) {
            taskENTER_CRITICAL(&s_renderer.lock);
            s_renderer.decoder_creations++;
            taskEXIT_CRITICAL(&s_renderer.lock);
        }
    }
    call_video_decoder_dma_guard_end(dma_escrow_lent, startup_ret);

    if (startup_ret == ESP_OK) {
        startup_ret = video_frame_converter_create(&converter_config, &converter);
    }
    if (startup_ret == ESP_OK) {
        BaseType_t convert_task_ret = xTaskCreatePinnedToCoreWithCaps(
            call_video_converter_task,
            "call_video_cvt",
            CALL_VIDEO_CONVERT_TASK_STACK_SIZE,
            converter,
            CALL_VIDEO_CONVERT_TASK_PRIORITY,
            &s_renderer.convert_task,
            APP_TASK_CORE_VIDEO_CONVERT,
            APP_TASK_STACK_CAPS_BACKGROUND);
        if (convert_task_ret != pdPASS) {
            startup_ret = ESP_ERR_NO_MEM;
        }
    }

    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.start_result = startup_ret;
    s_renderer.running = startup_ret == ESP_OK;
    s_renderer.start_pending = false;
    taskEXIT_CRITICAL(&s_renderer.lock);
    xSemaphoreGive(s_renderer.start_done);

    if (startup_ret != ESP_OK) {
        ESP_LOGE(TAG, "H264 downlink startup failed: %s", esp_err_to_name(startup_ret));
        goto task_exit;
    }

    ESP_LOGI(TAG,
             "H264 downlink renderer ready: decoder=dual-task conversion=pipelined-%s output=%ux%u "
             "source_crop=%ux%u+%u+%u input_slots=%u input_cap=%u decoded_slots=%u "
             "decoded_cap=%u presentation=fifo-%u priorities=rx:%u helper:%u convert:%u "
             "cores=decode:%d helper:%d convert:smp ui:%d camera:%d",
             video_frame_converter_mode_name(video_frame_converter_get_mode(converter)),
             CALL_VIDEO_RENDER_WIDTH,
             CALL_VIDEO_RENDER_HEIGHT,
             CALL_VIDEO_RENDER_WIDTH,
             CALL_VIDEO_RENDER_HEIGHT,
             CALL_VIDEO_RENDER_SOURCE_X,
             CALL_VIDEO_RENDER_SOURCE_Y,
             CALL_VIDEO_INPUT_SLOT_COUNT,
             CALL_VIDEO_INPUT_SLOT_CAPACITY,
             CALL_VIDEO_DECODED_SLOT_COUNT,
             CALL_VIDEO_DECODED_SLOT_CAPACITY,
             CALL_VIDEO_OUTPUT_SLOT_COUNT,
             CALL_VIDEO_TASK_PRIORITY,
             CONFIG_ESP_H264_DUAL_TASK_PRIORITY,
             CALL_VIDEO_CONVERT_TASK_PRIORITY,
             APP_TASK_CORE_VIDEO_DECODE,
             CONFIG_ESP_H264_DUAL_TASK_CORE,
             APP_TASK_CORE_UI,
             APP_TASK_CORE_CAMERA);

    while (!call_video_stop_requested()) {
        uint8_t index = 0;
        if (xQueueReceive(s_renderer.ready_slots, &index, pdMS_TO_TICKS(50)) != pdTRUE) {
            call_video_log_stats_if_due(&log_window, converter);
            continue;
        }
        if (index >= CALL_VIDEO_INPUT_SLOT_COUNT) {
            continue;
        }

        call_video_input_slot_t *slot = &s_renderer.slots[index];
        bool generation_matches = false;
        bool waiting_for_key_frame = false;
        taskENTER_CRITICAL(&s_renderer.lock);
        generation_matches = slot->generation == s_renderer.generation;
        waiting_for_key_frame = s_renderer.waiting_for_key_frame;
        taskEXIT_CRITICAL(&s_renderer.lock);

        if (!generation_matches || (waiting_for_key_frame && !slot->key_frame)) {
            call_video_return_slot(index);
            continue;
        }

        int64_t dequeued_at_us = esp_timer_get_time();
        uint32_t queue_age_us = slot->queued_at_us > 0 && dequeued_at_us > slot->queued_at_us ?
                                    (uint32_t)(dequeued_at_us - slot->queued_at_us) : 0U;
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.input_queue_age_samples++;
        s_renderer.input_queue_age_us += queue_age_us;
        if (queue_age_us > s_renderer.input_queue_age_max_us) {
            s_renderer.input_queue_age_max_us = queue_age_us;
        }
        taskEXIT_CRITICAL(&s_renderer.lock);

        bool dma_escrow_lent = false;
        esp_err_t decode_ret = ESP_OK;
        if (decoder == NULL) {
            decode_ret = call_video_decoder_dma_guard_begin(&dma_escrow_lent);
            if (decode_ret == ESP_OK) {
                decode_ret = call_video_decoder_create(&decoder, &parameters);
                if (decode_ret == ESP_OK) {
                    taskENTER_CRITICAL(&s_renderer.lock);
                    s_renderer.decoder_creations++;
                    taskEXIT_CRITICAL(&s_renderer.lock);
                }
            }
        }
        int64_t access_unit_started_us = esp_timer_get_time();
        if (decode_ret == ESP_OK) {
            decode_ret = call_video_decode_slot(decoder, parameters, slot);
        }
        uint32_t access_unit_elapsed_us =
            (uint32_t)(esp_timer_get_time() - access_unit_started_us);
        uint32_t slot_generation = slot->generation;
        bool slot_key_frame = slot->key_frame;
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.decode_access_units++;
        s_renderer.decode_access_unit_time_us += access_unit_elapsed_us;
        if (access_unit_elapsed_us > s_renderer.decode_access_unit_max_us) {
            s_renderer.decode_access_unit_max_us = access_unit_elapsed_us;
        }
        if (slot_key_frame) {
            s_renderer.decode_key_access_units++;
            s_renderer.decode_key_time_us += access_unit_elapsed_us;
        } else {
            s_renderer.decode_delta_access_units++;
            s_renderer.decode_delta_time_us += access_unit_elapsed_us;
        }
        bool generation_stale = slot_generation != s_renderer.generation;
        taskEXIT_CRITICAL(&s_renderer.lock);
        call_video_decoder_dma_guard_end(dma_escrow_lent, decode_ret);
        call_video_return_slot(index);
        if (generation_stale) {
            if (decoder != NULL) {
                taskENTER_CRITICAL(&s_renderer.lock);
                s_renderer.decoder_restarts++;
                taskEXIT_CRITICAL(&s_renderer.lock);
            }
            call_video_decoder_destroy(&decoder);
            parameters = NULL;
            call_video_log_stats_if_due(&log_window, converter);
            continue;
        }
        if (decode_ret != ESP_OK) {
            taskENTER_CRITICAL(&s_renderer.lock);
            s_renderer.decode_failures++;
            if (decoder != NULL) {
                s_renderer.decoder_restarts++;
            }
            taskEXIT_CRITICAL(&s_renderer.lock);
            ESP_LOGW(TAG, "H264 downlink decode lost sync: ret=%s", esp_err_to_name(decode_ret));
            call_video_mark_discontinuity();
            call_video_decoder_destroy(&decoder);
            parameters = NULL;
        }
        call_video_log_stats_if_due(&log_window, converter);
    }

task_exit:
    call_video_decoder_destroy(&decoder);
    parameters = NULL;
    call_video_drain_decoded_ready_queue();
    if (s_renderer.convert_task != NULL) {
        (void)xSemaphoreTake(s_renderer.convert_stop_done,
                             pdMS_TO_TICKS(CALL_VIDEO_STOP_TIMEOUT_MS));
    }
    video_frame_converter_destroy(converter);
    converter = NULL;
    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.running = false;
    s_renderer.task = NULL;
    taskEXIT_CRITICAL(&s_renderer.lock);
    xSemaphoreGive(s_renderer.stop_done);
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t call_video_prepare_output_pool(void)
{
    for (uint8_t index = 0; index < CALL_VIDEO_OUTPUT_SLOT_COUNT; ++index) {
        if (s_renderer.output_slots[index].pixels != NULL) {
            continue;
        }
        s_renderer.output_slots[index].pixels = heap_caps_aligned_calloc(
            CALL_VIDEO_CACHE_LINE_SIZE,
            1,
            CALL_VIDEO_FRAME_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT |
                MALLOC_CAP_CACHE_ALIGNED);
        if (s_renderer.output_slots[index].pixels == NULL) {
            return ESP_ERR_NO_MEM;
        }
        s_renderer.output_slots[index].state = CALL_VIDEO_OUTPUT_FREE;
    }
    return ESP_OK;
}

static void call_video_reset_output_pool_for_start(void)
{
    if (s_renderer.frame_mutex == NULL ||
        xSemaphoreTake(s_renderer.frame_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    call_video_output_release_ready_locked();
    for (uint8_t index = 0; index < CALL_VIDEO_OUTPUT_SLOT_COUNT; ++index) {
        if (index != s_renderer.presented_output_slot) {
            s_renderer.output_slots[index].state = CALL_VIDEO_OUTPUT_FREE;
        }
    }
    xSemaphoreGive(s_renderer.frame_mutex);
}

static void call_video_reset_session_queues(bool populate)
{
    if (s_renderer.free_slots != NULL) {
        (void)xQueueReset(s_renderer.free_slots);
    }
    if (s_renderer.ready_slots != NULL) {
        (void)xQueueReset(s_renderer.ready_slots);
    }
    if (s_renderer.decoded_free_slots != NULL) {
        (void)xQueueReset(s_renderer.decoded_free_slots);
    }
    if (s_renderer.decoded_ready_slots != NULL) {
        (void)xQueueReset(s_renderer.decoded_ready_slots);
    }

    for (uint8_t index = 0; index < CALL_VIDEO_INPUT_SLOT_COUNT; ++index) {
        uint8_t *data = s_renderer.slots[index].data;
        s_renderer.slots[index] = (call_video_input_slot_t) {
            .data = data,
        };
        if (populate && s_renderer.free_slots != NULL) {
            (void)xQueueSend(s_renderer.free_slots, &index, 0);
        }
    }
    for (uint8_t index = 0; index < CALL_VIDEO_DECODED_SLOT_COUNT; ++index) {
        uint8_t *data = s_renderer.decoded_slots[index].data;
        s_renderer.decoded_slots[index] = (call_video_decoded_slot_t) {
            .data = data,
        };
        if (populate && s_renderer.decoded_free_slots != NULL) {
            (void)xQueueSend(s_renderer.decoded_free_slots, &index, 0);
        }
    }
}

static esp_err_t call_video_prepare_persistent_resources(void)
{
    if (s_renderer.frame_mutex == NULL) {
        s_renderer.frame_mutex = xSemaphoreCreateMutexWithCaps(APP_SYNC_CAPS_CONTROL);
    }
    if (s_renderer.submit_mutex == NULL) {
        s_renderer.submit_mutex = xSemaphoreCreateMutexWithCaps(APP_SYNC_CAPS_CONTROL);
    }
    if (s_renderer.free_slots == NULL) {
        s_renderer.free_slots = xQueueCreateWithCaps(CALL_VIDEO_INPUT_SLOT_COUNT,
                                                     sizeof(uint8_t),
                                                     APP_QUEUE_CAPS_CONTROL);
    }
    if (s_renderer.ready_slots == NULL) {
        s_renderer.ready_slots = xQueueCreateWithCaps(CALL_VIDEO_INPUT_SLOT_COUNT,
                                                      sizeof(uint8_t),
                                                      APP_QUEUE_CAPS_CONTROL);
    }
    if (s_renderer.decoded_free_slots == NULL) {
        s_renderer.decoded_free_slots = xQueueCreateWithCaps(CALL_VIDEO_DECODED_SLOT_COUNT,
                                                             sizeof(uint8_t),
                                                             APP_QUEUE_CAPS_CONTROL);
    }
    if (s_renderer.decoded_ready_slots == NULL) {
        s_renderer.decoded_ready_slots = xQueueCreateWithCaps(CALL_VIDEO_DECODED_SLOT_COUNT,
                                                              sizeof(uint8_t),
                                                              APP_QUEUE_CAPS_CONTROL);
    }
    if (s_renderer.start_done == NULL) {
        s_renderer.start_done = xSemaphoreCreateBinaryWithCaps(APP_SYNC_CAPS_CONTROL);
    }
    if (s_renderer.stop_done == NULL) {
        s_renderer.stop_done = xSemaphoreCreateBinaryWithCaps(APP_SYNC_CAPS_CONTROL);
    }
    if (s_renderer.convert_stop_done == NULL) {
        s_renderer.convert_stop_done = xSemaphoreCreateBinaryWithCaps(APP_SYNC_CAPS_CONTROL);
    }
    if (s_renderer.free_slots == NULL || s_renderer.ready_slots == NULL ||
        s_renderer.decoded_free_slots == NULL || s_renderer.decoded_ready_slots == NULL ||
        s_renderer.frame_mutex == NULL || s_renderer.submit_mutex == NULL ||
        s_renderer.start_done == NULL ||
        s_renderer.stop_done == NULL || s_renderer.convert_stop_done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (uint8_t index = 0; index < CALL_VIDEO_INPUT_SLOT_COUNT; ++index) {
        if (s_renderer.slots[index].data == NULL) {
            s_renderer.slots[index].data = app_memory_alloc_psram(CALL_VIDEO_INPUT_SLOT_CAPACITY);
        }
        if (s_renderer.slots[index].data == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    for (uint8_t index = 0; index < CALL_VIDEO_DECODED_SLOT_COUNT; ++index) {
        if (s_renderer.decoded_slots[index].data == NULL) {
            s_renderer.decoded_slots[index].data =
                app_memory_alloc_psram(CALL_VIDEO_DECODED_SLOT_CAPACITY);
        }
        if (s_renderer.decoded_slots[index].data == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    return call_video_prepare_output_pool();
}

esp_err_t call_video_renderer_prewarm(void)
{
    bool already_ready = false;

    taskENTER_CRITICAL(&s_renderer.lock);
    already_ready = s_renderer.resources_ready;
    if (s_renderer.resources_preparing) {
        taskEXIT_CRITICAL(&s_renderer.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!already_ready) {
        s_renderer.resources_preparing = true;
    }
    taskEXIT_CRITICAL(&s_renderer.lock);

    if (already_ready) {
        return ESP_OK;
    }

    esp_err_t ret = call_video_prepare_persistent_resources();
    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.resources_preparing = false;
    s_renderer.resources_ready = ret == ESP_OK;
    taskEXIT_CRITICAL(&s_renderer.lock);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "H264 downlink PSRAM pools reserved: input=%u decoded=%u rgb=%u total=%u psram_free=%u psram_largest=%u internal_largest=%u",
                 (unsigned)(CALL_VIDEO_INPUT_SLOT_COUNT * CALL_VIDEO_INPUT_SLOT_CAPACITY),
                 (unsigned)(CALL_VIDEO_DECODED_SLOT_COUNT * CALL_VIDEO_DECODED_SLOT_CAPACITY),
                 (unsigned)(CALL_VIDEO_OUTPUT_SLOT_COUNT * CALL_VIDEO_FRAME_BYTES),
                 (unsigned)CALL_VIDEO_PSRAM_POOL_BYTES,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    return ret;
}

esp_err_t call_video_renderer_start(void)
{
    ESP_RETURN_ON_ERROR(call_video_renderer_prewarm(), TAG, "reserve H264 downlink pools failed");

    taskENTER_CRITICAL(&s_renderer.lock);
    if (s_renderer.running || s_renderer.start_pending) {
        taskEXIT_CRITICAL(&s_renderer.lock);
        return ESP_OK;
    }
    if (s_renderer.task != NULL) {
        taskEXIT_CRITICAL(&s_renderer.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_renderer.start_pending = true;
    s_renderer.stop_requested = false;
    s_renderer.waiting_for_key_frame = true;
    s_renderer.frame_ready = false;
    s_renderer.start_result = ESP_ERR_INVALID_STATE;
    s_renderer.generation++;
    s_renderer.source_width = 0;
    s_renderer.source_height = 0;
    s_renderer.received_frames = 0;
    s_renderer.received_bytes = 0;
    s_renderer.submitted_frames = 0;
    s_renderer.decoded_frames = 0;
    s_renderer.converted_frames = 0;
    s_renderer.conversion_dropped_frames = 0;
    s_renderer.conversion_failures = 0;
    s_renderer.dropped_frames = 0;
    s_renderer.decode_failures = 0;
    s_renderer.latest_sequence = 0;
    s_renderer.presented_frames = 0;
    s_renderer.decode_process_calls = 0;
    s_renderer.decode_time_us = 0;
    s_renderer.decode_access_units = 0;
    s_renderer.decode_access_unit_time_us = 0;
    s_renderer.decode_access_unit_max_us = 0;
    s_renderer.decode_key_access_units = 0;
    s_renderer.decode_key_time_us = 0;
    s_renderer.decode_delta_access_units = 0;
    s_renderer.decode_delta_time_us = 0;
    s_renderer.decode_copy_time_us = 0;
    s_renderer.decode_copy_max_us = 0;
    s_renderer.convert_time_us = 0;
    s_renderer.convert_max_us = 0;
    s_renderer.present_copy_time_us = 0;
    s_renderer.present_copy_max_us = 0;
    s_renderer.input_queue_age_samples = 0;
    s_renderer.input_queue_age_us = 0;
    s_renderer.input_queue_age_max_us = 0;
    s_renderer.decoder_creations = 0;
    s_renderer.decoder_restarts = 0;
    s_renderer.discontinuities = 0;
    s_renderer.input_overflows = 0;
    s_renderer.convert_task = NULL;
    taskEXIT_CRITICAL(&s_renderer.lock);

    call_video_reset_session_queues(true);
    call_video_drain_binary_semaphore(s_renderer.start_done);
    call_video_drain_binary_semaphore(s_renderer.stop_done);
    call_video_drain_binary_semaphore(s_renderer.convert_stop_done);
    call_video_reset_output_pool_for_start();

    /* TinyH264 performs CPU-intensive call/stack traffic in this task. Keep
     * its small stack internal; frame and queue payloads remain in PSRAM. */
    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(call_video_renderer_task,
                                                          "call_video_rx",
                                                          CALL_VIDEO_TASK_STACK_SIZE,
                                                          NULL,
                                                          CALL_VIDEO_TASK_PRIORITY,
                                                          &s_renderer.task,
                                                          APP_TASK_CORE_VIDEO_DECODE,
                                                          APP_TASK_STACK_CAPS_REALTIME);
    if (task_ret != pdPASS) {
        call_video_reset_session_queues(false);
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.start_pending = false;
        s_renderer.task = NULL;
        taskEXIT_CRITICAL(&s_renderer.lock);
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_renderer.start_done,
                       pdMS_TO_TICKS(CALL_VIDEO_START_TIMEOUT_MS)) != pdTRUE) {
        (void)call_video_renderer_stop();
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = ESP_OK;
    taskENTER_CRITICAL(&s_renderer.lock);
    ret = s_renderer.start_result;
    taskEXIT_CRITICAL(&s_renderer.lock);
    if (ret != ESP_OK) {
        (void)xSemaphoreTake(s_renderer.stop_done,
                             pdMS_TO_TICKS(CALL_VIDEO_STOP_TIMEOUT_MS));
        call_video_reset_session_queues(false);
    }
    return ret;
}

esp_err_t call_video_renderer_stop(void)
{
    TaskHandle_t task = NULL;
    bool submit_locked = false;
    bool frame_locked = false;

    taskENTER_CRITICAL(&s_renderer.lock);
    task = s_renderer.task;
    s_renderer.stop_requested = true;
    s_renderer.generation++;
    taskEXIT_CRITICAL(&s_renderer.lock);
    if (s_renderer.submit_mutex != NULL) {
        submit_locked = xSemaphoreTake(s_renderer.submit_mutex,
                                       pdMS_TO_TICKS(CALL_VIDEO_STOP_TIMEOUT_MS)) == pdTRUE;
        if (!submit_locked) {
            return ESP_ERR_TIMEOUT;
        }
    }
    call_video_drain_ready_queue();

    if (task != NULL && xSemaphoreTake(s_renderer.stop_done,
                                       pdMS_TO_TICKS(CALL_VIDEO_STOP_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "H264 downlink renderer stop timed out");
        if (submit_locked) {
            xSemaphoreGive(s_renderer.submit_mutex);
        }
        return ESP_ERR_TIMEOUT;
    }
    if (s_renderer.frame_mutex != NULL) {
        frame_locked = xSemaphoreTake(s_renderer.frame_mutex,
                                      pdMS_TO_TICKS(CALL_VIDEO_STOP_TIMEOUT_MS)) == pdTRUE;
        if (!frame_locked) {
            if (submit_locked) {
                xSemaphoreGive(s_renderer.submit_mutex);
            }
            return ESP_ERR_TIMEOUT;
        }
    }
    call_video_reset_session_queues(false);
    call_video_output_release_ready_locked();
    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.stop_requested = false;
    s_renderer.start_pending = false;
    s_renderer.running = false;
    s_renderer.frame_ready = false;
    taskEXIT_CRITICAL(&s_renderer.lock);
    if (frame_locked) {
        xSemaphoreGive(s_renderer.frame_mutex);
    }
    if (submit_locked) {
        xSemaphoreGive(s_renderer.submit_mutex);
    }
    return ESP_OK;
}

void call_video_renderer_flush(void)
{
    if (s_renderer.submit_mutex != NULL &&
        xSemaphoreTake(s_renderer.submit_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.generation++;
    s_renderer.waiting_for_key_frame = true;
    s_renderer.frame_ready = false;
    taskEXIT_CRITICAL(&s_renderer.lock);
    if (s_renderer.frame_mutex != NULL &&
        xSemaphoreTake(s_renderer.frame_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        call_video_output_release_ready_locked();
        xSemaphoreGive(s_renderer.frame_mutex);
    }
    call_video_drain_ready_queue();
    call_video_drain_decoded_ready_queue();
    if (s_renderer.submit_mutex != NULL) {
        xSemaphoreGive(s_renderer.submit_mutex);
    }
}

esp_err_t call_video_renderer_submit_h264(const uint8_t *data,
                                          size_t data_len,
                                          bool key_frame,
                                          uint32_t pts)
{
    uint8_t index = 0;
    bool running = false;
    bool waiting_for_key_frame = false;
    uint32_t generation = 0;

    ESP_RETURN_ON_FALSE(data != NULL && data_len > 0U, ESP_ERR_INVALID_ARG, TAG, "invalid H264 frame");
    if (s_renderer.submit_mutex == NULL ||
        xSemaphoreTake(s_renderer.submit_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    taskENTER_CRITICAL(&s_renderer.lock);
    running = s_renderer.running && !s_renderer.stop_requested;
    waiting_for_key_frame = s_renderer.waiting_for_key_frame;
    generation = s_renderer.generation;
    taskEXIT_CRITICAL(&s_renderer.lock);
    if (!running) {
        xSemaphoreGive(s_renderer.submit_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.received_frames++;
    s_renderer.received_bytes += data_len;
    taskEXIT_CRITICAL(&s_renderer.lock);
    if (data_len > CALL_VIDEO_INPUT_SLOT_CAPACITY) {
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.dropped_frames++;
        taskEXIT_CRITICAL(&s_renderer.lock);
        call_video_mark_discontinuity();
        xSemaphoreGive(s_renderer.submit_mutex);
        return ESP_ERR_INVALID_SIZE;
    }
    if (waiting_for_key_frame && !key_frame) {
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.dropped_frames++;
        taskEXIT_CRITICAL(&s_renderer.lock);
        xSemaphoreGive(s_renderer.submit_mutex);
        return ESP_ERR_NOT_FINISHED;
    }
    if (xQueueReceive(s_renderer.free_slots, &index, 0) != pdTRUE) {
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.dropped_frames++;
        s_renderer.input_overflows++;
        taskEXIT_CRITICAL(&s_renderer.lock);
        call_video_mark_discontinuity();
        xSemaphoreGive(s_renderer.submit_mutex);
        return ESP_ERR_TIMEOUT;
    }

    call_video_input_slot_t *slot = &s_renderer.slots[index];
    memcpy(slot->data, data, data_len);
    slot->data_len = data_len;
    slot->key_frame = key_frame;
    slot->pts = pts;
    slot->generation = generation;
    slot->queued_at_us = esp_timer_get_time();
    if (xQueueSend(s_renderer.ready_slots, &index, 0) != pdTRUE) {
        call_video_return_slot(index);
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.dropped_frames++;
        taskEXIT_CRITICAL(&s_renderer.lock);
        call_video_mark_discontinuity();
        xSemaphoreGive(s_renderer.submit_mutex);
        return ESP_ERR_TIMEOUT;
    }

    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.submitted_frames++;
    taskEXIT_CRITICAL(&s_renderer.lock);
    xSemaphoreGive(s_renderer.submit_mutex);
    return ESP_OK;
}

esp_err_t call_video_renderer_present_next_rgb565(const uint16_t **pixels,
                                                   size_t *pixel_count,
                                                   uint32_t *sequence)
{
    ESP_RETURN_ON_FALSE(pixels != NULL && pixel_count != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid video presentation request");
    if (s_renderer.frame_mutex == NULL ||
        xSemaphoreTake(s_renderer.frame_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    uint8_t ready_index = CALL_VIDEO_OUTPUT_SLOT_INVALID;
    if (!call_video_output_pop_ready_locked(&ready_index) ||
        ready_index >= CALL_VIDEO_OUTPUT_SLOT_COUNT) {
        xSemaphoreGive(s_renderer.frame_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    call_video_output_slot_t *ready = &s_renderer.output_slots[ready_index];
    if (sequence != NULL && *sequence == ready->sequence) {
        ready->state = CALL_VIDEO_OUTPUT_FREE;
        xSemaphoreGive(s_renderer.frame_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    int64_t handoff_started_us = esp_timer_get_time();
    if (s_renderer.presented_output_slot < CALL_VIDEO_OUTPUT_SLOT_COUNT) {
        s_renderer.output_slots[s_renderer.presented_output_slot].state = CALL_VIDEO_OUTPUT_FREE;
    }
    ready->state = CALL_VIDEO_OUTPUT_PRESENTED;
    s_renderer.presented_output_slot = ready_index;
    *pixels = ready->pixels;
    *pixel_count = CALL_VIDEO_FRAME_PIXELS;
    if (sequence != NULL) {
        *sequence = ready->sequence;
    }
    uint32_t handoff_elapsed_us = (uint32_t)(esp_timer_get_time() - handoff_started_us);
    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.presented_frames++;
    s_renderer.present_copy_time_us += handoff_elapsed_us;
    if (handoff_elapsed_us > s_renderer.present_copy_max_us) {
        s_renderer.present_copy_max_us = handoff_elapsed_us;
    }
    taskEXIT_CRITICAL(&s_renderer.lock);
    xSemaphoreGive(s_renderer.frame_mutex);
    return ESP_OK;
}

void call_video_renderer_release_presented_rgb565(void)
{
    if (s_renderer.frame_mutex == NULL ||
        xSemaphoreTake(s_renderer.frame_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    if (s_renderer.presented_output_slot < CALL_VIDEO_OUTPUT_SLOT_COUNT) {
        s_renderer.output_slots[s_renderer.presented_output_slot].state = CALL_VIDEO_OUTPUT_FREE;
        s_renderer.presented_output_slot = CALL_VIDEO_OUTPUT_SLOT_INVALID;
    }
    xSemaphoreGive(s_renderer.frame_mutex);
}

void call_video_renderer_get_stats(call_video_renderer_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    taskENTER_CRITICAL(&s_renderer.lock);
    stats->running = s_renderer.running;
    stats->waiting_for_key_frame = s_renderer.waiting_for_key_frame;
    stats->frame_ready = s_renderer.frame_ready;
    stats->source_width = s_renderer.source_width;
    stats->source_height = s_renderer.source_height;
    stats->submitted_frames = s_renderer.submitted_frames;
    stats->decoded_frames = s_renderer.decoded_frames;
    stats->dropped_frames = s_renderer.dropped_frames;
    stats->decode_failures = s_renderer.decode_failures;
    stats->latest_sequence = s_renderer.latest_sequence;
    taskEXIT_CRITICAL(&s_renderer.lock);
    stats->queue_depth = s_renderer.ready_slots != NULL ?
                         (uint32_t)uxQueueMessagesWaiting(s_renderer.ready_slots) : 0U;
}
