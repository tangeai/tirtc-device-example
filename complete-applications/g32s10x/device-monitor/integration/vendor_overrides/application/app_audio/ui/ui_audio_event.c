#ifdef CONFIG_APPLICATION_ALIYUN
#include "aliyun.h"
#include "hmi_service.h"
#include "ui_audio_chat.h"
#include "third_party/lvgl/jz_lvgl_common/jz_lvgl_mutex.h"

static int g_asr_start = 0;
static int g_LLM_speech_start = 0;

/**
 * @brief 测试用视频控制UI侧处理函数
 */
static void jz_audio_hmi_event_handler(jz_hmi_msg_t *msg)
{
    jz_log_info(JZ_APP_COM_MOD, "[TEST] UI handler called for msg type: %d %d\n", msg->msg_type);

    if(!msg) {
        jz_log_error(JZ_APP_COM_MOD, "msg is null! \n");
    }

    switch (msg->cmd) {
        case ALI_CMD_SPEECH_PREPARE:
            jz_lv_mutex_lock();
            chat_update_status_bar("请说话...");
            g_asr_start = 0;
            g_LLM_speech_start = 0;
            jz_lv_mutex_unlock();
            break;
        case ALI_CMD_SPEECH_START:
            break;
        case ALI_CMD_ASR_START:
            g_asr_start = 1;
            jz_log_dump(JZ_APP_COM_MOD, "ALI_CMD_ASR_START \n");
            break;
        case ALI_CMD_ASR_INCOMPLETE:
            jz_log_info(JZ_APP_COM_MOD, "ALI_CMD_ASR_INCOMPLETE data:%s\n", msg->data);
            jz_lv_mutex_lock();
            if(g_asr_start) {
                g_asr_start = 0;
                chat_start_speech(0);
            }
            chat_update_speech_full_text(0, msg->data);
            jz_lv_mutex_unlock();
            break;
        case ALI_CMD_ASR_COMPLETE:
            jz_log_info(JZ_APP_COM_MOD, "ALI_CMD_ASR_COMPLETE data:%s\n", msg->data);
            jz_lv_mutex_lock();
            chat_update_speech_full_text(0, msg->data);
            jz_lv_mutex_unlock();
            break;
        case ALI_CMD_ASR_END:
            jz_log_dump(JZ_APP_COM_MOD, "ALI_CMD_ASR_END \n");
            jz_lv_mutex_lock();
            chat_end_speech(0);
            jz_lv_mutex_unlock();
            break;
        case ALI_CMD_LLM_INCOMPLETE:
            jz_log_info(JZ_APP_COM_MOD, "ALI_CMD_LLM_INCOMPLETE data:%s\n", msg->data);
            if(g_LLM_speech_start == 0) {
                jz_lv_mutex_lock();
                chat_start_speech(1);
                jz_lv_mutex_unlock();
                g_LLM_speech_start = 1;
            }
            jz_lv_mutex_lock();
            chat_update_speech_full_text(1, msg->data);
            jz_lv_mutex_unlock();
            break;
        case ALI_CMD_LLM_COMPLETE:
            jz_log_info(JZ_APP_COM_MOD, "ALI_CMD_LLM_COMPLETE data:%s\n", msg->data);
            if(g_LLM_speech_start == 0) {
                jz_lv_mutex_lock();
                chat_start_speech(1);
                jz_lv_mutex_unlock();
            }
            jz_lv_mutex_lock();
            chat_update_speech_full_text(1, msg->data);
            chat_end_speech(1);
            jz_lv_mutex_unlock();
            g_LLM_speech_start = 0;
            break;
        case ALI_CMD_TTS_START:
            break;
        case ALI_CMD_TTS_END:
            jz_lv_mutex_lock();
            chat_update_status_bar("请说话...");
            jz_lv_mutex_unlock();
            break;
        default:
            jz_log_warning(JZ_APP_COM_MOD, "not support,cmd:0x%X\n", msg->cmd);
            break;
    }

    if(msg->data) {
        free(msg->data);
        msg->data = NULL;
    }

    return;
}

int jz_audio_hmi_init(void)
{
    // 步骤1：注册新业务处理函数
    int ret = jz_hmi_register_ui_handler(JZ_HMI_CMD_AUDIO, jz_audio_hmi_event_handler, "hmi_audio");
    if (ret != 0) {
        jz_log_error(JZ_APP_COM_MOD, "[TEST] Register UI handler failed!\n");
        return -1;
    }

    jz_log_info(JZ_APP_COM_MOD, "[TEST] New registered msg process success!\n");
    return 0;
}
#else
int jz_audio_hmi_init(void)
{
    return 0;
}
#endif
