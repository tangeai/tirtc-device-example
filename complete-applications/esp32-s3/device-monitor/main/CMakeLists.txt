set(APP_DEBUG_SCREEN_SERVER_SRCS "")
set(APP_DEBUG_SCREEN_SERVER_INCLUDE_DIRS "")
set(APP_SERIAL_NET_CLI_SRCS "")
set(APP_SERIAL_NET_CLI_INCLUDE_DIRS "")

if(CONFIG_APP_SERIAL_NET_CLI_ENABLE)
    list(APPEND APP_SERIAL_NET_CLI_SRCS
        "debug/serial_net_cli/serial_net_cli.c")
    list(APPEND APP_SERIAL_NET_CLI_INCLUDE_DIRS
        "debug/serial_net_cli")
endif()

if(CONFIG_APP_DEBUG_SCREEN_SERVER_ENABLE)
    list(APPEND APP_DEBUG_SCREEN_SERVER_SRCS
        "debug/screen_server/screen_debug_server.c")
    list(APPEND APP_DEBUG_SCREEN_SERVER_INCLUDE_DIRS
        "debug/screen_server")
endif()

idf_component_register(
    SRCS
        "app_main.c"
        "application/app.c"
        "application/app_ai_device_action.c"
        "application/app_ai_chat_config.c"
        "application/app_audio_config.c"
        "application/app_call.c"
        "application/app_call_contacts.c"
        "application/app_rtc_config.c"
        "application/app_snapshot.c"
        "application/app_state.c"
        "application/app_tirtc_config_scan.c"
        "application/app_wechat.c"
        "ui/app_ui.c"
        "ui/display.c"
        "ui/wifi_page_model.c"
        "ui/font/ai_chat_font.c"
        "ui/font/lv_font_cn_18.c"
        "ui/ai_chat_assets.c"
        "ui/ai_chat_avatar_assets.c"
        "ui/call_assets.c"
        "ui/home_assets.c"
        "ui/text_assets.c"
        "hardware/hardware_board.c"
        "connectivity/network.c"
        "connectivity/wifi.c"
        "platform/platform_nvs_async.c"
        "platform/platform_storage.c"
        "platform/platform_task_reaper.c"
        "platform/tirtc_task_stack_policy.c"
        "platform/system_time.c"
        "drivers/audio/audio_device.c"
        "drivers/audio/audio_alaw_codec.c"
        "drivers/audio/audio_echo_cancel.c"
        "drivers/audio/audio.c"
        "drivers/camera/camera_driver.c"
        "drivers/display/display_driver.c"
        "drivers/virtual_audio/virtual_audio_source.c"
        "protocols/tirtc/tirtc_session.c"
        "protocols/tirtc/tirtc_connect.c"
        "protocols/tirtc/tirtc_commands.c"
        "protocols/tirtc/tirtc_runtime.c"
        "protocols/tirtc/tirtc_token.c"
        "protocols/http/thing_http_client.c"
        "protocols/mqtt/binding_mqtt_client.c"
        "protocols/mqtt/thing_mqtt_client.c"
        "protocols/rtc/rtc_transport.c"
        "services/ai_chat.c"
        "services/ai_chat_events.c"
        "services/ai_chat_token.c"
        "services/device.c"
        "services/device_binding/device_auth_http.c"
        "services/device_binding/device_binding.c"
        "services/device_binding/device_binding_http.c"
        "services/device_binding/device_identity.c"
        "services/device_call/device_call.c"
        "services/device_call/device_call_ringtone.c"
        "services/device_online/device_online.c"
        "services/thing_connect/thing_service_registry.c"
        "services/media_sink.c"
        "services/ota.c"
        "services/qr_scanner.c"
        "services/rtc_media_bridge.c"
        "services/sender_test.c"
        "services/wechat_voip/wechat_voip_media.c"
        "services/wechat_voip/wechat_voip_api.c"
        "services/wechat_voip/wechat_voip_contacts.c"
        "services/wechat_voip/wechat_voip_service.c"
        "services/wechat_voip/wechat_voip_session.c"
        "services/wechat_voip/wechat_voip_thing.c"
        ${APP_SERIAL_NET_CLI_SRCS}
        ${APP_DEBUG_SCREEN_SERVER_SRCS}
    INCLUDE_DIRS "."
                 "application"
                 "hardware"
                 "ui"
                 "ui/font"
                 "connectivity"
                 "platform"
                 "drivers/audio"
                 "drivers/camera"
                 "drivers/display"
                 "drivers/virtual_audio"
                 "protocols/http"
                 "protocols/rtc"
                 "protocols/tirtc"
                 "protocols/mqtt"
                 "services"
                 "services/device_binding"
                 "services/device_call"
                 "services/device_online"
                 "services/thing_connect"
                 "services/wechat_voip"
                 ${APP_SERIAL_NET_CLI_INCLUDE_DIRS}
                 ${APP_DEBUG_SCREEN_SERVER_INCLUDE_DIRS}
    REQUIRES driver esp_driver_usb_serial_jtag esp_event esp_netif esp_wifi nvs_flash esp_timer esp_codec_dev tirtc_sdk esp_lcd lwip esp_hw_support esp_http_client app_update esp_app_format json mbedtls mqtt espressif__esp-sr
    WHOLE_ARCHIVE
)

set_source_files_properties("services/qr_scanner.c" PROPERTIES COMPILE_OPTIONS "-O3")

target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=freertos_ThreadCreateWithStackSize")
idf_component_get_property(QUIRC_LIB espressif__quirc COMPONENT_LIB)
target_compile_options(${QUIRC_LIB} PRIVATE -O3)
