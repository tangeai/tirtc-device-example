#include "media_governor.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "media_governor";

#define MEDIA_GOVERNOR_CAPTURE_WIDTH 1280U
#define MEDIA_GOVERNOR_CAPTURE_HEIGHT 960U
#define MEDIA_GOVERNOR_COMPACT_CAPTURE_WIDTH 800U
#define MEDIA_GOVERNOR_COMPACT_CAPTURE_HEIGHT 640U
#define MEDIA_GOVERNOR_FULL_WIDTH 1280U
#define MEDIA_GOVERNOR_FULL_HEIGHT 960U

#ifndef CONFIG_APP_RTC_H264_FPS
#define CONFIG_APP_RTC_H264_FPS 20U
#endif

#define MEDIA_GOVERNOR_FULL_FPS CONFIG_APP_RTC_H264_FPS

#ifndef CONFIG_APP_RTC_H264_OUTPUT_BUFFER_BYTES
#define CONFIG_APP_RTC_H264_OUTPUT_BUFFER_BYTES (1024U * 1024U)
#endif

#ifndef CONFIG_APP_RTC_H264_BITRATE
#define CONFIG_APP_RTC_H264_BITRATE 4000000U
#endif

#ifndef CONFIG_APP_RTC_H264_MAX_DELTA_PAYLOAD_BYTES
#define CONFIG_APP_RTC_H264_MAX_DELTA_PAYLOAD_BYTES (128U * 1024U)
#endif

#ifndef CONFIG_APP_DEVICE_CALL_VIDEO_WIDTH
#define CONFIG_APP_DEVICE_CALL_VIDEO_WIDTH 480U
#endif

#ifndef CONFIG_APP_DEVICE_CALL_VIDEO_HEIGHT
#define CONFIG_APP_DEVICE_CALL_VIDEO_HEIGHT 320U
#endif

#ifndef CONFIG_APP_DEVICE_CALL_VIDEO_FPS
#define CONFIG_APP_DEVICE_CALL_VIDEO_FPS 20U
#endif

#ifndef CONFIG_APP_DEVICE_CALL_VIDEO_BITRATE
#define CONFIG_APP_DEVICE_CALL_VIDEO_BITRATE 2000000U
#endif

#ifndef CONFIG_APP_RTC_H264_MIN_QP
#define CONFIG_APP_RTC_H264_MIN_QP 34U
#endif

#ifndef CONFIG_APP_RTC_H264_MAX_QP
#define CONFIG_APP_RTC_H264_MAX_QP 45U
#endif

#ifndef CONFIG_APP_DEVICE_CALL_VIDEO_MIN_QP
#define CONFIG_APP_DEVICE_CALL_VIDEO_MIN_QP 26U
#endif

#ifndef CONFIG_APP_DEVICE_CALL_VIDEO_MAX_QP
#define CONFIG_APP_DEVICE_CALL_VIDEO_MAX_QP 38U
#endif

#ifndef CONFIG_APP_RTC_VIDEO_AUTO_ADAPT_ENABLE
#define CONFIG_APP_RTC_VIDEO_AUTO_ADAPT_ENABLE 0
#endif

#ifndef CONFIG_APP_RTC_TRANSPORT_BACKPRESSURE_HOLD_MS
#define CONFIG_APP_RTC_TRANSPORT_BACKPRESSURE_HOLD_MS 300
#endif

#define MEDIA_GOVERNOR_BACKPRESSURE_HOLD_MS CONFIG_APP_RTC_TRANSPORT_BACKPRESSURE_HOLD_MS
#define MEDIA_GOVERNOR_BACKPRESSURE_LOG_INTERVAL_MS 5000U

#define MEDIA_GOVERNOR_FULL_VIDEO_CONFIG_INIT \
    { \
        .width = MEDIA_GOVERNOR_FULL_WIDTH, \
        .height = MEDIA_GOVERNOR_FULL_HEIGHT, \
        .fps = MEDIA_GOVERNOR_FULL_FPS, \
        .bitrate_bps = CONFIG_APP_RTC_H264_BITRATE, \
        .weak_network_mode = MEDIA_GOVERNOR_WEAK_NETWORK_OFF, \
        .weak_network_level = 0, \
        .h264_min_qp = CONFIG_APP_RTC_H264_MIN_QP, \
        .h264_max_qp = CONFIG_APP_RTC_H264_MAX_QP, \
    }

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static media_governor_profile_t s_profile = MEDIA_GOVERNOR_PROFILE_IDLE;
static uint32_t s_backpressure_count;
static TickType_t s_backpressure_until_tick;
static TickType_t s_last_backpressure_log_tick;
static bool s_auto_adapt_disabled_logged;
static media_governor_video_config_t s_rtc_video_config = MEDIA_GOVERNOR_FULL_VIDEO_CONFIG_INIT;

static const media_governor_camera_policy_t s_policy_idle = {
    .capture_width = MEDIA_GOVERNOR_CAPTURE_WIDTH,
    .capture_height = MEDIA_GOVERNOR_CAPTURE_HEIGHT,
    .capture_fps = 1,
    .rtc_video_fps = 0,
    .rtc_width = 480,
    .rtc_height = 360,
    .h264_bitrate_bps = 800U * 1000U,
    .h264_min_qp = CONFIG_APP_RTC_H264_MIN_QP,
    .h264_max_qp = CONFIG_APP_RTC_H264_MAX_QP,
    .h264_output_buffer_bytes = 512U * 1024U,
    .h264_max_delta_payload_bytes = 96U * 1024U,
    .dma_free_min_bytes = 24U * 1024U,
    .dma_largest_min_bytes = 12U * 1024U,
};

static media_governor_video_config_t media_governor_full_video_config(void)
{
    return (media_governor_video_config_t)MEDIA_GOVERNOR_FULL_VIDEO_CONFIG_INIT;
}

static void media_governor_select_native_capture_size(const media_governor_video_config_t *config,
                                                       uint16_t *width,
                                                       uint16_t *height)
{
    *width = MEDIA_GOVERNOR_CAPTURE_WIDTH;
    *height = MEDIA_GOVERNOR_CAPTURE_HEIGHT;

    /*
     * OV5647 exposes discrete sensor modes. RTC output sizes are scaler targets,
     * not arbitrary sensor modes; use the smallest native mode that contains
     * the requested frame and let the P4 media layer crop/scale it.
     */
    if (config != NULL &&
        config->width <= MEDIA_GOVERNOR_COMPACT_CAPTURE_WIDTH &&
        config->height <= MEDIA_GOVERNOR_COMPACT_CAPTURE_HEIGHT) {
        *width = MEDIA_GOVERNOR_COMPACT_CAPTURE_WIDTH;
        *height = MEDIA_GOVERNOR_COMPACT_CAPTURE_HEIGHT;
    }
}

static media_governor_camera_policy_t media_governor_make_rtc_av_policy(const media_governor_video_config_t *config)
{
    media_governor_video_config_t safe_config = {0};
    uint16_t capture_width = MEDIA_GOVERNOR_CAPTURE_WIDTH;
    uint16_t capture_height = MEDIA_GOVERNOR_CAPTURE_HEIGHT;

    if (config != NULL) {
        safe_config = *config;
    } else {
        safe_config = s_rtc_video_config;
    }

    if (safe_config.width == 0U) {
        safe_config.width = MEDIA_GOVERNOR_FULL_WIDTH;
    }
    if (safe_config.height == 0U) {
        safe_config.height = MEDIA_GOVERNOR_FULL_HEIGHT;
    }
    if (safe_config.fps == 0U) {
        safe_config.fps = MEDIA_GOVERNOR_FULL_FPS;
    }
    if (safe_config.bitrate_bps == 0U) {
        safe_config.bitrate_bps = CONFIG_APP_RTC_H264_BITRATE;
    }
    if (safe_config.h264_min_qp < 10U || safe_config.h264_min_qp > 51U) {
        safe_config.h264_min_qp = CONFIG_APP_RTC_H264_MIN_QP;
    }
    if (safe_config.h264_max_qp < safe_config.h264_min_qp ||
        safe_config.h264_max_qp > 51U) {
        safe_config.h264_max_qp = CONFIG_APP_RTC_H264_MAX_QP;
        if (safe_config.h264_max_qp < safe_config.h264_min_qp) {
            safe_config.h264_max_qp = safe_config.h264_min_qp;
        }
    }
    media_governor_select_native_capture_size(&safe_config, &capture_width, &capture_height);

    return (media_governor_camera_policy_t) {
        .capture_width = capture_width,
        .capture_height = capture_height,
        .capture_fps = safe_config.fps,
        .rtc_video_fps = safe_config.fps,
        .rtc_width = safe_config.width,
        .rtc_height = safe_config.height,
        .h264_bitrate_bps = safe_config.bitrate_bps,
        .h264_min_qp = safe_config.h264_min_qp,
        .h264_max_qp = safe_config.h264_max_qp,
        .h264_output_buffer_bytes = CONFIG_APP_RTC_H264_OUTPUT_BUFFER_BYTES,
        .h264_max_delta_payload_bytes = CONFIG_APP_RTC_H264_MAX_DELTA_PAYLOAD_BYTES,
        .dma_free_min_bytes = 8U * 1024U,
        .dma_largest_min_bytes = 4U * 1024U,
    };
}

static const media_governor_rtc_policy_t s_rtc_policy_normal = {
    .defer_audio_for_local_video = false,
    .prepare_playback_while_video_first = true,
};

static const media_governor_rtc_policy_t s_rtc_policy_video_first = {
    .defer_audio_for_local_video = false,
    .prepare_playback_while_video_first = true,
};

esp_err_t media_governor_init(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_initialized = true;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

const char *media_governor_profile_name(media_governor_profile_t profile)
{
    switch (profile) {
    case MEDIA_GOVERNOR_PROFILE_IDLE:
        return "idle";
    case MEDIA_GOVERNOR_PROFILE_QR_SCAN:
        return "qr_scan";
    case MEDIA_GOVERNOR_PROFILE_LOCAL_PREVIEW:
        return "local_preview";
    case MEDIA_GOVERNOR_PROFILE_RTC_AUDIO:
        return "rtc_audio";
    case MEDIA_GOVERNOR_PROFILE_RTC_AV_SAFE:
        return "rtc_av_safe";
    case MEDIA_GOVERNOR_PROFILE_RTC_AV_PREVIEW:
        return "rtc_av_preview";
    case MEDIA_GOVERNOR_PROFILE_AI_CHAT:
        return "ai_chat";
    default:
        return "unknown";
    }
}

static bool media_governor_tick_before(TickType_t tick, TickType_t deadline)
{
    return (int32_t)(tick - deadline) < 0;
}

void media_governor_set_profile(media_governor_profile_t profile)
{
    media_governor_profile_t old_profile = MEDIA_GOVERNOR_PROFILE_IDLE;
    bool changed = false;

    taskENTER_CRITICAL(&s_lock);
    old_profile = s_profile;
    if (s_profile != profile) {
        s_profile = profile;
        s_backpressure_count = 0;
        s_backpressure_until_tick = 0;
        s_last_backpressure_log_tick = 0;
        changed = true;
    }
    s_initialized = true;
    taskEXIT_CRITICAL(&s_lock);

    if (changed) {
        ESP_LOGI(TAG,
                 "media profile: %s -> %s",
                 media_governor_profile_name(old_profile),
                 media_governor_profile_name(profile));
    }
}

media_governor_profile_t media_governor_get_profile(void)
{
    media_governor_profile_t profile;

    taskENTER_CRITICAL(&s_lock);
    profile = s_profile;
    taskEXIT_CRITICAL(&s_lock);
    return profile;
}

static media_governor_video_config_t media_governor_normalize_video_config(const media_governor_video_config_t *config)
{
    media_governor_video_config_t normalized = s_rtc_video_config;

    if (config != NULL) {
        normalized = *config;
    }

    if (normalized.width < 320U) {
        normalized.width = 320U;
    }
    if (normalized.height < 240U) {
        normalized.height = 240U;
    }
    if (normalized.fps < 5U) {
        normalized.fps = 5U;
    } else if (normalized.fps > 30U) {
        normalized.fps = 30U;
    }
    if (normalized.bitrate_bps < 300U * 1000U) {
        normalized.bitrate_bps = 300U * 1000U;
    } else if (normalized.bitrate_bps > 12U * 1000U * 1000U) {
        normalized.bitrate_bps = 12U * 1000U * 1000U;
    }
    if ((normalized.width & 1U) != 0U) {
        normalized.width--;
    }
    if ((normalized.height & 1U) != 0U) {
        normalized.height--;
    }
    if (normalized.weak_network_mode > MEDIA_GOVERNOR_WEAK_NETWORK_RESOLUTION_PRIORITY) {
        normalized.weak_network_mode = MEDIA_GOVERNOR_WEAK_NETWORK_OFF;
    }
    if (normalized.weak_network_mode == MEDIA_GOVERNOR_WEAK_NETWORK_OFF) {
        normalized.weak_network_level = 0U;
    } else if (normalized.weak_network_level > 3U) {
        normalized.weak_network_level = 3U;
    }
    if (normalized.h264_min_qp < 10U || normalized.h264_min_qp > 51U) {
        normalized.h264_min_qp = CONFIG_APP_RTC_H264_MIN_QP;
    }
    if (normalized.h264_max_qp < normalized.h264_min_qp ||
        normalized.h264_max_qp > 51U) {
        normalized.h264_max_qp = CONFIG_APP_RTC_H264_MAX_QP;
        if (normalized.h264_max_qp < normalized.h264_min_qp) {
            normalized.h264_max_qp = normalized.h264_min_qp;
        }
    }

    return normalized;
}

esp_err_t media_governor_set_rtc_video_config(const media_governor_video_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "rtc video config is null");

    media_governor_video_config_t normalized = media_governor_normalize_video_config(config);
    media_governor_video_config_t old_config = {0};
    bool changed = false;

    taskENTER_CRITICAL(&s_lock);
    old_config = s_rtc_video_config;
    if (memcmp(&s_rtc_video_config, &normalized, sizeof(normalized)) != 0) {
        s_rtc_video_config = normalized;
        changed = true;
    }
    s_initialized = true;
    taskEXIT_CRITICAL(&s_lock);

    if (changed) {
        ESP_LOGI(TAG,
                 "rtc video config: %ux%u@%u %ukbps qp=%u-%u mode=%u level=%u -> "
                 "%ux%u@%u %ukbps qp=%u-%u mode=%u level=%u",
                 (unsigned)old_config.width,
                 (unsigned)old_config.height,
                 (unsigned)old_config.fps,
                 (unsigned)(old_config.bitrate_bps / 1000U),
                 (unsigned)old_config.h264_min_qp,
                 (unsigned)old_config.h264_max_qp,
                 (unsigned)old_config.weak_network_mode,
                 (unsigned)old_config.weak_network_level,
                 (unsigned)normalized.width,
                 (unsigned)normalized.height,
                 (unsigned)normalized.fps,
                 (unsigned)(normalized.bitrate_bps / 1000U),
                 (unsigned)normalized.h264_min_qp,
                 (unsigned)normalized.h264_max_qp,
                 (unsigned)normalized.weak_network_mode,
                 (unsigned)normalized.weak_network_level);
    }

    return ESP_OK;
}

void media_governor_build_device_call_video_config(media_governor_video_config_t *config)
{
    if (config == NULL) {
        return;
    }

    *config = (media_governor_video_config_t) {
        .width = CONFIG_APP_DEVICE_CALL_VIDEO_WIDTH,
        .height = CONFIG_APP_DEVICE_CALL_VIDEO_HEIGHT,
        .fps = CONFIG_APP_DEVICE_CALL_VIDEO_FPS,
        .bitrate_bps = CONFIG_APP_DEVICE_CALL_VIDEO_BITRATE,
        .weak_network_mode = MEDIA_GOVERNOR_WEAK_NETWORK_OFF,
        .weak_network_level = 0U,
        .h264_min_qp = CONFIG_APP_DEVICE_CALL_VIDEO_MIN_QP,
        .h264_max_qp = CONFIG_APP_DEVICE_CALL_VIDEO_MAX_QP,
    };
}

void media_governor_build_camera_policy(const media_governor_video_config_t *config,
                                        media_governor_camera_policy_t *policy)
{
    if (policy == NULL) {
        return;
    }

    *policy = media_governor_make_rtc_av_policy(config);
}

bool media_governor_auto_adaptation_enabled(void)
{
    return CONFIG_APP_RTC_VIDEO_AUTO_ADAPT_ENABLE != 0;
}

esp_err_t media_governor_apply_weak_network_level(media_governor_weak_network_mode_t mode, uint8_t level)
{
    media_governor_video_config_t config = media_governor_full_video_config();
    config.weak_network_mode = mode;
    config.weak_network_level = level;

    if (mode == MEDIA_GOVERNOR_WEAK_NETWORK_OFF || level == 0U) {
        config = media_governor_full_video_config();
        return media_governor_set_rtc_video_config(&config);
    }

    if (level > 3U) {
        level = 3U;
    }
    config.weak_network_level = level;

    if (mode == MEDIA_GOVERNOR_WEAK_NETWORK_FRAMERATE_PRIORITY) {
        /* The current CSI path is native-resolution only; lower work and bitrate without adding a frame copy. */
        static const media_governor_video_config_t table[] = {
            {1280, 960, 20, 3500U * 1000U, MEDIA_GOVERNOR_WEAK_NETWORK_FRAMERATE_PRIORITY, 1,
             CONFIG_APP_RTC_H264_MIN_QP, CONFIG_APP_RTC_H264_MAX_QP},
            {1280, 960, 18, 2500U * 1000U, MEDIA_GOVERNOR_WEAK_NETWORK_FRAMERATE_PRIORITY, 2,
             CONFIG_APP_RTC_H264_MIN_QP, CONFIG_APP_RTC_H264_MAX_QP},
            {1280, 960, 15, 1500U * 1000U, MEDIA_GOVERNOR_WEAK_NETWORK_FRAMERATE_PRIORITY, 3,
             CONFIG_APP_RTC_H264_MIN_QP, CONFIG_APP_RTC_H264_MAX_QP},
        };
        config = table[level - 1U];
    } else if (mode == MEDIA_GOVERNOR_WEAK_NETWORK_RESOLUTION_PRIORITY) {
        static const media_governor_video_config_t table[] = {
            {1280, 960, 18, 3500U * 1000U, MEDIA_GOVERNOR_WEAK_NETWORK_RESOLUTION_PRIORITY, 1,
             CONFIG_APP_RTC_H264_MIN_QP, CONFIG_APP_RTC_H264_MAX_QP},
            {1280, 960, 15, 2500U * 1000U, MEDIA_GOVERNOR_WEAK_NETWORK_RESOLUTION_PRIORITY, 2,
             CONFIG_APP_RTC_H264_MIN_QP, CONFIG_APP_RTC_H264_MAX_QP},
            {1280, 960, 10, 1500U * 1000U, MEDIA_GOVERNOR_WEAK_NETWORK_RESOLUTION_PRIORITY, 3,
             CONFIG_APP_RTC_H264_MIN_QP, CONFIG_APP_RTC_H264_MAX_QP},
        };
        config = table[level - 1U];
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    return media_governor_set_rtc_video_config(&config);
}

esp_err_t media_governor_apply_auto_weak_network_level(media_governor_weak_network_mode_t mode, uint8_t level)
{
    /*
     * Automatic downshift is intentionally gated by Kconfig. The normal RTC
     * path should keep the full configured output capability; only explicit
     * user control or an enabled weak-network callback may lower it.
     */
    if (!media_governor_auto_adaptation_enabled()) {
        bool log_disabled = false;

        taskENTER_CRITICAL(&s_lock);
        if (!s_auto_adapt_disabled_logged) {
            s_auto_adapt_disabled_logged = true;
            log_disabled = true;
        }
        taskEXIT_CRITICAL(&s_lock);

        if (log_disabled) {
            ESP_LOGI(TAG,
                     "auto weak-network video adaptation disabled; keep full output profile");
        }
        return ESP_OK;
    }

    return media_governor_apply_weak_network_level(mode, level);
}

void media_governor_get_rtc_video_config(media_governor_video_config_t *config)
{
    if (config == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_lock);
    *config = s_rtc_video_config;
    taskEXIT_CRITICAL(&s_lock);
}

void media_governor_get_camera_policy(media_governor_camera_policy_t *policy)
{
    if (policy == NULL) {
        return;
    }

    media_governor_profile_t profile = media_governor_get_profile();
    media_governor_video_config_t video_config = {0};

    media_governor_get_rtc_video_config(&video_config);
    switch (profile) {
    case MEDIA_GOVERNOR_PROFILE_RTC_AV_SAFE:
    case MEDIA_GOVERNOR_PROFILE_RTC_AV_PREVIEW:
        *policy = media_governor_make_rtc_av_policy(&video_config);
        break;
    case MEDIA_GOVERNOR_PROFILE_LOCAL_PREVIEW:
    case MEDIA_GOVERNOR_PROFILE_QR_SCAN:
    case MEDIA_GOVERNOR_PROFILE_AI_CHAT:
    case MEDIA_GOVERNOR_PROFILE_RTC_AUDIO:
    case MEDIA_GOVERNOR_PROFILE_IDLE:
    default:
        *policy = s_policy_idle;
        break;
    }

    if (!s_initialized) {
        (void)media_governor_init();
    }
}

void media_governor_get_rtc_av_camera_policy(media_governor_camera_policy_t *policy)
{
    if (policy == NULL) {
        return;
    }

    media_governor_video_config_t video_config = {0};
    media_governor_get_rtc_video_config(&video_config);
    *policy = media_governor_make_rtc_av_policy(&video_config);
    if (!s_initialized) {
        (void)media_governor_init();
    }
}

void media_governor_get_rtc_policy(media_governor_rtc_policy_t *policy)
{
    if (policy == NULL) {
        return;
    }

    media_governor_profile_t profile = media_governor_get_profile();
    switch (profile) {
    case MEDIA_GOVERNOR_PROFILE_RTC_AV_SAFE:
        *policy = s_rtc_policy_video_first;
        break;
    case MEDIA_GOVERNOR_PROFILE_RTC_AV_PREVIEW:
    case MEDIA_GOVERNOR_PROFILE_RTC_AUDIO:
    case MEDIA_GOVERNOR_PROFILE_LOCAL_PREVIEW:
    case MEDIA_GOVERNOR_PROFILE_QR_SCAN:
    case MEDIA_GOVERNOR_PROFILE_AI_CHAT:
    case MEDIA_GOVERNOR_PROFILE_IDLE:
    default:
        *policy = s_rtc_policy_normal;
        break;
    }

    if (!s_initialized) {
        (void)media_governor_init();
    }
}

void media_governor_note_network_backpressure(void)
{
    uint32_t count = 0;
    bool should_log = false;
    TickType_t now = xTaskGetTickCount();
    TickType_t until = now + pdMS_TO_TICKS(MEDIA_GOVERNOR_BACKPRESSURE_HOLD_MS);
    if (until == 0) {
        until = 1;
    }

    taskENTER_CRITICAL(&s_lock);
    s_backpressure_count++;
    count = s_backpressure_count;
    if (s_backpressure_until_tick == 0 ||
        !media_governor_tick_before(until, s_backpressure_until_tick)) {
        s_backpressure_until_tick = until;
    }
    if (s_last_backpressure_log_tick == 0 ||
        now - s_last_backpressure_log_tick >=
            pdMS_TO_TICKS(MEDIA_GOVERNOR_BACKPRESSURE_LOG_INTERVAL_MS)) {
        s_last_backpressure_log_tick = now;
        should_log = true;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (should_log) {
        ESP_LOGW(TAG, "network backpressure noted: count=%lu", (unsigned long)count);
    }
}

bool media_governor_is_network_backpressured(void)
{
    TickType_t until = 0;

    taskENTER_CRITICAL(&s_lock);
    until = s_backpressure_until_tick;
    taskEXIT_CRITICAL(&s_lock);

    if (until == 0) {
        return false;
    }
    return media_governor_tick_before(xTaskGetTickCount(), until);
}
