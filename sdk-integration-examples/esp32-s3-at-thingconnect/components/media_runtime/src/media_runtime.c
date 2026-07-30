#include "media_runtime.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "device/device_media_file.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tirtc_adapter.h"

#define MEDIA_MOUNT_POINT "/media"
#define MEDIA_PARTITION_LABEL "storage"
#define MEDIA_PROFILE_PATH MEDIA_MOUNT_POINT "/media_profile.json"
#define MEDIA_JSON_MAX_BYTES 8192U
#define MEDIA_AUDIO_BUFFER_BYTES 1500U
#define MEDIA_VIDEO_BUFFER_BYTES (256U * 1024U)
#define MEDIA_CONSECUTIVE_SEND_FAILURE_LIMIT 10U

typedef struct {
    device_g711_file_t g711;
    device_amr_file_t amr;
    device_opus_packet_file_t opus;
} audio_source_t;

static const char *TAG = "media_runtime";
static device_media_config_t s_config;
static bool s_ready;
static volatile bool s_task_running;
static atomic_bool s_uplink_active;
static atomic_bool s_uplink_video_enabled;
static atomic_int s_session_profile;
static atomic_uint_fast32_t s_session_generation;
static TaskHandle_t s_task;
static media_runtime_error_callback_t s_error_callback;
static void *s_error_user_data;

static void report_media_error(uint32_t generation,
                               esp_err_t error,
                               const char *stage)
{
    ESP_LOGE(TAG,
             "recorded-media %s failed for session generation=%lu; "
             "the log example has no live capture fallback",
             stage,
             (unsigned long)generation);
    if (s_error_callback != NULL) {
        s_error_callback(generation, error, stage, s_error_user_data);
    }
}

static bool json_u32(const cJSON *object, const char *name, uint32_t *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 ||
        item->valuedouble > UINT32_MAX ||
        item->valuedouble != (double)(uint32_t)item->valuedouble) {
        return false;
    }
    *value = (uint32_t)item->valuedouble;
    return true;
}

static bool json_bool(const cJSON *object, const char *name, bool *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsBool(item)) {
        return false;
    }
    *value = cJSON_IsTrue(item);
    return true;
}

static bool json_optional_u16(const cJSON *object, const char *name, uint16_t *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 ||
        item->valuedouble > UINT16_MAX ||
        item->valuedouble != (double)(uint16_t)item->valuedouble) {
        return false;
    }
    *value = (uint16_t)item->valuedouble;
    return true;
}

static bool json_optional_positive_number(const cJSON *object,
                                          const char *name,
                                          double *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble <= 0) {
        return false;
    }
    *value = item->valuedouble;
    return true;
}

static bool json_optional_bool(const cJSON *object, const char *name, bool *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsBool(item)) {
        return false;
    }
    *value = cJSON_IsTrue(item);
    return true;
}

static bool json_optional_string(const cJSON *object,
                                 const char *name,
                                 char *value,
                                 size_t value_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        strlen(item->valuestring) >= value_size) {
        return false;
    }
    (void)snprintf(value, value_size, "%s", item->valuestring);
    return true;
}

static bool json_string(const cJSON *object,
                        const char *name,
                        char *value,
                        size_t value_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL || item->valuestring[0] == '\0' ||
        strlen(item->valuestring) >= value_size) {
        return false;
    }
    (void)snprintf(value, value_size, "%s", item->valuestring);
    return true;
}

static bool json_file_name(const cJSON *object,
                           const char *name,
                           char *value,
                           size_t value_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        strlen(item->valuestring) >= value_size) {
        return false;
    }
    (void)snprintf(value, value_size, "%s", item->valuestring);
    return true;
}

static bool parse_audio_codec(const char *name, device_audio_codec_t *codec)
{
    if (strcmp(name, "g711a") == 0) {
        *codec = DEVICE_AUDIO_CODEC_G711A;
    } else if (strcmp(name, "amr-nb") == 0) {
        *codec = DEVICE_AUDIO_CODEC_AMR_NB;
    } else if (strcmp(name, "amr-wb") == 0) {
        *codec = DEVICE_AUDIO_CODEC_AMR_WB;
    } else if (strcmp(name, "opus") == 0) {
        *codec = DEVICE_AUDIO_CODEC_OPUS;
    } else {
        return false;
    }
    return true;
}

static bool parse_video_codec(const char *name, device_video_codec_t *codec)
{
    if (strcmp(name, "mjpeg") == 0) {
        *codec = DEVICE_VIDEO_CODEC_MJPEG;
    } else if (strcmp(name, "h264") == 0) {
        *codec = DEVICE_VIDEO_CODEC_H264;
    } else {
        return false;
    }
    return true;
}

static esp_err_t load_json_file(const char *path, char **text, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "cannot open %s: errno=%d", path, errno);
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    long file_length = ftell(file);
    if (file_length <= 0 || (unsigned long)file_length > MEDIA_JSON_MAX_BYTES ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buffer = malloc((size_t)file_length + 1U);
    if (buffer == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    size_t count = fread(buffer, 1, (size_t)file_length, file);
    fclose(file);
    if (count != (size_t)file_length) {
        free(buffer);
        return ESP_FAIL;
    }
    buffer[count] = '\0';
    *text = buffer;
    *length = count;
    return ESP_OK;
}

static esp_err_t load_profile(device_media_config_t *config)
{
    char *text = NULL;
    size_t length = 0;
    esp_err_t err = load_json_file(MEDIA_PROFILE_PATH, &text, &length);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_ParseWithLength(text, length);
    free(text);
    if (root == NULL) {
        ESP_LOGE(TAG, "invalid JSON in %s", MEDIA_PROFILE_PATH);
        return ESP_ERR_INVALID_ARG;
    }

    device_media_config_set_defaults(config);
    cJSON *audio = cJSON_GetObjectItemCaseSensitive(root, "audio");
    cJSON *video = cJSON_GetObjectItemCaseSensitive(root, "video");
    char audio_codec[16];
    char video_codec[16];
    uint32_t channels;
    uint32_t packet_ms;
    uint32_t width;
    uint32_t height;
    uint32_t fps;

    bool ok = cJSON_IsObject(audio) && cJSON_IsObject(video) &&
              json_optional_u16(video, "camera_rotation",
                                &config->video.camera_rotation) &&
              json_optional_positive_number(video, "aspect_ratio",
                                            &config->video.aspect_ratio) &&
              json_optional_string(video, "object_fit",
                                   config->video.object_fit,
                                   sizeof(config->video.object_fit)) &&
              json_optional_bool(video, "hor_mirror",
                                 &config->video.hor_mirror) &&
              json_optional_bool(video, "vert_mirror",
                                 &config->video.vert_mirror) &&
              json_string(audio, "file", config->audio.asset_path,
                          sizeof(config->audio.asset_path)) &&
              json_string(audio, "codec", audio_codec, sizeof(audio_codec)) &&
              parse_audio_codec(audio_codec, &config->audio.codec) &&
              json_u32(audio, "sample_rate_hz", &config->audio.sample_rate_hz) &&
              json_u32(audio, "channels", &channels) && channels <= UINT8_MAX &&
              json_u32(audio, "packet_ms", &packet_ms) && packet_ms <= UINT16_MAX &&
              json_u32(audio, "duration_ms", &config->audio.duration_ms) &&
              json_u32(audio, "packet_count", &config->audio.packet_count) &&
              json_file_name(video, "file", config->video.asset_path,
                             sizeof(config->video.asset_path)) &&
              json_string(video, "codec", video_codec, sizeof(video_codec)) &&
              parse_video_codec(video_codec, &config->video.codec) &&
              json_u32(video, "width", &width) && width <= UINT16_MAX &&
              json_u32(video, "height", &height) && height <= UINT16_MAX &&
              json_u32(video, "fps", &fps) && fps <= UINT16_MAX &&
              json_u32(video, "duration_ms", &config->video.duration_ms) &&
              json_u32(video, "frame_count", &config->video.frame_count) &&
              json_bool(video, "uplink_enabled", &config->video.uplink_enabled) &&
              json_bool(video, "downlink_enabled", &config->video.downlink_enabled);

    if (ok) {
        config->audio.channels = (uint8_t)channels;
        config->audio.packet_ms = (uint16_t)packet_ms;
        config->video.width = (uint16_t)width;
        config->video.height = (uint16_t)height;
        config->video.fps = (uint16_t)fps;
    }
    cJSON_Delete(root);

    char validation_error[128];
    if (!ok || !device_media_config_validate(config,
                                              validation_error,
                                              sizeof(validation_error))) {
        ESP_LOGE(TAG, "invalid media profile: %s",
                 ok ? validation_error : "missing or invalid field");
        return ESP_ERR_INVALID_ARG;
    }
    if (strchr(config->audio.asset_path, '/') != NULL ||
        strchr(config->video.asset_path, '/') != NULL) {
        ESP_LOGE(TAG, "asset paths must be file names in %s", MEDIA_MOUNT_POINT);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static void asset_path(char *path, size_t path_size, const char *file_name)
{
    (void)snprintf(path, path_size, MEDIA_MOUNT_POINT "/%s", file_name);
}

static uint8_t *allocate_video_buffer(void)
{
    uint8_t *buffer = heap_caps_malloc(MEDIA_VIDEO_BUFFER_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = heap_caps_malloc(MEDIA_VIDEO_BUFFER_BYTES, MALLOC_CAP_8BIT);
    }
    return buffer;
}

static device_media_file_result_t open_audio_source(audio_source_t *source,
                                                    const char *path)
{
    size_t fixed_packet_bytes =
        s_config.audio.sample_rate_hz * s_config.audio.packet_ms / 1000U;
    switch (s_config.audio.codec) {
    case DEVICE_AUDIO_CODEC_G711A:
        return device_g711_file_open(&source->g711, path, fixed_packet_bytes);
    case DEVICE_AUDIO_CODEC_AMR_NB:
        return device_amr_file_open(&source->amr, path, false);
    case DEVICE_AUDIO_CODEC_AMR_WB:
        return device_amr_file_open(&source->amr, path, true);
    case DEVICE_AUDIO_CODEC_OPUS:
        return device_opus_packet_file_open(&source->opus, path);
    default:
        return DEVICE_MEDIA_FILE_INVALID;
    }
}

static device_media_file_result_t next_audio_packet(audio_source_t *source,
                                                    uint8_t *buffer,
                                                    size_t capacity,
                                                    size_t *size,
                                                    bool loop)
{
    switch (s_config.audio.codec) {
    case DEVICE_AUDIO_CODEC_G711A:
        return device_g711_file_next(&source->g711, buffer, capacity, size, loop);
    case DEVICE_AUDIO_CODEC_AMR_NB:
    case DEVICE_AUDIO_CODEC_AMR_WB:
        return device_amr_file_next(&source->amr, buffer, capacity, size, loop);
    case DEVICE_AUDIO_CODEC_OPUS:
        return device_opus_packet_file_next(&source->opus,
                                            buffer,
                                            capacity,
                                            size,
                                            loop);
    default:
        return DEVICE_MEDIA_FILE_INVALID;
    }
}

static bool audio_source_is_open(const audio_source_t *source)
{
    return source->g711.file != NULL || source->amr.file != NULL ||
           source->opus.file != NULL;
}

static void close_audio_source(audio_source_t *source)
{
    device_g711_file_close(&source->g711);
    device_amr_file_close(&source->amr);
    device_opus_packet_file_close(&source->opus);
}

static device_media_file_result_t open_video_source(device_mjpeg_file_t *mjpeg,
                                                    device_h264_file_t *h264,
                                                    const char *path)
{
    if (s_config.video.codec == DEVICE_VIDEO_CODEC_MJPEG) {
        return device_mjpeg_file_open(mjpeg, path);
    }
    if (s_config.video.codec == DEVICE_VIDEO_CODEC_H264) {
        return device_h264_file_open(h264, path);
    }
    return DEVICE_MEDIA_FILE_INVALID;
}

static device_media_file_result_t next_video_frame(device_mjpeg_file_t *mjpeg,
                                                   device_h264_file_t *h264,
                                                   uint8_t *buffer,
                                                   size_t capacity,
                                                   size_t *size,
                                                   bool *key_frame,
                                                   bool loop)
{
    if (s_config.video.codec == DEVICE_VIDEO_CODEC_MJPEG) {
        *key_frame = true;
        return device_mjpeg_file_next(mjpeg, buffer, capacity, size, loop);
    }
    if (s_config.video.codec == DEVICE_VIDEO_CODEC_H264) {
        return device_h264_file_next(h264, buffer, capacity, size, key_frame, loop);
    }
    return DEVICE_MEDIA_FILE_INVALID;
}

static void close_video_source(device_mjpeg_file_t *mjpeg, device_h264_file_t *h264)
{
    device_mjpeg_file_close(mjpeg);
    device_h264_file_close(h264);
}

static esp_err_t validate_assets(void)
{
    char audio_path[DEVICE_MEDIA_ASSET_PATH_MAX + sizeof(MEDIA_MOUNT_POINT)];
    char video_path[DEVICE_MEDIA_ASSET_PATH_MAX + sizeof(MEDIA_MOUNT_POINT)];
    asset_path(audio_path, sizeof(audio_path), s_config.audio.asset_path);
    asset_path(video_path, sizeof(video_path), s_config.video.asset_path);

    struct stat info;
    uint32_t audio_packet_bytes =
        s_config.audio.sample_rate_hz * s_config.audio.packet_ms / 1000U;
    uint32_t expected_audio_bytes = audio_packet_bytes * s_config.audio.packet_count;
    if (s_config.audio.codec == DEVICE_AUDIO_CODEC_G711A &&
        (stat(audio_path, &info) != 0 || info.st_size != (off_t)expected_audio_bytes)) {
        long actual = stat(audio_path, &info) == 0 ? (long)info.st_size : -1L;
        ESP_LOGE(TAG,
                 "audio asset mismatch: %s expected=%lu bytes actual=%ld",
                 audio_path,
                 (unsigned long)expected_audio_bytes,
                 actual);
        return ESP_ERR_INVALID_SIZE;
    }

    if (s_config.audio.codec != DEVICE_AUDIO_CODEC_G711A) {
        audio_source_t source = {0};
        uint8_t packet[MEDIA_AUDIO_BUFFER_BYTES];
        device_media_file_result_t audio_result = open_audio_source(&source, audio_path);
        uint32_t packet_count = 0;
        size_t largest_packet = 0;
        if (audio_result == DEVICE_MEDIA_FILE_OK) {
            for (;;) {
                size_t packet_size = 0;
                audio_result = next_audio_packet(&source,
                                                 packet,
                                                 sizeof(packet),
                                                 &packet_size,
                                                 false);
                if (audio_result == DEVICE_MEDIA_FILE_EOF) {
                    break;
                }
                if (audio_result != DEVICE_MEDIA_FILE_OK) {
                    break;
                }
                packet_count++;
                if (packet_size > largest_packet) {
                    largest_packet = packet_size;
                }
            }
        }
        close_audio_source(&source);
        if (audio_result != DEVICE_MEDIA_FILE_EOF ||
            packet_count != s_config.audio.packet_count) {
            ESP_LOGE(TAG,
                     "audio asset mismatch: %s expected=%lu packets actual=%lu result=%s",
                     audio_path,
                     (unsigned long)s_config.audio.packet_count,
                     (unsigned long)packet_count,
                     device_media_file_result_name(audio_result));
            return ESP_ERR_INVALID_SIZE;
        }
        ESP_LOGI(TAG, "variable audio verified: packets=%lu max-packet=%u bytes",
                 (unsigned long)packet_count, (unsigned)largest_packet);
    }

    if (!s_config.video.uplink_enabled) {
        ESP_LOGI(TAG,
                 "assets verified: audio=%lu packets, uplink video=disabled",
                 (unsigned long)s_config.audio.packet_count);
        return ESP_OK;
    }
    uint8_t *frame = allocate_video_buffer();
    if (frame == NULL) {
        return ESP_ERR_NO_MEM;
    }
    device_mjpeg_file_t mjpeg = {0};
    device_h264_file_t h264 = {0};
    device_media_file_result_t result = open_video_source(&mjpeg, &h264, video_path);
    uint32_t count = 0;
    size_t largest = 0;
    bool first_frame = true;
    if (result == DEVICE_MEDIA_FILE_OK) {
        for (;;) {
            size_t size = 0;
            bool key_frame = false;
            result = next_video_frame(&mjpeg,
                                      &h264,
                                      frame,
                                      MEDIA_VIDEO_BUFFER_BYTES,
                                      &size,
                                      &key_frame,
                                      false);
            if (result == DEVICE_MEDIA_FILE_EOF) {
                break;
            }
            if (result != DEVICE_MEDIA_FILE_OK) {
                break;
            }
            if (first_frame && !key_frame) {
                result = DEVICE_MEDIA_FILE_INVALID;
                break;
            }
            first_frame = false;
            count++;
            if (size > largest) {
                largest = size;
            }
        }
        close_video_source(&mjpeg, &h264);
    }
    free(frame);

    if (result != DEVICE_MEDIA_FILE_EOF || count != s_config.video.frame_count) {
        ESP_LOGE(TAG,
                 "video asset mismatch: %s expected=%lu frames actual=%lu result=%s",
                 video_path,
                 (unsigned long)s_config.video.frame_count,
                 (unsigned long)count,
                 device_media_file_result_name(result));
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG,
             "assets verified: audio=%lu packets, video=%lu frames, max-frame=%u bytes",
             (unsigned long)s_config.audio.packet_count,
             (unsigned long)count,
             (unsigned)largest);
    return ESP_OK;
}

static bool open_sources(audio_source_t *audio,
                          device_mjpeg_file_t *mjpeg,
                          device_h264_file_t *h264,
                          bool video_enabled)
{
    char audio_path[DEVICE_MEDIA_ASSET_PATH_MAX + sizeof(MEDIA_MOUNT_POINT)];
    char video_path[DEVICE_MEDIA_ASSET_PATH_MAX + sizeof(MEDIA_MOUNT_POINT)];
    asset_path(audio_path, sizeof(audio_path), s_config.audio.asset_path);
    asset_path(video_path, sizeof(video_path), s_config.video.asset_path);

    if (open_audio_source(audio, audio_path) != DEVICE_MEDIA_FILE_OK) {
        return false;
    }
    if (video_enabled &&
        open_video_source(mjpeg, h264, video_path) != DEVICE_MEDIA_FILE_OK) {
        close_audio_source(audio);
        return false;
    }
    return true;
}

static void sender_task(void *argument)
{
    (void)argument;
    uint8_t audio_packet[MEDIA_AUDIO_BUFFER_BYTES];
    uint8_t *video_frame = s_config.video.uplink_enabled
                               ? allocate_video_buffer()
                               : NULL;
    audio_source_t audio = {0};
    device_mjpeg_file_t mjpeg = {0};
    device_h264_file_t h264 = {0};
    uint32_t generation = UINT32_MAX;
    uint32_t session_generation = 0;
    tirtc_adapter_media_profile_t profile = TIRTC_ADAPTER_MEDIA_NONE;
    bool session_video_enabled = false;
    uint64_t audio_index = 0;
    uint64_t video_index = 0;
    unsigned audio_send_failures = 0;
    unsigned video_send_failures = 0;
    int64_t wall_start_ms = 0;

    if (s_config.video.uplink_enabled && video_frame == NULL) {
        ESP_LOGE(TAG, "cannot allocate %u-byte video frame buffer", MEDIA_VIDEO_BUFFER_BYTES);
        s_task_running = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (s_task_running) {
        uint32_t current_generation = tirtc_adapter_connection_generation();
        uint32_t current_session_generation = (uint32_t)atomic_load_explicit(
            &s_session_generation, memory_order_acquire);
        tirtc_adapter_media_profile_t current_profile =
            (tirtc_adapter_media_profile_t)atomic_load_explicit(
                &s_session_profile, memory_order_acquire);
        bool current_video_enabled = atomic_load_explicit(
            &s_uplink_video_enabled, memory_order_acquire);
        bool uplink_active = atomic_load_explicit(
            &s_uplink_active, memory_order_acquire);
        if (!tirtc_adapter_has_connection() || !uplink_active ||
            current_profile == TIRTC_ADAPTER_MEDIA_NONE ||
            current_session_generation == 0U) {
            if (audio_source_is_open(&audio)) {
                close_audio_source(&audio);
                close_video_source(&mjpeg, &h264);
            }
            generation = current_generation;
            session_generation = current_session_generation;
            profile = current_profile;
            session_video_enabled = current_video_enabled;
            audio_send_failures = 0;
            video_send_failures = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!audio_source_is_open(&audio) || generation != current_generation ||
            session_generation != current_session_generation ||
            profile != current_profile ||
            session_video_enabled != current_video_enabled) {
            close_audio_source(&audio);
            close_video_source(&mjpeg, &h264);
            if (!open_sources(&audio, &mjpeg, &h264, current_video_enabled)) {
                report_media_error(current_session_generation,
                                   ESP_ERR_NOT_FOUND,
                                   "source-open");
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            generation = current_generation;
            session_generation = current_session_generation;
            profile = current_profile;
            session_video_enabled = current_video_enabled;
            audio_index = 0;
            video_index = 0;
            audio_send_failures = 0;
            video_send_failures = 0;
            wall_start_ms = esp_timer_get_time() / 1000;
            ESP_LOGI(TAG, "media sender started for connection generation=%lu",
                     (unsigned long)generation);
        }

        uint64_t audio_pts = audio_index * s_config.audio.packet_ms;
        uint64_t video_pts = session_video_enabled
                                 ? video_index * 1000U / s_config.video.fps
                                 : UINT64_MAX;
        uint64_t target_pts = audio_pts < video_pts ? audio_pts : video_pts;
        int64_t elapsed = esp_timer_get_time() / 1000 - wall_start_ms;
        if ((int64_t)target_pts > elapsed + 1) {
            uint64_t wait_ms = target_pts - (uint64_t)elapsed;
            vTaskDelay(pdMS_TO_TICKS(wait_ms > 20U ? 20U : wait_ms));
            continue;
        }

        if (audio_pts <= video_pts) {
            size_t size = 0;
            device_media_file_result_t result = next_audio_packet(
                &audio, audio_packet, sizeof(audio_packet), &size, true);
            if (result != DEVICE_MEDIA_FILE_OK) {
                ESP_LOGE(TAG, "audio read failed: %s", device_media_file_result_name(result));
                report_media_error(session_generation,
                                   ESP_ERR_INVALID_RESPONSE,
                                   "audio-read");
                close_audio_source(&audio);
                close_video_source(&mjpeg, &h264);
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
            int rc = tirtc_adapter_send_audio(profile,
                                               session_generation,
                                               &s_config.audio,
                                               (uint32_t)audio_pts,
                                               audio_packet,
                                               (uint32_t)size);
            if (rc != 0) {
                ESP_LOGW(TAG, "audio send failed rc=%d", rc);
                audio_send_failures++;
                if (audio_send_failures >=
                    MEDIA_CONSECUTIVE_SEND_FAILURE_LIMIT) {
                    atomic_store_explicit(&s_uplink_active,
                                          false,
                                          memory_order_release);
                    report_media_error(session_generation,
                                       ESP_FAIL,
                                       "audio-send");
                    close_audio_source(&audio);
                    close_video_source(&mjpeg, &h264);
                    audio_send_failures = 0;
                    video_send_failures = 0;
                    continue;
                }
            } else {
                audio_send_failures = 0;
            }
            audio_index++;
        } else {
            size_t size = 0;
            bool key_frame = false;
            device_media_file_result_t result = next_video_frame(
                &mjpeg,
                &h264,
                video_frame,
                MEDIA_VIDEO_BUFFER_BYTES,
                &size,
                &key_frame,
                true);
            if (result != DEVICE_MEDIA_FILE_OK) {
                ESP_LOGE(TAG, "video read failed: %s", device_media_file_result_name(result));
                report_media_error(session_generation,
                                   ESP_ERR_INVALID_RESPONSE,
                                   "video-read");
                close_audio_source(&audio);
                close_video_source(&mjpeg, &h264);
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
            int rc = tirtc_adapter_send_video(profile,
                                               session_generation,
                                               &s_config.video,
                                               (uint32_t)video_pts,
                                               key_frame,
                                               video_frame,
                                               (uint32_t)size);
            if (rc != 0) {
                ESP_LOGW(TAG, "video send failed rc=%d", rc);
                video_send_failures++;
                if (video_send_failures >=
                    MEDIA_CONSECUTIVE_SEND_FAILURE_LIMIT) {
                    atomic_store_explicit(&s_uplink_active,
                                          false,
                                          memory_order_release);
                    report_media_error(session_generation,
                                       ESP_FAIL,
                                       "video-send");
                    close_audio_source(&audio);
                    close_video_source(&mjpeg, &h264);
                    audio_send_failures = 0;
                    video_send_failures = 0;
                    continue;
                }
            } else {
                video_send_failures = 0;
            }
            video_index++;
        }
    }

    close_audio_source(&audio);
    close_video_source(&mjpeg, &h264);
    free(video_frame);
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t media_runtime_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    esp_vfs_spiffs_conf_t mount = {
        .base_path = MEDIA_MOUNT_POINT,
        .partition_label = MEDIA_PARTITION_LABEL,
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&mount);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return err;
    }
    err = load_profile(&s_config);
    if (err == ESP_OK) {
        err = validate_assets();
    }
    if (err != ESP_OK) {
        esp_vfs_spiffs_unregister(MEDIA_PARTITION_LABEL);
        return err;
    }
    atomic_store_explicit(&s_uplink_active, false, memory_order_release);
    atomic_store_explicit(&s_uplink_video_enabled, false, memory_order_release);
    atomic_store_explicit(&s_session_profile,
                          TIRTC_ADAPTER_MEDIA_NONE,
                          memory_order_release);
    atomic_store_explicit(&s_session_generation, 0, memory_order_release);
    s_ready = true;
    ESP_LOGI(TAG,
             "profile loaded: audio=%s video=%s %ux%u@%u",
             s_config.audio.asset_path,
             s_config.video.asset_path,
             s_config.video.width,
             s_config.video.height,
             s_config.video.fps);
    return ESP_OK;
}

esp_err_t media_runtime_start(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task_running) {
        return ESP_OK;
    }
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_task_running = true;
    BaseType_t created = xTaskCreate(sender_task,
                                     "media_sender",
                                     6144,
                                     NULL,
                                     5,
                                     &s_task);
    if (created != pdPASS) {
        s_task_running = false;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void media_runtime_stop(void)
{
    s_task_running = false;
}

bool media_runtime_ready(void)
{
    return s_ready;
}

const device_media_config_t *media_runtime_config(void)
{
    return s_ready ? &s_config : NULL;
}

esp_err_t media_runtime_set_session(tirtc_adapter_media_profile_t profile,
                                    uint32_t session_generation,
                                    bool uplink_video_enabled)
{
    if (!s_ready || profile == TIRTC_ADAPTER_MEDIA_NONE ||
        session_generation == 0U ||
        (profile != TIRTC_ADAPTER_MEDIA_AI &&
         profile != TIRTC_ADAPTER_MEDIA_CALL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (uplink_video_enabled && !s_config.video.uplink_enabled) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    atomic_store_explicit(&s_uplink_active, false, memory_order_release);
    atomic_store_explicit(&s_uplink_video_enabled,
                          uplink_video_enabled,
                          memory_order_release);
    atomic_store_explicit(&s_session_generation,
                          session_generation,
                          memory_order_release);
    atomic_store_explicit(&s_session_profile, profile, memory_order_release);
    return ESP_OK;
}

void media_runtime_clear_session(uint32_t session_generation)
{
    uint32_t current = (uint32_t)atomic_load_explicit(
        &s_session_generation, memory_order_acquire);
    if (session_generation != 0U && current != session_generation) {
        return;
    }
    atomic_store_explicit(&s_uplink_active, false, memory_order_release);
    atomic_store_explicit(&s_uplink_video_enabled, false, memory_order_release);
    atomic_store_explicit(&s_session_profile,
                          TIRTC_ADAPTER_MEDIA_NONE,
                          memory_order_release);
    atomic_store_explicit(&s_session_generation, 0, memory_order_release);
}

void media_runtime_set_uplink_active(bool active)
{
    if (active &&
        ((uint32_t)atomic_load_explicit(
             &s_session_generation, memory_order_acquire) == 0U ||
         atomic_load_explicit(&s_session_profile, memory_order_acquire) ==
             TIRTC_ADAPTER_MEDIA_NONE)) {
        ESP_LOGW(TAG, "ignoring uplink enable without an owned media session");
        return;
    }
    atomic_store_explicit(&s_uplink_active, active, memory_order_release);
}

bool media_runtime_uplink_active(void)
{
    return atomic_load_explicit(&s_uplink_active, memory_order_acquire);
}

void media_runtime_set_error_callback(media_runtime_error_callback_t callback,
                                      void *user_data)
{
    s_error_callback = callback;
    s_error_user_data = user_data;
}
