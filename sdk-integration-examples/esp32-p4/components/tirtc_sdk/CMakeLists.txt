idf_component_register(INCLUDE_DIRS "include")

add_prebuilt_library(tirtc_sdk_prebuilt
    "${CMAKE_CURRENT_LIST_DIR}/lib/esp32p4/libTiRTC.a"
    REQUIRES freertos lwip mbedtls esp_timer
)

target_link_libraries(${COMPONENT_LIB} INTERFACE tirtc_sdk_prebuilt)
target_include_directories(${COMPONENT_LIB} INTERFACE "${CMAKE_CURRENT_LIST_DIR}/include")
