#pragma once

/* 从 Annex-B H264 文件中逐帧取数据，供本地测试视频源使用。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    H264_FILE_SOURCE_OK = 0,
    H264_FILE_SOURCE_EOF = 1,
    H264_FILE_SOURCE_ERR_INVALID_ARG = -1,
    H264_FILE_SOURCE_ERR_IO = -2,
    H264_FILE_SOURCE_ERR_NO_MEM = -3,
    H264_FILE_SOURCE_ERR_BAD_STREAM = -4,
} h264_file_source_result_t;

typedef struct
{
    FILE *fp;
    uint8_t *data;
    size_t size;
    size_t capacity;
    size_t consumed_bytes;
    bool file_end_reached;
} h264_file_source_t;

int h264_file_source_open(const char *path, h264_file_source_t *source);
void h264_file_source_reset(h264_file_source_t *source);
void h264_file_source_close(h264_file_source_t *source);
int h264_file_source_next_frame(h264_file_source_t *source,
                                const uint8_t **data,
                                size_t *length,
                                bool *is_key_frame);

#ifdef __cplusplus
}
#endif
