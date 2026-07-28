#include "rtc_transport.h"

esp_err_t rtc_transport_init(const rtc_transport_config_t *config)
{
    return tirtc_session_init(config);
}

esp_err_t rtc_transport_configure(const rtc_transport_config_t *config)
{
    return tirtc_session_configure(config);
}

void rtc_transport_set_identity_ready(bool ready)
{
    tirtc_session_set_identity_ready(ready);
}

esp_err_t rtc_transport_prepare_sdk(void)
{
    return tirtc_session_prepare_sdk();
}

esp_err_t rtc_transport_set_media_bridge(const rtc_transport_media_ops_t *ops, void *ctx)
{
    return tirtc_session_set_media_bridge(ops, ctx);
}

void rtc_transport_set_hooks(const rtc_transport_hooks_t *hooks, void *ctx)
{
    tirtc_session_set_hooks(hooks, ctx);
}

void rtc_transport_set_control_ops(const rtc_transport_control_ops_t *ops, void *ctx)
{
    tirtc_session_set_control_ops(ops, ctx);
}

esp_err_t rtc_transport_register_observer(const rtc_transport_observer_t *observer, void *ctx)
{
    return tirtc_session_register_observer(observer, ctx);
}

esp_err_t rtc_transport_start_if_ready(void)
{
    return tirtc_session_prepare_sdk();
}

esp_err_t rtc_transport_connect_peer(const char *remote_device_id, const char *remote_device_secret_key)
{
    return tirtc_session_connect_peer(remote_device_id, remote_device_secret_key);
}

esp_err_t rtc_transport_connect_peer_with_token(const char *remote_device_id, const char *connect_token)
{
    return tirtc_session_connect_peer_with_token(remote_device_id, connect_token);
}

esp_err_t rtc_transport_send_command(uint32_t cmdw, const void *data, size_t data_len)
{
    return tirtc_session_send_active_command(cmdw, data, data_len);
}

void rtc_transport_set_next_connection_auto_media(bool enabled)
{
    tirtc_session_set_next_connection_auto_media(enabled);
}

void rtc_transport_set_next_connection_defer_media(bool enabled)
{
    tirtc_session_set_next_connection_defer_media(enabled);
}

esp_err_t rtc_transport_restart(void)
{
    return tirtc_session_restart();
}

esp_err_t rtc_transport_stop(void)
{
    return tirtc_session_disconnect();
}

esp_err_t rtc_transport_disconnect(void)
{
    return tirtc_session_disconnect();
}

void rtc_transport_flush_remote_media(void)
{
    tirtc_session_flush_remote_media();
}

esp_err_t rtc_transport_accept_incoming_call(void)
{
    return tirtc_session_accept_incoming_call();
}

esp_err_t rtc_transport_reject_incoming_call(void)
{
    return tirtc_session_reject_incoming_call();
}

esp_err_t rtc_transport_hangup(void)
{
    return tirtc_session_hangup();
}

void rtc_transport_on_network_state_changed(const rtc_transport_network_state_t *state)
{
    tirtc_session_on_network_state_changed(state);
}

esp_err_t rtc_transport_set_local_video_send_enabled(bool enabled)
{
    return tirtc_session_set_local_video_send_enabled(enabled);
}

esp_err_t rtc_transport_set_local_audio_send_enabled(bool enabled)
{
    return tirtc_session_set_local_audio_send_enabled(enabled);
}

esp_err_t rtc_transport_set_remote_audio_stream_id(uint8_t stream_id)
{
    return tirtc_session_set_remote_audio_stream_id(stream_id);
}

esp_err_t rtc_transport_use_builtin_media(void)
{
    return tirtc_session_use_builtin_media();
}

esp_err_t rtc_transport_query_peer_state(void)
{
    return tirtc_session_query_peer_state();
}

bool rtc_transport_get_last_peer_state(rtc_transport_peer_state_t *state)
{
    return tirtc_session_get_last_peer_state(state);
}

rtc_transport_state_t rtc_transport_get_state(void)
{
    return tirtc_session_get_state();
}

void rtc_transport_get_config(rtc_transport_config_t *config)
{
    tirtc_session_get_config(config);
}

void rtc_transport_get_stats(rtc_transport_stats_t *stats)
{
    tirtc_session_get_stats(stats);
}

void rtc_transport_refresh_media_policy(void)
{
    tirtc_session_refresh_media_policy();
}
