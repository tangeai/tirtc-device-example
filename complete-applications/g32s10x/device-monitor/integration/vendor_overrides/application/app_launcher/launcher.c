#include "third_party/lvgl/lvgl_ingenic.h"
#include "third_party/lvgl/lvgl/lvgl.h"
#include "application/app_include/ui.h"
#include "os/thread.h"
#ifdef CONFIG_APPLICATION_IVS_FACE
#include "application/app_ivs_face/app_core/include/face_app.h"
#endif
#include "video_core_service.h"
#ifdef CONFIG_APPLICATION_SERVICES_AUDIO
#include "audio_common_ctrl.h"
#endif
#ifdef CONFIG_APPLICATION_SERVICES_AUDIO_CAP
#include "audio_core_service.h"
#endif
#include "dma_memcpy.h"
#include "hmi_service.h"
#include "upgrade_service.h"
#include "time_service.h"
#include "jpeg_cap_api.h"
#ifdef CONFIG_APPLICATION_TIRTC_SCREEN_DEBUG
#include "tirtc_screen_debug.h"
#endif
#ifdef CONFIG_APPLICATION_MULTI_OBJ_DET
#include "multi_obj_app.h"
#endif

#include <devices/gt9xx_touch.h>
#include <driver/gpio.h>
#include <driver/backlight.h>

extern void backlight_enable(void);

static struct goodix_ts_data g_gt9xx;

void jz_lvgl_thread(void *user_data)
{
    (void)user_data;
#ifndef CONFIG_RUN_ON_FPGA
    lvgl_start(10 * 1000);
#else
    lvgl_start(10 * 1000 * 1000);
#endif
    return;
}


void jz_ui_display(void)
{
    #ifdef CONFIG_G32S10X_MIPI_TFT050_V30A05T
        gpio_set_func(GPIO_PC(27), GPIO_OUTPUT1);

        char *device_name = "gt9xx";
        goodix_touch_init(&g_gt9xx);

        backlight_enable();
    #endif

    lv_init();

    int ret = lvgl_init_fb_display("fb0");
    assert(!ret);

    lvgl_init_mouse_input();

    #ifdef CONFIG_G32S10X_MIPI_TFT050_V30A05T
        ret = lvgl_init_tp_input(device_name);
        assert(!ret);
    #endif

    ui_main();

    thread_create("jz_lvgl_thread", 32 * 1024, jz_lvgl_thread, NULL);

    return;
}

void jz_launcher(void)
{
    // dma_memcpy_init();

    jz_time_stats_init();

    /*
     * tft050_v30a05t LCD 使用前设置 PSRAM 低功耗控制位，
     * 临时放置，待后续驱动处理。
     */
    *(volatile unsigned int *)(0x133a1064) |= (1ul << 31);
    jz_ui_display();

#ifdef CONFIG_APPLICATION_SERVICES_HMI
    jz_hmi_service_init();
    jz_wireless_handler_init();
#endif

#ifdef CONFIG_APPLICATION_TIRTC_SCREEN_DEBUG
    (void)tirtc_screen_debug_start();
#endif

#ifdef CONFIG_APPLICATION_SERVICES_TIME
    jz_time_service_init();
#endif

#ifdef CONFIG_APPLICATION_SERVICES_VIDEO
    jz_video_core_service_init();
#endif

#ifdef CONFIG_RUN_ON_FPGA
    #ifdef CONFIG_APPLICATION_IVS_FACE
        jz_face_app_start();
    #endif
#endif

#ifdef CONFIG_APPLICATION_SERVICES_AUDIO
    jz_audio_config_init();
#endif

#ifdef CONFIG_APPLICATION_SERVICES_AUDIO_CAP
    jz_audio_core_service_init();
#endif

#ifdef CONFIG_APPLICATION_SERVICES_JPEG_CAP
    jz_jpeg_init();
#endif

#ifdef CONFIG_APPLICATION_SERVICES_UPGRADE
    jz_upgrade_service_init();
#endif

    return;
}
