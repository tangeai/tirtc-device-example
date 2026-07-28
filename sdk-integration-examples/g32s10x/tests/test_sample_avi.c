#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "tirtc_sample_avi.h"

#define EXPECTED_DURATION_MS 9400U
#define EXPECTED_VIDEO_FRAMES \
    ((TIRTC_SAMPLE_AVI_VIDEO_FPS * EXPECTED_DURATION_MS) / 1000U)

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL: %s\n", message); \
            free(asset); \
            return 1; \
        } \
    } while (0)

int main(int argc, char **argv)
{
    tirtc_sample_avi_cursor_t cursor;
    tirtc_sample_avi_t avi;
    FILE *file;
    uint8_t *asset;
    long file_size;
    unsigned int index;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <sample.avi>\n", argv[0]);
        return 2;
    }
    file = fopen(argv[1], "rb");
    if (file == NULL || fseek(file, 0L, SEEK_END) != 0) {
        fprintf(stderr, "FAIL: cannot open sample asset\n");
        return 2;
    }
    file_size = ftell(file);
    if (file_size <= 0L || fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        fprintf(stderr, "FAIL: invalid sample asset size\n");
        return 2;
    }
    asset = malloc((size_t)file_size);
    if (asset == NULL ||
        fread(asset, 1U, (size_t)file_size, file) != (size_t)file_size) {
        free(asset);
        fclose(file);
        fprintf(stderr, "FAIL: cannot read sample asset\n");
        return 2;
    }
    fclose(file);

    CHECK(tirtc_sample_avi_open(&avi, asset, (uint32_t)file_size,
                                128U * 1024U),
          "valid delivery asset was rejected");
    CHECK(avi.video_frames == EXPECTED_VIDEO_FRAMES,
          "unexpected video frame count");
    CHECK(avi.audio_bytes == 75200U, "unexpected audio byte count");
    CHECK(avi.duration_ms == EXPECTED_DURATION_MS,
          "unexpected loop duration");

    tirtc_sample_avi_cursor_reset(&avi, &cursor);
    for (index = 0U; index <= EXPECTED_VIDEO_FRAMES; ++index) {
        const uint8_t *frame;
        uint32_t length;
        uint32_t timestamp_ms;

        CHECK(tirtc_sample_avi_next_video(&avi, &cursor, &frame, &length,
                                          &timestamp_ms),
              "video cursor stopped before looping");
        CHECK(length >= 4U && frame[0] == 0xffU && frame[1] == 0xd8U,
              "invalid JPEG packet returned");
        if (index == EXPECTED_VIDEO_FRAMES) {
            CHECK(timestamp_ms == EXPECTED_DURATION_MS,
                  "video loop timestamp is not monotonic");
        }
    }

    tirtc_sample_avi_cursor_reset(&avi, &cursor);
    for (index = 0U; index < 201U; ++index) {
        uint8_t packet[160];
        uint32_t length;
        uint32_t timestamp_ms;

        CHECK(tirtc_sample_avi_next_audio(&avi, &cursor, packet,
                                          sizeof(packet), &length,
                                          &timestamp_ms),
              "audio cursor stopped before looping");
        CHECK(length == sizeof(packet), "audio packet size changed");
        CHECK(timestamp_ms == index * 20U,
              "audio timestamp is not monotonic");
    }

    asset[0] ^= 0xffU;
    CHECK(!tirtc_sample_avi_open(&avi, asset, (uint32_t)file_size,
                                 128U * 1024U),
          "invalid RIFF header was accepted");
    asset[0] ^= 0xffU;
    CHECK(!tirtc_sample_avi_open(&avi, asset, (uint32_t)file_size, 1024U),
          "oversized video frame was accepted");

    free(asset);
    puts("PASS: sample AVI parser, packet boundaries and loop timestamps");
    return 0;
}
