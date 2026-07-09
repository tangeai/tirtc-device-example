#include "app_internal.h"

#include <stdint.h>

#include "device_online.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#ifndef CONFIG_APP_DEVICE_ONLINE_PAUSE_DURING_RTC
#define CONFIG_APP_DEVICE_ONLINE_PAUSE_DURING_RTC 1
#endif

static const uint64_t APP_RTC_RATE_WINDOW_US = 1000000ULL;

typedef struct {
	uint32_t tx_video_frames;
	uint32_t tx_audio_frames;
	uint32_t rx_audio_frames;
	uint16_t tx_video_fps;
	uint16_t tx_audio_fps;
	uint16_t rx_audio_fps;
	int64_t last_sample_us;
} app_rtc_rate_state_t;

static app_control_state_t s_control_state = {
	.video_enabled = true,
	.audio_enabled = true,
};
static bool s_last_call_active;
static portMUX_TYPE s_control_lock = portMUX_INITIALIZER_UNLOCKED;
static app_rtc_rate_state_t s_rtc_rate_state;
static portMUX_TYPE s_rtc_rate_lock = portMUX_INITIALIZER_UNLOCKED;

static uint16_t app_state_compute_frame_rate(uint32_t frame_delta, uint64_t elapsed_us)
{
	if (elapsed_us == 0) {
		return 0;
	}

	uint64_t fps = (((uint64_t)frame_delta * 1000000ULL) + (elapsed_us / 2ULL)) / elapsed_us;
	return fps > UINT16_MAX ? UINT16_MAX : (uint16_t)fps;
}

app_control_state_t app_state_get_control(void)
{
	app_control_state_t control = {0};

	taskENTER_CRITICAL(&s_control_lock);
	control = s_control_state;
	taskEXIT_CRITICAL(&s_control_lock);

	return control;
}

bool app_state_is_call_active(void)
{
	rtc_transport_stats_t rtc = {0};

	rtc_transport_get_stats(&rtc);
	return rtc.call_active;
}

void app_state_set_video_enabled(bool enabled)
{
	taskENTER_CRITICAL(&s_control_lock);
	s_control_state.video_enabled = enabled;
	taskEXIT_CRITICAL(&s_control_lock);
}

void app_state_set_audio_enabled(bool enabled)
{
	taskENTER_CRITICAL(&s_control_lock);
	s_control_state.audio_enabled = enabled;
	taskEXIT_CRITICAL(&s_control_lock);
}

bool app_state_sync_call_media_defaults(bool call_active, app_control_state_t *control)
{
	bool changed = false;

	taskENTER_CRITICAL(&s_control_lock);
	if (s_last_call_active != call_active) {
		s_last_call_active = call_active;
		s_control_state.video_enabled = call_active;
		s_control_state.audio_enabled = call_active;
		changed = true;
	}
	if (control != NULL) {
		*control = s_control_state;
	}
	taskEXIT_CRITICAL(&s_control_lock);

	if (changed) {
#if CONFIG_APP_DEVICE_ONLINE_PAUSE_DURING_RTC
		device_online_set_realtime_media_active(call_active);
#else
		(void)device_online_report_state_async(call_active ? "call-active" : "call-idle");
#endif
	}
	return changed;
}

void app_state_fill_rtc_frame_rates(app_rtc_snapshot_t *snapshot, const rtc_transport_stats_t *rtc)
{
	int64_t now_us = esp_timer_get_time();

	if (snapshot == NULL || rtc == NULL) {
		return;
	}

	taskENTER_CRITICAL(&s_rtc_rate_lock);
	if (!rtc->active_connection || !rtc->call_active) {
		s_rtc_rate_state.tx_video_fps = 0;
		s_rtc_rate_state.tx_audio_fps = 0;
		s_rtc_rate_state.rx_audio_fps = 0;
		s_rtc_rate_state.tx_video_frames = rtc->tx_video_frames;
		s_rtc_rate_state.tx_audio_frames = rtc->tx_audio_frames;
		s_rtc_rate_state.rx_audio_frames = rtc->rx_audio_frames;
		s_rtc_rate_state.last_sample_us = now_us;
	} else if (s_rtc_rate_state.last_sample_us == 0 || now_us <= s_rtc_rate_state.last_sample_us) {
		s_rtc_rate_state.tx_video_frames = rtc->tx_video_frames;
		s_rtc_rate_state.tx_audio_frames = rtc->tx_audio_frames;
		s_rtc_rate_state.rx_audio_frames = rtc->rx_audio_frames;
		s_rtc_rate_state.last_sample_us = now_us;
	} else {
		uint64_t elapsed_us = (uint64_t)(now_us - s_rtc_rate_state.last_sample_us);
		uint32_t tx_video_delta = rtc->tx_video_frames - s_rtc_rate_state.tx_video_frames;
		uint32_t tx_audio_delta = rtc->tx_audio_frames - s_rtc_rate_state.tx_audio_frames;
		uint32_t rx_audio_delta = rtc->rx_audio_frames - s_rtc_rate_state.rx_audio_frames;

		if (elapsed_us >= APP_RTC_RATE_WINDOW_US) {
			s_rtc_rate_state.tx_video_fps = app_state_compute_frame_rate(tx_video_delta, elapsed_us);
			s_rtc_rate_state.tx_audio_fps = app_state_compute_frame_rate(tx_audio_delta, elapsed_us);
			s_rtc_rate_state.rx_audio_fps = app_state_compute_frame_rate(rx_audio_delta, elapsed_us);
			s_rtc_rate_state.tx_video_frames = rtc->tx_video_frames;
			s_rtc_rate_state.tx_audio_frames = rtc->tx_audio_frames;
			s_rtc_rate_state.rx_audio_frames = rtc->rx_audio_frames;
			s_rtc_rate_state.last_sample_us = now_us;
		} else if (tx_video_delta > 0 || tx_audio_delta > 0 || rx_audio_delta > 0) {
			s_rtc_rate_state.tx_video_fps = app_state_compute_frame_rate(tx_video_delta, elapsed_us);
			s_rtc_rate_state.tx_audio_fps = app_state_compute_frame_rate(tx_audio_delta, elapsed_us);
			s_rtc_rate_state.rx_audio_fps = app_state_compute_frame_rate(rx_audio_delta, elapsed_us);
		}
	}

	snapshot->tx_video_fps = s_rtc_rate_state.tx_video_fps;
	snapshot->tx_audio_fps = s_rtc_rate_state.tx_audio_fps;
	snapshot->rx_audio_fps = s_rtc_rate_state.rx_audio_fps;
	taskEXIT_CRITICAL(&s_rtc_rate_lock);
}
