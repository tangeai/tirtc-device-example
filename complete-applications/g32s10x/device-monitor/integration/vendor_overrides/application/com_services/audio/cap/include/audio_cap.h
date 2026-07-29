#ifndef AUDIO_CAP_H
#define AUDIO_CAP_H

/**
 * @brief 获取录音功能是否开启
 * @return 0-off 1-on
 */
int audio_cap_status_get(void);

/**
 * @brief Configure whether the capture thread uses the VOXA 3A pipeline.
 * @param enabled 0 selects raw microphone capture, non-zero enables VOXA.
 * @return 0 on success, -1 while capture is already active.
 */
int jz_audio_cap_set_3a_enabled(int enabled);

void jz_audio_cap_set_stop_status(void);

int jz_audio_cap_start(void);

int jz_audio_cap_stop(void);

#endif // AUDIO_CAP_H
