#include "media_sink.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "audio_device.h"
#include "platform/app_task_affinity.h"
#include "virtual_audio_source.h"

static const char *TAG = "media_sink";

#define MEDIA_SINK_AUDIO_QUEUE_LEN 32
#define MEDIA_SINK_AUDIO_TASK_STACK (5 * 1024)
#define MEDIA_SINK_AUDIO_TASK_PRIORITY 13
#define MEDIA_SINK_AUDIO_TASK_CORE APP_TASK_CORE_AUDIO
#define MEDIA_SINK_AUDIO_PLAY_CHUNK_MS 20
#define MEDIA_SINK_AUDIO_FADE_IN_MS 5
#define MEDIA_SINK_AUDIO_BULK_PREBUFFER_MS 200
#define MEDIA_SINK_AUDIO_PCM_BUFFER_MS 10000
#define MEDIA_SINK_AUDIO_INTEGRITY_CHUNKS \
    ((MEDIA_SINK_AUDIO_PCM_BUFFER_MS / MEDIA_SINK_AUDIO_PLAY_CHUNK_MS) + 16U)
#define MEDIA_SINK_AUDIO_REALTIME_PACKET_MAX_MS 60
#define MEDIA_SINK_AUDIO_JITTER_BOOST_STEP_MS 20
#define MEDIA_SINK_AUDIO_JITTER_STABLE_WINDOWS 5
#define MEDIA_SINK_AUDIO_JITTER_SAMPLE_MAX_MS 1000
#define MEDIA_SINK_AUDIO_DELIVERY_BATCH_MAX_MS 10
#define MEDIA_SINK_AUDIO_TALKSPURT_GAP_MIN_MS 400
#define MEDIA_SINK_AUDIO_SOURCE_DELTA_MAX_MS 10000
#define MEDIA_SINK_AUDIO_SOURCE_GAP_CONFIRM_PACKETS 10
#define MEDIA_SINK_AUDIO_BULK_TARGET_MS 240
#define MEDIA_SINK_AUDIO_PLAYOUT_RELEASE_MARGIN_MS 20

typedef struct {
    uint32_t realtime_prebuffer_ms;
    uint32_t realtime_target_ms;
    uint32_t jitter_boost_max_ms;
    uint32_t adaptive_target_boost_max_ms;
    uint32_t trim_hysteresis_ms;
    uint32_t underflow_grace_max_ms;
    uint32_t underflow_concealment_max_ms;
    uint32_t source_gap_fill_max_ms;
    uint8_t playout_slow_frames;
    uint8_t playout_fast_frames;
    uint32_t playout_low_margin_ms;
    uint32_t playout_high_margin_ms;
    uint32_t playout_startup_hold_ms;
    uint32_t slow_play_us;
    uint8_t drain_burst_max;
    bool absorb_network_bursts;
    bool adapt_jitter_buffer;
    bool trust_source_timestamps;
    bool fast_start_on_talkspurt;
    bool pace_playback_clock;
} media_sink_audio_tuning_t;

typedef struct {
    uint32_t generation;
    audio_format_t format;
    uint8_t *data;
    size_t data_len;
    uint32_t source_gap_ms;
    uint32_t integrity_sequence;
    bool integrity_valid;
} media_sink_audio_packet_t;

typedef struct {
    uint32_t received_frames;
    uint32_t buffered_frames;
    uint32_t played_frames;
    uint32_t checksum_failures;
    uint32_t missing_frames;
    uint32_t duplicate_frames;
    uint32_t reordered_frames;
    uint32_t first_sequence;
    uint32_t last_sequence;
    bool sequence_valid;
    uint32_t queue_dropped_frames;
    uint32_t trimmed_frames;
    uint32_t play_failures;
    uint32_t play_missing_frames;
    uint32_t play_duplicate_frames;
    uint32_t play_reordered_frames;
    uint32_t first_play_sequence;
    uint32_t last_play_sequence;
    bool play_sequence_valid;
} media_sink_audio_integrity_state_t;

typedef struct {
    uint32_t sequence;
    bool valid;
} media_sink_audio_integrity_chunk_t;

typedef struct {
    int64_t last_arrival_us;
    int64_t first_arrival_us;
    int64_t talkspurt_ended_us;
    uint32_t last_source_timestamp_ms;
    uint32_t first_source_timestamp_ms;
    uint32_t arrival_jitter_ewma_ms;
    uint32_t arrival_jitter_peak_ms;
    uint32_t underflow_events_in_window;
    uint32_t underflow_active_events_in_window;
    uint32_t max_underflow_arrival_gap_ms_in_window;
    uint32_t fast_start_events_in_window;
    uint32_t delayed_burst_events_in_window;
    uint32_t talkspurt_events_in_window;
    uint32_t max_delivery_variation_ms_in_window;
    uint32_t max_arrival_gap_ms;
    uint32_t first_play_delay_ms;
    uint32_t rx_packets;
    uint32_t rx_ms;
    uint32_t played_packets;
    uint32_t played_ms;
    uint32_t play_drop_packets;
    uint32_t queue_drop_packets;
    uint32_t trim_drop_packets;
    uint32_t underflow_events;
    uint32_t active_underflow_events;
    uint32_t underflow_grace_waits;
    uint32_t underflow_grace_recoveries;
    uint32_t delayed_burst_events;
    uint32_t source_late_events;
    uint32_t source_late_ms;
    uint32_t max_source_late_ms;
    int32_t source_clock_error_ms;
    int32_t source_gap_pending_ms;
    uint8_t source_gap_pending_packets;
    uint32_t source_gap_fill_events;
    uint32_t source_gap_fill_ms;
    uint32_t concealment_events;
    uint32_t concealed_ms;
    uint32_t clock_recovery_events;
    uint32_t clock_recovery_frames;
    uint32_t clock_recovery_fast_events;
    uint32_t clock_recovery_fast_frames;
    uint32_t pacing_wait_events;
    uint32_t pacing_wait_ms;
    uint32_t pacing_wait_max_ms;
    uint32_t pacing_late_events;
    uint32_t pacing_late_ms;
    uint32_t pacing_late_max_ms;
    uint8_t stable_windows;
    int8_t playout_rate_mode;
    bool source_timestamp_valid;
    bool fast_start_pending;
    bool talkspurt_hint_valid;
    bool talkspurt_started;
    bool talkspurt_active;
    bool playback_active;
    uint32_t talkspurt_generation;
} media_sink_audio_adaptive_state_t;

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
static uint32_t s_audio_queue_drop_packets_in_window;
static uint32_t s_audio_queue_drop_ms_in_window;
static uint32_t s_audio_jitter_boost_ms;
static EXT_RAM_BSS_ATTR media_sink_audio_integrity_state_t s_audio_integrity;
static EXT_RAM_BSS_ATTR media_sink_audio_integrity_chunk_t
    s_audio_pcm_integrity[MEDIA_SINK_AUDIO_INTEGRITY_CHUNKS];
static size_t s_audio_pcm_integrity_read;
static size_t s_audio_pcm_integrity_count;

static void media_sink_classify_audio_integrity(
    media_sink_audio_packet_t *packet)
{
    virtual_audio_packet_info_t info = {0};

    if (packet == NULL) {
        return;
    }

    int parse_ret = virtual_audio_source_parse_packet(packet->data,
                                                      packet->data_len,
                                                      &info);
    if (parse_ret == VIRTUAL_AUDIO_ERR_NOT_TEST) {
        return;
    }

    taskENTER_CRITICAL(&s_sink_lock);
    if (parse_ret != VIRTUAL_AUDIO_OK) {
        s_audio_integrity.checksum_failures++;
        taskEXIT_CRITICAL(&s_sink_lock);
        return;
    }

    packet->integrity_valid = true;
    packet->integrity_sequence = info.sequence;
    if (!s_audio_integrity.sequence_valid) {
        s_audio_integrity.sequence_valid = true;
        s_audio_integrity.first_sequence = info.sequence;
        s_audio_integrity.last_sequence = info.sequence;
    } else {
        int32_t delta = (int32_t)(info.sequence - s_audio_integrity.last_sequence);
        if (delta == 0) {
            s_audio_integrity.duplicate_frames++;
        } else if (delta < 0) {
            s_audio_integrity.reordered_frames++;
        } else {
            if (delta > 1) {
                s_audio_integrity.missing_frames += (uint32_t)(delta - 1);
            }
            s_audio_integrity.last_sequence = info.sequence;
        }
    }
    s_audio_integrity.received_frames++;
    taskEXIT_CRITICAL(&s_sink_lock);
}

static void media_sink_note_buffered_audio_integrity(
    const media_sink_audio_packet_t *packet)
{
    if (packet == NULL || !packet->integrity_valid) {
        return;
    }

    taskENTER_CRITICAL(&s_sink_lock);
    s_audio_integrity.buffered_frames++;
    taskEXIT_CRITICAL(&s_sink_lock);
}

static void media_sink_note_played_audio_integrity(uint32_t sequence)
{
    taskENTER_CRITICAL(&s_sink_lock);
    if (!s_audio_integrity.play_sequence_valid) {
        s_audio_integrity.play_sequence_valid = true;
        s_audio_integrity.first_play_sequence = sequence;
        s_audio_integrity.last_play_sequence = sequence;
    } else {
        int32_t delta = (int32_t)(sequence - s_audio_integrity.last_play_sequence);
        if (delta == 0) {
            s_audio_integrity.play_duplicate_frames++;
        } else if (delta < 0) {
            s_audio_integrity.play_reordered_frames++;
        } else {
            if (delta > 1) {
                s_audio_integrity.play_missing_frames += (uint32_t)(delta - 1);
            }
            s_audio_integrity.last_play_sequence = sequence;
        }
    }
    s_audio_integrity.played_frames++;
    taskEXIT_CRITICAL(&s_sink_lock);
}

static void media_sink_audio_integrity_drop_chunks_locked(size_t chunks)
{
    while (chunks > 0U && s_audio_pcm_integrity_count > 0U) {
        media_sink_audio_integrity_chunk_t *entry =
            &s_audio_pcm_integrity[s_audio_pcm_integrity_read];
        if (entry->valid) {
            s_audio_integrity.trimmed_frames++;
        }
        memset(entry, 0, sizeof(*entry));
        s_audio_pcm_integrity_read =
            (s_audio_pcm_integrity_read + 1U) % MEDIA_SINK_AUDIO_INTEGRITY_CHUNKS;
        s_audio_pcm_integrity_count--;
        chunks--;
    }
}

static void media_sink_audio_integrity_append_locked(size_t chunks,
                                                     bool valid,
                                                     uint32_t sequence)
{
    for (size_t i = 0; i < chunks; ++i) {
        if (s_audio_pcm_integrity_count >= MEDIA_SINK_AUDIO_INTEGRITY_CHUNKS) {
            media_sink_audio_integrity_drop_chunks_locked(1U);
        }
        size_t write_index =
            (s_audio_pcm_integrity_read + s_audio_pcm_integrity_count) %
            MEDIA_SINK_AUDIO_INTEGRITY_CHUNKS;
        s_audio_pcm_integrity[write_index].valid = valid && i == 0U;
        s_audio_pcm_integrity[write_index].sequence = sequence;
        s_audio_pcm_integrity_count++;
    }
}

static bool media_sink_audio_integrity_pop_locked(uint32_t *sequence)
{
    if (s_audio_pcm_integrity_count == 0U) {
        return false;
    }

    media_sink_audio_integrity_chunk_t *entry =
        &s_audio_pcm_integrity[s_audio_pcm_integrity_read];
    bool valid = entry->valid;
    if (valid && sequence != NULL) {
        *sequence = entry->sequence;
    }
    memset(entry, 0, sizeof(*entry));
    s_audio_pcm_integrity_read =
        (s_audio_pcm_integrity_read + 1U) % MEDIA_SINK_AUDIO_INTEGRITY_CHUNKS;
    s_audio_pcm_integrity_count--;
    return valid;
}
static uint32_t s_audio_last_source_packet_ms;
static EXT_RAM_BSS_ATTR media_sink_audio_adaptive_state_t s_audio_adaptive;
static uint8_t *s_audio_pcm_buffer;
static size_t s_audio_pcm_buffer_size;
static size_t s_audio_pcm_read_offset;
static size_t s_audio_pcm_used_bytes;
static uint8_t *s_audio_pcm_chunk_buffer;
static size_t s_audio_pcm_chunk_buffer_size;
static uint8_t *s_audio_pcm_plc_buffer;
static media_sink_audio_profile_t s_audio_profile = MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY;

static const media_sink_audio_tuning_t s_audio_tunings[] = {
    [MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY] = {
        .realtime_prebuffer_ms = 20,
        .realtime_target_ms = 60,
        .jitter_boost_max_ms = 40,
        .adaptive_target_boost_max_ms = 40,
        .trim_hysteresis_ms = 0,
        .underflow_grace_max_ms = 0,
        .underflow_concealment_max_ms = 0,
        .source_gap_fill_max_ms = 0,
        .playout_slow_frames = 0,
        .playout_fast_frames = 0,
        .playout_low_margin_ms = 0,
        .playout_high_margin_ms = 0,
        .playout_startup_hold_ms = 0,
        .slow_play_us = 25000,
        .drain_burst_max = 0,
        .absorb_network_bursts = false,
        .adapt_jitter_buffer = true,
        .trust_source_timestamps = true,
        .fast_start_on_talkspurt = true,
        .pace_playback_clock = false,
    },
    [MEDIA_SINK_AUDIO_PROFILE_DEVICE_CALL] = {
        /* Measured 100 +/- 50 ms, 3% loss relay runs produced first-stage
         * delivery holes up to 277 ms before adaptive jitter history existed.
         * Hold 320 ms in the PSRAM ring at device-call startup; bounded rate
         * recovery below drains that one-time reserve after the link settles.
         * IPC, WeChat, and AI keep their independent startup policies. */
        .realtime_prebuffer_ms = 320,
        .realtime_target_ms = 140,
        /* Clean calls still start from 140 ms. Under the measured relay
         * profile (100 +/- 50 ms, 3% loss), recovered media reaches the app
         * in 350-500 ms bursts even though the call remains connected. Let
         * the observed arrival peak raise the PSRAM-backed target far enough
         * to bridge that gap instead of repeatedly draining at 240 ms. */
        .jitter_boost_max_ms = 520,
        .adaptive_target_boost_max_ms = 480,
        /* Recovered device-call media arrives in 300-500 ms callback bursts.
         * The startup reserve hold exposed a measured 560 ms burst while the
         * adaptive target was still 368 ms; a 200 ms ceiling then deleted
         * 140 ms of valid speech. Keep that recoverable burst in the existing
         * 10 s PSRAM ring and let bounded fast playout recenter it. */
        .trim_hysteresis_ms = 400,
        .underflow_grace_max_ms = 0,
        /* Keep I2S running across a short loss, but never replay a complete
         * 20 ms speech chunk. Repetition turns periodic packet holes into a
         * tonal buzz that is easily mistaken for acoustic feedback. */
        .underflow_concealment_max_ms = 40,
        /* TGTRP 1.5.11 relay tests show that a recovered callback can still
         * skip bounded 20 ms media timestamps. Preserve the sender timeline
         * with silence PLC instead of consuming received PCM faster than it
         * was produced. Large discontinuities remain talkspurt boundaries. */
        .source_gap_fill_max_ms = 240,
        /* Keep the speaker clock at 20 ms while steering only the amount of
         * PCM consumed from the PSRAM ring. Four 16 kHz frames are 1.25%,
         * small enough to avoid an audible pitch step but large enough to
         * recenter a burst-shifted buffer before it underflows or is trimmed.
         * The 60 ms dead bands keep clean-Wi-Fi calls at exactly 1.0x. */
        .playout_slow_frames = 4,
        .playout_fast_frames = 4,
        .playout_low_margin_ms = 60,
        .playout_high_margin_ms = 60,
        /* Do not spend the startup reserve before the jitter estimate has
         * observed the first weak-network delivery burst. Only fast recovery
         * is held; slow recovery still protects a falling ring immediately. */
        .playout_startup_hold_ms = 8000,
        .slow_play_us = 25000,
        /* Weak-network recovery delivers several 20 ms packets in one RTC
         * callback burst. Move that burst out of the bounded packet queue and
         * into the existing PSRAM PCM ring before playing the next chunk.
         * This changes no buffer ownership and allocates no additional RAM. */
        .drain_burst_max = 16,
        .absorb_network_bursts = false,
        .adapt_jitter_buffer = true,
        .trust_source_timestamps = true,
        .fast_start_on_talkspurt = false,
        /* esp_codec_dev_write() returns after DMA accepts the chunk, which can
         * be slightly earlier than the 20 ms represented by that PCM. Without
         * a wall-clock floor the consumer slowly drains the jitter buffer even
         * when RTC delivery is healthy. Keep this policy scoped to ordinary
         * device calls until the other playback profiles are measured. */
        .pace_playback_clock = true,
    },
    [MEDIA_SINK_AUDIO_PROFILE_JITTER_SAFE] = {
        /* AI TTS arrives as 40 ms packets and can be delivered in 160-320 ms
         * callback bursts. Keep one burst plus a playback chunk before the
         * first write; short replies still drain immediately on round_end. */
        .realtime_prebuffer_ms = 340,
        .realtime_target_ms = 420,
        /* Field traces contain bounded 600-700 ms delivery holes inside one
         * AI response. The PSRAM ring can absorb those holes without changing
         * the low-latency IPC/VoIP profiles. */
        .jitter_boost_max_ms = 800,
        /* A single KCP head-of-line stall can produce a 1 s peak sample. Do
         * not let that one sample hold every later restart at 1.22 s. EWMA
         * and repeated underflows can still raise the AI target to 900 ms. */
        .adaptive_target_boost_max_ms = 480,
        .trim_hysteresis_ms = 120,
        /* A short late burst should create one bounded gap, not trigger a
         * second-long rebuffer cycle. This applies only to AI TTS. */
        .underflow_grace_max_ms = 200,
        .underflow_concealment_max_ms = 0,
        .source_gap_fill_max_ms = 0,
        .playout_slow_frames = 0,
        .playout_fast_frames = 0,
        .playout_low_margin_ms = 0,
        .playout_high_margin_ms = 0,
        .playout_startup_hold_ms = 0,
        .slow_play_us = 30000,
        .drain_burst_max = 2,
        .absorb_network_bursts = false,
        .adapt_jitter_buffer = true,
        .trust_source_timestamps = true,
        /* AI TTS packets can arrive just before round_start. Waiting for the
         * normal prebuffer avoids consuming that first packet immediately. */
        .fast_start_on_talkspurt = false,
        .pace_playback_clock = false,
    },
    [MEDIA_SINK_AUDIO_PROFILE_IPC_TALKBACK] = {
        /* Browser callbacks arrive in batches. Keep those packets in PSRAM PCM
         * instead of deleting speech from the internal-RAM packet queue. */
        .realtime_prebuffer_ms = 80,
        .realtime_target_ms = 180,
        .jitter_boost_max_ms = 400,
        .adaptive_target_boost_max_ms = 0,
        .trim_hysteresis_ms = 800,
        .underflow_grace_max_ms = 0,
        .underflow_concealment_max_ms = 0,
        .source_gap_fill_max_ms = 0,
        .playout_slow_frames = 0,
        .playout_fast_frames = 0,
        .playout_low_margin_ms = 0,
        .playout_high_margin_ms = 0,
        .playout_startup_hold_ms = 0,
        .slow_play_us = 25000,
        .drain_burst_max = 8,
        .absorb_network_bursts = true,
        /* PTT silence is a talkspurt boundary, not network jitter. */
        .adapt_jitter_buffer = false,
        .trust_source_timestamps = false,
        .fast_start_on_talkspurt = true,
        .pace_playback_clock = false,
    },
};

static void media_sink_audio_task(void *ctx);

static bool media_sink_audio_profile_valid(media_sink_audio_profile_t profile)
{
    return profile == MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY ||
           profile == MEDIA_SINK_AUDIO_PROFILE_DEVICE_CALL ||
           profile == MEDIA_SINK_AUDIO_PROFILE_JITTER_SAFE ||
           profile == MEDIA_SINK_AUDIO_PROFILE_IPC_TALKBACK;
}

static const char *media_sink_audio_profile_name(media_sink_audio_profile_t profile)
{
    switch (profile) {
    case MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY:
        return "low_latency";
    case MEDIA_SINK_AUDIO_PROFILE_DEVICE_CALL:
        return "device_call";
    case MEDIA_SINK_AUDIO_PROFILE_JITTER_SAFE:
        return "jitter_safe";
    case MEDIA_SINK_AUDIO_PROFILE_IPC_TALKBACK:
        return "ipc_talkback";
    default:
        return "unknown";
    }
}

static media_sink_audio_tuning_t media_sink_audio_get_tuning(void)
{
    media_sink_audio_profile_t profile = MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY;

    taskENTER_CRITICAL(&s_sink_lock);
    profile = s_audio_profile;
    taskEXIT_CRITICAL(&s_sink_lock);

    if (!media_sink_audio_profile_valid(profile)) {
        profile = MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY;
    }
    return s_audio_tunings[profile];
}

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
    media_sink_audio_tuning_t tuning = media_sink_audio_get_tuning();

    if (source_packet_ms > 0U && source_packet_ms <= MEDIA_SINK_AUDIO_REALTIME_PACKET_MAX_MS) {
        uint32_t target_ms = tuning.realtime_target_ms;
        uint32_t packet_floor_ms = source_packet_ms + MEDIA_SINK_AUDIO_PLAY_CHUNK_MS;

        if (target_ms < packet_floor_ms) {
            target_ms = packet_floor_ms;
        }
        uint32_t adaptive_boost_ms = 0;

        if (!tuning.adapt_jitter_buffer) {
            return target_ms;
        }

        taskENTER_CRITICAL(&s_sink_lock);
        adaptive_boost_ms = s_audio_jitter_boost_ms;
        if (s_audio_adaptive.arrival_jitter_peak_ms > adaptive_boost_ms) {
            adaptive_boost_ms = s_audio_adaptive.arrival_jitter_peak_ms;
        }
        if (s_audio_adaptive.arrival_jitter_ewma_ms * 2U > adaptive_boost_ms) {
            adaptive_boost_ms = s_audio_adaptive.arrival_jitter_ewma_ms * 2U;
        }
        taskEXIT_CRITICAL(&s_sink_lock);
        uint32_t target_boost_max_ms = tuning.adaptive_target_boost_max_ms;
        if (target_boost_max_ms == 0U ||
            target_boost_max_ms > tuning.jitter_boost_max_ms) {
            target_boost_max_ms = tuning.jitter_boost_max_ms;
        }
        if (adaptive_boost_ms > target_boost_max_ms) {
            adaptive_boost_ms = target_boost_max_ms;
        }
        return target_ms + adaptive_boost_ms;
    }
    return MEDIA_SINK_AUDIO_BULK_TARGET_MS;
}

static uint32_t media_sink_audio_prebuffer_ms(uint32_t source_packet_ms)
{
    media_sink_audio_tuning_t tuning = media_sink_audio_get_tuning();
    bool fast_start_pending = false;

    taskENTER_CRITICAL(&s_sink_lock);
    fast_start_pending = s_audio_adaptive.fast_start_pending;
    taskEXIT_CRITICAL(&s_sink_lock);

    if (source_packet_ms > 0U && source_packet_ms <= MEDIA_SINK_AUDIO_REALTIME_PACKET_MAX_MS) {
        if (fast_start_pending) {
            return source_packet_ms > MEDIA_SINK_AUDIO_PLAY_CHUNK_MS ?
                       source_packet_ms :
                       MEDIA_SINK_AUDIO_PLAY_CHUNK_MS;
        }

        uint32_t prebuffer_ms = tuning.realtime_prebuffer_ms;
        uint32_t packet_floor_ms = source_packet_ms + MEDIA_SINK_AUDIO_PLAY_CHUNK_MS;
        if (prebuffer_ms < packet_floor_ms) {
            prebuffer_ms = packet_floor_ms;
        }

        /* Adapt only from arrivals observed in the current talkspurt. The
         * state is reset at round_start, so a bad previous round cannot delay
         * the next one. Starting below the already measured latency target
         * creates an avoidable first underflow on bursty hotspot links. */
        if (tuning.adapt_jitter_buffer) {
            uint32_t target_ms = media_sink_audio_latency_target_ms(source_packet_ms);
            if (target_ms > prebuffer_ms) {
                prebuffer_ms = target_ms;
            }
        }
        return prebuffer_ms;
    }
    return MEDIA_SINK_AUDIO_BULK_PREBUFFER_MS;
}

static uint32_t media_sink_audio_rebuffer_ms(uint32_t source_packet_ms)
{
    uint32_t prebuffer_ms = media_sink_audio_prebuffer_ms(source_packet_ms);
    uint32_t target_ms = media_sink_audio_latency_target_ms(source_packet_ms);

    return target_ms > prebuffer_ms ? target_ms : prebuffer_ms;
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
    media_sink_audio_tuning_t tuning = media_sink_audio_get_tuning();
    int64_t now_us = esp_timer_get_time();

    taskENTER_CRITICAL(&s_sink_lock);
    s_audio_adaptive.underflow_events_in_window++;
    s_audio_adaptive.underflow_events++;
    if (s_audio_adaptive.talkspurt_active) {
        s_audio_adaptive.underflow_active_events_in_window++;
        s_audio_adaptive.active_underflow_events++;
    }
    if (s_audio_adaptive.last_arrival_us > 0 &&
        now_us > s_audio_adaptive.last_arrival_us) {
        uint32_t arrival_gap_ms =
            (uint32_t)((now_us - s_audio_adaptive.last_arrival_us) / 1000ULL);
        if (arrival_gap_ms > s_audio_adaptive.max_underflow_arrival_gap_ms_in_window) {
            s_audio_adaptive.max_underflow_arrival_gap_ms_in_window = arrival_gap_ms;
        }
    }
    s_audio_adaptive.stable_windows = 0;
    if (tuning.adapt_jitter_buffer &&
        s_audio_jitter_boost_ms < tuning.jitter_boost_max_ms) {
        s_audio_jitter_boost_ms += MEDIA_SINK_AUDIO_JITTER_BOOST_STEP_MS;
        if (s_audio_jitter_boost_ms > tuning.jitter_boost_max_ms) {
            s_audio_jitter_boost_ms = tuning.jitter_boost_max_ms;
        }
    }
    taskEXIT_CRITICAL(&s_sink_lock);
}

static void media_sink_audio_mark_playback_idle(void)
{
    media_sink_audio_tuning_t tuning = media_sink_audio_get_tuning();

    taskENTER_CRITICAL(&s_sink_lock);
    s_audio_adaptive.last_arrival_us = 0;
    s_audio_adaptive.last_source_timestamp_ms = 0;
    s_audio_adaptive.first_source_timestamp_ms = 0;
    s_audio_adaptive.playback_active = false;
    s_audio_adaptive.arrival_jitter_ewma_ms = 0;
    s_audio_adaptive.arrival_jitter_peak_ms = 0;
    s_audio_adaptive.stable_windows = 0;
    s_audio_adaptive.source_timestamp_valid = false;
    s_audio_adaptive.playout_rate_mode = 0;
    s_audio_adaptive.fast_start_pending =
        tuning.fast_start_on_talkspurt &&
        (!s_audio_adaptive.talkspurt_hint_valid || s_audio_adaptive.talkspurt_active);
    s_audio_jitter_boost_ms = 0;
    taskEXIT_CRITICAL(&s_sink_lock);
}

static void media_sink_audio_mark_talkspurt_idle(void)
{
    media_sink_audio_tuning_t tuning = media_sink_audio_get_tuning();

    taskENTER_CRITICAL(&s_sink_lock);
    /* Keep the final arrival/end timestamps until the next talkspurt starts.
     * The AI control channel can close before its last media packets finish
     * playing, so the lifecycle owner needs these timestamps to prove that the
     * source has stayed quiet for the jitter guard after local PCM drains. */
    s_audio_adaptive.last_source_timestamp_ms = 0;
    s_audio_adaptive.first_source_timestamp_ms = 0;
    s_audio_adaptive.source_timestamp_valid = false;
    s_audio_adaptive.stable_windows = 0;
    s_audio_adaptive.playout_rate_mode = 0;
    s_audio_adaptive.fast_start_pending =
        tuning.fast_start_on_talkspurt && s_audio_adaptive.talkspurt_active;
    taskEXIT_CRITICAL(&s_sink_lock);
}

static bool media_sink_audio_talkspurt_ended(void)
{
    bool ended = false;

    taskENTER_CRITICAL(&s_sink_lock);
    ended = s_audio_adaptive.talkspurt_started &&
            !s_audio_adaptive.talkspurt_active;
    taskEXIT_CRITICAL(&s_sink_lock);
    return ended;
}

static void media_sink_audio_set_playback_active(bool active)
{
    taskENTER_CRITICAL(&s_sink_lock);
    s_audio_adaptive.playback_active = active;
    taskEXIT_CRITICAL(&s_sink_lock);
}

static bool media_sink_audio_completed_talkspurt_ready(uint32_t source_packet_ms,
                                                        size_t buffered_bytes,
                                                        const audio_format_t *format)
{
    media_sink_audio_tuning_t tuning = media_sink_audio_get_tuning();
    const size_t chunk_bytes = media_sink_audio_bytes_for_duration_ms(
        MEDIA_SINK_AUDIO_PLAY_CHUNK_MS, format);
    int64_t last_activity_us = 0;
    uint32_t jitter_guard_ms = 0;
    bool ended = false;

    if (chunk_bytes == 0U || buffered_bytes == 0U) {
        return false;
    }

    taskENTER_CRITICAL(&s_sink_lock);
    ended = s_audio_adaptive.talkspurt_started &&
            !s_audio_adaptive.talkspurt_active;
    last_activity_us = s_audio_adaptive.last_arrival_us;
    if (s_audio_adaptive.talkspurt_ended_us > last_activity_us) {
        last_activity_us = s_audio_adaptive.talkspurt_ended_us;
    }
    jitter_guard_ms = s_audio_adaptive.arrival_jitter_peak_ms;
    if (s_audio_jitter_boost_ms > jitter_guard_ms) {
        jitter_guard_ms = s_audio_jitter_boost_ms;
    }
    taskEXIT_CRITICAL(&s_sink_lock);

    if (!ended || last_activity_us <= 0) {
        return false;
    }

    uint32_t quiet_guard_ms = tuning.realtime_prebuffer_ms;
    uint32_t packet_guard_ms = source_packet_ms * 2U;
    if (packet_guard_ms > quiet_guard_ms) {
        quiet_guard_ms = packet_guard_ms;
    }
    if (jitter_guard_ms > quiet_guard_ms) {
        quiet_guard_ms = jitter_guard_ms;
    }
    if (tuning.jitter_boost_max_ms > 0U &&
        quiet_guard_ms > tuning.jitter_boost_max_ms) {
        quiet_guard_ms = tuning.jitter_boost_max_ms;
    }

    return (esp_timer_get_time() - last_activity_us) >=
           (int64_t)quiet_guard_ms * 1000LL;
}

static uint32_t media_sink_audio_talkspurt_generation(void)
{
    uint32_t generation = 0;

    taskENTER_CRITICAL(&s_sink_lock);
    generation = s_audio_adaptive.talkspurt_generation;
    taskEXIT_CRITICAL(&s_sink_lock);
    return generation;
}

static void media_sink_audio_decay_jitter_boost(void)
{
    taskENTER_CRITICAL(&s_sink_lock);
    if (s_audio_jitter_boost_ms >= MEDIA_SINK_AUDIO_JITTER_BOOST_STEP_MS) {
        s_audio_jitter_boost_ms -= MEDIA_SINK_AUDIO_JITTER_BOOST_STEP_MS;
    } else {
        s_audio_jitter_boost_ms = 0;
    }
    if (s_audio_adaptive.arrival_jitter_peak_ms >= MEDIA_SINK_AUDIO_JITTER_BOOST_STEP_MS) {
        s_audio_adaptive.arrival_jitter_peak_ms -= MEDIA_SINK_AUDIO_JITTER_BOOST_STEP_MS;
    } else {
        s_audio_adaptive.arrival_jitter_peak_ms = 0;
    }
    s_audio_adaptive.arrival_jitter_ewma_ms =
        (s_audio_adaptive.arrival_jitter_ewma_ms * 7U) / 8U;
    s_audio_adaptive.stable_windows = 0;
    taskEXIT_CRITICAL(&s_sink_lock);
}

static void media_sink_audio_note_stable_window(void)
{
    bool should_decay = false;

    taskENTER_CRITICAL(&s_sink_lock);
    if (s_audio_adaptive.stable_windows < UINT8_MAX) {
        s_audio_adaptive.stable_windows++;
    }
    should_decay = s_audio_adaptive.stable_windows >= MEDIA_SINK_AUDIO_JITTER_STABLE_WINDOWS;
    taskEXIT_CRITICAL(&s_sink_lock);

    if (should_decay) {
        media_sink_audio_decay_jitter_boost();
    }
}

static void media_sink_audio_reset_adaptation(void)
{
    taskENTER_CRITICAL(&s_sink_lock);
    memset(&s_audio_adaptive, 0, sizeof(s_audio_adaptive));
    s_audio_jitter_boost_ms = 0;
    taskEXIT_CRITICAL(&s_sink_lock);
}

static bool media_sink_audio_consume_fast_start(void)
{
    bool consumed = false;

    taskENTER_CRITICAL(&s_sink_lock);
    consumed = s_audio_adaptive.fast_start_pending;
    if (consumed) {
        s_audio_adaptive.fast_start_pending = false;
        s_audio_adaptive.fast_start_events_in_window++;
    }
    taskEXIT_CRITICAL(&s_sink_lock);

    return consumed;
}

static uint32_t media_sink_audio_note_arrival(uint32_t source_timestamp_ms,
                                              uint32_t source_packet_ms)
{
    media_sink_audio_tuning_t tuning = media_sink_audio_get_tuning();
    int64_t now_us = esp_timer_get_time();
    uint32_t talkspurt_gap_ms = source_packet_ms * 4U;
    uint32_t source_gap_ms = 0;

    if (source_packet_ms == 0U || source_packet_ms > MEDIA_SINK_AUDIO_REALTIME_PACKET_MAX_MS) {
        return 0;
    }
    if (talkspurt_gap_ms < MEDIA_SINK_AUDIO_TALKSPURT_GAP_MIN_MS) {
        talkspurt_gap_ms = MEDIA_SINK_AUDIO_TALKSPURT_GAP_MIN_MS;
    }

    taskENTER_CRITICAL(&s_sink_lock);
    s_audio_adaptive.rx_packets++;
    s_audio_adaptive.rx_ms += source_packet_ms;
    if (s_audio_adaptive.first_arrival_us <= 0) {
        s_audio_adaptive.first_arrival_us = now_us;
        s_audio_adaptive.first_source_timestamp_ms = source_timestamp_ms;
    }
    if (s_audio_adaptive.last_arrival_us <= 0 ||
        !s_audio_adaptive.source_timestamp_valid ||
        now_us <= s_audio_adaptive.last_arrival_us) {
        s_audio_adaptive.fast_start_pending =
            s_audio_adaptive.fast_start_pending ||
            tuning.fast_start_on_talkspurt;
    } else {
        uint32_t arrival_delta_ms =
            (uint32_t)((now_us - s_audio_adaptive.last_arrival_us) / 1000ULL);
        if (arrival_delta_ms > s_audio_adaptive.max_arrival_gap_ms) {
            s_audio_adaptive.max_arrival_gap_ms = arrival_delta_ms;
        }
        uint32_t source_delta_ms =
            source_timestamp_ms - s_audio_adaptive.last_source_timestamp_ms;
        bool source_delta_valid = source_delta_ms > 0U &&
                                  source_delta_ms <= MEDIA_SINK_AUDIO_SOURCE_DELTA_MAX_MS;
        bool source_discontinuity = source_delta_ms > MEDIA_SINK_AUDIO_SOURCE_DELTA_MAX_MS;
        bool explicit_talkspurt_active = s_audio_adaptive.talkspurt_hint_valid &&
                                         s_audio_adaptive.talkspurt_active;
        bool same_delivery_batch = arrival_delta_ms <= MEDIA_SINK_AUDIO_DELIVERY_BATCH_MAX_MS &&
                                   source_delta_valid &&
                                   source_delta_ms <= talkspurt_gap_ms;

        if (source_delta_valid && source_delta_ms > source_packet_ms) {
            uint32_t late_ms = source_delta_ms - source_packet_ms;

            /* This is timestamp spacing, not proof of packet loss: a later
             * short delta can compensate it. The signed clock error below
             * distinguishes bursty production from cumulative drift. */
            s_audio_adaptive.source_late_events++;
            s_audio_adaptive.source_late_ms += late_ms;
            if (late_ms > s_audio_adaptive.max_source_late_ms) {
                s_audio_adaptive.max_source_late_ms = late_ms;
            }
        }
        bool talkspurt_start = tuning.trust_source_timestamps &&
                               !explicit_talkspurt_active &&
                               (source_delta_valid ?
                                    source_delta_ms > talkspurt_gap_ms :
                                    (source_discontinuity || arrival_delta_ms > talkspurt_gap_ms));
        if (talkspurt_start || source_discontinuity ||
            tuning.source_gap_fill_max_ms == 0U) {
            s_audio_adaptive.source_gap_pending_ms = 0;
            s_audio_adaptive.source_gap_pending_packets = 0;
        } else {
            /* Capture timestamps can alternate between a long and a short
             * frame interval even on a clean link. Treat the signed spacing
             * error as a debt and give later packets time to cancel it before
             * inserting silence. A real missing frame leaves positive debt;
             * scheduler jitter converges back to zero without corrupting the
             * speech timeline. A zero source delta is valid compensation for
             * a preceding long delta, so it participates in this ledger. */
            bool source_delta_ordered =
                source_delta_ms <= MEDIA_SINK_AUDIO_SOURCE_DELTA_MAX_MS;
            if (source_delta_ordered) {
                int32_t delta_error_ms =
                    (int32_t)source_delta_ms - (int32_t)source_packet_ms;
                int32_t pending_limit_ms =
                    (int32_t)tuning.source_gap_fill_max_ms * 4;
                int32_t pending_ms =
                    s_audio_adaptive.source_gap_pending_ms + delta_error_ms;

                if (pending_ms > pending_limit_ms) {
                    pending_ms = pending_limit_ms;
                } else if (pending_ms < -pending_limit_ms) {
                    pending_ms = -pending_limit_ms;
                }
                s_audio_adaptive.source_gap_pending_ms = pending_ms;
                if (pending_ms >= (int32_t)source_packet_ms) {
                    if (s_audio_adaptive.source_gap_pending_packets < UINT8_MAX) {
                        s_audio_adaptive.source_gap_pending_packets++;
                    }
                } else {
                    s_audio_adaptive.source_gap_pending_packets = 0;
                }

                if (s_audio_adaptive.source_gap_pending_packets >=
                    MEDIA_SINK_AUDIO_SOURCE_GAP_CONFIRM_PACKETS) {
                    source_gap_ms = (uint32_t)pending_ms;
                    if (source_gap_ms > tuning.source_gap_fill_max_ms) {
                        source_gap_ms = tuning.source_gap_fill_max_ms;
                    }
                    source_gap_ms -= source_gap_ms % source_packet_ms;
                    s_audio_adaptive.source_gap_pending_ms -=
                        (int32_t)source_gap_ms;
                    s_audio_adaptive.source_gap_pending_packets = 0;
                }
            }
        }

        /* TiRTC can deliver several contiguous PCM packets in one callback
         * burst. Keep the first packet of that burst as the arrival baseline;
         * comparing every batched packet (0 ms wall time versus 40 ms media
         * time) would manufacture jitter that is not present on the network. */
        if (same_delivery_batch) {
            s_audio_adaptive.last_arrival_us = now_us;
            s_audio_adaptive.last_source_timestamp_ms = source_timestamp_ms;
            s_audio_adaptive.source_timestamp_valid = true;
            taskEXIT_CRITICAL(&s_sink_lock);
            return source_gap_ms;
        }

        /* Trusted RTC producers use media-clock gaps to mark a new talkspurt.
         * Explicit AI round hints take precedence so a delayed burst inside a
         * round remains measurable instead of being mistaken for new speech. */
        if (talkspurt_start) {
            s_audio_adaptive.fast_start_pending = tuning.fast_start_on_talkspurt;
            s_audio_adaptive.stable_windows = 0;
            s_audio_adaptive.first_arrival_us = now_us;
            s_audio_adaptive.first_play_delay_ms = 0;
            s_audio_adaptive.max_arrival_gap_ms = 0;
            s_audio_adaptive.talkspurt_events_in_window++;
            if (!source_delta_valid || source_delta_ms > talkspurt_gap_ms) {
                s_audio_jitter_boost_ms = 0;
            }
        } else {
            uint32_t expected_delta_ms = tuning.trust_source_timestamps && source_delta_valid ?
                                             source_delta_ms :
                                             source_packet_ms;
            uint32_t variation_ms = arrival_delta_ms > expected_delta_ms ?
                                        arrival_delta_ms - expected_delta_ms :
                                        expected_delta_ms - arrival_delta_ms;
            if (variation_ms > MEDIA_SINK_AUDIO_JITTER_SAMPLE_MAX_MS) {
                variation_ms = MEDIA_SINK_AUDIO_JITTER_SAMPLE_MAX_MS;
            }
            uint32_t delayed_margin_ms = source_packet_ms > MEDIA_SINK_AUDIO_PLAY_CHUNK_MS ?
                                             source_packet_ms :
                                             MEDIA_SINK_AUDIO_PLAY_CHUNK_MS;
            if (arrival_delta_ms > expected_delta_ms + delayed_margin_ms) {
                s_audio_adaptive.delayed_burst_events_in_window++;
                s_audio_adaptive.delayed_burst_events++;
                if (variation_ms > s_audio_adaptive.max_delivery_variation_ms_in_window) {
                    s_audio_adaptive.max_delivery_variation_ms_in_window = variation_ms;
                }
            }
            if (s_audio_adaptive.arrival_jitter_ewma_ms == 0U) {
                s_audio_adaptive.arrival_jitter_ewma_ms = variation_ms;
            } else {
                s_audio_adaptive.arrival_jitter_ewma_ms =
                    (s_audio_adaptive.arrival_jitter_ewma_ms * 7U + variation_ms) / 8U;
            }
            if (variation_ms > s_audio_adaptive.arrival_jitter_peak_ms) {
                s_audio_adaptive.arrival_jitter_peak_ms = variation_ms;
            }
        }
    }
    s_audio_adaptive.last_arrival_us = now_us;
    s_audio_adaptive.last_source_timestamp_ms = source_timestamp_ms;
    s_audio_adaptive.source_timestamp_valid = true;
    if (s_audio_adaptive.first_source_timestamp_ms != 0U) {
        uint32_t source_elapsed_ms =
            source_timestamp_ms - s_audio_adaptive.first_source_timestamp_ms;
        uint64_t expected_elapsed_ms =
            (uint64_t)(s_audio_adaptive.rx_packets - 1U) * source_packet_ms;
        int64_t clock_error_ms = (int64_t)source_elapsed_ms -
                                 (int64_t)expected_elapsed_ms;
        if (clock_error_ms > INT32_MAX) {
            clock_error_ms = INT32_MAX;
        } else if (clock_error_ms < INT32_MIN) {
            clock_error_ms = INT32_MIN;
        }
        s_audio_adaptive.source_clock_error_ms = (int32_t)clock_error_ms;
    }
    taskEXIT_CRITICAL(&s_sink_lock);
    return source_gap_ms;
}

static void media_sink_audio_reset_playback_clock(TickType_t *deadline_tick,
                                                  bool *clock_valid)
{
    if (deadline_tick != NULL) {
        *deadline_tick = 0;
    }
    if (clock_valid != NULL) {
        *clock_valid = false;
    }
}

static void media_sink_audio_pace_playback(TickType_t write_start_tick,
                                           uint32_t duration_ms,
                                           TickType_t *deadline_tick,
                                           bool *clock_valid)
{
    TickType_t period_ticks = pdMS_TO_TICKS(duration_ms);
    TickType_t now_tick = xTaskGetTickCount();

    if (deadline_tick == NULL || clock_valid == NULL || period_ticks == 0U) {
        return;
    }
    if (!*clock_valid) {
        *deadline_tick = write_start_tick;
        *clock_valid = true;
    }

    TickType_t next_tick = *deadline_tick + period_ticks;
    int32_t wait_ticks = (int32_t)(next_tick - now_tick);
    if (wait_ticks > 0) {
        TickType_t wait_start_tick = now_tick;
        vTaskDelay((TickType_t)wait_ticks);
        TickType_t waited_ticks = xTaskGetTickCount() - wait_start_tick;
        uint32_t waited_ms = (uint32_t)(waited_ticks * portTICK_PERIOD_MS);

        taskENTER_CRITICAL(&s_sink_lock);
        s_audio_adaptive.pacing_wait_events++;
        s_audio_adaptive.pacing_wait_ms += waited_ms;
        if (waited_ms > s_audio_adaptive.pacing_wait_max_ms) {
            s_audio_adaptive.pacing_wait_max_ms = waited_ms;
        }
        taskEXIT_CRITICAL(&s_sink_lock);
        *deadline_tick = next_tick;
        return;
    }

    uint32_t late_ms = (uint32_t)(-wait_ticks) * portTICK_PERIOD_MS;
    if (late_ms > 0U) {
        taskENTER_CRITICAL(&s_sink_lock);
        s_audio_adaptive.pacing_late_events++;
        s_audio_adaptive.pacing_late_ms += late_ms;
        if (late_ms > s_audio_adaptive.pacing_late_max_ms) {
            s_audio_adaptive.pacing_late_max_ms = late_ms;
        }
        taskEXIT_CRITICAL(&s_sink_lock);
    }

    /* Never run catch-up writes after a late scheduler wake-up. Rebase the
     * next chunk on the current hardware clock instead of consuming buffered
     * speech faster than real time. */
    *deadline_tick = now_tick;
}

static uint8_t *media_sink_alloc_audio_buffer(size_t size)
{
    /* These queues are latency storage, not DMA or cache-off data. Keep their
     * potentially large backing memory out of scarce internal RAM. */
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static esp_err_t media_sink_ensure_audio_playback_buffers(void)
{
    const audio_format_t *playback_format = speaker_get_playback_format();
    const size_t pcm_buffer_size =
        media_sink_audio_bytes_for_duration_ms(MEDIA_SINK_AUDIO_PCM_BUFFER_MS,
                                                        playback_format);
    const size_t chunk_buffer_size =
        media_sink_audio_bytes_for_duration_ms(MEDIA_SINK_AUDIO_PLAY_CHUNK_MS + 1U,
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

    if (s_audio_pcm_plc_buffer == NULL) {
        s_audio_pcm_plc_buffer = media_sink_alloc_audio_buffer(chunk_buffer_size);
        ESP_RETURN_ON_FALSE(s_audio_pcm_plc_buffer != NULL,
                            ESP_ERR_NO_MEM,
                            TAG,
                            "audio concealment buffer alloc failed");
    }

    return ESP_OK;
}

static void media_sink_reset_audio_pcm_buffer(void)
{
    taskENTER_CRITICAL(&s_sink_lock);
    s_audio_pcm_read_offset = 0;
    s_audio_pcm_used_bytes = 0;
    s_audio_pcm_integrity_read = 0;
    s_audio_pcm_integrity_count = 0;
    memset(s_audio_pcm_integrity, 0, sizeof(s_audio_pcm_integrity));
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
    uint32_t dropped_packets =
        (dropped_ms + MEDIA_SINK_AUDIO_PLAY_CHUNK_MS - 1U) / MEDIA_SINK_AUDIO_PLAY_CHUNK_MS;
    taskENTER_CRITICAL(&s_sink_lock);
    s_audio_adaptive.trim_drop_packets += dropped_packets;
    taskEXIT_CRITICAL(&s_sink_lock);

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
    const size_t play_chunk_bytes =
        media_sink_audio_bytes_for_duration_ms(MEDIA_SINK_AUDIO_PLAY_CHUNK_MS,
                                               playback_format);

    if (playback_format == NULL || s_audio_pcm_buffer_size == 0 || s_audio_pcm_used_bytes == 0 || drop_bytes == 0) {
        return 0;
    }

    if (drop_bytes > s_audio_pcm_used_bytes) {
        drop_bytes = s_audio_pcm_used_bytes;
    }

    s_audio_pcm_read_offset = (s_audio_pcm_read_offset + drop_bytes) % s_audio_pcm_buffer_size;
    s_audio_pcm_used_bytes -= drop_bytes;
    if (play_chunk_bytes > 0U) {
        media_sink_audio_integrity_drop_chunks_locked(
            (drop_bytes + play_chunk_bytes - 1U) / play_chunk_bytes);
    }
    return media_sink_audio_duration_ms_for_bytes(drop_bytes, playback_format);
}

static esp_err_t media_sink_append_audio_pcm(const uint8_t *data,
                                             size_t data_len,
                                             uint32_t source_packet_ms,
                                             uint32_t source_gap_ms,
                                             uint32_t generation,
                                             bool integrity_valid,
                                             uint32_t integrity_sequence)
{
    media_sink_audio_tuning_t tuning = media_sink_audio_get_tuning();
    const audio_format_t *playback_format = speaker_get_playback_format();
    const size_t play_chunk_bytes =
        media_sink_audio_bytes_for_duration_ms(MEDIA_SINK_AUDIO_PLAY_CHUNK_MS,
                                                         playback_format);
    const size_t latency_target_bytes =
        media_sink_audio_bytes_for_duration_ms(media_sink_audio_latency_target_ms(source_packet_ms),
                                                        playback_format);
    const size_t trim_hysteresis_bytes =
        media_sink_audio_bytes_for_duration_ms(tuning.trim_hysteresis_ms,
                                                        playback_format);
    const uint8_t *write_data = data;
    size_t gap_bytes = media_sink_audio_bytes_for_duration_ms(source_gap_ms,
                                                               playback_format);
    uint32_t trimmed_ms = 0;
    uint32_t buffered_ms = 0;

    ESP_RETURN_ON_FALSE(data != NULL && data_len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid audio pcm append");
    ESP_RETURN_ON_ERROR(media_sink_ensure_audio_playback_buffers(), TAG, "audio pcm buffer prepare failed");

    taskENTER_CRITICAL(&s_sink_lock);
    if (generation != s_generation) {
        taskEXIT_CRITICAL(&s_sink_lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (data_len > s_audio_pcm_buffer_size) {
        size_t trim_bytes = data_len - s_audio_pcm_buffer_size;

        write_data += trim_bytes;
        data_len = s_audio_pcm_buffer_size;
        trimmed_ms += media_sink_audio_duration_ms_for_bytes(trim_bytes, playback_format);
    }

    size_t append_bytes = data_len + gap_bytes;
    if (append_bytes > (s_audio_pcm_buffer_size - s_audio_pcm_used_bytes)) {
        size_t drop_bytes = append_bytes - (s_audio_pcm_buffer_size - s_audio_pcm_used_bytes);
        if (play_chunk_bytes > 0 && (drop_bytes % play_chunk_bytes) != 0) {
            drop_bytes += play_chunk_bytes - (drop_bytes % play_chunk_bytes);
        }
        trimmed_ms += media_sink_drop_audio_pcm_head(drop_bytes);
    }

    size_t write_offset = (s_audio_pcm_read_offset + s_audio_pcm_used_bytes) % s_audio_pcm_buffer_size;
    if (gap_bytes > 0U) {
        size_t first_gap = s_audio_pcm_buffer_size - write_offset;
        if (first_gap > gap_bytes) {
            first_gap = gap_bytes;
        }
        memset(s_audio_pcm_buffer + write_offset, 0, first_gap);
        if (gap_bytes > first_gap) {
            memset(s_audio_pcm_buffer, 0, gap_bytes - first_gap);
        }
        s_audio_pcm_used_bytes += gap_bytes;
        if (play_chunk_bytes > 0U) {
            media_sink_audio_integrity_append_locked(
                (gap_bytes + play_chunk_bytes - 1U) / play_chunk_bytes,
                false,
                0);
        }
        s_audio_adaptive.source_gap_fill_events++;
        s_audio_adaptive.source_gap_fill_ms += source_gap_ms;
        write_offset = (s_audio_pcm_read_offset + s_audio_pcm_used_bytes) %
                       s_audio_pcm_buffer_size;
    }
    size_t first_copy = s_audio_pcm_buffer_size - write_offset;
    if (first_copy > data_len) {
        first_copy = data_len;
    }
    memcpy(s_audio_pcm_buffer + write_offset, write_data, first_copy);
    if (data_len > first_copy) {
        memcpy(s_audio_pcm_buffer, write_data + first_copy, data_len - first_copy);
    }
    s_audio_pcm_used_bytes += data_len;
    if (play_chunk_bytes > 0U) {
        media_sink_audio_integrity_append_locked(
            (data_len + play_chunk_bytes - 1U) / play_chunk_bytes,
            integrity_valid,
            integrity_sequence);
    }
    if (source_packet_ms > 0U) {
        s_audio_last_source_packet_ms = source_packet_ms;
    }

    size_t trim_limit_bytes = latency_target_bytes + trim_hysteresis_bytes;
    if (s_audio_adaptive.playback_active &&
        latency_target_bytes > 0U &&
        s_audio_pcm_used_bytes > trim_limit_bytes) {
        /* The hysteresis is the allowed latency ceiling, not a license to
         * delete another half-window of valid speech after crossing it. A
         * recovered network burst can exceed the ceiling by one 20 ms chunk;
         * dropping back to the midpoint turned that small overshoot into a
         * 100-120 ms audible hole. Remove only the true excess and let the
         * existing playback clock drain the retained PSRAM-backed audio. */
        size_t drop_bytes = s_audio_pcm_used_bytes - trim_limit_bytes;
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

static esp_err_t media_sink_pop_audio_pcm_chunk(size_t chunk_bytes,
                                                bool allow_short_tail,
                                                uint8_t **chunk_data,
                                                size_t *media_bytes,
                                                uint32_t generation,
                                                bool *integrity_valid,
                                                uint32_t *integrity_sequence)
{
    ESP_RETURN_ON_FALSE(chunk_data != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid audio chunk output");
    ESP_RETURN_ON_FALSE(media_bytes != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid audio media size output");
    ESP_RETURN_ON_ERROR(media_sink_ensure_audio_playback_buffers(), TAG, "audio chunk buffer prepare failed");
    ESP_RETURN_ON_FALSE(chunk_bytes <= s_audio_pcm_chunk_buffer_size,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "audio chunk too large");

    taskENTER_CRITICAL(&s_sink_lock);
    if (generation != s_generation) {
        taskEXIT_CRITICAL(&s_sink_lock);
        return ESP_ERR_INVALID_STATE;
    }
    size_t copy_bytes = chunk_bytes;
    if (copy_bytes > s_audio_pcm_used_bytes) {
        if (!allow_short_tail || s_audio_pcm_used_bytes == 0U) {
            taskEXIT_CRITICAL(&s_sink_lock);
            return ESP_ERR_NOT_FOUND;
        }
        copy_bytes = s_audio_pcm_used_bytes;
    }
    if (copy_bytes == 0U) {
        taskEXIT_CRITICAL(&s_sink_lock);
        return ESP_ERR_NOT_FOUND;
    }

    size_t first_copy = s_audio_pcm_buffer_size - s_audio_pcm_read_offset;
    if (first_copy > copy_bytes) {
        first_copy = copy_bytes;
    }
    memcpy(s_audio_pcm_chunk_buffer, s_audio_pcm_buffer + s_audio_pcm_read_offset, first_copy);
    if (copy_bytes > first_copy) {
        memcpy(s_audio_pcm_chunk_buffer + first_copy, s_audio_pcm_buffer, copy_bytes - first_copy);
    }
    if (copy_bytes < chunk_bytes) {
        memset(s_audio_pcm_chunk_buffer + copy_bytes, 0, chunk_bytes - copy_bytes);
    }

    s_audio_pcm_read_offset = (s_audio_pcm_read_offset + copy_bytes) % s_audio_pcm_buffer_size;
    s_audio_pcm_used_bytes -= copy_bytes;
    bool popped_integrity_valid =
        media_sink_audio_integrity_pop_locked(integrity_sequence);
    taskEXIT_CRITICAL(&s_sink_lock);

    *chunk_data = s_audio_pcm_chunk_buffer;
    *media_bytes = copy_bytes;
    if (integrity_valid != NULL) {
        *integrity_valid = popped_integrity_valid;
    }
    return ESP_OK;
}

static bool media_sink_prepare_audio_concealment(size_t chunk_bytes,
                                                 uint32_t concealed_ms,
                                                 const audio_format_t *format,
                                                 uint8_t **chunk_data)
{
    ESP_RETURN_ON_FALSE(chunk_data != NULL && format != NULL,
                        false,
                        TAG,
                        "invalid concealment output");
    ESP_RETURN_ON_FALSE(s_audio_pcm_plc_buffer != NULL &&
                            s_audio_pcm_chunk_buffer != NULL &&
                            chunk_bytes <= s_audio_pcm_chunk_buffer_size &&
                            format->bits_per_sample == 16U &&
                            format->channels > 0U,
                        false,
                        TAG,
                        "audio concealment buffer unavailable");

    const int16_t *source = (const int16_t *)s_audio_pcm_plc_buffer;
    int16_t *output = (int16_t *)s_audio_pcm_chunk_buffer;
    size_t sample_count = chunk_bytes / sizeof(int16_t);
    size_t frame_count = sample_count / format->channels;

    memset(output, 0, chunk_bytes);
    if (concealed_ms == 0U && frame_count > 0U) {
        size_t fade_frames = ((size_t)format->sample_rate_hz * 5U) / 1000U;
        if (fade_frames == 0U) {
            fade_frames = 1U;
        }
        if (fade_frames > frame_count) {
            fade_frames = frame_count;
        }

        size_t source_base = (frame_count - 1U) * format->channels;
        for (size_t frame = 0; frame < fade_frames; ++frame) {
            int32_t remaining = (int32_t)(fade_frames - frame);
            for (uint8_t channel = 0; channel < format->channels; ++channel) {
                int16_t tail = source[source_base + channel];
                output[frame * format->channels + channel] =
                    (int16_t)(((int32_t)tail * remaining) / (int32_t)fade_frames);
            }
        }
    }
    *chunk_data = s_audio_pcm_chunk_buffer;
    return true;
}

static int8_t media_sink_audio_playout_adjustment_frames(
    const media_sink_audio_tuning_t *tuning,
    uint32_t buffered_ms,
    uint32_t target_ms,
    bool allow_fast)
{
    if (tuning == NULL || tuning->playout_slow_frames == 0U ||
        tuning->playout_fast_frames == 0U) {
        return 0;
    }

    uint32_t low_enter_ms = target_ms > tuning->playout_low_margin_ms ?
                                target_ms - tuning->playout_low_margin_ms : 0U;
    uint32_t low_release_ms = target_ms > MEDIA_SINK_AUDIO_PLAYOUT_RELEASE_MARGIN_MS ?
                                  target_ms - MEDIA_SINK_AUDIO_PLAYOUT_RELEASE_MARGIN_MS : 0U;
    uint32_t high_enter_ms = target_ms + tuning->playout_high_margin_ms;
    uint32_t high_release_ms = target_ms + MEDIA_SINK_AUDIO_PLAYOUT_RELEASE_MARGIN_MS;
    int8_t mode = 0;
    bool enabled = false;

    taskENTER_CRITICAL(&s_sink_lock);
    enabled = s_audio_profile == MEDIA_SINK_AUDIO_PROFILE_DEVICE_CALL &&
              !s_audio_integrity.sequence_valid;
    mode = enabled ? s_audio_adaptive.playout_rate_mode : 0;
    if (mode < 0) {
        if (buffered_ms >= low_release_ms) {
            mode = 0;
        }
    } else if (mode > 0) {
        if (buffered_ms <= high_release_ms) {
            mode = 0;
        }
    } else if (buffered_ms <= low_enter_ms) {
        mode = -1;
    } else if (allow_fast && buffered_ms >= high_enter_ms) {
        mode = 1;
    }
    s_audio_adaptive.playout_rate_mode = mode;
    taskEXIT_CRITICAL(&s_sink_lock);

    if (mode < 0) {
        return -(int8_t)tuning->playout_slow_frames;
    }
    if (mode > 0) {
        return (int8_t)tuning->playout_fast_frames;
    }
    return 0;
}

static size_t media_sink_resample_audio_chunk(int16_t *samples,
                                              size_t source_bytes,
                                              size_t output_bytes,
                                              const audio_format_t *format)
{
    if (samples == NULL || format == NULL || format->bits_per_sample != 16U ||
        format->channels == 0U || output_bytes > s_audio_pcm_chunk_buffer_size) {
        return source_bytes;
    }

    const size_t frame_bytes = sizeof(int16_t) * format->channels;
    const size_t source_frames = source_bytes / frame_bytes;
    const size_t output_frames = output_bytes / frame_bytes;

    if (source_frames < 2U || output_frames < 2U ||
        source_frames * frame_bytes != source_bytes ||
        output_frames * frame_bytes != output_bytes) {
        return source_bytes;
    }
    if (source_frames == output_frames) {
        return output_bytes;
    }

    /* Expansion runs backwards and contraction runs forwards so this can use
     * the existing PSRAM chunk buffer in place. The I2S side still receives
     * exactly one 20 ms chunk; only the amount consumed from the jitter ring
     * changes by 1.25 percent. */
    if (output_frames > source_frames) {
        for (size_t output_frame = output_frames; output_frame-- > 0U;) {
            uint64_t position_num =
                (uint64_t)output_frame * (source_frames - 1U);
            size_t left_frame = (size_t)(position_num / (output_frames - 1U));
            uint32_t fraction_num =
                (uint32_t)(position_num % (output_frames - 1U));
            for (uint8_t channel = 0; channel < format->channels; ++channel) {
                size_t left_index = left_frame * format->channels + channel;
                size_t right_frame = left_frame + 1U < source_frames ?
                                         left_frame + 1U : left_frame;
                size_t right_index = right_frame * format->channels + channel;
                int32_t left = samples[left_index];
                int32_t right = samples[right_index];
                samples[output_frame * format->channels + channel] =
                    (int16_t)(left +
                              (int32_t)(((int64_t)(right - left) * fraction_num) /
                                        (int64_t)(output_frames - 1U)));
            }
        }
    } else {
        for (size_t output_frame = 0; output_frame < output_frames; ++output_frame) {
            uint64_t position_num =
                (uint64_t)output_frame * (source_frames - 1U);
            size_t left_frame = (size_t)(position_num / (output_frames - 1U));
            uint32_t fraction_num =
                (uint32_t)(position_num % (output_frames - 1U));
            for (uint8_t channel = 0; channel < format->channels; ++channel) {
                size_t left_index = left_frame * format->channels + channel;
                size_t right_frame = left_frame + 1U < source_frames ?
                                         left_frame + 1U : left_frame;
                size_t right_index = right_frame * format->channels + channel;
                int32_t left = samples[left_index];
                int32_t right = samples[right_index];
                samples[output_frame * format->channels + channel] =
                    (int16_t)(left +
                              (int32_t)(((int64_t)(right - left) * fraction_num) /
                                        (int64_t)(output_frames - 1U)));
            }
        }
    }
    return output_bytes;
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

static void media_sink_audio_apply_fade_in(int16_t *samples,
                                           size_t data_len,
                                           const audio_format_t *format)
{
    if (samples == NULL || format == NULL || format->channels == 0U ||
        format->bits_per_sample != 16U || format->sample_rate_hz == 0U) {
        return;
    }

    size_t frame_count = data_len / (sizeof(int16_t) * format->channels);
    size_t fade_frames = ((size_t)format->sample_rate_hz * MEDIA_SINK_AUDIO_FADE_IN_MS) / 1000U;
    if (fade_frames > frame_count) {
        fade_frames = frame_count;
    }
    if (fade_frames == 0U) {
        return;
    }

    for (size_t frame = 0; frame < fade_frames; ++frame) {
        int32_t gain_q15 = (int32_t)(((frame + 1U) * 32767U) / fade_frames);
        for (uint8_t channel = 0; channel < format->channels; ++channel) {
            size_t sample_index = frame * format->channels + channel;
            samples[sample_index] = (int16_t)(((int32_t)samples[sample_index] * gain_q15) / 32767);
        }
    }
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

static uint32_t media_sink_drop_oldest_audio(void)
{
    media_sink_audio_packet_t stale = {0};

    if (s_audio_queue != NULL && xQueueReceive(s_audio_queue, &stale, 0) == pdTRUE) {
        uint32_t dropped_ms = media_sink_audio_packet_duration_us(&stale) / 1000U;
        if (stale.integrity_valid) {
            taskENTER_CRITICAL(&s_sink_lock);
            s_audio_integrity.queue_dropped_frames++;
            taskEXIT_CRITICAL(&s_sink_lock);
        }
        media_sink_free_audio_packet(&stale);
        return dropped_ms;
    }
    return 0;
}

static void media_sink_note_audio_queue_drop(uint32_t dropped_ms)
{
    if (dropped_ms == 0U) {
        return;
    }

    s_audio_queue_drop_packets_in_window++;
    s_audio_queue_drop_ms_in_window += dropped_ms;
    taskENTER_CRITICAL(&s_sink_lock);
    s_audio_adaptive.queue_drop_packets++;
    taskEXIT_CRITICAL(&s_sink_lock);

    TickType_t now = xTaskGetTickCount();
    if (s_last_audio_trim_log_tick == 0 ||
        now - s_last_audio_trim_log_tick >= pdMS_TO_TICKS(1000)) {
        const audio_format_t *playback_format = speaker_get_playback_format();

        s_last_audio_trim_log_tick = now;
        ESP_LOGW(TAG,
                 "remote audio queue full: dropped_oldest_ms=%u queued=%u buffered_ms=%u",
                 (unsigned)dropped_ms,
                 (unsigned)uxQueueMessagesWaiting(s_audio_queue),
                 (unsigned)media_sink_audio_duration_ms_for_bytes(
                     media_sink_get_audio_pcm_used_bytes(), playback_format));
    }
}

static void media_sink_maybe_log_audio_rate(void)
{
    TickType_t now = xTaskGetTickCount();
    uint32_t underflow_events = 0;
    uint32_t underflow_active_events = 0;
    uint32_t max_underflow_arrival_gap_ms = 0;
    uint32_t jitter_ewma_ms = 0;
    uint32_t jitter_peak_ms = 0;
    uint32_t jitter_boost_ms = 0;
    uint32_t fast_start_events = 0;
    uint32_t delayed_burst_events = 0;
    uint32_t talkspurt_events = 0;
    uint32_t max_delivery_variation_ms = 0;
    if (s_last_audio_rate_log_tick == 0) {
        s_last_audio_rate_log_tick = now;
        return;
    }
    if (now - s_last_audio_rate_log_tick < pdMS_TO_TICKS(1000)) {
        return;
    }

    taskENTER_CRITICAL(&s_sink_lock);
    underflow_events = s_audio_adaptive.underflow_events_in_window;
    underflow_active_events = s_audio_adaptive.underflow_active_events_in_window;
    max_underflow_arrival_gap_ms =
        s_audio_adaptive.max_underflow_arrival_gap_ms_in_window;
    jitter_ewma_ms = s_audio_adaptive.arrival_jitter_ewma_ms;
    jitter_peak_ms = s_audio_adaptive.arrival_jitter_peak_ms;
    jitter_boost_ms = s_audio_jitter_boost_ms;
    fast_start_events = s_audio_adaptive.fast_start_events_in_window;
    delayed_burst_events = s_audio_adaptive.delayed_burst_events_in_window;
    talkspurt_events = s_audio_adaptive.talkspurt_events_in_window;
    max_delivery_variation_ms = s_audio_adaptive.max_delivery_variation_ms_in_window;
    taskEXIT_CRITICAL(&s_sink_lock);

    bool material_pressure = s_audio_play_drop_packets_in_window > 0 ||
                             s_audio_queue_drop_packets_in_window > 0 ||
                             s_audio_trim_drop_packets_in_window > 0 ||
                             underflow_active_events >= 3U ||
                             max_underflow_arrival_gap_ms >= 100U;
    if (material_pressure) {
        uint32_t source_packet_ms = media_sink_get_last_source_packet_ms();

        ESP_LOGW(TAG,
                 "remote audio pressure: rx=%up/%ums played=%up/%ums play_drop=%up/%ums queue_drop=%up/%ums trim_drop=%up/%ums underflow=%u active=%u gap_ms=%u fast_start=%u delayed_burst=%u/%ums talkspurt=%u queued=%u buffered_ms=%u prebuffer_ms=%u target_ms=%u jitter=%u/%ums boost_ms=%u",
                 (unsigned)s_audio_rx_packets_in_window,
                 (unsigned)s_audio_rx_ms_in_window,
                 (unsigned)s_audio_play_ok_packets_in_window,
                 (unsigned)s_audio_play_ok_ms_in_window,
                 (unsigned)s_audio_play_drop_packets_in_window,
                 (unsigned)s_audio_play_drop_ms_in_window,
                 (unsigned)s_audio_queue_drop_packets_in_window,
                 (unsigned)s_audio_queue_drop_ms_in_window,
                 (unsigned)s_audio_trim_drop_packets_in_window,
                 (unsigned)s_audio_trim_drop_ms_in_window,
                 (unsigned)underflow_events,
                 (unsigned)underflow_active_events,
                 (unsigned)max_underflow_arrival_gap_ms,
                 (unsigned)fast_start_events,
                 (unsigned)delayed_burst_events,
                 (unsigned)max_delivery_variation_ms,
                 (unsigned)talkspurt_events,
                 (unsigned)uxQueueMessagesWaiting(s_audio_queue),
                 (unsigned)media_sink_audio_duration_ms_for_bytes(
                      media_sink_get_audio_pcm_used_bytes(), speaker_get_playback_format()),
                 (unsigned)media_sink_audio_prebuffer_ms(source_packet_ms),
                 (unsigned)media_sink_audio_latency_target_ms(source_packet_ms),
                 (unsigned)jitter_ewma_ms,
                 (unsigned)jitter_peak_ms,
                 (unsigned)jitter_boost_ms);
    } else if (underflow_events > 0) {
        ESP_LOGD(TAG,
                 "remote audio transient: underflow=%u active=%u gap_ms=%u buffered_ms=%u",
                 (unsigned)underflow_events,
                 (unsigned)underflow_active_events,
                 (unsigned)max_underflow_arrival_gap_ms,
                 (unsigned)media_sink_audio_duration_ms_for_bytes(
                     media_sink_get_audio_pcm_used_bytes(), speaker_get_playback_format()));
    } else if (s_audio_rx_packets_in_window > 0 && s_audio_play_ok_packets_in_window == 0) {
        ESP_LOGI(TAG,
                 "remote audio buffering: rx=%up/%ums queued=%u buffered_ms=%u prebuffer_ms=%u",
                 (unsigned)s_audio_rx_packets_in_window,
                 (unsigned)s_audio_rx_ms_in_window,
                 (unsigned)uxQueueMessagesWaiting(s_audio_queue),
                 (unsigned)media_sink_audio_duration_ms_for_bytes(
                     media_sink_get_audio_pcm_used_bytes(), speaker_get_playback_format()),
                 (unsigned)media_sink_audio_prebuffer_ms(media_sink_get_last_source_packet_ms()));
    } else if (s_audio_rx_packets_in_window > 0 && s_audio_play_ok_packets_in_window > 0) {
        media_sink_audio_note_stable_window();
#if CONFIG_APP_DEBUG_SCREEN_SERVER_ENABLE
        ESP_LOGI(TAG,
#else
        ESP_LOGD(TAG,
#endif
                 "remote audio steady: rx=%up/%ums played=%up/%ums queued=%u buffered_ms=%u jitter=%u/%ums boost_ms=%u",
                 (unsigned)s_audio_rx_packets_in_window,
                 (unsigned)s_audio_rx_ms_in_window,
                 (unsigned)s_audio_play_ok_packets_in_window,
                 (unsigned)s_audio_play_ok_ms_in_window,
                 (unsigned)uxQueueMessagesWaiting(s_audio_queue),
                 (unsigned)media_sink_audio_duration_ms_for_bytes(
                      media_sink_get_audio_pcm_used_bytes(), speaker_get_playback_format()),
                 (unsigned)jitter_ewma_ms,
                 (unsigned)jitter_peak_ms,
                 (unsigned)jitter_boost_ms);
    }

    s_audio_rx_packets_in_window = 0;
    s_audio_rx_ms_in_window = 0;
    s_audio_play_ok_packets_in_window = 0;
    s_audio_play_ok_ms_in_window = 0;
    s_audio_play_drop_packets_in_window = 0;
    s_audio_play_drop_ms_in_window = 0;
    s_audio_trim_drop_packets_in_window = 0;
    s_audio_trim_drop_ms_in_window = 0;
    s_audio_queue_drop_packets_in_window = 0;
    s_audio_queue_drop_ms_in_window = 0;
    taskENTER_CRITICAL(&s_sink_lock);
    s_audio_adaptive.underflow_events_in_window = 0;
    s_audio_adaptive.underflow_active_events_in_window = 0;
    s_audio_adaptive.max_underflow_arrival_gap_ms_in_window = 0;
    s_audio_adaptive.fast_start_events_in_window = 0;
    s_audio_adaptive.delayed_burst_events_in_window = 0;
    s_audio_adaptive.talkspurt_events_in_window = 0;
    s_audio_adaptive.max_delivery_variation_ms_in_window = 0;
    taskEXIT_CRITICAL(&s_sink_lock);
    s_last_audio_rate_log_tick = now;
}

static esp_err_t media_sink_queue_audio(media_sink_audio_packet_t *packet)
{
    if (packet == NULL || s_audio_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(s_audio_queue, packet, 0) == pdTRUE) {
        memset(packet, 0, sizeof(*packet));
        return ESP_OK;
    }

    media_sink_note_audio_queue_drop(media_sink_drop_oldest_audio());
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

    if (packet->generation != media_sink_get_generation()) {
        media_sink_free_audio_packet(packet);
        return;
    }

    esp_err_t append_ret = media_sink_append_audio_pcm((const uint8_t *)rendered_samples,
                                                       rendered_bytes,
                                                       packet_duration_us / 1000U,
                                                       packet->source_gap_ms,
                                                       packet->generation,
                                                       packet->integrity_valid,
                                                       packet->integrity_sequence);
    TickType_t now = xTaskGetTickCount();
    if (!s_remote_audio_render_logged) {
        s_remote_audio_render_logged = true;
        s_last_audio_render_log_tick = now;
        ESP_LOGD(TAG,
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
    } else {
        media_sink_note_buffered_audio_integrity(packet);
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
    bool rebuffering_after_underflow = false;
    bool fade_in_pending = false;
    bool plc_valid = false;
    uint32_t concealed_ms = 0;
    TickType_t playback_deadline_tick = 0;
    TickType_t playback_started_tick = 0;
    bool playback_clock_valid = false;
    uint32_t playback_generation = media_sink_get_generation();
    uint32_t playback_talkspurt_generation = media_sink_audio_talkspurt_generation();
    media_sink_audio_packet_t packet = {0};

    while (true) {
        uint32_t current_generation = media_sink_get_generation();
        if (current_generation != playback_generation) {
            playback_generation = current_generation;
            playback_started = false;
            media_sink_audio_set_playback_active(false);
            rebuffering_after_underflow = false;
            fade_in_pending = false;
            plc_valid = false;
            concealed_ms = 0;
            media_sink_audio_reset_playback_clock(&playback_deadline_tick,
                                                  &playback_clock_valid);
        }

        uint32_t current_talkspurt_generation = media_sink_audio_talkspurt_generation();
        if (current_talkspurt_generation != playback_talkspurt_generation) {
            playback_talkspurt_generation = current_talkspurt_generation;
            rebuffering_after_underflow = false;
            taskENTER_CRITICAL(&s_sink_lock);
            s_audio_adaptive.playout_rate_mode = 0;
            taskEXIT_CRITICAL(&s_sink_lock);
            media_sink_audio_reset_playback_clock(&playback_deadline_tick,
                                                  &playback_clock_valid);
        }

        size_t buffered_bytes = media_sink_get_audio_pcm_used_bytes();
        uint32_t source_packet_ms = media_sink_get_last_source_packet_ms();
        size_t refill_target_bytes = media_sink_audio_bytes_for_duration_ms(
            media_sink_audio_latency_target_ms(source_packet_ms), playback_format);
        media_sink_audio_tuning_t tuning = media_sink_audio_get_tuning();
        bool should_receive = tuning.absorb_network_bursts ||
                              !playback_started ||
                              refill_target_bytes == 0U ||
                              buffered_bytes < refill_target_bytes;
        BaseType_t received_packet = pdFALSE;
        bool underflow_grace_wait = false;
        bool conceal_underflow = false;

        if (should_receive) {
            TickType_t receive_wait_ticks = playback_started ? 0 : portMAX_DELAY;

            /* round_end is asynchronous to this task. Do not sleep forever
             * with a short final tail that can never reach normal prebuffer. */
            if (!playback_started && buffered_bytes > 0U) {
                receive_wait_ticks = pdMS_TO_TICKS(MEDIA_SINK_AUDIO_PLAY_CHUNK_MS);
            } else if (playback_started && buffered_bytes < play_chunk_bytes &&
                       tuning.underflow_grace_max_ms > 0U) {
                uint32_t grace_ms = source_packet_ms * 4U;

                if (grace_ms < MEDIA_SINK_AUDIO_PLAY_CHUNK_MS) {
                    grace_ms = MEDIA_SINK_AUDIO_PLAY_CHUNK_MS;
                }
                if (grace_ms > tuning.underflow_grace_max_ms) {
                    grace_ms = tuning.underflow_grace_max_ms;
                }
                receive_wait_ticks = pdMS_TO_TICKS(grace_ms);
                underflow_grace_wait = true;
                taskENTER_CRITICAL(&s_sink_lock);
                s_audio_adaptive.underflow_grace_waits++;
                taskEXIT_CRITICAL(&s_sink_lock);
            }
            received_packet = xQueueReceive(s_audio_queue,
                                            &packet,
                                            receive_wait_ticks);
        }

        if (received_packet == pdTRUE) {
            uint8_t drained_packets = 0;

            playback_generation = packet.generation;
            media_sink_buffer_audio_packet(&packet);
            while (drained_packets < tuning.drain_burst_max &&
                   xQueueReceive(s_audio_queue, &packet, 0) == pdTRUE) {
                playback_generation = packet.generation;
                media_sink_buffer_audio_packet(&packet);
                drained_packets++;
            }
            if (underflow_grace_wait) {
                taskENTER_CRITICAL(&s_sink_lock);
                s_audio_adaptive.underflow_grace_recoveries++;
                taskEXIT_CRITICAL(&s_sink_lock);
            }
        }

        buffered_bytes = media_sink_get_audio_pcm_used_bytes();
        source_packet_ms = media_sink_get_last_source_packet_ms();
        current_talkspurt_generation = media_sink_audio_talkspurt_generation();
        if (current_talkspurt_generation != playback_talkspurt_generation) {
            playback_talkspurt_generation = current_talkspurt_generation;
            rebuffering_after_underflow = false;
        }
        uint32_t prebuffer_ms = rebuffering_after_underflow ?
                                    media_sink_audio_rebuffer_ms(source_packet_ms) :
                                    media_sink_audio_prebuffer_ms(source_packet_ms);
        size_t prebuffer_bytes =
            media_sink_audio_bytes_for_duration_ms(prebuffer_ms, playback_format);
        if (prebuffer_bytes == 0U) {
            prebuffer_bytes = play_chunk_bytes;
        }

        /* round_end is delivered on the command path and can overtake audio
         * packets on the media path. It means generation ended, not that all
         * PCM has arrived. A short tail may drain only after the media path
         * has stayed quiet for a bounded guard interval. */
        bool drain_completed_talkspurt =
            media_sink_audio_completed_talkspurt_ready(source_packet_ms,
                                                        buffered_bytes,
                                                        playback_format);
        if (!playback_started &&
            (buffered_bytes >= prebuffer_bytes ||
             drain_completed_talkspurt)) {
            playback_started = true;
            playback_started_tick = xTaskGetTickCount();
            media_sink_audio_set_playback_active(true);
            rebuffering_after_underflow = false;
            fade_in_pending = tuning.absorb_network_bursts;
            media_sink_audio_reset_playback_clock(&playback_deadline_tick,
                                                  &playback_clock_valid);
            media_sink_audio_consume_fast_start();
        }

        if (playback_started && buffered_bytes < play_chunk_bytes &&
            !drain_completed_talkspurt) {
            if (plc_valid && tuning.underflow_concealment_max_ms > concealed_ms) {
                conceal_underflow = true;
                if (concealed_ms == 0U) {
                    media_sink_audio_note_underflow();
                    taskENTER_CRITICAL(&s_sink_lock);
                    s_audio_adaptive.concealment_events++;
                    taskEXIT_CRITICAL(&s_sink_lock);
                }
            } else {
                playback_started = false;
                media_sink_audio_set_playback_active(false);
                fade_in_pending = false;
                media_sink_audio_reset_playback_clock(&playback_deadline_tick,
                                                      &playback_clock_valid);
            }
            if (conceal_underflow) {
                /* Keep the hardware playback clock running across a bounded
                 * late/missing packet. The next real chunk resets this state. */
            } else if (tuning.absorb_network_bursts) {
                media_sink_audio_mark_playback_idle();
                rebuffering_after_underflow = false;
            } else if (media_sink_audio_talkspurt_ended()) {
                /* A completed AI response is not a network recovery event. */
                media_sink_audio_mark_talkspurt_idle();
                rebuffering_after_underflow = false;
            } else {
                media_sink_audio_note_underflow();
                rebuffering_after_underflow = true;
            }
        }

        if (playback_started &&
            (buffered_bytes >= play_chunk_bytes || drain_completed_talkspurt ||
             conceal_underflow)) {
            uint8_t *play_chunk = NULL;
            size_t played_media_bytes = 0;
            size_t consume_bytes = play_chunk_bytes;
            uint32_t buffered_ms_before_play =
                media_sink_audio_duration_ms_for_bytes(buffered_bytes, playback_format);
            uint32_t played_media_ms = 0;
            size_t playback_write_bytes = play_chunk_bytes;
            uint32_t output_level = 0;
            int64_t play_start_us = 0;
            int64_t play_elapsed_us = 0;
            TickType_t write_start_tick = 0;
            esp_err_t play_ret = ESP_OK;
            bool integrity_valid = false;
            uint32_t integrity_sequence = 0;
            int8_t playout_adjustment_frames = 0;

            if (conceal_underflow) {
                if (!media_sink_prepare_audio_concealment(play_chunk_bytes,
                                                          concealed_ms,
                                                          playback_format,
                                                          &play_chunk)) {
                    play_ret = ESP_ERR_INVALID_STATE;
                }
            } else {
                if (!drain_completed_talkspurt) {
                    uint32_t current_target_ms =
                        media_sink_audio_latency_target_ms(source_packet_ms);
                    bool allow_fast = tuning.playout_startup_hold_ms == 0U ||
                                      playback_started_tick == 0U ||
                                      (xTaskGetTickCount() - playback_started_tick) >=
                                          pdMS_TO_TICKS(tuning.playout_startup_hold_ms);
                    playout_adjustment_frames =
                        media_sink_audio_playout_adjustment_frames(
                            &tuning,
                            buffered_ms_before_play,
                            current_target_ms,
                            allow_fast);
                    size_t frame_bytes = media_sink_audio_frame_bytes(playback_format);
                    size_t output_frames = frame_bytes > 0U ?
                                               play_chunk_bytes / frame_bytes : 0U;
                    int32_t source_frames =
                        (int32_t)output_frames + playout_adjustment_frames;
                    if (source_frames >= 2 && frame_bytes > 0U) {
                        size_t adjusted_bytes = (size_t)source_frames * frame_bytes;
                        if (adjusted_bytes <= s_audio_pcm_chunk_buffer_size &&
                            adjusted_bytes <= buffered_bytes) {
                            consume_bytes = adjusted_bytes;
                        } else {
                            playout_adjustment_frames = 0;
                        }
                    } else {
                        playout_adjustment_frames = 0;
                    }
                }

                play_ret = media_sink_pop_audio_pcm_chunk(consume_bytes,
                                                          drain_completed_talkspurt,
                                                          &play_chunk,
                                                          &played_media_bytes,
                                                          playback_generation,
                                                          &integrity_valid,
                                                          &integrity_sequence);
                if (play_ret == ESP_OK && played_media_bytes == consume_bytes &&
                    playout_adjustment_frames != 0) {
                    playback_write_bytes = media_sink_resample_audio_chunk(
                        (int16_t *)play_chunk,
                        played_media_bytes,
                        play_chunk_bytes,
                        playback_format);
                    if (playback_write_bytes != play_chunk_bytes) {
                        play_ret = ESP_ERR_INVALID_SIZE;
                    } else {
                        taskENTER_CRITICAL(&s_sink_lock);
                        if (playout_adjustment_frames < 0) {
                            s_audio_adaptive.clock_recovery_events++;
                            s_audio_adaptive.clock_recovery_frames +=
                                (uint32_t)(-playout_adjustment_frames);
                        } else {
                            s_audio_adaptive.clock_recovery_fast_events++;
                            s_audio_adaptive.clock_recovery_fast_frames +=
                                (uint32_t)playout_adjustment_frames;
                        }
                        taskEXIT_CRITICAL(&s_sink_lock);
                        /* Statistics and final-tail detection describe the
                         * fixed 20 ms hardware output, not ring consumption. */
                        played_media_bytes = play_chunk_bytes;
                    }
                }
                if (play_ret == ESP_OK && played_media_bytes == play_chunk_bytes) {
                    memcpy(s_audio_pcm_plc_buffer, play_chunk, play_chunk_bytes);
                    plc_valid = true;
                    concealed_ms = 0;
                }
            }

            if (play_ret != ESP_OK) {
                playback_started = false;
                media_sink_audio_set_playback_active(false);
                media_sink_audio_reset_playback_clock(&playback_deadline_tick,
                                                      &playback_clock_valid);
                continue;
            }

            if (playback_generation != media_sink_get_generation()) {
                if (integrity_valid) {
                    taskENTER_CRITICAL(&s_sink_lock);
                    s_audio_integrity.play_failures++;
                    taskEXIT_CRITICAL(&s_sink_lock);
                }
                playback_started = false;
                media_sink_audio_set_playback_active(false);
                media_sink_audio_reset_playback_clock(&playback_deadline_tick,
                                                      &playback_clock_valid);
                continue;
            }

            played_media_ms = media_sink_audio_duration_ms_for_bytes(played_media_bytes,
                                                                      playback_format);
            output_level = media_sink_audio_level_percent((const int16_t *)play_chunk,
                                                           playback_write_bytes);
            if (fade_in_pending) {
                media_sink_audio_apply_fade_in((int16_t *)play_chunk,
                                               playback_write_bytes,
                                               playback_format);
                output_level = media_sink_audio_level_percent((const int16_t *)play_chunk,
                                                               playback_write_bytes);
                fade_in_pending = false;
            }
            play_start_us = esp_timer_get_time();
            write_start_tick = xTaskGetTickCount();
            play_ret = speaker_write_rendered_pcm((int16_t *)play_chunk,
                                                              playback_write_bytes,
                                                              output_level);
            play_elapsed_us = esp_timer_get_time() - play_start_us;
            audio_playback_timing_t playback_timing = {0};
            speaker_get_last_playback_timing(&playback_timing);
            if (play_ret == ESP_ERR_TIMEOUT) {
                s_audio_play_drop_packets_in_window++;
                s_audio_play_drop_ms_in_window += MEDIA_SINK_AUDIO_PLAY_CHUNK_MS;
                taskENTER_CRITICAL(&s_sink_lock);
                s_audio_adaptive.play_drop_packets++;
                if (integrity_valid) {
                    s_audio_integrity.play_failures++;
                }
                taskEXIT_CRITICAL(&s_sink_lock);
                media_sink_audio_reset_playback_clock(&playback_deadline_tick,
                                                      &playback_clock_valid);
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
                if (integrity_valid) {
                    taskENTER_CRITICAL(&s_sink_lock);
                    s_audio_integrity.play_failures++;
                    taskEXIT_CRITICAL(&s_sink_lock);
                }
                ESP_LOGW(TAG,
                         "remote audio playback failed: ret=%d chunk_ms=%u buffered_ms=%u",
                         play_ret,
                         MEDIA_SINK_AUDIO_PLAY_CHUNK_MS,
                         (unsigned)buffered_ms_before_play);
                playback_started = false;
                media_sink_audio_set_playback_active(false);
                media_sink_audio_reset_playback_clock(&playback_deadline_tick,
                                                      &playback_clock_valid);
            } else {
                uint32_t first_play_delay_ms = 0;
                taskENTER_CRITICAL(&s_sink_lock);
                if (conceal_underflow) {
                    s_audio_adaptive.concealed_ms += MEDIA_SINK_AUDIO_PLAY_CHUNK_MS;
                } else {
                    s_audio_adaptive.played_packets++;
                    s_audio_adaptive.played_ms += played_media_ms;
                }
                if (s_audio_adaptive.first_play_delay_ms == 0U &&
                    s_audio_adaptive.first_arrival_us > 0) {
                    int64_t first_play_elapsed_us =
                        esp_timer_get_time() - s_audio_adaptive.first_arrival_us;
                    if (first_play_elapsed_us > 0) {
                        s_audio_adaptive.first_play_delay_ms =
                            (uint32_t)((first_play_elapsed_us + 999LL) / 1000LL);
                    }
                }
                first_play_delay_ms = s_audio_adaptive.first_play_delay_ms;
                taskEXIT_CRITICAL(&s_sink_lock);
                if (integrity_valid) {
                    media_sink_note_played_audio_integrity(integrity_sequence);
                }
                if (!s_remote_audio_playback_started_logged) {
                    s_remote_audio_playback_started_logged = true;
                    ESP_LOGI(TAG,
                             "remote audio playback started: mode=%s first_packet_delay_ms=%u prebuffer_ms=%u chunk_ms=%u buffered_ms=%u level=%u",
                             drain_completed_talkspurt ? "completed" : "stream",
                             (unsigned)first_play_delay_ms,
                             (unsigned)prebuffer_ms,
                             MEDIA_SINK_AUDIO_PLAY_CHUNK_MS,
                             (unsigned)buffered_ms_before_play,
                             (unsigned)output_level);
                }
                s_audio_play_ok_packets_in_window++;
                s_audio_play_ok_ms_in_window += conceal_underflow ?
                                                     MEDIA_SINK_AUDIO_PLAY_CHUNK_MS :
                                                     played_media_ms;
                if (conceal_underflow) {
                    concealed_ms += MEDIA_SINK_AUDIO_PLAY_CHUNK_MS;
                } else if (played_media_bytes < play_chunk_bytes) {
                    ESP_LOGI(TAG,
                             "remote audio final tail played: media_ms=%u padded_ms=%u",
                             (unsigned)played_media_ms,
                             (unsigned)(MEDIA_SINK_AUDIO_PLAY_CHUNK_MS - played_media_ms));
                    playback_started = false;
                    media_sink_audio_set_playback_active(false);
                    media_sink_audio_mark_talkspurt_idle();
                    rebuffering_after_underflow = false;
                    media_sink_audio_reset_playback_clock(&playback_deadline_tick,
                                                          &playback_clock_valid);
                }
                if (playback_started && tuning.pace_playback_clock) {
                    media_sink_audio_pace_playback(write_start_tick,
                                                   MEDIA_SINK_AUDIO_PLAY_CHUNK_MS,
                                                   &playback_deadline_tick,
                                                   &playback_clock_valid);
                }
            }

            if (play_ret == ESP_OK && play_elapsed_us > media_sink_audio_get_tuning().slow_play_us) {
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

void media_sink_set_audio_profile(media_sink_audio_profile_t profile)
{
    bool changed = false;

    if (!media_sink_audio_profile_valid(profile)) {
        profile = MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY;
    }

    taskENTER_CRITICAL(&s_sink_lock);
    changed = s_audio_profile != profile;
    s_audio_profile = profile;
    taskEXIT_CRITICAL(&s_sink_lock);

    if (changed) {
        media_sink_audio_reset_adaptation();
        ESP_LOGI(TAG, "remote audio profile: %s", media_sink_audio_profile_name(profile));
    }
}

void media_sink_set_remote_audio_talkspurt(bool active)
{
    media_sink_audio_tuning_t tuning = media_sink_audio_get_tuning();
    bool new_talkspurt = false;
    bool ended_talkspurt = false;
    int64_t now_us = esp_timer_get_time();

    taskENTER_CRITICAL(&s_sink_lock);
    new_talkspurt = active && !s_audio_adaptive.talkspurt_active;
    ended_talkspurt = !active && s_audio_adaptive.talkspurt_active;
    s_audio_adaptive.talkspurt_hint_valid = true;
    s_audio_adaptive.talkspurt_active = active;
    s_audio_adaptive.fast_start_pending = active && tuning.fast_start_on_talkspurt;
    if (new_talkspurt) {
        s_audio_adaptive.last_arrival_us = 0;
        s_audio_adaptive.first_arrival_us = 0;
        s_audio_adaptive.first_play_delay_ms = 0;
        s_audio_adaptive.max_arrival_gap_ms = 0;
        s_audio_adaptive.talkspurt_ended_us = 0;
        s_audio_adaptive.last_source_timestamp_ms = 0;
        s_audio_adaptive.source_timestamp_valid = false;
        s_audio_adaptive.stable_windows = 0;
        s_audio_adaptive.playout_rate_mode = 0;
        s_audio_adaptive.talkspurt_started = true;
        s_audio_adaptive.talkspurt_generation++;
        s_audio_adaptive.arrival_jitter_ewma_ms = 0;
        s_audio_adaptive.arrival_jitter_peak_ms = 0;
        s_audio_jitter_boost_ms = 0;
        s_audio_adaptive.talkspurt_events_in_window++;
    } else if (ended_talkspurt) {
        s_audio_adaptive.talkspurt_ended_us = now_us;
    }
    taskEXIT_CRITICAL(&s_sink_lock);
}
bool media_sink_get_audio_diagnostics(media_sink_audio_diagnostics_t *diagnostics)
{
    const audio_format_t *format = speaker_get_playback_format();
    size_t buffered_bytes = 0;

    if (diagnostics == NULL) {
        return false;
    }
    memset(diagnostics, 0, sizeof(*diagnostics));
    if (!s_initialized || s_audio_queue == NULL || format == NULL) {
        return false;
    }

    diagnostics->queued_packets = (uint32_t)uxQueueMessagesWaiting(s_audio_queue);
    taskENTER_CRITICAL(&s_sink_lock);
    diagnostics->profile = s_audio_profile;
    diagnostics->source_packet_ms = s_audio_last_source_packet_ms;
    diagnostics->jitter_ewma_ms = s_audio_adaptive.arrival_jitter_ewma_ms;
    diagnostics->jitter_peak_ms = s_audio_adaptive.arrival_jitter_peak_ms;
    diagnostics->jitter_boost_ms = s_audio_jitter_boost_ms;
    diagnostics->max_arrival_gap_ms = s_audio_adaptive.max_arrival_gap_ms;
    diagnostics->first_play_delay_ms = s_audio_adaptive.first_play_delay_ms;
    diagnostics->rx_packets = s_audio_adaptive.rx_packets;
    diagnostics->rx_ms = s_audio_adaptive.rx_ms;
    diagnostics->played_packets = s_audio_adaptive.played_packets;
    diagnostics->played_ms = s_audio_adaptive.played_ms;
    diagnostics->play_drop_packets = s_audio_adaptive.play_drop_packets;
    diagnostics->queue_drop_packets = s_audio_adaptive.queue_drop_packets;
    diagnostics->trim_drop_packets = s_audio_adaptive.trim_drop_packets;
    diagnostics->underflow_events = s_audio_adaptive.underflow_events;
    diagnostics->active_underflow_events = s_audio_adaptive.active_underflow_events;
    diagnostics->underflow_grace_waits = s_audio_adaptive.underflow_grace_waits;
    diagnostics->underflow_grace_recoveries = s_audio_adaptive.underflow_grace_recoveries;
    diagnostics->delayed_burst_events = s_audio_adaptive.delayed_burst_events;
    diagnostics->source_late_events = s_audio_adaptive.source_late_events;
    diagnostics->source_late_ms = s_audio_adaptive.source_late_ms;
    diagnostics->max_source_late_ms = s_audio_adaptive.max_source_late_ms;
    diagnostics->source_clock_error_ms = s_audio_adaptive.source_clock_error_ms;
    diagnostics->source_gap_pending_ms = s_audio_adaptive.source_gap_pending_ms;
    diagnostics->source_gap_pending_packets =
        s_audio_adaptive.source_gap_pending_packets;
    diagnostics->source_gap_fill_events = s_audio_adaptive.source_gap_fill_events;
    diagnostics->source_gap_fill_ms = s_audio_adaptive.source_gap_fill_ms;
    diagnostics->concealment_events = s_audio_adaptive.concealment_events;
    diagnostics->concealed_ms = s_audio_adaptive.concealed_ms;
    diagnostics->clock_recovery_events = s_audio_adaptive.clock_recovery_events;
    diagnostics->clock_recovery_frames = s_audio_adaptive.clock_recovery_frames;
    diagnostics->clock_recovery_fast_events =
        s_audio_adaptive.clock_recovery_fast_events;
    diagnostics->clock_recovery_fast_frames =
        s_audio_adaptive.clock_recovery_fast_frames;
    diagnostics->playout_rate_mode = s_audio_adaptive.playout_rate_mode;
    diagnostics->playback_pacing_enabled =
        s_audio_tunings[s_audio_profile].pace_playback_clock;
    diagnostics->pacing_wait_events = s_audio_adaptive.pacing_wait_events;
    diagnostics->pacing_wait_ms = s_audio_adaptive.pacing_wait_ms;
    diagnostics->pacing_wait_max_ms = s_audio_adaptive.pacing_wait_max_ms;
    diagnostics->pacing_late_events = s_audio_adaptive.pacing_late_events;
    diagnostics->pacing_late_ms = s_audio_adaptive.pacing_late_ms;
    diagnostics->pacing_late_max_ms = s_audio_adaptive.pacing_late_max_ms;
    diagnostics->integrity_rx_frames = s_audio_integrity.received_frames;
    diagnostics->integrity_buffered_frames = s_audio_integrity.buffered_frames;
    diagnostics->integrity_played_frames = s_audio_integrity.played_frames;
    diagnostics->integrity_checksum_failures = s_audio_integrity.checksum_failures;
    diagnostics->integrity_missing_frames = s_audio_integrity.missing_frames;
    diagnostics->integrity_duplicate_frames = s_audio_integrity.duplicate_frames;
    diagnostics->integrity_reordered_frames = s_audio_integrity.reordered_frames;
    diagnostics->integrity_first_sequence = s_audio_integrity.first_sequence;
    diagnostics->integrity_last_sequence = s_audio_integrity.last_sequence;
    diagnostics->integrity_sequence_valid = s_audio_integrity.sequence_valid;
    diagnostics->integrity_queue_dropped_frames = s_audio_integrity.queue_dropped_frames;
    diagnostics->integrity_trimmed_frames = s_audio_integrity.trimmed_frames;
    diagnostics->integrity_play_failures = s_audio_integrity.play_failures;
    diagnostics->integrity_play_missing_frames = s_audio_integrity.play_missing_frames;
    diagnostics->integrity_play_duplicate_frames = s_audio_integrity.play_duplicate_frames;
    diagnostics->integrity_play_reordered_frames = s_audio_integrity.play_reordered_frames;
    diagnostics->integrity_first_play_sequence = s_audio_integrity.first_play_sequence;
    diagnostics->integrity_last_play_sequence = s_audio_integrity.last_play_sequence;
    diagnostics->integrity_play_sequence_valid = s_audio_integrity.play_sequence_valid;
    diagnostics->playback_active = s_audio_adaptive.playback_active;
    diagnostics->talkspurt_active = s_audio_adaptive.talkspurt_active;
    buffered_bytes = s_audio_pcm_used_bytes;
    taskEXIT_CRITICAL(&s_sink_lock);

    diagnostics->buffered_ms =
        media_sink_audio_duration_ms_for_bytes(buffered_bytes, format);
    diagnostics->prebuffer_ms =
        media_sink_audio_prebuffer_ms(diagnostics->source_packet_ms);
    diagnostics->target_ms =
        media_sink_audio_latency_target_ms(diagnostics->source_packet_ms);
    return true;
}

void media_sink_reset_audio_integrity_stats(void)
{
    taskENTER_CRITICAL(&s_sink_lock);
    memset(&s_audio_integrity, 0, sizeof(s_audio_integrity));
    /* Keep already buffered production PCM intact, but stop pre-reset test
     * markers from being reported as newly played evidence. */
    for (size_t i = 0; i < s_audio_pcm_integrity_count; ++i) {
        size_t index = (s_audio_pcm_integrity_read + i) %
                       MEDIA_SINK_AUDIO_INTEGRITY_CHUNKS;
        s_audio_pcm_integrity[index].valid = false;
    }
    taskEXIT_CRITICAL(&s_sink_lock);
}


bool media_sink_remote_audio_is_drained(media_sink_audio_drain_status_t *status)
{
    media_sink_audio_drain_status_t current = {0};
    media_sink_audio_tuning_t tuning = media_sink_audio_get_tuning();
    const audio_format_t *format = speaker_get_playback_format();
    int64_t last_activity_us = 0;
    uint32_t source_packet_ms = 0;
    uint32_t jitter_guard_ms = 0;
    size_t buffered_bytes = 0;
    bool talkspurt_ended = false;

    if (!s_initialized || s_audio_queue == NULL || format == NULL) {
        current.source_quiet = true;
        if (status != NULL) {
            *status = current;
        }
        return true;
    }

    current.queued_packets = (uint32_t)uxQueueMessagesWaiting(s_audio_queue);
    taskENTER_CRITICAL(&s_sink_lock);
    buffered_bytes = s_audio_pcm_used_bytes;
    current.playback_active = s_audio_adaptive.playback_active;
    talkspurt_ended = s_audio_adaptive.talkspurt_started &&
                       !s_audio_adaptive.talkspurt_active;
    last_activity_us = s_audio_adaptive.last_arrival_us;
    if (s_audio_adaptive.talkspurt_ended_us > last_activity_us) {
        last_activity_us = s_audio_adaptive.talkspurt_ended_us;
    }
    source_packet_ms = s_audio_last_source_packet_ms;
    jitter_guard_ms = s_audio_adaptive.arrival_jitter_peak_ms;
    if (s_audio_jitter_boost_ms > jitter_guard_ms) {
        jitter_guard_ms = s_audio_jitter_boost_ms;
    }
    taskEXIT_CRITICAL(&s_sink_lock);

    current.buffered_ms = media_sink_audio_duration_ms_for_bytes(buffered_bytes, format);

    uint32_t quiet_guard_ms = tuning.realtime_prebuffer_ms;
    uint32_t packet_guard_ms = source_packet_ms * 2U;
    if (packet_guard_ms > quiet_guard_ms) {
        quiet_guard_ms = packet_guard_ms;
    }
    if (jitter_guard_ms > quiet_guard_ms) {
        quiet_guard_ms = jitter_guard_ms;
    }
    if (tuning.jitter_boost_max_ms > 0U &&
        quiet_guard_ms > tuning.jitter_boost_max_ms) {
        quiet_guard_ms = tuning.jitter_boost_max_ms;
    }

    current.source_quiet = talkspurt_ended &&
                           last_activity_us > 0 &&
                           esp_timer_get_time() - last_activity_us >=
                               (int64_t)quiet_guard_ms * 1000LL;
    if (status != NULL) {
        *status = current;
    }

    return current.source_quiet &&
           !current.playback_active &&
           current.queued_packets == 0U &&
           buffered_bytes == 0U;
}

esp_err_t media_sink_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* RTC callbacks can arrive before the playback clock has consumed its
     * startup prebuffer.  A 16-entry queue left no room beyond that normal
     * startup debt and dropped otherwise valid 20 ms packets.  Only the small
     * queue descriptors live here; packet payloads and the PCM ring already
     * live in PSRAM.  This queue is task-only (never ISR/cache-off), so keep the
     * complete 32-entry staging queue in PSRAM as well. */
    s_audio_queue = xQueueCreateWithCaps(MEDIA_SINK_AUDIO_QUEUE_LEN,
                                         sizeof(media_sink_audio_packet_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
                                         const audio_format_t *format,
                                         uint32_t source_timestamp_ms)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "media sink not initialized");
    ESP_RETURN_ON_FALSE(data != NULL && data_len > 0 && format != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid audio packet");

    media_sink_audio_packet_t packet = {
        .generation = media_sink_get_generation(),
        .format = *format,
        .data_len = data_len,
    };
    packet.data = media_sink_alloc_audio_buffer(data_len);
    ESP_RETURN_ON_FALSE(packet.data != NULL, ESP_ERR_NO_MEM, TAG, "audio packet alloc failed");
    memcpy(packet.data, data, data_len);
    media_sink_classify_audio_integrity(&packet);

    uint32_t packet_ms = media_sink_audio_packet_duration_us(&packet) / 1000U;
    packet.source_gap_ms = media_sink_audio_note_arrival(source_timestamp_ms, packet_ms);
    s_audio_rx_packets_in_window++;
    s_audio_rx_ms_in_window += packet_ms;
    media_sink_maybe_log_audio_enqueue(&packet);

    return media_sink_submit_audio_packet(&packet);
}

esp_err_t media_sink_submit_remote_audio_owned(uint8_t *data,
                                               size_t data_len,
                                               const audio_format_t *format,
                                               uint32_t source_timestamp_ms)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "media sink not initialized");
    ESP_RETURN_ON_FALSE(data != NULL && data_len > 0 && format != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid audio packet");

    media_sink_audio_packet_t packet = {
        .generation = media_sink_get_generation(),
        .format = *format,
        .data = data,
        .data_len = data_len,
    };
    media_sink_classify_audio_integrity(&packet);

    uint32_t packet_ms = media_sink_audio_packet_duration_us(&packet) / 1000U;
    packet.source_gap_ms = media_sink_audio_note_arrival(source_timestamp_ms, packet_ms);
    s_audio_rx_packets_in_window++;
    s_audio_rx_ms_in_window += packet_ms;
    media_sink_maybe_log_audio_enqueue(&packet);

    return media_sink_submit_audio_packet(&packet);
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
    media_sink_audio_reset_adaptation();
    media_sink_set_last_source_packet_ms(0);

    speaker_stop_playback();
    media_sink_reset_audio_pcm_buffer();

    media_sink_audio_packet_t audio_packet = {0};
    while (s_audio_queue != NULL && xQueueReceive(s_audio_queue, &audio_packet, 0) == pdTRUE) {
        media_sink_free_audio_packet(&audio_packet);
    }

}
