#ifndef DEVICE_MEDIA_FILE_H
#define DEVICE_MEDIA_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEVICE_MEDIA_FILE_OK = 0,
    DEVICE_MEDIA_FILE_EOF,
    DEVICE_MEDIA_FILE_IO_ERROR,
    DEVICE_MEDIA_FILE_INVALID,
    DEVICE_MEDIA_FILE_BUFFER_TOO_SMALL,
} device_media_file_result_t;

typedef struct {
    FILE *file;
    size_t packet_size;
    uint32_t packet_index;
} device_g711_file_t;

typedef struct {
    FILE *file;
    bool wideband;
    long data_offset;
    uint32_t frame_index;
} device_amr_file_t;

typedef struct {
    FILE *file;
    long data_offset;
    uint32_t packet_index;
} device_opus_packet_file_t;

typedef struct {
    FILE *file;
    uint32_t frame_index;
} device_mjpeg_file_t;

typedef struct {
    FILE *file;
    uint32_t frame_index;
} device_h264_file_t;

device_media_file_result_t device_g711_file_open(device_g711_file_t *source,
                                                 const char *path,
                                                 size_t packet_size);
device_media_file_result_t device_g711_file_next(device_g711_file_t *source,
                                                 uint8_t *buffer,
                                                 size_t capacity,
                                                 size_t *size,
                                                 bool loop);
void device_g711_file_close(device_g711_file_t *source);

device_media_file_result_t device_amr_file_open(device_amr_file_t *source,
                                                const char *path,
                                                bool wideband);
device_media_file_result_t device_amr_file_next(device_amr_file_t *source,
                                                uint8_t *buffer,
                                                size_t capacity,
                                                size_t *size,
                                                bool loop);
void device_amr_file_close(device_amr_file_t *source);

/* Demo Opus packet container: ASCII magic "TIRTCOPUS1\n", followed by
 * repeated uint16 big-endian packet length + one encoded Opus packet. */
device_media_file_result_t device_opus_packet_file_open(
    device_opus_packet_file_t *source,
    const char *path);
device_media_file_result_t device_opus_packet_file_next(
    device_opus_packet_file_t *source,
    uint8_t *buffer,
    size_t capacity,
    size_t *size,
    bool loop);
void device_opus_packet_file_close(device_opus_packet_file_t *source);

device_media_file_result_t device_mjpeg_file_open(device_mjpeg_file_t *source,
                                                  const char *path);
device_media_file_result_t device_mjpeg_file_next(device_mjpeg_file_t *source,
                                                  uint8_t *buffer,
                                                  size_t capacity,
                                                  size_t *size,
                                                  bool loop);
void device_mjpeg_file_close(device_mjpeg_file_t *source);

device_media_file_result_t device_h264_file_open(device_h264_file_t *source,
                                                 const char *path);
device_media_file_result_t device_h264_file_next(device_h264_file_t *source,
                                                 uint8_t *buffer,
                                                 size_t capacity,
                                                 size_t *size,
                                                 bool *key_frame,
                                                 bool loop);
void device_h264_file_close(device_h264_file_t *source);

const char *device_media_file_result_name(device_media_file_result_t result);

#ifdef __cplusplus
}
#endif

#endif
