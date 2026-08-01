idf_component_register(
    SRCS
        "app_main.c"
        "app_identity.c"
        "app_at_parser.c"
        "app_at_facade.c"
        "app_at_commands.c"
        "app_controller.c"
        "app_user_log.c"
        "app_stdio_filter.c"
    INCLUDE_DIRS "."
    REQUIRES
        at_server
        wifi_manager
        platform_client
        runtime_config
        device_core
        session_runtime
        media_runtime
        tirtc_adapter
        nvs_flash
        esp_hw_support
        esp_app_format
    PRIV_REQUIRES
        esp_event
        esp_netif
        esp_timer
)

target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=printf")

spiffs_create_partition_image(storage "${PROJECT_DIR}/media" FLASH_IN_PROJECT)
