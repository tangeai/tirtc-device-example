#include <stdio.h>
#include <limits.h>
#include <os.h>
#include <driver/camera.h>
#include <driver/camera_pixel_format.h>
#include <common.h>
#include <dfs_posix.h>
#include <driver/cache.h>
#include "video_core_service.h"
#include "video_cap.h"
#include "third_party/lvgl/lvgl_ingenic.h"
#include "jz_video_utils.h"
#include "save_pic.h"

#define VIDOE_CAP_MAX_NUM 4
#define SW_ROTATE_USE 0
#define VIDEO_CAP_STOP_POLL_MS 5
#define VIDEO_CAP_STOP_TIMEOUT_MS 2000

//#define DEBUG 1

static struct camera_info *g_cam_info[VIDOE_CAP_MAX_NUM];
static struct camera_info g_cam_output_info[VIDOE_CAP_MAX_NUM];
static struct camera_device *g_camera[VIDOE_CAP_MAX_NUM];
static void *g_buf[VIDOE_CAP_MAX_NUM];
static char video_cap_status[VIDOE_CAP_MAX_NUM] = {0};
static char video_cap_display_mode_control[VIDOE_CAP_MAX_NUM] = {1, 1, 1, 1};
static volatile int g_video_cap_save_sensor_pic[VIDOE_CAP_MAX_NUM];
static volatile int g_video_cap_save_i2d_pic[VIDOE_CAP_MAX_NUM];
static void* i2d_buf = NULL;

#ifdef CONFIG_G32S10X_MIPI_TFT050_V30A05T
static int rotate = ROTATOR_ANGLE_270;
#else
static int rotate = ROTATOR_ANGLE_0;
#endif

static bool video_cap_is_packed_yuv422(camera_pixel_fmt format)
{
    return format == CAMERA_PIX_FMT_UYVY ||
           format == CAMERA_PIX_FMT_VYUY ||
           format == CAMERA_PIX_FMT_YUYV ||
           format == CAMERA_PIX_FMT_YVYU;
}

static void video_cap_normalize_output_info(struct camera_info *output,
                                            const struct camera_info *source)
{
    size_t nv12_size;
    size_t uv_offset;

    memcpy(output, source, sizeof(*output));
    nv12_size = (size_t)source->width * (size_t)source->height * 3U / 2U;
    uv_offset = (size_t)source->line_length * (size_t)source->height;

    /* G32 CIM can receive packed YUV422 while writing NV12 to memory. */
    if (video_cap_is_packed_yuv422(source->data_fmt) &&
        (size_t)source->frame_size == nv12_size &&
        (size_t)source->uv_data_offset == uv_offset) {
        output->data_fmt = CAMERA_PIX_FMT_NV12;
    }
}

static int video_cap_i2d_rotate(struct camera_info *cam_info, void* cam_buf, int frame_count, ImgFrameHeader* img_header)
{
    int ret = 0;

    jz_i2d_rotate_param param;

    param.angle = rotate;    //当前写死为选择90度后续做扩展
    param.width = cam_info->width;
    param.height = cam_info->height;
    param.src_addr = cam_buf;
    param.dst_addr = i2d_buf;
    /* GREY格式使用ROTATOR_Y8，NV12使用ROTATOR_NV12 */
    if (cam_info->data_fmt == CAMERA_PIX_FMT_GREY)
        param.src_fmt = ROTATOR_Y8;
    else
        param.src_fmt = ROTATOR_NV12;

    #if SW_ROTATE_USE
        nv12_rotate(cam_buf, cam_info->width, cam_info->height, JZ_ROTATE_90_CW,
             i2d_buf, &img_header->width, &img_header->width);
    #else
        ret = jz_i2d_rotate(param);
        if(ret < 0)
        {
            jz_log_error(JZ_APP_COM_MOD, "i2d rotate error \n");
            return ret;
        }
    #endif

    img_header->frame_id = frame_count;
    img_header->format = cam_info->data_fmt;
    img_header->width = cam_info->height;
    img_header->height = cam_info->width;
    img_header->timestamp = systick_get_time_us();
    /* GREY格式只有Y分量，data_size=w*h；NV12格式data_size=w*h*3/2 */
    if (cam_info->data_fmt == CAMERA_PIX_FMT_GREY)
        img_header->data_size = img_header->width * img_header->height;
    else
        img_header->data_size = img_header->width * img_header->height * 3 / 2;

    return ret;
}

/**
 * 保存指定 camera 的下一帧原始输出图像。
 */
static void video_cap_save_sensor_pic(int cam_index, const void *buf, size_t size, unsigned int w, unsigned int h)
{
    char name[32];

    if (cam_index < 0 || cam_index >= VIDOE_CAP_MAX_NUM || !g_video_cap_save_sensor_pic[cam_index]) {
        return;
    }

    g_video_cap_save_sensor_pic[cam_index] = 0;
    snprintf(name, sizeof(name), "sensor%d_raw", cam_index);

    jz_save_pic_save_raw(name, buf, size, (int)w, (int)h);
}

/**
 * 保存指定 camera 的下一帧 I2D 输出图像。
 */
static void video_cap_save_i2d_pic(int cam_index, const void *buf, const ImgFrameHeader *header)
{
    char name[32];

    if (cam_index < 0 || cam_index >= VIDOE_CAP_MAX_NUM ||
        !g_video_cap_save_i2d_pic[cam_index] || header == NULL) {
        return;
    }

    g_video_cap_save_i2d_pic[cam_index] = 0;
    snprintf(name, sizeof(name), "i2d%d_output", cam_index);

    jz_save_pic_save_raw(name, buf, header->data_size, header->width, header->height);
}

struct camera_info *video_cap_get_cam_info(int index)
{
    if (index < 0 || index >= VIDOE_CAP_MAX_NUM)
        return NULL;

    return g_cam_info[index];
}

struct camera_device *video_cap_get_camera(int index)
{
    if (index < 0 || index >= VIDOE_CAP_MAX_NUM)
        return NULL;

    return g_camera[index];
}

int video_cap_init(int index)
{
    struct camera_info *source_info;
    int ret = 0;

    jz_log_dump(JZ_APP_COM_MOD, "start cam_index:%d \n", index);
    for(int i = 0; i < 5; i++) {
        g_camera[index] = camera_detect(index);
        if (!g_camera[index]) {
            usleep(10);
            jz_log_dump(JZ_APP_COM_MOD, "camera[%d] not found, i]:%d \n", index, i);
        }
        else {
            jz_log_dump(JZ_APP_COM_MOD, "camera[%d] found\n", index);
            break;
        }
    }

    if (!g_camera[index]) {
        jz_log_error(JZ_APP_COM_MOD, "camera[%d] not found\n", index);
        return -1;
    }

    source_info = camera_get_info(g_camera[index]);
    if (!source_info) {
        jz_log_error(JZ_APP_COM_MOD, "camera[%d] info unavailable\n", index);
        return -1;
    }
    video_cap_normalize_output_info(&g_cam_output_info[index], source_info);
    g_cam_info[index] = &g_cam_output_info[index];
    jz_log_dump(JZ_APP_COM_MOD,
                "camera found %s (%dx%d),fps:%d,source_fmt:0x%08x output_fmt:0x%08x frame:%u uv:%u\n",
                g_cam_info[index]->name, g_cam_info[index]->width,
                g_cam_info[index]->height, g_cam_info[index]->fps,
                source_info->data_fmt, g_cam_info[index]->data_fmt,
                g_cam_info[index]->frame_size,
                g_cam_info[index]->uv_data_offset);

    ret = camera_power_on(g_camera[index]);
    if (ret < 0) {
        printf("camera failed to power on\n");
        return -1;
    }

    ret = camera_stream_on(g_camera[index]);
    if (ret < 0) {
        jz_log_error(JZ_APP_COM_MOD, "camera failed to stream on\n");
        camera_power_off(g_camera[index]);
        return -1;
    }

    return ret;
}

void video_cap_thread(void *arg)
{
    unsigned int camera_w = 0;
    unsigned int camera_h = 0;
    int frame_count = 0;
    struct camera_device *camera;
    int cam_index = (int)arg;
    struct camera_info *cam_info = g_cam_info[cam_index];
    void *buf = g_buf[cam_index];
    bool control_display_mode = video_cap_display_mode_control[cam_index] != 0;

    jz_log_dump(JZ_APP_COM_MOD, "start cam_index:%d \n", cam_index);

    if (!cam_info) {
        jz_log_error(JZ_APP_COM_MOD, " cam_info[%d] is null \n", cam_index);
        video_cap_status[cam_index] = VIDEO_CAP_INIT;
        return;
    }

    if (!g_camera[cam_index]) {
        jz_log_error(JZ_APP_COM_MOD, " g_camera[%d] is null \n", cam_index);
        video_cap_status[cam_index] = VIDEO_CAP_INIT;
        return;
    }

    camera_w = cam_info->width;
    camera_h = cam_info->height;
    camera = g_camera[cam_index];

    int retry_count = 0;

    if (rotate != ROTATOR_ANGLE_0) {
        i2d_buf = memalign(64, cam_info->frame_size);
        if (i2d_buf == NULL) {
            jz_log_error(JZ_APP_COM_MOD, "i2d buffer alloc failed, size:%u\n",
                         cam_info->frame_size);
            camera_power_off(camera);
            video_cap_status[cam_index] = VIDEO_CAP_INIT;
            return;
        }
    }

    if (control_display_mode) {
        // Vendor camera pages temporarily take over the display pipeline.
        lvgl_set_display_mode(0);
    }

    while (1) {
        if (video_cap_status[cam_index] == VIDEO_CAP_STOP) {
            break;
        }

        buf = camera_wait_frame(camera);
        if (buf == NULL) {
            if (video_cap_status[cam_index] == VIDEO_CAP_STOP)
                break;

            camera_frame_error_type err = camera_get_frame_error(camera);
            jz_log_error(JZ_APP_COM_MOD, "camera failed to get frame:%d\n", err);
            if (retry_count++ == 1) {
                jz_log_error(JZ_APP_COM_MOD, "camera reset failed\n");
                break;
            }

            if (err == camera_error_dma_error) {
                camera_stream_off(camera);
                camera_stream_on(camera);
            } else {
                camera_power_off(camera);
                camera_power_on(camera);
                camera_stream_on(camera);
            }

            continue;
        }

        retry_count = 0;

        frame_count++;

        /* 存图：保存摄像头原始数据 */
        size_t raw_size = cam_info->frame_size;
        video_cap_save_sensor_pic(cam_index, buf, raw_size, camera_w, camera_h);

        // video 分发
        jz_video_dist_data data = {0};
        data.cam_header.frame_id = frame_count;
        data.cam_header.format = cam_info->data_fmt;
        data.cam_header.width = camera_w;
        data.cam_header.height = camera_h;
        data.cam_header.timestamp = systick_get_time_us();
        /* GREY格式只有Y分量，data_size=w*h；NV12格式data_size=w*h*3/2 */
        data.cam_header.data_size = cam_info->frame_size;
        data.cam_buf = buf;
        data.is_supprot_i2d = false;
        if (rotate != ROTATOR_ANGLE_0)
        {
            data.is_supprot_i2d = true;
            video_cap_i2d_rotate(cam_info, buf, frame_count, &data.i2d_header);
            data.i2d_buf = i2d_buf;

            /* 存图：保存I2D输出 */
            video_cap_save_i2d_pic(cam_index, i2d_buf, &data.i2d_header);

            jz_video_distribute(&data);
        } else {
            jz_video_distribute(&data);
        }

        camera_put_frame(camera, buf);
    }
    camera_power_off(camera);

    if (control_display_mode) {
        lvgl_set_display_mode(1);
        lvgl_disp_flush_ready();
    }

    g_video_cap_save_sensor_pic[cam_index] = 0;
    g_video_cap_save_i2d_pic[cam_index] = 0;

    if(i2d_buf)
    {
        free(i2d_buf);
        i2d_buf = NULL;
    }

    video_cap_status[cam_index] = VIDEO_CAP_INIT;

    jz_log_dump(JZ_APP_COM_MOD, "video_cap stop, cam:%d !!!!\n", cam_index);
}

int video_cap_stop(int index)
{
    int elapsed_ms = 0;

    if (index < 0 || index >= VIDOE_CAP_MAX_NUM)
        return -1;
    if (video_cap_status[index] == VIDEO_CAP_INIT)
        return 0;

    video_cap_status[index] = VIDEO_CAP_STOP;
    if (g_camera[index] != NULL)
        camera_stream_off(g_camera[index]);

    while (elapsed_ms < VIDEO_CAP_STOP_TIMEOUT_MS) {
        int status = video_cap_get_status(index);
        if (status == VIDEO_CAP_INIT)
            return 0;

        msleep(VIDEO_CAP_STOP_POLL_MS);
        elapsed_ms += VIDEO_CAP_STOP_POLL_MS;
    }
    jz_log_error(JZ_APP_COM_MOD,
        "video_cap stop timeout, cam:%d elapsed:%dms\n",
        index, elapsed_ms);
    return -1;
}

int video_cap_start(int index)
{
    thread_ptr_t worker;
    int result;

    if (index < 0 || index >= VIDOE_CAP_MAX_NUM)
        return -1;
    if (video_cap_status[index] != VIDEO_CAP_INIT) {
        jz_log_dump(JZ_APP_COM_MOD, "video_cap is running!!!!\n");
        return 1;
    }

    g_video_cap_save_sensor_pic[index] = 0;
    g_video_cap_save_i2d_pic[index] = 0;
    result = video_cap_init(index);
    if (result != 0)
        return result;
    video_cap_status[index] = VIDEO_CAP_RUNING;
    worker = thread_create("video_cap_thread", 16 * 1024,
                           video_cap_thread, (void *)index);
    if (worker == NULL) {
        video_cap_status[index] = VIDEO_CAP_INIT;
        camera_power_off(g_camera[index]);
        return -1;
    }

    return 0;
}

int video_cap_get_status(int index)
{
    if (index < 0 || index >= VIDOE_CAP_MAX_NUM)
        return VIDEO_CAP_INIT;
    return video_cap_status[index];
}

int video_cap_set_display_mode_control(int index, int enabled)
{
    if (index < 0 || index >= VIDOE_CAP_MAX_NUM ||
        video_cap_status[index] != VIDEO_CAP_INIT)
        return -1;

    video_cap_display_mode_control[index] = enabled != 0;
    return 0;
}

/**
 * 设置保存下一帧摄像头原始图像。
 */
void video_cap_set_save_sensor_pic(void)
{
    int i;

    for (i = 0; i < VIDOE_CAP_MAX_NUM; i++) {
        if (video_cap_status[i] == VIDEO_CAP_RUNING) {
            g_video_cap_save_sensor_pic[i] = 1;
        }
    }
}

/**
 * 设置保存下一帧 I2D 输出图像。
 */
void video_cap_set_save_i2d_pic(void)
{
    int i;

    for (i = 0; i < VIDOE_CAP_MAX_NUM; i++) {
        if (video_cap_status[i] == VIDEO_CAP_RUNING) {
            g_video_cap_save_i2d_pic[i] = 1;
        }
    }
}
