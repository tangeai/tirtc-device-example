#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "device/device_media.h"
#include "device/device_media_file.h"
#include "device/device_session.h"
#include "device/device_utf8.h"

static void create_temp_file(char *path, const uint8_t *data, size_t size)
{
    (void)snprintf(path, 64, "/tmp/device-media-test-XXXXXX");
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *file = fdopen(fd, "wb");
    assert(file != NULL);
    assert(fwrite(data, 1, size, file) == size);
    assert(fclose(file) == 0);
}

static void test_media_files(void)
{
    uint8_t buffer[64];
    size_t size;
    char path[64];

    const uint8_t g711[] = {1, 2, 3, 4};
    create_temp_file(path, g711, sizeof(g711));
    device_g711_file_t audio;
    assert(device_g711_file_open(&audio, path, 2) == DEVICE_MEDIA_FILE_OK);
    assert(device_g711_file_next(&audio, buffer, sizeof(buffer), &size, true) ==
           DEVICE_MEDIA_FILE_OK);
    assert(size == 2 && buffer[0] == 1 && buffer[1] == 2);
    assert(device_g711_file_next(&audio, buffer, sizeof(buffer), &size, true) ==
           DEVICE_MEDIA_FILE_OK);
    assert(buffer[0] == 3 && buffer[1] == 4);
    assert(device_g711_file_next(&audio, buffer, sizeof(buffer), &size, true) ==
           DEVICE_MEDIA_FILE_OK);
    assert(buffer[0] == 1 && buffer[1] == 2);
    device_g711_file_close(&audio);
    assert(unlink(path) == 0);

    const uint8_t amr[] = {
        '#', '!', 'A', 'M', 'R', '\n',
        0x04, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
        0x0c, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33,
    };
    create_temp_file(path, amr, sizeof(amr));
    device_amr_file_t amr_source;
    assert(device_amr_file_open(&amr_source, path, false) == DEVICE_MEDIA_FILE_OK);
    assert(device_amr_file_next(&amr_source, buffer, sizeof(buffer), &size, false) ==
           DEVICE_MEDIA_FILE_OK);
    assert(size == 13 && buffer[0] == 0x04 && buffer[12] == 12);
    assert(device_amr_file_next(&amr_source, buffer, sizeof(buffer), &size, false) ==
           DEVICE_MEDIA_FILE_OK);
    assert(size == 14 && buffer[0] == 0x0c && buffer[13] == 33);
    assert(device_amr_file_next(&amr_source, buffer, sizeof(buffer), &size, true) ==
           DEVICE_MEDIA_FILE_OK);
    assert(size == 13 && buffer[0] == 0x04);
    device_amr_file_close(&amr_source);
    assert(unlink(path) == 0);

    const uint8_t opus_packets[] = {
        'T', 'I', 'R', 'T', 'C', 'O', 'P', 'U', 'S', '1', '\n',
        0x00, 0x03, 0x11, 0x12, 0x13,
        0x00, 0x02, 0x21, 0x22,
    };
    create_temp_file(path, opus_packets, sizeof(opus_packets));
    device_opus_packet_file_t opus;
    assert(device_opus_packet_file_open(&opus, path) == DEVICE_MEDIA_FILE_OK);
    assert(device_opus_packet_file_next(&opus, buffer, sizeof(buffer), &size, false) ==
           DEVICE_MEDIA_FILE_OK);
    assert(size == 3 && buffer[0] == 0x11 && buffer[2] == 0x13);
    assert(device_opus_packet_file_next(&opus, buffer, sizeof(buffer), &size, false) ==
           DEVICE_MEDIA_FILE_OK);
    assert(size == 2 && buffer[0] == 0x21 && buffer[1] == 0x22);
    assert(device_opus_packet_file_next(&opus, buffer, sizeof(buffer), &size, false) ==
           DEVICE_MEDIA_FILE_EOF);
    device_opus_packet_file_close(&opus);
    assert(unlink(path) == 0);

    const uint8_t mjpeg[] = {
        0x11, 0xff, 0xd8, 0x01, 0xff, 0xd9,
        0xff, 0xd8, 0x02, 0x03, 0xff, 0xd9,
    };
    create_temp_file(path, mjpeg, sizeof(mjpeg));
    device_mjpeg_file_t jpeg;
    assert(device_mjpeg_file_open(&jpeg, path) == DEVICE_MEDIA_FILE_OK);
    assert(device_mjpeg_file_next(&jpeg, buffer, sizeof(buffer), &size, false) ==
           DEVICE_MEDIA_FILE_OK);
    assert(size == 5 && buffer[0] == 0xff && buffer[1] == 0xd8 && buffer[4] == 0xd9);
    assert(device_mjpeg_file_next(&jpeg, buffer, sizeof(buffer), &size, false) ==
           DEVICE_MEDIA_FILE_OK);
    assert(size == 6 && buffer[2] == 0x02 && buffer[3] == 0x03);
    assert(device_mjpeg_file_next(&jpeg, buffer, sizeof(buffer), &size, false) ==
           DEVICE_MEDIA_FILE_EOF);
    device_mjpeg_file_close(&jpeg);
    assert(unlink(path) == 0);

    const uint8_t h264[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0xaa,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xbb,
        0x00, 0x00, 0x00, 0x01, 0x65, 0xcc,
        0x00, 0x00, 0x00, 0x01, 0x65, 0x40,
        0x00, 0x00, 0x00, 0x01, 0x09, 0xf0,
        0x00, 0x00, 0x00, 0x01, 0x61, 0xdd,
    };
    create_temp_file(path, h264, sizeof(h264));
    device_h264_file_t annexb;
    bool key_frame;
    assert(device_h264_file_open(&annexb, path) == DEVICE_MEDIA_FILE_OK);
    assert(device_h264_file_next(&annexb,
                                 buffer,
                                 sizeof(buffer),
                                 &size,
                                 &key_frame,
                                 false) == DEVICE_MEDIA_FILE_OK);
    assert(size == 24 && key_frame);
    assert(device_h264_file_next(&annexb,
                                 buffer,
                                 sizeof(buffer),
                                 &size,
                                 &key_frame,
                                 false) == DEVICE_MEDIA_FILE_OK);
    assert(size == 12 && !key_frame);
    assert(device_h264_file_next(&annexb,
                                 buffer,
                                 sizeof(buffer),
                                 &size,
                                 &key_frame,
                                 false) == DEVICE_MEDIA_FILE_EOF);
    device_h264_file_close(&annexb);
    assert(unlink(path) == 0);
}

static void test_h264_asset_if_configured(void)
{
    const char *path = getenv("TIRTC_TEST_H264_ASSET");
    if (path == NULL || path[0] == '\0') {
        return;
    }

    const size_t capacity = 256U * 1024U;
    uint8_t *buffer = malloc(capacity);
    assert(buffer != NULL);
    device_h264_file_t source;
    assert(device_h264_file_open(&source, path) == DEVICE_MEDIA_FILE_OK);

    uint32_t frame_count = 0;
    bool first_key_frame = false;
    for (;;) {
        size_t size = 0;
        bool key_frame = false;
        device_media_file_result_t result = device_h264_file_next(
            &source, buffer, capacity, &size, &key_frame, false);
        if (result == DEVICE_MEDIA_FILE_EOF) {
            break;
        }
        assert(result == DEVICE_MEDIA_FILE_OK);
        assert(size > 0);
        if (frame_count == 0U) {
            first_key_frame = key_frame;
        }
        frame_count++;
    }
    assert(first_key_frame);
    assert(frame_count == 150U);
    device_h264_file_close(&source);
    free(buffer);
}

static void test_utf8(void)
{
    const char valid[] = {'A', (char)0xe5, (char)0xb0, (char)0x8f, 0};
    const char overlong[] = {(char)0xe0, (char)0x80, (char)0x80, 0};
    const char surrogate[] = {(char)0xed, (char)0xa0, (char)0x80, 0};
    const char too_large[] = {
        (char)0xf4, (char)0x90, (char)0x80, (char)0x80, 0,
    };
    assert(device_utf8_validate(valid));
    assert(!device_utf8_validate(overlong));
    assert(!device_utf8_validate(surrogate));
    assert(!device_utf8_validate(too_large));

    const char controls[] = {
        'A',
        (char)0xc2, (char)0x85,
        'B',
        (char)0xe2, (char)0x80, (char)0xa8,
        'C',
        (char)0xff,
        0,
    };
    char output[32];
    assert(device_utf8_sanitize_line(
               controls, output, sizeof(output), sizeof(output) - 1U) == 6U);
    assert(strcmp(output, "A B C?") == 0);

    const char three_chinese[] = {
        (char)0xe5, (char)0xb0, (char)0x8f,
        (char)0xe6, (char)0x9d, (char)0x8e,
        (char)0xe5, (char)0xbc, (char)0xa0,
        0,
    };
    assert(device_utf8_sanitize_line(
               three_chinese, output, sizeof(output), 6U) == 6U);
    assert(memcmp(output, three_chinese, 3U) == 0);
    assert(strcmp(output + 3, "...") == 0);
}

int main(void)
{
    device_media_config_t media;
    char error[128];

    device_media_config_set_defaults(&media);
    assert(device_media_config_validate(&media, error, sizeof(error)));
    assert(media.audio.codec == DEVICE_AUDIO_CODEC_G711A);
    assert(media.audio.sample_rate_hz == 8000);
    assert(media.audio.packet_count == 500);
    assert(media.video.codec == DEVICE_VIDEO_CODEC_MJPEG);
    assert(media.video.fps == 8);
    assert(media.video.frame_count == 80);
    assert(media.video.camera_rotation == 0);
    assert(media.video.aspect_ratio > 1.333 && media.video.aspect_ratio < 1.334);
    assert(media.video.object_fit[0] == '\0');
    assert(!media.video.hor_mirror);
    assert(!media.video.vert_mirror);
    media.video.camera_rotation = 90;
    assert(device_media_config_validate(&media, error, sizeof(error)));
    media.video.camera_rotation = 45;
    assert(!device_media_config_validate(&media, error, sizeof(error)));
    media.video.camera_rotation = 0;
    media.video.aspect_ratio = 0;
    assert(!device_media_config_validate(&media, error, sizeof(error)));
    media.video.aspect_ratio = 16.0 / 9.0;
    (void)snprintf(media.video.object_fit,
                   sizeof(media.video.object_fit),
                   "%s",
                   "contain");
    media.video.hor_mirror = true;
    media.video.vert_mirror = true;
    assert(device_media_config_validate(&media, error, sizeof(error)));
    (void)snprintf(media.video.object_fit,
                   sizeof(media.video.object_fit),
                   "%s",
                   "cover");
    assert(!device_media_config_validate(&media, error, sizeof(error)));
    (void)snprintf(media.video.object_fit,
                   sizeof(media.video.object_fit),
                   "%s",
                   "contain");

    assert(!device_session_video_enabled(&media, DEVICE_SERVICE_NONE, DEVICE_MEDIA_UPLINK));
    assert(!device_session_video_enabled(&media, DEVICE_SERVICE_NONE, DEVICE_MEDIA_DOWNLINK));
    assert(device_session_video_enabled(&media, DEVICE_SERVICE_AI, DEVICE_MEDIA_UPLINK));
    assert(!device_session_video_enabled(&media, DEVICE_SERVICE_AI, DEVICE_MEDIA_DOWNLINK));
    assert(device_session_video_enabled(&media, DEVICE_SERVICE_CALL, DEVICE_MEDIA_DOWNLINK));

    media.video.uplink_enabled = false;
    media.video.downlink_enabled = false;
    assert(device_media_config_validate(&media, error, sizeof(error)));
    assert(!device_session_video_enabled(&media, DEVICE_SERVICE_CALL, DEVICE_MEDIA_UPLINK));
    assert(!device_session_video_enabled(&media, DEVICE_SERVICE_CALL, DEVICE_MEDIA_DOWNLINK));

    media.audio.codec = DEVICE_AUDIO_CODEC_AMR_WB;
    media.audio.sample_rate_hz = 8000;
    assert(!device_media_config_validate(&media, error, sizeof(error)));

    media.audio.sample_rate_hz = 16000;
    assert(device_media_config_validate(&media, error, sizeof(error)));

    media.video.uplink_enabled = true;
    media.video.codec = DEVICE_VIDEO_CODEC_H264;
    assert(!device_media_config_validate(&media, error, sizeof(error)));
    media.video.fps = 15;
    media.video.frame_count = 150;
    assert(device_media_config_validate(&media, error, sizeof(error)));

    media.video.codec = DEVICE_VIDEO_CODEC_H265_RESERVED;
    assert(!device_media_config_validate(&media, error, sizeof(error)));

    test_media_files();
    test_h264_asset_if_configured();
    test_utf8();

    puts("device_core tests passed");
    return 0;
}
