#include "board_camera_selftest.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"

#include "board_profile.h"

static const char *TAG = "board_camera";

#define CAMERA_SELFTEST_I2C_PORT 0
#define CAMERA_SELFTEST_I2C_FREQ 100000
#define CAMERA_SELFTEST_BUFFER_COUNT 2
#define CAMERA_SELFTEST_FRAME_TIMEOUT_MS 3000
#define CAMERA_SELFTEST_TARGET_FORMAT V4L2_PIX_FMT_RGB565

static bool s_video_ready;

static void format_to_str(uint32_t format, char out[5])
{
    out[0] = (char)(format & 0xff);
    out[1] = (char)((format >> 8) & 0xff);
    out[2] = (char)((format >> 16) & 0xff);
    out[3] = (char)((format >> 24) & 0xff);
    out[4] = '\0';
}

static esp_err_t camera_video_init_once(void)
{
    if (s_video_ready)
    {
        return ESP_OK;
    }

    const board_pin_profile_t *pins = board_get_pin_profile();
    const esp_video_init_csi_config_t csi_config = {
        .sccb_config = {
            .init_sccb = true,
            .i2c_config = {
                .port = CAMERA_SELFTEST_I2C_PORT,
                .scl_pin = pins->i2c_scl,
                .sda_pin = pins->i2c_sda,
            },
            .freq = CAMERA_SELFTEST_I2C_FREQ,
        },
        .reset_pin = -1,
        .pwdn_pin = -1,
    };
    const esp_video_init_config_t video_config = {
        .csi = &csi_config,
    };

    ESP_RETURN_ON_ERROR(esp_video_init(&video_config), TAG, "init esp_video failed");
    s_video_ready = true;
    ESP_LOGI(TAG, "esp_video ready: dev=%s I2C%d SCL=%d SDA=%d", ESP_VIDEO_MIPI_CSI_DEVICE_NAME, CAMERA_SELFTEST_I2C_PORT, pins->i2c_scl, pins->i2c_sda);
    return ESP_OK;
}

static esp_err_t camera_open_device(int *out_fd)
{
    struct v4l2_capability capability = {0};
    int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY | O_NONBLOCK);
    ESP_RETURN_ON_FALSE(fd >= 0, ESP_FAIL, TAG, "open %s failed errno=%d", ESP_VIDEO_MIPI_CSI_DEVICE_NAME, errno);

    if (ioctl(fd, VIDIOC_QUERYCAP, &capability) != 0)
    {
        ESP_LOGE(TAG, "query camera capability failed errno=%d", errno);
        close(fd);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "Camera capability: driver=%s card=%s bus=%s version=%u.%u.%u caps=0x%08" PRIx32,
             capability.driver,
             capability.card,
             capability.bus_info,
             (uint16_t)(capability.version >> 16),
             (uint8_t)(capability.version >> 8),
             (uint8_t)capability.version,
             capability.capabilities);
    *out_fd = fd;
    return ESP_OK;
}

static esp_err_t camera_config_format(int fd, struct v4l2_format *active_format)
{
    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    };

    if (ioctl(fd, VIDIOC_G_FMT, &format) != 0)
    {
        ESP_LOGE(TAG, "get camera format failed errno=%d", errno);
        return ESP_FAIL;
    }

    char current_fourcc[5];
    format_to_str(format.fmt.pix.pixelformat, current_fourcc);
    ESP_LOGI(TAG,
             "Camera default format: %ux%u %s sizeimage=%u",
             (unsigned)format.fmt.pix.width,
             (unsigned)format.fmt.pix.height,
             current_fourcc,
             (unsigned)format.fmt.pix.sizeimage);

    if (format.fmt.pix.pixelformat != CAMERA_SELFTEST_TARGET_FORMAT)
    {
        struct v4l2_format target_format = format;
        target_format.fmt.pix.pixelformat = CAMERA_SELFTEST_TARGET_FORMAT;
        if (ioctl(fd, VIDIOC_S_FMT, &target_format) == 0)
        {
            format = target_format;

            if (ioctl(fd, VIDIOC_G_FMT, &format) != 0)
            {
                ESP_LOGE(TAG, "get active camera format failed errno=%d", errno);
                return ESP_FAIL;
            }
        }
        else
        {
            ESP_LOGW(TAG, "set RGB565 format failed errno=%d, keep default format", errno);
        }
    }

    char active_fourcc[5];
    format_to_str(format.fmt.pix.pixelformat, active_fourcc);
    ESP_LOGI(TAG,
             "Camera active format: %ux%u %s sizeimage=%u bytesperline=%u",
             (unsigned)format.fmt.pix.width,
             (unsigned)format.fmt.pix.height,
             active_fourcc,
             (unsigned)format.fmt.pix.sizeimage,
             (unsigned)format.fmt.pix.bytesperline);
    *active_format = format;
    return ESP_OK;
}

static esp_err_t camera_prepare_buffers(int fd, void *buffers[CAMERA_SELFTEST_BUFFER_COUNT], size_t lengths[CAMERA_SELFTEST_BUFFER_COUNT])
{
    struct v4l2_requestbuffers req = {
        .count = CAMERA_SELFTEST_BUFFER_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };

    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0)
    {
        ESP_LOGE(TAG, "request camera buffers failed errno=%d", errno);
        return ESP_FAIL;
    }

    for (uint32_t i = 0; i < CAMERA_SELFTEST_BUFFER_COUNT; ++i)
    {
        struct v4l2_buffer buf = {
            .index = i,
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0)
        {
            ESP_LOGE(TAG, "query camera buffer %" PRIu32 " failed errno=%d", i, errno);
            return ESP_FAIL;
        }

        buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (buffers[i] == NULL)
        {
            ESP_LOGE(TAG, "map camera buffer %" PRIu32 " failed length=%u", i, (unsigned)buf.length);
            return ESP_FAIL;
        }
        lengths[i] = buf.length;

        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0)
        {
            ESP_LOGE(TAG, "queue camera buffer %" PRIu32 " failed errno=%d", i, errno);
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Camera buffers ready: count=%d len0=%u len1=%u", CAMERA_SELFTEST_BUFFER_COUNT, (unsigned)lengths[0], (unsigned)lengths[1]);
    return ESP_OK;
}

static esp_err_t camera_dequeue_one_frame(int fd, struct v4l2_buffer *out_buf)
{
    const int64_t deadline_us = esp_timer_get_time() + ((int64_t)CAMERA_SELFTEST_FRAME_TIMEOUT_MS * 1000);

    while (esp_timer_get_time() < deadline_us)
    {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };

        if (ioctl(fd, VIDIOC_DQBUF, &buf) == 0)
        {
            *out_buf = buf;
            return ESP_OK;
        }

        if (errno != EAGAIN)
        {
            ESP_LOGE(TAG, "dequeue camera frame failed errno=%d", errno);
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGE(TAG, "dequeue camera frame timeout after %d ms", CAMERA_SELFTEST_FRAME_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}

esp_err_t board_camera_selftest_run(void)
{
    ESP_LOGI(TAG, "Camera self-test begin");

    int fd = -1;
    bool stream_on = false;
    void *buffers[CAMERA_SELFTEST_BUFFER_COUNT] = {0};
    size_t lengths[CAMERA_SELFTEST_BUFFER_COUNT] = {0};
    struct v4l2_format format = {0};
    struct v4l2_buffer frame = {0};
    esp_err_t ret = camera_video_init_once();

    if (ret == ESP_OK)
    {
        ret = camera_open_device(&fd);
    }
    if (ret == ESP_OK)
    {
        ret = camera_config_format(fd, &format);
    }
    if (ret == ESP_OK)
    {
        ret = camera_prepare_buffers(fd, buffers, lengths);
    }
    if (ret == ESP_OK)
    {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_STREAMON, &type) == 0)
        {
            stream_on = true;
            ESP_LOGI(TAG, "Camera stream on");
        }
        else
        {
            ESP_LOGE(TAG, "camera stream on failed errno=%d", errno);
            ret = ESP_FAIL;
        }
    }
    if (ret == ESP_OK)
    {
        ret = camera_dequeue_one_frame(fd, &frame);
    }
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG,
                 "Camera frame ok: index=%u bytesused=%u length=%u sequence=%u timestamp=%ld.%06ld",
                 (unsigned)frame.index,
                 (unsigned)frame.bytesused,
                 (unsigned)frame.length,
                 (unsigned)frame.sequence,
                 (long)frame.timestamp.tv_sec,
                 (long)frame.timestamp.tv_usec);

        if (ioctl(fd, VIDIOC_QBUF, &frame) != 0)
        {
            ESP_LOGW(TAG, "requeue camera frame failed errno=%d", errno);
        }
    }

    if (stream_on)
    {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_STREAMOFF, &type) != 0)
        {
            ESP_LOGW(TAG, "camera stream off failed errno=%d", errno);
        }
    }
    for (uint32_t i = 0; i < CAMERA_SELFTEST_BUFFER_COUNT; ++i)
    {
        if (buffers[i] != NULL)
        {
            (void)munmap(buffers[i], lengths[i]);
        }
    }
    if (fd >= 0)
    {
        close(fd);
    }

    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "Camera self-test done");
    }
    else
    {
        ESP_LOGE(TAG, "Camera self-test failed: %s", esp_err_to_name(ret));
    }
    return ret;
}
