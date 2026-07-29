idf_component_register(
    SRCS
        "app_main.c"
        "system/boot_button.c"
        "system/wifi_sta.c"
        "system/time_sync.c"
        "tirtc/tirtc_app.c"
        "tirtc/tirtc_connect.c"
        "tirtc/h264_file_source.c"
        "tirtc/local_media_stream.c"
    INCLUDE_DIRS
        "."
        "system"
        "tirtc"
    REQUIRES driver esp_event esp_netif esp_rom esp_timer esp_wifi mbedtls spiffs tirtc_sdk
)

spiffs_create_partition_image(storage "${CMAKE_CURRENT_LIST_DIR}/tirtc/test_assets" FLASH_IN_PROJECT)
