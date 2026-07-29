#include "audio_app.h"
#include "audio_background.h"
#include "audio_cap.h"
#include "audio_kws.h"
#include "audio_record.h"
#include "audio_run_status_ctrl.h"
#include "jz_audio_info.h"
#ifdef CONFIG_APPLICATION_ALIYUN
#include "aliyun.h"
#endif

static volatile audio_run_status_t audio_status = AUDIO_RUN_INIT;

bool jz_audio_app_is_busy(void)
{
    if (audio_status != AUDIO_RUN_INIT) {
        jz_log_dump(JZ_APP_COM_MOD, "audio is busy\n");
        return true;
    }

    return false;
}

int jz_audio_app_start(void)
{
    if (audio_status != AUDIO_RUN_INIT) {
        jz_log_dump(JZ_APP_COM_MOD, "audio already started\n");
        return -1;
    }
    audio_status = AUDIO_RUN_RUNNING;

    jz_audio_run_status_lock_init();
    jz_audio_cap_start();
    jz_audio_record_start();
    jz_audio_kws_start();
    jz_audio_background_start();
#ifdef CONFIG_APPLICATION_ALIYUN
    jz_aliyun_init();
#endif

    return 0;
}

int jz_audio_app_stop(void)
{
    if (audio_status == AUDIO_RUN_INIT) {
        jz_log_dump(JZ_APP_COM_MOD, "audio already stopped\n");
        return 0;
    }

#ifdef CONFIG_APPLICATION_ALIYUN
    jz_aliyun_uninit();
#endif
    jz_audio_kws_stop();
    jz_audio_background_stop();
    jz_audio_cap_set_stop_status();
    jz_audio_record_stop();
    jz_audio_cap_stop();

    audio_status = AUDIO_RUN_INIT;

    return 0;
}
