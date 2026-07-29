#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <driver/camera_pixel_format.h>
#include <driver/jpeg.h>
#include <driver/systick.h>
#include <FreeRTOS.h>
#include <os.h>
#include <os/thread.h>
#include <semphr.h>
#include <spinlock.h>
#include <task.h>
#include <opus.h>

#include "audio_cap.h"
#include "audio_core_service.h"
#include "audio_player_core.h"
#include "audio_player_session.h"
#include "tirtc_demo_media.h"
#include "tirtc_demo_sdk_gate.h"
#include "video_cap.h"
#include "video_core_service.h"

#define TIRTC_MEDIA_AUDIO_SERVICE "tirtc_audio"
#define TIRTC_MEDIA_VIDEO_SERVICE "tirtc_video"

#define TIRTC_MEDIA_AUDIO_TX_DEPTH 12U
#define TIRTC_MEDIA_AUDIO_RX_DEPTH 10U
#define TIRTC_MEDIA_AUDIO_PCM_BYTES 640U
#define TIRTC_MEDIA_AUDIO_RX_MAX 2048U
#define TIRTC_MEDIA_AUDIO_SEND_SOFT_LIMIT (384U * 1024U)
#define TIRTC_MEDIA_VIDEO_SEND_SOFT_LIMIT (320U * 1024U)
#define TIRTC_MEDIA_AUDIO_WORKER_STACK 24576U
#define TIRTC_MEDIA_VIDEO_WORKER_STACK 24576U
#define TIRTC_MEDIA_AUDIO_POLL_MS 5U
#define TIRTC_MEDIA_AUDIO_STACK_LOG_INTERVAL_MS 60000U
#define TIRTC_MEDIA_VIDEO_POLL_MS 20U
#define TIRTC_MEDIA_VIDEO_FRAME_INTERVAL_MS 200U
#define TIRTC_MEDIA_VIDEO_CALLBACK_DRAIN_MS 2000U
#define TIRTC_MEDIA_AUDIO_START_TIMEOUT_MS 1000U
#define TIRTC_MEDIA_VIDEO_SLOT_COUNT 2U
#define TIRTC_MEDIA_REMOTE_VIDEO_SLOT_COUNT 3U
#define TIRTC_MEDIA_PLAYER_RING_BYTES 12800U
#define TIRTC_MEDIA_PLAYER_START_BYTES 1280U
#define TIRTC_MEDIA_PLAYER_RESUME_BYTES 640U
#define TIRTC_MEDIA_PLAYER_LOW_BYTES 320U
#define TIRTC_MEDIA_JPEG_QUALITY 30
#define TIRTC_MEDIA_OPUS_SAMPLE_RATE 16000
#define TIRTC_MEDIA_OPUS_CHANNELS 1
#define TIRTC_MEDIA_OPUS_MAX_FRAME_SAMPLES 1920

#ifndef TIRTC_DEMO_MEDIA_ENABLE_VOXA_3A
#define TIRTC_DEMO_MEDIA_ENABLE_VOXA_3A 0
#endif

#ifndef TIRTC_DEMO_MEDIA_KEEP_UI_ACTIVE
#define TIRTC_DEMO_MEDIA_KEEP_UI_ACTIVE 1
#endif

#ifndef TIRTC_VIDEO_MJPEG
#define TIRTC_VIDEO_MJPEG TIRTC_VIDEO_JPEG
#endif

typedef struct {
    uint32_t generation;
    uint32_t timestamp_ms;
    uint16_t length;
    tirtc_demo_media_mode_t mode;
    uint8_t data[TIRTC_MEDIA_AUDIO_PCM_BYTES];
} tirtc_media_audio_tx_packet_t;

typedef struct {
    uint32_t generation;
    uint32_t timestamp_ms;
    uint16_t length;
    uint8_t stream_id;
    uint8_t media;
    uint8_t flags;
    uint8_t data[TIRTC_MEDIA_AUDIO_RX_MAX];
} tirtc_media_audio_rx_packet_t;

typedef enum {
    TIRTC_MEDIA_VIDEO_SLOT_FREE = 0,
    TIRTC_MEDIA_VIDEO_SLOT_READY,
    TIRTC_MEDIA_VIDEO_SLOT_PROCESSING,
} tirtc_media_video_slot_state_t;

typedef struct {
    void *data;
    size_t capacity;
    size_t length;
    uint16_t width;
    uint16_t height;
    uint32_t timestamp_ms;
    uint32_t generation;
    tirtc_media_video_slot_state_t state;
} tirtc_media_video_slot_t;

typedef enum {
    TIRTC_MEDIA_REMOTE_VIDEO_FREE = 0,
    TIRTC_MEDIA_REMOTE_VIDEO_WRITING,
    TIRTC_MEDIA_REMOTE_VIDEO_READY,
    TIRTC_MEDIA_REMOTE_VIDEO_DISPLAYING,
} tirtc_media_remote_video_state_t;

typedef struct {
    uint8_t *data;
    size_t length;
    uint32_t timestamp_ms;
    uint32_t generation;
    uint32_t sequence;
    tirtc_media_remote_video_state_t state;
} tirtc_media_remote_video_slot_t;

typedef struct {
    bool initialized;
    bool initializing;
    bool audio_registered;
    bool video_registered;
    bool audio_capture_started;
    bool video_capture_owned;
    bool uplink_enabled;
    bool video_enabled;
    bool force_key_frame;
    bool video_capture_logged;
    bool video_send_logged;
    bool video_format_error_logged;
    bool audio_capture_logged;
    bool audio_send_logged;
    bool audio_receive_logged;
    bool audio_playback_logged;
    bool audio_output_logged;
    bool audio_decode_error_logged;
    uint32_t generation;
    uint64_t next_video_capture_ms;
    uint64_t session_started_at_ms;
    uint64_t video_start_begin_ms;
    tirtc_conn_t connection;
    tirtc_demo_media_mode_t mode;
    thread_ptr_t audio_worker;
    thread_ptr_t video_worker;
    SemaphoreHandle_t control_mutex;
    jz_audio_player_handle_t player;
    OpusDecoder *opus_decoder;
    uint32_t opus_decoder_generation;
    tirtc_media_audio_tx_packet_t audio_tx[TIRTC_MEDIA_AUDIO_TX_DEPTH];
    uint8_t audio_tx_read;
    uint8_t audio_tx_write;
    uint8_t audio_tx_count;
    tirtc_media_audio_rx_packet_t audio_rx[TIRTC_MEDIA_AUDIO_RX_DEPTH];
    uint8_t audio_rx_read;
    uint8_t audio_rx_write;
    uint8_t audio_rx_count;
    uint8_t audio_uplink_alaw[TIRTC_MEDIA_AUDIO_PCM_BYTES / 4U];
    int16_t audio_playback_pcm[TIRTC_MEDIA_AUDIO_RX_MAX * 2U];
    tirtc_media_video_slot_t video_slots[TIRTC_MEDIA_VIDEO_SLOT_COUNT];
    tirtc_media_remote_video_slot_t
        remote_video_slots[TIRTC_MEDIA_REMOTE_VIDEO_SLOT_COUNT];
    uint32_t remote_video_sequence;
    uint8_t video_callbacks_inflight;
    uint8_t video_workers_inflight;
    struct jpege_encoder *jpeg_encoder;
    struct jpege_encoder_output *jpeg_output;
    size_t jpeg_output_capacity;
    uint16_t jpeg_width;
    uint16_t jpeg_height;
    tirtc_demo_media_stats_t stats;
} tirtc_media_runtime_t;

static DEFINE_SPINLOCK(g_tirtc_media_lock);
static tirtc_media_runtime_t g_media = {
    .player = JZ_AUDIO_PLAYER_INVALID_HANDLE,
};

static void media_lock(unsigned long *flags)
{
    spin_lock_irqsave(&g_tirtc_media_lock, *flags);
}

static void media_unlock(unsigned long flags)
{
    spin_unlock_irqrestore(&g_tirtc_media_lock, flags);
}

static bool media_control_lock(void)
{
    return g_media.control_mutex != NULL &&
           xSemaphoreTake(g_media.control_mutex, portMAX_DELAY) == pdTRUE;
}

static void media_control_unlock(void)
{
    if (g_media.control_mutex == NULL ||
        xSemaphoreGive(g_media.control_mutex) != pdTRUE) {
        printf("[tirtc_media] control mutex give rejected task=%s\n",
               pcTaskGetName(NULL));
    }
}

static bool media_should_log_count(uint32_t count)
{
    return count != 0U && (count & (count - 1U)) == 0U;
}

static unsigned int media_pcm_peak(const int16_t *pcm, size_t samples)
{
    unsigned int peak = 0U;

    if (pcm == NULL)
        return 0U;
    for (size_t i = 0U; i < samples; ++i) {
        int32_t sample = pcm[i];
        unsigned int level = sample < 0 ? (unsigned int)-sample :
                                          (unsigned int)sample;

        if (level > peak)
            peak = level;
    }
    return peak;
}

static void media_set_error(int error)
{
    unsigned long flags;

    media_lock(&flags);
    g_media.stats.last_error = error;
    media_unlock(flags);
}

static int16_t media_alaw_to_linear(uint8_t sample)
{
    int16_t value;
    uint8_t segment;

    sample ^= 0x55U;
    value = (int16_t)((sample & 0x0fU) << 4);
    segment = (uint8_t)((sample & 0x70U) >> 4);
    if (segment == 0U) {
        value += 8;
    } else if (segment == 1U) {
        value += 0x108;
    } else {
        value += 0x108;
        value = (int16_t)(value << (segment - 1U));
    }
    return (sample & 0x80U) != 0U ? value : (int16_t)-value;
}

static uint8_t media_linear_to_alaw(int16_t sample)
{
    static const int16_t segment_end[8] = {
        0x00ff, 0x01ff, 0x03ff, 0x07ff,
        0x0fff, 0x1fff, 0x3fff, 0x7fff,
    };
    int32_t magnitude = sample;
    uint8_t mask = 0xd5U;
    uint8_t segment = 0U;
    uint8_t alaw;

    if (magnitude < 0) {
        mask = 0x55U;
        magnitude = -magnitude - 1;
    }
    if (magnitude > 32767) {
        magnitude = 32767;
    }
    while (segment < 8U && magnitude > segment_end[segment]) {
        ++segment;
    }
    if (segment >= 8U) {
        return (uint8_t)(0x7fU ^ mask);
    }
    alaw = (uint8_t)(segment << 4);
    if (segment < 2U) {
        alaw |= (uint8_t)((magnitude >> 4) & 0x0f);
    } else {
        alaw |= (uint8_t)((magnitude >> (segment + 3U)) & 0x0f);
    }
    return (uint8_t)(alaw ^ mask);
}

static void media_audio_release(void *user_data)
{
    (void)user_data;
}

static void media_video_release(void *user_data)
{
    (void)user_data;
}

static void media_audio_capture(audio_frame_header_t *header, void *buffer, int size)
{
    tirtc_media_audio_tx_packet_t *packet;
    thread_ptr_t worker;
    const uint8_t *source;
    size_t source_size;
    unsigned long flags;
    bool log_first_capture = false;

    if (header == NULL || buffer == NULL || size <= 0 ||
        header->channels != 1U || header->sample_rate != 16000U ||
        header->bit_depth != 16U) {
        return;
    }

    source = (const uint8_t *)buffer;
    source_size = (size_t)size;
    if (header->multi_data != 0U && source_size >= 3U && source_size % 3U == 0U) {
        source_size /= 3U;
        source += source_size * 2U;
    }
    if (source_size != TIRTC_MEDIA_AUDIO_PCM_BYTES) {
        return;
    }

    media_lock(&flags);
    if (g_media.connection == NULL || !g_media.uplink_enabled ||
        g_media.audio_tx_count >= TIRTC_MEDIA_AUDIO_TX_DEPTH) {
        if (g_media.connection != NULL && g_media.uplink_enabled) {
            ++g_media.stats.tx_audio_dropped;
        }
        media_unlock(flags);
        return;
    }
    packet = &g_media.audio_tx[g_media.audio_tx_write];
    packet->generation = g_media.generation;
    packet->timestamp_ms = (uint32_t)(header->timestamp / 1000U);
    packet->length = TIRTC_MEDIA_AUDIO_PCM_BYTES;
    packet->mode = g_media.mode;
    memcpy(packet->data, source, TIRTC_MEDIA_AUDIO_PCM_BYTES);
    g_media.audio_tx_write = (uint8_t)((g_media.audio_tx_write + 1U) %
                                       TIRTC_MEDIA_AUDIO_TX_DEPTH);
    ++g_media.audio_tx_count;
    if (!g_media.audio_capture_logged) {
        g_media.audio_capture_logged = true;
        log_first_capture = true;
    }
    worker = g_media.audio_worker;
    media_unlock(flags);

    if (log_first_capture) {
        printf("[tirtc_media] first audio capture format=PCM/16K/S16/mono bytes=%lu peak=%u ts=%lu\n",
               (unsigned long)source_size,
               media_pcm_peak((const int16_t *)source,
                              source_size / sizeof(int16_t)),
               (unsigned long)(header->timestamp / 1000U));
    }

    if (worker != NULL) {
        thread_wakeup(worker);
    }
}

static void media_video_capture(jz_video_dist_data *data)
{
    tirtc_media_video_slot_t *slot = NULL;
    const ImgFrameHeader *header;
    const void *source;
    uint64_t now_ms;
    thread_ptr_t worker;
    unsigned long flags;
    bool log_first_capture = false;
    bool log_format_error = false;

    if (data == NULL || data->cam_buf == NULL) {
        return;
    }
    header = &data->cam_header;
    source = data->cam_buf;
    if (header->format != CAMERA_PIX_FMT_NV12 || header->width == 0U ||
        header->height == 0U || header->data_size == 0U) {
        media_lock(&flags);
        if (!g_media.video_format_error_logged) {
            g_media.video_format_error_logged = true;
            g_media.stats.last_error = -1;
            log_format_error = true;
        }
        media_unlock(flags);
        if (log_format_error) {
            printf("[tirtc_media] video frame rejected format=0x%08x size=%lu frame=%ux%u\n",
                   (unsigned)header->format,
                   (unsigned long)header->data_size,
                   (unsigned)header->width, (unsigned)header->height);
        }
        return;
    }
    now_ms = header->timestamp / 1000U;

    media_lock(&flags);
    if (g_media.connection == NULL || !g_media.video_enabled ||
        now_ms < g_media.next_video_capture_ms) {
        media_unlock(flags);
        return;
    }
    for (size_t i = 0; i < TIRTC_MEDIA_VIDEO_SLOT_COUNT; ++i) {
        if (g_media.video_slots[i].state == TIRTC_MEDIA_VIDEO_SLOT_FREE &&
            g_media.video_slots[i].capacity >= header->data_size) {
            slot = &g_media.video_slots[i];
            break;
        }
    }
    if (slot == NULL) {
        ++g_media.stats.tx_video_dropped;
        media_unlock(flags);
        return;
    }
    slot->state = TIRTC_MEDIA_VIDEO_SLOT_PROCESSING;
    ++g_media.video_callbacks_inflight;
    slot->generation = g_media.generation;
    g_media.next_video_capture_ms = now_ms + TIRTC_MEDIA_VIDEO_FRAME_INTERVAL_MS;
    if (!g_media.video_capture_logged) {
        g_media.video_capture_logged = true;
        log_first_capture = true;
    }
    media_unlock(flags);

    memcpy(slot->data, source, header->data_size);

    media_lock(&flags);
    slot->length = header->data_size;
    slot->width = header->width;
    slot->height = header->height;
    slot->timestamp_ms = (uint32_t)now_ms;
    slot->state = g_media.connection != NULL &&
                  slot->generation == g_media.generation
                      ? TIRTC_MEDIA_VIDEO_SLOT_READY
                      : TIRTC_MEDIA_VIDEO_SLOT_FREE;
    if (g_media.video_callbacks_inflight > 0U) {
        --g_media.video_callbacks_inflight;
    }
    worker = g_media.video_worker;
    media_unlock(flags);
    if (log_first_capture) {
        printf("[tirtc_media] first video capture format=NV12 raw=%lu frame=%ux%u ts=%lu\n",
               (unsigned long)header->data_size,
               (unsigned)header->width, (unsigned)header->height,
               (unsigned long)slot->timestamp_ms);
    }
    if (worker != NULL) {
        thread_wakeup(worker);
    }
}

static bool media_pop_audio_tx(tirtc_media_audio_tx_packet_t *packet)
{
    unsigned long flags;

    media_lock(&flags);
    if (g_media.audio_tx_count == 0U) {
        media_unlock(flags);
        return false;
    }
    *packet = g_media.audio_tx[g_media.audio_tx_read];
    g_media.audio_tx_read = (uint8_t)((g_media.audio_tx_read + 1U) %
                                      TIRTC_MEDIA_AUDIO_TX_DEPTH);
    --g_media.audio_tx_count;
    media_unlock(flags);
    return true;
}

static bool media_pop_audio_rx(tirtc_media_audio_rx_packet_t *packet)
{
    unsigned long flags;

    media_lock(&flags);
    if (g_media.audio_rx_count == 0U) {
        media_unlock(flags);
        return false;
    }
    *packet = g_media.audio_rx[g_media.audio_rx_read];
    g_media.audio_rx_read = (uint8_t)((g_media.audio_rx_read + 1U) %
                                      TIRTC_MEDIA_AUDIO_RX_DEPTH);
    --g_media.audio_rx_count;
    media_unlock(flags);
    return true;
}

static bool media_session_snapshot(uint32_t generation, tirtc_conn_t *connection,
                                   tirtc_demo_media_mode_t *mode)
{
    unsigned long flags;
    bool active;

    media_lock(&flags);
    active = generation == g_media.generation && g_media.connection != NULL;
    if (connection != NULL) {
        *connection = active ? g_media.connection : NULL;
    }
    if (mode != NULL) {
        *mode = g_media.mode;
    }
    media_unlock(flags);
    return active;
}

static void media_send_audio(const tirtc_media_audio_tx_packet_t *packet)
{
#ifdef CONFIG_TIRTC
    TIRTCFRAMEINFO frame = {0};
    tirtc_conn_t connection;
    const void *payload = packet->data;
    size_t payload_size = packet->length;
    size_t send_buffer = 0U;
    int result;
    bool log_first_send = false;

    if (!media_session_snapshot(packet->generation, &connection, NULL)) {
        return;
    }
    frame.stream_id = packet->mode == TIRTC_DEMO_MEDIA_AI ?
                          TIRTC_DEMO_AI_AUDIO_STREAM_ID :
                          TIRTC_DEMO_DEVICE_AUDIO_STREAM_ID;
    frame.media = TIRTC_AUDIO_PCM;
    frame.flags = TIRTC_AUDIOSAMPLE_16K16B1C;
    frame.ts = packet->timestamp_ms != 0U ? packet->timestamp_ms :
                                              (uint32_t)systick_get_time_ms();
    if (packet->mode == TIRTC_DEMO_MEDIA_MONITOR ||
        packet->mode == TIRTC_DEMO_MEDIA_WECHAT) {
        const int16_t *pcm = (const int16_t *)packet->data;
        size_t output_samples = packet->length / sizeof(int16_t) / 2U;

        for (size_t i = 0; i < output_samples; ++i)
            g_media.audio_uplink_alaw[i] = media_linear_to_alaw(pcm[i * 2U]);
        payload = g_media.audio_uplink_alaw;
        payload_size = output_samples;
        frame.media = TIRTC_AUDIO_ALAW;
        frame.flags = TIRTC_AUDIOSAMPLE_8K16B1C;
    }
    frame.length = (uint32_t)payload_size;
    if (!tirtc_demo_sdk_gate_take(TIRTC_DEMO_SDK_MEDIA_WAIT_MS)) {
        result = TIRTC_E_BUSY;
    } else {
        if (!media_session_snapshot(packet->generation, &connection, NULL)) {
            result = TIRTC_E_INVALID_HANDLE;
        } else {
            send_buffer = TiRtcGetSendBufferUsed(connection);
            result = send_buffer >= TIRTC_MEDIA_AUDIO_SEND_SOFT_LIMIT ?
                         TIRTC_E_BUSY :
                         TiRtcSendAudioStream(connection, &frame, payload);
        }
        tirtc_demo_sdk_gate_give();
    }

    {
        unsigned long flags;
        media_lock(&flags);
        g_media.stats.last_send_buffer_used = (uint32_t)send_buffer;
        if (result > 0) {
            ++g_media.stats.tx_audio_frames;
        } else {
            ++g_media.stats.tx_audio_dropped;
            g_media.stats.last_error = result;
        }
        if (!g_media.audio_send_logged) {
            g_media.audio_send_logged = true;
            log_first_send = true;
        }
        media_unlock(flags);
    }
    if (log_first_send) {
        printf("[tirtc_media] first audio send mode=%u media=%u flags=%u bytes=%lu result=%d buffer=%lu\n",
               (unsigned)packet->mode, (unsigned)frame.media,
               (unsigned)frame.flags,
               (unsigned long)payload_size, result,
               (unsigned long)send_buffer);
    }
#else
    (void)packet;
#endif
}

static size_t media_decode_remote_audio(const tirtc_media_audio_rx_packet_t *packet,
                                        int16_t *output, size_t output_samples,
                                        int *decode_error)
{
    int decoded;
    int opus_error = OPUS_OK;
    bool input_8k;
    bool mono;
    size_t input_samples;

    input_8k = packet->flags == TIRTC_AUDIOSAMPLE_8K16B1C;
    mono = input_8k || packet->flags == TIRTC_AUDIOSAMPLE_16K16B1C;
    if (decode_error != NULL) {
        *decode_error = OPUS_OK;
    }
    if (!mono) {
        return 0U;
    }
    if (packet->media == TIRTC_AUDIO_OPUS) {
        if (output_samples < TIRTC_MEDIA_OPUS_MAX_FRAME_SAMPLES) {
            return 0U;
        }
        if (g_media.opus_decoder == NULL) {
            g_media.opus_decoder = opus_decoder_create(
                TIRTC_MEDIA_OPUS_SAMPLE_RATE, TIRTC_MEDIA_OPUS_CHANNELS,
                &opus_error);
            if (g_media.opus_decoder == NULL || opus_error != OPUS_OK) {
                if (decode_error != NULL) {
                    *decode_error = opus_error;
                }
                return 0U;
            }
            printf("[tirtc_media] opus decoder ready rate=%d channels=%d state=%d bytes\n",
                   TIRTC_MEDIA_OPUS_SAMPLE_RATE, TIRTC_MEDIA_OPUS_CHANNELS,
                   opus_decoder_get_size(TIRTC_MEDIA_OPUS_CHANNELS));
        }
        if (g_media.opus_decoder_generation != packet->generation) {
            (void)opus_decoder_ctl(g_media.opus_decoder, OPUS_RESET_STATE);
            g_media.opus_decoder_generation = packet->generation;
        }
        decoded = opus_decode(g_media.opus_decoder, packet->data,
                              (opus_int32)packet->length, output,
                              TIRTC_MEDIA_OPUS_MAX_FRAME_SAMPLES, 0);
        if (decoded < 0) {
            if (decode_error != NULL) {
                *decode_error = decoded;
            }
            return 0U;
        }
        return (size_t)decoded;
    }

    if (packet->media == TIRTC_AUDIO_ALAW) {
        input_samples = packet->length;
        if ((!input_8k && input_samples > output_samples) ||
            (input_8k && input_samples > output_samples / 2U)) {
            return 0U;
        }
        for (size_t i = 0; i < input_samples; ++i) {
            int16_t sample = media_alaw_to_linear(packet->data[i]);
            if (input_8k) {
                output[i * 2U] = sample;
                output[i * 2U + 1U] = sample;
            } else {
                output[i] = sample;
            }
        }
        return input_samples * (input_8k ? 2U : 1U);
    }

    if (packet->media == TIRTC_AUDIO_PCM &&
        (packet->length & (sizeof(int16_t) - 1U)) == 0U) {
        const int16_t *input = (const int16_t *)packet->data;
        input_samples = packet->length / sizeof(int16_t);
        if ((!input_8k && input_samples > output_samples) ||
            (input_8k && input_samples > output_samples / 2U)) {
            return 0U;
        }
        if (input_8k) {
            for (size_t i = 0; i < input_samples; ++i) {
                output[i * 2U] = input[i];
                output[i * 2U + 1U] = input[i];
            }
            return input_samples * 2U;
        }
        memcpy(output, input, input_samples * sizeof(int16_t));
        return input_samples;
    }
    return 0U;
}

static void media_play_audio(const tirtc_media_audio_rx_packet_t *packet)
{
    int16_t *pcm = g_media.audio_playback_pcm;
    jz_audio_player_session_info_t session_info = {0};
    jz_audio_player_stream_buffer_info_t buffer_info = {0};
    jz_audio_player_handle_t player;
    size_t samples;
    int result;
    int decode_error = 0;
    int info_result = JZ_AUDIO_ERR_BAD_STATE;
    int buffer_result = JZ_AUDIO_ERR_BAD_STATE;
    unsigned long flags;
    bool log_first_playback = false;
    bool probe_output = false;
    bool log_output = false;
    bool log_decode_error = false;

    if (!media_session_snapshot(packet->generation, NULL, NULL)) {
        return;
    }
    samples = media_decode_remote_audio(
        packet, pcm, TIRTC_MEDIA_AUDIO_RX_MAX * 2U, &decode_error);
    if (samples == 0U) {
        media_lock(&flags);
        ++g_media.stats.rx_audio_dropped;
        if (!g_media.audio_decode_error_logged) {
            g_media.audio_decode_error_logged = true;
            log_decode_error = true;
        }
        media_unlock(flags);
        if (log_decode_error) {
            printf("[tirtc_media] remote audio decode rejected stream=%u media=%u flags=%u bytes=%u error=%d\n",
                   (unsigned)packet->stream_id, (unsigned)packet->media,
                   (unsigned)packet->flags, (unsigned)packet->length,
                   decode_error);
        }
        return;
    }

    if (!media_control_lock()) {
        media_set_error(JZ_AUDIO_ERR_BAD_STATE);
        return;
    }
    media_lock(&flags);
    player = g_media.player;
    probe_output = !g_media.audio_output_logged;
    media_unlock(flags);
    result = player != JZ_AUDIO_PLAYER_INVALID_HANDLE ?
                 jz_audio_player_session_write(player, pcm,
                                               (uint32_t)(samples * sizeof(int16_t)),
                                               0U) : JZ_AUDIO_ERR_BAD_STATE;
    if (result == JZ_AUDIO_OK && probe_output) {
        session_info.size = sizeof(session_info);
        buffer_info.size = sizeof(buffer_info);
        info_result = jz_audio_player_session_get_info(player, &session_info);
        buffer_result = jz_audio_player_session_get_stream_buffer_info(
            player, &buffer_info);
    }
    media_control_unlock();

    media_lock(&flags);
    if (result == JZ_AUDIO_OK) {
        ++g_media.stats.rx_audio_frames;
    } else {
        ++g_media.stats.rx_audio_dropped;
        g_media.stats.last_error = result;
    }
    if (!g_media.audio_playback_logged) {
        g_media.audio_playback_logged = true;
        log_first_playback = true;
    }
    if (probe_output && info_result == JZ_AUDIO_OK &&
        session_info.bytes_out > 0U &&
        packet->generation == g_media.generation &&
        !g_media.audio_output_logged) {
        g_media.audio_output_logged = true;
        log_output = true;
    }
    media_unlock(flags);
    if (log_first_playback) {
        printf("[tirtc_media] first audio playback stream=%u media=%u flags=%u input=%u decoded_samples=%lu peak=%u result=%d\n",
               (unsigned)packet->stream_id, (unsigned)packet->media,
               (unsigned)packet->flags, (unsigned)packet->length,
               (unsigned long)samples, media_pcm_peak(pcm, samples), result);
    }
    if (log_output) {
        printf("[tirtc_media] remote audio output committed in=%llu out=%llu dropped=%llu buffer=%lu/%lu underflow=%lu overflow=%lu buffer_result=%d\n",
               (unsigned long long)session_info.bytes_in,
               (unsigned long long)session_info.bytes_out,
               (unsigned long long)session_info.dropped_bytes,
               (unsigned long)buffer_info.used_bytes,
               (unsigned long)buffer_info.capacity_bytes,
               (unsigned long)buffer_info.underflow_count,
               (unsigned long)buffer_info.overflow_count, buffer_result);
    }
}

static void media_audio_worker(void *data)
{
    tirtc_media_audio_tx_packet_t tx_packet;
    tirtc_media_audio_rx_packet_t rx_packet;
    uint64_t next_stack_log_ms = systick_get_time_ms() +
                                 TIRTC_MEDIA_AUDIO_STACK_LOG_INTERVAL_MS;
    uint16_t stack_poll_count = 0U;

    (void)data;
    for (;;) {
        unsigned int processed = 0U;

        while (processed < TIRTC_MEDIA_AUDIO_TX_DEPTH &&
               media_pop_audio_tx(&tx_packet)) {
            media_send_audio(&tx_packet);
            ++processed;
        }
        processed = 0U;
        while (processed < TIRTC_MEDIA_AUDIO_RX_DEPTH &&
               media_pop_audio_rx(&rx_packet)) {
            media_play_audio(&rx_packet);
            ++processed;
        }
        if (++stack_poll_count == 256U) {
            uint64_t now_ms = systick_get_time_ms();

            stack_poll_count = 0U;
            if (now_ms >= next_stack_log_ms) {
                UBaseType_t free_words = uxTaskGetStackHighWaterMark(NULL);

                printf("[tirtc_media] audio worker stack free_min=%lu bytes configured=%u\n",
                       (unsigned long)(free_words * sizeof(StackType_t)),
                       TIRTC_MEDIA_AUDIO_WORKER_STACK);
                next_stack_log_ms = now_ms +
                                    TIRTC_MEDIA_AUDIO_STACK_LOG_INTERVAL_MS;
            }
        }
        (void)thread_wait_timeout(TIRTC_MEDIA_AUDIO_POLL_MS);
    }
}

static tirtc_media_video_slot_t *media_take_video_slot(void)
{
    tirtc_media_video_slot_t *slot = NULL;
    unsigned long flags;

    media_lock(&flags);
    for (size_t i = 0; i < TIRTC_MEDIA_VIDEO_SLOT_COUNT; ++i) {
        if (g_media.video_slots[i].state == TIRTC_MEDIA_VIDEO_SLOT_READY) {
            slot = &g_media.video_slots[i];
            slot->state = TIRTC_MEDIA_VIDEO_SLOT_PROCESSING;
            ++g_media.video_workers_inflight;
            break;
        }
    }
    media_unlock(flags);
    return slot;
}

static void media_release_video_slot(tirtc_media_video_slot_t *slot)
{
    unsigned long flags;

    media_lock(&flags);
    slot->state = TIRTC_MEDIA_VIDEO_SLOT_FREE;
    if (g_media.video_workers_inflight > 0U) {
        --g_media.video_workers_inflight;
    }
    media_unlock(flags);
}

static int media_prepare_jpeg(uint16_t width, uint16_t height)
{
    struct jpege_encoder_param params = {
        .width = width,
        .height = height,
        .mode = JPEG_MODE_AUTO,
        .reset = JPEG_RESET_AUTO,
        .linkage = JPEG_WITHOUT_SCALER,
        .in_fmt = JPEGE_PIX_FMT_NV12,
        .quality = TIRTC_MEDIA_JPEG_QUALITY,
    };

    if (g_media.jpeg_encoder != NULL && g_media.jpeg_output != NULL &&
        g_media.jpeg_width == width && g_media.jpeg_height == height) {
        return 0;
    }
    if (g_media.jpeg_output != NULL) {
        jpege_encoder_free_output_buf(g_media.jpeg_output);
        g_media.jpeg_output = NULL;
    }
    if (g_media.jpeg_encoder != NULL) {
        jpege_encoder_deinit(g_media.jpeg_encoder);
        g_media.jpeg_encoder = NULL;
    }
    g_media.jpeg_encoder = jpege_encoder_init(&params);
    if (g_media.jpeg_encoder == NULL) {
        return -1;
    }
    g_media.jpeg_output = jpege_encoder_alloc_output_buf(g_media.jpeg_encoder);
    if (g_media.jpeg_output == NULL) {
        jpege_encoder_deinit(g_media.jpeg_encoder);
        g_media.jpeg_encoder = NULL;
        return -1;
    }
    g_media.jpeg_output_capacity = g_media.jpeg_output->data_size;
    g_media.jpeg_width = width;
    g_media.jpeg_height = height;
    printf("[tirtc_media] jpeg ready frame=%ux%u quality=%d output_capacity=%lu\n",
           (unsigned)width, (unsigned)height, TIRTC_MEDIA_JPEG_QUALITY,
           (unsigned long)g_media.jpeg_output_capacity);
    return 0;
}

static void media_send_video(tirtc_media_video_slot_t *slot)
{
#if defined(CONFIG_TIRTC) && TIRTC_DEMO_MEDIA_ENABLE_VIDEO
    TIRTCFRAMEINFO frame = {0};
    tirtc_conn_t connection;
    size_t send_buffer = 0U;
    size_t jpeg_size = 0U;
    uint64_t encode_begin_ms;
    uint32_t encode_elapsed_ms;
    uint32_t dropped_count = 0U;
    uint64_t send_done_ms;
    uint64_t session_started_at_ms = 0U;
    uint64_t video_start_begin_ms = 0U;
    int result;
    unsigned long flags;
    bool log_first_send = false;
    bool log_failure = false;

    if (!media_session_snapshot(slot->generation, &connection, NULL)) {
        return;
    }
    if (!media_control_lock()) {
        media_set_error(-1);
        return;
    }
    if (!media_session_snapshot(slot->generation, &connection, NULL) ||
        media_prepare_jpeg(slot->width, slot->height) != 0) {
        media_control_unlock();
        media_set_error(-1);
        return;
    }
    g_media.jpeg_output->data_size = g_media.jpeg_output_capacity;
    encode_begin_ms = systick_get_time_ms();
    result = jpege_encoder_encode(g_media.jpeg_encoder, slot->data,
                                  (int)slot->length, g_media.jpeg_output);
    encode_elapsed_ms = (uint32_t)(systick_get_time_ms() - encode_begin_ms);
    if (result == 0 && g_media.jpeg_output->data_size > 0) {
        jpeg_size = (size_t)g_media.jpeg_output->data_size;
        frame.stream_id = TIRTC_DEMO_DEVICE_VIDEO_STREAM_ID;
        frame.media = TIRTC_VIDEO_JPEG;
        frame.flags = TIRTC_FRAME_FLAG_KEY_FRAME;
        frame.ts = slot->timestamp_ms;
        frame.length = (uint32_t)g_media.jpeg_output->data_size;
        if (!tirtc_demo_sdk_gate_take(TIRTC_DEMO_SDK_MEDIA_WAIT_MS)) {
            result = TIRTC_E_BUSY;
        } else {
            if (!media_session_snapshot(slot->generation, &connection, NULL)) {
                result = TIRTC_E_INVALID_HANDLE;
            } else {
                send_buffer = TiRtcGetSendBufferUsed(connection);
                result = send_buffer >= TIRTC_MEDIA_VIDEO_SEND_SOFT_LIMIT ?
                             TIRTC_E_BUSY :
                             TiRtcSendVideoStream(connection, &frame,
                                                  g_media.jpeg_output->data);
            }
            tirtc_demo_sdk_gate_give();
        }
    }
    media_control_unlock();
    send_done_ms = systick_get_time_ms();

    media_lock(&flags);
    g_media.force_key_frame = false;
    g_media.stats.last_send_buffer_used = (uint32_t)send_buffer;
    if (result > 0) {
        ++g_media.stats.tx_video_frames;
        if (!g_media.video_send_logged) {
            g_media.video_send_logged = true;
            log_first_send = true;
            session_started_at_ms = g_media.session_started_at_ms;
            video_start_begin_ms = g_media.video_start_begin_ms;
        }
    } else {
        ++g_media.stats.tx_video_dropped;
        g_media.stats.last_error = result;
        dropped_count = g_media.stats.tx_video_dropped;
        log_failure = media_should_log_count(dropped_count);
    }
    media_unlock(flags);
    if (log_first_send) {
        unsigned long video_to_first_ms = video_start_begin_ms != 0U &&
                                                  send_done_ms >= video_start_begin_ms ?
                                              (unsigned long)(send_done_ms -
                                                              video_start_begin_ms) : 0UL;
        unsigned long session_to_first_ms = session_started_at_ms != 0U &&
                                                    send_done_ms >= session_started_at_ms ?
                                                (unsigned long)(send_done_ms -
                                                                session_started_at_ms) : 0UL;

        printf("[tirtc_media] first JPEG sent raw=%lu jpeg=%lu encode=%lums send=%d buffer=%lu video_to_first=%lums session_to_first=%lums\n",
               (unsigned long)slot->length, (unsigned long)jpeg_size,
               (unsigned long)encode_elapsed_ms, result,
               (unsigned long)send_buffer, video_to_first_ms,
               session_to_first_ms);
    } else if (log_failure) {
        printf("[tirtc_media] JPEG send failed result=%d raw=%lu jpeg=%lu encode=%lums dropped=%lu\n",
               result, (unsigned long)slot->length,
               (unsigned long)jpeg_size,
               (unsigned long)encode_elapsed_ms,
               (unsigned long)dropped_count);
    }
#else
    (void)slot;
#endif
}

static void media_video_worker(void *data)
{
    (void)data;
    for (;;) {
        tirtc_media_video_slot_t *slot = media_take_video_slot();

        if (slot != NULL) {
            media_send_video(slot);
            media_release_video_slot(slot);
            continue;
        }
        (void)thread_wait_timeout(TIRTC_MEDIA_VIDEO_POLL_MS);
    }
}

static int media_open_player(void)
{
    jz_audio_player_session_stream_open_param_t stream;
    jz_audio_player_core_cfg_t core;
    jz_audio_pcm_format_t render_format = {0};
    jz_audio_player_handle_t handle = JZ_AUDIO_PLAYER_INVALID_HANDLE;
    int result;
    int format_result;
    bool core_reused;
    unsigned long flags;

    if (!media_control_lock()) {
        return JZ_AUDIO_ERR_BAD_STATE;
    }
    core_reused = jz_audio_player_core_is_initialized();
    if (!core_reused) {
        jz_audio_player_core_fill_default_cfg(&core);
        core.flags |= JZ_AUDIO_PLAYER_CORE_F_ALLOW_HEAP |
                      JZ_AUDIO_PLAYER_CORE_F_ENABLE_STREAM;
        core.use_aic_path = true;
        core.render_format.sample_rate = 16000U;
        core.render_format.channels = 1U;
        core.render_format.bits_per_sample = 16U;
        result = jz_audio_player_core_init(&core);
        if (result != JZ_AUDIO_OK) {
            media_control_unlock();
            return result;
        }
    }

    memset(&stream, 0, sizeof(stream));
    stream.size = sizeof(stream);
    stream.name = "tirtc_remote_audio";
    stream.input_format.sample_rate = 16000U;
    stream.input_format.channels = 1U;
    stream.input_format.bits_per_sample = 16U;
    stream.target_format = stream.input_format;
    stream.ringbuf_size = TIRTC_MEDIA_PLAYER_RING_BYTES;
    stream.start_threshold = TIRTC_MEDIA_PLAYER_START_BYTES;
    stream.resume_threshold = TIRTC_MEDIA_PLAYER_RESUME_BYTES;
    stream.low_watermark = TIRTC_MEDIA_PLAYER_LOW_BYTES;
    stream.overflow_policy = JZ_AUDIO_PLAYER_OVERFLOW_DROP_OLD;
    stream.underflow_policy = JZ_AUDIO_PLAYER_UNDERFLOW_SILENCE;
    stream.common.size = sizeof(stream.common);
    stream.common.priority = JZ_AUDIO_PLAYER_PRIORITY_HIGH;
    stream.common.render_mode = JZ_AUDIO_PLAYER_RENDER_EXCLUSIVE;
    stream.common.resume_policy = JZ_AUDIO_PLAYER_RESUME_BUFFER;
    stream.common.format_policy = JZ_AUDIO_PLAYER_FMT_POLICY_STRICT;
    stream.common.rate_policy = JZ_AUDIO_PLAYER_RATE_POLICY_STRICT;
    stream.common.channel_policy = JZ_AUDIO_PLAYER_CHANNEL_POLICY_STRICT;
    stream.common.flags = JZ_AUDIO_PLAYER_SESSION_F_LOW_LATENCY |
                          JZ_AUDIO_PLAYER_SESSION_F_FLUSH_ON_STOP;

    result = jz_audio_player_session_open_stream(&stream, NULL, NULL, &handle);
    if (result == JZ_AUDIO_OK) {
        result = jz_audio_player_session_start(handle);
    }
    if (result != JZ_AUDIO_OK && handle != JZ_AUDIO_PLAYER_INVALID_HANDLE) {
        (void)jz_audio_player_session_close(handle);
        handle = JZ_AUDIO_PLAYER_INVALID_HANDLE;
    }
    media_lock(&flags);
    g_media.player = handle;
    g_media.stats.playback_ready = result == JZ_AUDIO_OK;
    media_unlock(flags);
    format_result = jz_audio_player_core_get_render_format(&render_format);
    media_control_unlock();
    if (result == JZ_AUDIO_OK) {
        printf("[tirtc_media] remote playback ready core_reused=%d aic_requested=%d format=%lu/%u/%u format_result=%d handle=%lu\n",
               core_reused ? 1 : 0, core_reused ? 0 : 1,
               (unsigned long)render_format.sample_rate,
               (unsigned)render_format.bits_per_sample,
               (unsigned)render_format.channels, format_result,
               (unsigned long)handle);
    }
    return result;
}

static void media_close_player(void)
{
    jz_audio_player_handle_t handle;
    unsigned long flags;

    if (!media_control_lock()) {
        return;
    }
    media_lock(&flags);
    handle = g_media.player;
    g_media.player = JZ_AUDIO_PLAYER_INVALID_HANDLE;
    g_media.stats.playback_ready = false;
    media_unlock(flags);
    if (handle != JZ_AUDIO_PLAYER_INVALID_HANDLE) {
        (void)jz_audio_player_session_stop(handle, true);
        (void)jz_audio_player_session_close(handle);
    }
    media_control_unlock();
}

static void media_free_video_buffers(void)
{
    if (!media_control_lock()) {
        printf("[tirtc_media] video buffer release skipped: control lock unavailable\n");
        return;
    }
    if (g_media.jpeg_output != NULL) {
        jpege_encoder_free_output_buf(g_media.jpeg_output);
        g_media.jpeg_output = NULL;
        g_media.jpeg_output_capacity = 0U;
    }
    if (g_media.jpeg_encoder != NULL) {
        jpege_encoder_deinit(g_media.jpeg_encoder);
        g_media.jpeg_encoder = NULL;
    }
    g_media.jpeg_width = 0U;
    g_media.jpeg_height = 0U;
    for (size_t i = 0; i < TIRTC_MEDIA_VIDEO_SLOT_COUNT; ++i) {
        free(g_media.video_slots[i].data);
        memset(&g_media.video_slots[i], 0, sizeof(g_media.video_slots[i]));
    }
    media_control_unlock();
}

static int media_prepare_remote_video_buffers(void)
{
    int result = 0;

    if (!media_control_lock()) {
        return -1;
    }
    for (size_t i = 0U; i < TIRTC_MEDIA_REMOTE_VIDEO_SLOT_COUNT; ++i) {
        if (g_media.remote_video_slots[i].data == NULL) {
            g_media.remote_video_slots[i].data =
                memalign(64U, TIRTC_DEMO_REMOTE_VIDEO_MAX_BYTES);
            if (g_media.remote_video_slots[i].data == NULL) {
                result = -1;
                break;
            }
        }
    }
    media_control_unlock();
    return result;
}

static int media_start_video_capture(void)
{
#if TIRTC_DEMO_MEDIA_ENABLE_VIDEO
    struct camera_info *info;
    bool buffers_present = false;
    uint8_t callbacks_inflight;
    uint8_t workers_inflight;
    unsigned long flags;
    size_t frame_size;
    uint64_t video_start_begin_ms;
    int result;

    if (g_media.video_capture_owned) {
        (void)jz_video_service_set_status(TIRTC_MEDIA_VIDEO_SERVICE, 1);
        return 0;
    }
    media_lock(&flags);
    callbacks_inflight = g_media.video_callbacks_inflight;
    workers_inflight = g_media.video_workers_inflight;
    for (size_t i = 0; i < TIRTC_MEDIA_VIDEO_SLOT_COUNT; ++i) {
        buffers_present |= g_media.video_slots[i].data != NULL;
    }
    media_unlock(flags);
    if (buffers_present) {
        if (callbacks_inflight != 0U || workers_inflight != 0U) {
            media_set_error(-1);
            return -1;
        }
        media_free_video_buffers();
    }
    result = video_cap_set_display_mode_control(
        VIDEO_CAP_INDEX, TIRTC_DEMO_MEDIA_KEEP_UI_ACTIVE ? 0 : 1);
    if (result != 0) {
        media_set_error(result);
        printf("[tirtc_media] camera display policy unavailable: %d\n", result);
        return result;
    }
    video_start_begin_ms = systick_get_time_ms();
    media_lock(&flags);
    g_media.video_start_begin_ms = video_start_begin_ms;
    media_unlock(flags);
    printf("[tirtc_media] video start begin cam=%d keep_ui=%d\n",
           VIDEO_CAP_INDEX, TIRTC_DEMO_MEDIA_KEEP_UI_ACTIVE);
    result = video_cap_start(VIDEO_CAP_INDEX);
    if (result != 0) {
        media_set_error(result);
        return result;
    }
    info = video_cap_get_cam_info(VIDEO_CAP_INDEX);
    if (info == NULL || info->width <= 0 || info->height <= 0 ||
        info->data_fmt != CAMERA_PIX_FMT_NV12) {
        printf("[tirtc_media] video format unavailable info=%p format=0x%08x\n",
               (void *)info, info != NULL ? (unsigned)info->data_fmt : 0U);
        (void)video_cap_stop(VIDEO_CAP_INDEX);
        media_set_error(-1);
        return -1;
    }
    frame_size = (size_t)info->width * (size_t)info->height * 3U / 2U;
    if ((size_t)info->frame_size != frame_size) {
        printf("[tirtc_media] video size mismatch expected=%lu actual=%u\n",
               (unsigned long)frame_size, info->frame_size);
        (void)video_cap_stop(VIDEO_CAP_INDEX);
        media_set_error(-1);
        return -1;
    }
    for (size_t i = 0; i < TIRTC_MEDIA_VIDEO_SLOT_COUNT; ++i) {
        g_media.video_slots[i].data = memalign(256U, frame_size);
        if (g_media.video_slots[i].data == NULL) {
            (void)video_cap_stop(VIDEO_CAP_INDEX);
            media_free_video_buffers();
            media_set_error(-1);
            return -1;
        }
        g_media.video_slots[i].capacity = frame_size;
        g_media.video_slots[i].state = TIRTC_MEDIA_VIDEO_SLOT_FREE;
    }
    g_media.video_capture_owned = true;
    media_lock(&flags);
    g_media.video_capture_logged = false;
    g_media.video_send_logged = false;
    g_media.video_format_error_logged = false;
    media_unlock(flags);
    (void)jz_video_service_set_status(TIRTC_MEDIA_VIDEO_SERVICE, 1);
    printf("[tirtc_media] video start ready cam=%d frame=%ux%u format=NV12 bytes=%lu startup=%lums\n",
           VIDEO_CAP_INDEX, (unsigned)info->width, (unsigned)info->height,
           (unsigned long)frame_size,
           (unsigned long)(systick_get_time_ms() - video_start_begin_ms));
    return 0;
#else
    return -1;
#endif
}

static void media_stop_video_capture(void)
{
    unsigned long flags;
    bool owned;
    uint8_t callbacks_inflight;
    uint8_t workers_inflight;
    uint64_t deadline_ms;
    int stop_result = 0;

    (void)jz_video_service_set_status(TIRTC_MEDIA_VIDEO_SERVICE, 0);
    media_lock(&flags);
    owned = g_media.video_capture_owned;
    g_media.video_capture_owned = false;
    g_media.video_enabled = false;
    g_media.stats.camera_ready = false;
    media_unlock(flags);
    if (owned) {
        stop_result = video_cap_stop(VIDEO_CAP_INDEX);
        if (stop_result != 0) {
            media_set_error(stop_result);
            printf("[tirtc_media] video stop incomplete cam=%d result=%d\n",
                   VIDEO_CAP_INDEX, stop_result);
        }
    }
    deadline_ms = systick_get_time_ms() +
                  TIRTC_MEDIA_VIDEO_CALLBACK_DRAIN_MS;
    do {
        media_lock(&flags);
        callbacks_inflight = g_media.video_callbacks_inflight;
        workers_inflight = g_media.video_workers_inflight;
        media_unlock(flags);
        if (callbacks_inflight != 0U || workers_inflight != 0U) {
            if (systick_get_time_ms() >= deadline_ms) {
                media_set_error(-1);
                printf("[tirtc_media] video callback drain timeout; buffers retained\n");
                return;
            }
            (void)thread_wait_timeout(1U);
        }
    } while (callbacks_inflight != 0U || workers_inflight != 0U);
    media_free_video_buffers();
    printf("[tirtc_media] video stop done cam=%d result=%d\n",
           VIDEO_CAP_INDEX, stop_result);
}

int tirtc_demo_media_init(void)
{
    jz_audio_serviceCfg audio_config = {
        .name = TIRTC_MEDIA_AUDIO_SERVICE,
        .is_running = false,
        .data_cb = media_audio_capture,
        .release_cb = media_audio_release,
        .user_data = NULL,
        .priority = 100U,
    };
    jz_video_serviceCfg video_config = {
        .name = TIRTC_MEDIA_VIDEO_SERVICE,
        .data_cb = media_video_capture,
        .release_cb = media_video_release,
        .user_data = NULL,
        .priority = 100U,
        .is_running = 0U,
    };
    uint64_t wait_deadline_ms = systick_get_time_ms() + 1000U;
    thread_ptr_t audio_worker = NULL;
    thread_ptr_t video_worker = NULL;
    unsigned long flags;
    bool control_mutex_created = false;

    for (;;) {
        media_lock(&flags);
        if (g_media.initialized) {
            media_unlock(flags);
            return 0;
        }
        if (!g_media.initializing) {
            g_media.initializing = true;
            media_unlock(flags);
            break;
        }
        media_unlock(flags);
        if (systick_get_time_ms() >= wait_deadline_ms) {
            return -1;
        }
        (void)thread_wait_timeout(1U);
    }

    if (g_media.control_mutex == NULL) {
        g_media.control_mutex = xSemaphoreCreateMutex();
        if (g_media.control_mutex == NULL) {
            goto failed;
        }
        control_mutex_created = true;
    }
    if (jz_audio_service_register(&audio_config) != pdPASS) {
        goto failed;
    }
    g_media.audio_registered = true;
    if (jz_video_service_register(&video_config) != pdPASS) {
        goto failed;
    }
    g_media.video_registered = true;
    audio_worker = thread_create("tirtc_audio_media",
                                 TIRTC_MEDIA_AUDIO_WORKER_STACK,
                                 media_audio_worker, NULL);
    video_worker = thread_create("tirtc_video_media",
                                 TIRTC_MEDIA_VIDEO_WORKER_STACK,
                                 media_video_worker, NULL);
    if (audio_worker == NULL || video_worker == NULL) {
        goto failed;
    }

    media_lock(&flags);
    g_media.audio_worker = audio_worker;
    g_media.video_worker = video_worker;
    g_media.initializing = false;
    g_media.initialized = true;
    g_media.stats.initialized = true;
    media_unlock(flags);
    printf("[tirtc_media] runtime ready audio_stack=%u scratch=%lu control_lock=freertos\n",
           TIRTC_MEDIA_AUDIO_WORKER_STACK,
           (unsigned long)(sizeof(g_media.audio_uplink_alaw) +
                           sizeof(g_media.audio_playback_pcm)));
    return 0;

failed:
    if (video_worker != NULL) {
        thread_delete(video_worker);
    }
    if (audio_worker != NULL) {
        thread_delete(audio_worker);
    }
    if (g_media.video_registered) {
        jz_video_service_unregister(TIRTC_MEDIA_VIDEO_SERVICE);
    }
    if (g_media.audio_registered) {
        jz_audio_service_unregister(TIRTC_MEDIA_AUDIO_SERVICE);
    }
    if (control_mutex_created && g_media.control_mutex != NULL) {
        vSemaphoreDelete(g_media.control_mutex);
        g_media.control_mutex = NULL;
    }
    media_lock(&flags);
    g_media.audio_worker = NULL;
    g_media.video_worker = NULL;
    g_media.audio_registered = false;
    g_media.video_registered = false;
    g_media.initializing = false;
    g_media.stats.initialized = false;
    media_unlock(flags);
    return -1;
}

int tirtc_demo_media_start(tirtc_conn_t connection,
                           tirtc_demo_media_mode_t mode,
                           bool enable_video)
{
    uint64_t audio_deadline_ms;
    uint64_t session_started_at_ms;
    unsigned long flags;
    int result;

    if (connection == NULL || mode == TIRTC_DEMO_MEDIA_NONE) {
        return -1;
    }
    result = tirtc_demo_media_init();
    if (result != 0) {
        return result;
    }

    session_started_at_ms = systick_get_time_ms();
    media_lock(&flags);
    if (g_media.connection != NULL && g_media.connection != connection) {
        media_unlock(flags);
        return -1;
    }
    ++g_media.generation;
    if (g_media.generation == 0U) {
        g_media.generation = 1U;
    }
    g_media.connection = connection;
    g_media.mode = mode;
    g_media.session_started_at_ms = session_started_at_ms;
    g_media.video_start_begin_ms = 0U;
    g_media.uplink_enabled = false;
    g_media.video_enabled = false;
    g_media.audio_tx_read = 0U;
    g_media.audio_tx_write = 0U;
    g_media.audio_tx_count = 0U;
    g_media.audio_rx_read = 0U;
    g_media.audio_rx_write = 0U;
    g_media.audio_rx_count = 0U;
    g_media.audio_capture_logged = false;
    g_media.audio_send_logged = false;
    g_media.audio_receive_logged = false;
    g_media.audio_playback_logged = false;
    g_media.audio_output_logged = false;
    g_media.audio_decode_error_logged = false;
    memset(&g_media.stats, 0, sizeof(g_media.stats));
    g_media.stats.initialized = true;
    media_unlock(flags);

    if (mode == TIRTC_DEMO_MEDIA_MONITOR ||
        mode == TIRTC_DEMO_MEDIA_DEVICE_CALL) {
        result = media_prepare_remote_video_buffers();
        if (result != 0) {
            media_set_error(result);
            printf("[tirtc_media] remote video buffers unavailable; audio remains active\n");
        }
    }

    if (!g_media.audio_capture_started) {
        result = jz_audio_cap_set_3a_enabled(
            TIRTC_DEMO_MEDIA_ENABLE_VOXA_3A ? 1 : 0);
        printf("[tirtc_media] audio configure 3a=%d result=%d running=%d\n",
               TIRTC_DEMO_MEDIA_ENABLE_VOXA_3A, result,
               audio_cap_status_get());
        if (result != 0 && !audio_cap_status_get()) {
            tirtc_demo_media_stop(connection);
            return result;
        }
        result = jz_audio_cap_start();
        if (result != 0 && result != 1) {
            tirtc_demo_media_stop(connection);
            return result;
        }
        g_media.audio_capture_started = true;
    }
    printf("[tirtc_media] audio capture bootstrap started; opening AIC playback\n");
    result = media_open_player();
    if (result != JZ_AUDIO_OK) {
        tirtc_demo_media_stop(connection);
        return result;
    }
    printf("[tirtc_media] AIC playback opened; waiting for capture ready\n");

    audio_deadline_ms = systick_get_time_ms() +
                        TIRTC_MEDIA_AUDIO_START_TIMEOUT_MS;
    while (!audio_cap_status_get() &&
           systick_get_time_ms() < audio_deadline_ms) {
        (void)thread_wait_timeout(1U);
    }
    if (!audio_cap_status_get()) {
        g_media.audio_capture_started = false;
        media_set_error(-1);
        printf("[tirtc_media] audio capture start timeout 3a=%d\n",
               TIRTC_DEMO_MEDIA_ENABLE_VOXA_3A);
        tirtc_demo_media_stop(connection);
        return -1;
    }
    (void)jz_audio_service_set_status(TIRTC_MEDIA_AUDIO_SERVICE, 1);
    media_lock(&flags);
    g_media.stats.capture_ready = true;
    media_unlock(flags);
    printf("[tirtc_media] audio capture ready 3a=%d\n",
           TIRTC_DEMO_MEDIA_ENABLE_VOXA_3A);

    if (enable_video && mode != TIRTC_DEMO_MEDIA_WECHAT &&
        mode != TIRTC_DEMO_MEDIA_AI) {
        result = tirtc_demo_media_set_video(true);
        if (result != 0) {
            printf("[tirtc_media] video unavailable, audio remains active: %d\n", result);
        }
    }
    return 0;
}

void tirtc_demo_media_stop(tirtc_conn_t connection)
{
    unsigned long flags;
    bool stop_video;

    media_lock(&flags);
    if (connection != NULL && g_media.connection != connection) {
        media_unlock(flags);
        return;
    }
    g_media.connection = NULL;
    g_media.mode = TIRTC_DEMO_MEDIA_NONE;
    g_media.session_started_at_ms = 0U;
    g_media.video_start_begin_ms = 0U;
    g_media.uplink_enabled = false;
    stop_video = g_media.video_capture_owned;
    ++g_media.generation;
    g_media.audio_tx_read = 0U;
    g_media.audio_tx_write = 0U;
    g_media.audio_tx_count = 0U;
    g_media.audio_rx_read = 0U;
    g_media.audio_rx_write = 0U;
    g_media.audio_rx_count = 0U;
    for (size_t i = 0U; i < TIRTC_MEDIA_REMOTE_VIDEO_SLOT_COUNT; ++i) {
        if (g_media.remote_video_slots[i].state ==
            TIRTC_MEDIA_REMOTE_VIDEO_READY) {
            g_media.remote_video_slots[i].state =
                TIRTC_MEDIA_REMOTE_VIDEO_FREE;
        }
    }
    g_media.stats.capture_ready = false;
    g_media.stats.uplink_enabled = false;
    media_unlock(flags);

    (void)jz_audio_service_set_status(TIRTC_MEDIA_AUDIO_SERVICE, 0);
    if (stop_video) {
        media_stop_video_capture();
    }
    media_close_player();
}

int tirtc_demo_media_set_uplink(bool enabled)
{
    unsigned long flags;

    media_lock(&flags);
    if (g_media.connection == NULL) {
        media_unlock(flags);
        return -1;
    }
    g_media.uplink_enabled = enabled;
    g_media.stats.uplink_enabled = enabled;
    if (!enabled) {
        g_media.audio_tx_read = 0U;
        g_media.audio_tx_write = 0U;
        g_media.audio_tx_count = 0U;
    }
    media_unlock(flags);
    return 0;
}

int tirtc_demo_media_set_video(bool enabled)
{
    unsigned long flags;
    tirtc_demo_media_mode_t mode;
    int result = 0;

    media_lock(&flags);
    mode = g_media.mode;
    if (g_media.connection == NULL || mode == TIRTC_DEMO_MEDIA_WECHAT ||
        mode == TIRTC_DEMO_MEDIA_AI) {
        media_unlock(flags);
        return -1;
    }
    media_unlock(flags);

    if (enabled) {
        result = media_start_video_capture();
    } else {
        media_stop_video_capture();
    }
    media_lock(&flags);
    g_media.video_enabled = enabled && result == 0;
    g_media.stats.video_enabled = g_media.video_enabled;
    g_media.stats.camera_ready = g_media.video_enabled;
    media_unlock(flags);
    return result;
}

void tirtc_demo_media_request_key_frame(void)
{
    unsigned long flags;

    media_lock(&flags);
    g_media.force_key_frame = true;
    g_media.next_video_capture_ms = 0U;
    media_unlock(flags);
}

int tirtc_demo_media_submit_remote_audio(tirtc_conn_t connection,
                                         const TIRTCFRAMEINFO *frame,
                                         const void *data)
{
    tirtc_media_audio_rx_packet_t *packet;
    thread_ptr_t worker;
    unsigned long flags;
    bool log_first_receive = false;

    if (connection == NULL || frame == NULL || data == NULL ||
        frame->length == 0U || frame->length > TIRTC_MEDIA_AUDIO_RX_MAX ||
        (frame->media != TIRTC_AUDIO_PCM && frame->media != TIRTC_AUDIO_ALAW &&
         frame->media != TIRTC_AUDIO_OPUS)) {
        return -1;
    }
    media_lock(&flags);
    if (connection != g_media.connection ||
        g_media.audio_rx_count >= TIRTC_MEDIA_AUDIO_RX_DEPTH) {
        if (connection == g_media.connection) {
            ++g_media.stats.rx_audio_dropped;
        }
        media_unlock(flags);
        return -1;
    }
    packet = &g_media.audio_rx[g_media.audio_rx_write];
    packet->generation = g_media.generation;
    packet->timestamp_ms = frame->ts;
    packet->length = (uint16_t)frame->length;
    packet->stream_id = frame->stream_id;
    packet->media = frame->media;
    packet->flags = frame->flags;
    memcpy(packet->data, data, frame->length);
    g_media.audio_rx_write = (uint8_t)((g_media.audio_rx_write + 1U) %
                                       TIRTC_MEDIA_AUDIO_RX_DEPTH);
    ++g_media.audio_rx_count;
    if (!g_media.audio_receive_logged) {
        g_media.audio_receive_logged = true;
        log_first_receive = true;
    }
    worker = g_media.audio_worker;
    media_unlock(flags);
    if (log_first_receive) {
        printf("[tirtc_media] first remote audio media=%u flags=%u bytes=%lu ts=%lu\n",
               (unsigned)frame->media, (unsigned)frame->flags,
               (unsigned long)frame->length, (unsigned long)frame->ts);
    }
    if (worker != NULL) {
        thread_wakeup(worker);
    }
    return 0;
}

int tirtc_demo_media_submit_remote_video(tirtc_conn_t connection,
                                         const TIRTCFRAMEINFO *frame,
                                         const void *data)
{
    tirtc_media_remote_video_slot_t *slot = NULL;
    tirtc_media_remote_video_slot_t *oldest_ready = NULL;
    uint32_t generation;
    unsigned long flags;

    if (connection == NULL || frame == NULL || data == NULL ||
        frame->stream_id != TIRTC_DEMO_DEVICE_VIDEO_STREAM_ID ||
        (frame->media != TIRTC_VIDEO_JPEG &&
         frame->media != TIRTC_VIDEO_MJPEG) ||
        frame->length == 0U ||
        frame->length > TIRTC_DEMO_REMOTE_VIDEO_MAX_BYTES) {
        return -1;
    }

    media_lock(&flags);
    if (connection != g_media.connection ||
        (g_media.mode != TIRTC_DEMO_MEDIA_MONITOR &&
         g_media.mode != TIRTC_DEMO_MEDIA_DEVICE_CALL)) {
        media_unlock(flags);
        return -1;
    }
    for (size_t i = 0U; i < TIRTC_MEDIA_REMOTE_VIDEO_SLOT_COUNT; ++i) {
        tirtc_media_remote_video_slot_t *candidate =
            &g_media.remote_video_slots[i];

        if (candidate->data == NULL) {
            continue;
        }
        if (candidate->state == TIRTC_MEDIA_REMOTE_VIDEO_FREE) {
            slot = candidate;
            break;
        }
        if (candidate->state == TIRTC_MEDIA_REMOTE_VIDEO_READY &&
            (oldest_ready == NULL ||
             candidate->sequence < oldest_ready->sequence)) {
            oldest_ready = candidate;
        }
    }
    if (slot == NULL) {
        slot = oldest_ready;
        if (slot != NULL) {
            ++g_media.stats.rx_video_dropped;
        }
    }
    if (slot == NULL) {
        ++g_media.stats.rx_video_dropped;
        media_unlock(flags);
        return -1;
    }
    generation = g_media.generation;
    slot->state = TIRTC_MEDIA_REMOTE_VIDEO_WRITING;
    slot->generation = generation;
    media_unlock(flags);

    memcpy(slot->data, data, frame->length);

    media_lock(&flags);
    if (slot->state != TIRTC_MEDIA_REMOTE_VIDEO_WRITING ||
        connection != g_media.connection || generation != g_media.generation) {
        slot->state = TIRTC_MEDIA_REMOTE_VIDEO_FREE;
        ++g_media.stats.rx_video_dropped;
        media_unlock(flags);
        return -1;
    }
    ++g_media.remote_video_sequence;
    if (g_media.remote_video_sequence == 0U) {
        g_media.remote_video_sequence = 1U;
    }
    slot->length = frame->length;
    slot->timestamp_ms = frame->ts;
    slot->sequence = g_media.remote_video_sequence;
    slot->state = TIRTC_MEDIA_REMOTE_VIDEO_READY;
    ++g_media.stats.rx_video_frames;
    media_unlock(flags);
    return 0;
}

bool tirtc_demo_media_acquire_remote_video(tirtc_demo_remote_video_t *video)
{
    tirtc_media_remote_video_slot_t *latest = NULL;
    size_t latest_index = 0U;
    unsigned long flags;

    if (video == NULL) {
        return false;
    }
    memset(video, 0, sizeof(*video));
    media_lock(&flags);
    for (size_t i = 0U; i < TIRTC_MEDIA_REMOTE_VIDEO_SLOT_COUNT; ++i) {
        tirtc_media_remote_video_slot_t *candidate =
            &g_media.remote_video_slots[i];

        if (candidate->state == TIRTC_MEDIA_REMOTE_VIDEO_READY &&
            candidate->generation == g_media.generation &&
            (latest == NULL || candidate->sequence > latest->sequence)) {
            latest = candidate;
            latest_index = i;
        }
    }
    if (latest == NULL) {
        media_unlock(flags);
        return false;
    }
    for (size_t i = 0U; i < TIRTC_MEDIA_REMOTE_VIDEO_SLOT_COUNT; ++i) {
        tirtc_media_remote_video_slot_t *candidate =
            &g_media.remote_video_slots[i];

        if (candidate != latest &&
            candidate->state == TIRTC_MEDIA_REMOTE_VIDEO_READY) {
            candidate->state = TIRTC_MEDIA_REMOTE_VIDEO_FREE;
            ++g_media.stats.rx_video_dropped;
        }
    }
    latest->state = TIRTC_MEDIA_REMOTE_VIDEO_DISPLAYING;
    video->data = latest->data;
    video->length = latest->length;
    video->timestamp_ms = latest->timestamp_ms;
    video->token = (uint32_t)latest_index + 1U;
    media_unlock(flags);
    return true;
}

void tirtc_demo_media_release_remote_video(uint32_t token)
{
    size_t index;
    unsigned long flags;

    if (token == 0U || token > TIRTC_MEDIA_REMOTE_VIDEO_SLOT_COUNT) {
        return;
    }
    index = (size_t)(token - 1U);
    media_lock(&flags);
    if (g_media.remote_video_slots[index].state ==
        TIRTC_MEDIA_REMOTE_VIDEO_DISPLAYING) {
        g_media.remote_video_slots[index].state =
            TIRTC_MEDIA_REMOTE_VIDEO_FREE;
    }
    media_unlock(flags);
}

void tirtc_demo_media_get_stats(tirtc_demo_media_stats_t *stats)
{
    unsigned long flags;

    if (stats == NULL) {
        return;
    }
    media_lock(&flags);
    *stats = g_media.stats;
    media_unlock(flags);
}
