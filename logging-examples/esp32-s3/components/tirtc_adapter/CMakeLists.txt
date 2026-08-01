idf_component_register(
    SRCS "src/tirtc_adapter.c"
    INCLUDE_DIRS "include"
    REQUIRES device_core
    PRIV_REQUIRES tirtc_sdk
)
