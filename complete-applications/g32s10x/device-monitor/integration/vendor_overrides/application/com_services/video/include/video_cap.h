#ifndef VIDEO_CAP_H
#define VIDEO_CAP_H

#include "img_circular_buffer.h"
#include "driver/camera.h"

#if CONFIG_SOC_G32S10X
    #define VIDEO_CAP_INDEX 1
#else
    #define VIDEO_CAP_INDEX 0
#endif

typedef enum {
    VIDEO_CAP_INIT = 0,
    VIDEO_CAP_STOP,
    VIDEO_CAP_RUNING,
    VIDEO_CAP_PAUSE,
} jz_video_cap_status;

typedef struct {
    ImgFrameHeader cam_header;
    void *cam_buf;
    bool is_supprot_i2d;
    ImgFrameHeader i2d_header;
    void *i2d_buf;
} jz_video_dist_data;

struct camera_info *video_cap_get_cam_info(int index);
struct camera_device *video_cap_get_camera(int index);
int video_cap_stop(int index);
int video_cap_start(int index);
int video_cap_get_status(int index);

/**
 * @brief Select whether camera capture temporarily owns the LVGL display mode.
 * @param index Camera index.
 * @param enabled Non-zero preserves the vendor display takeover behavior.
 * @return 0 on success, -1 for an invalid index or an active capture.
 */
int video_cap_set_display_mode_control(int index, int enabled);

/**
 * 设置保存下一帧摄像头原始图像。
 */
void video_cap_set_save_sensor_pic(void);

/**
 * 设置保存下一帧 I2D 输出图像。
 */
void video_cap_set_save_i2d_pic(void);

#endif
