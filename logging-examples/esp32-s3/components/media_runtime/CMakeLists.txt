idf_component_register(
    SRCS "src/media_runtime.c"
    INCLUDE_DIRS "include"
    REQUIRES device_core tirtc_adapter
    PRIV_REQUIRES json spiffs esp_timer esp_psram
)
