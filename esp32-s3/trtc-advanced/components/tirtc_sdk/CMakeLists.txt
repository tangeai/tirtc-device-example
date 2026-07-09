idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_http_client esp_netif esp_timer hal json lwip mbedtls soc
)

set(TIRTC_LIB "${CMAKE_CURRENT_LIST_DIR}/lib/esp32s3/libTiRTC.a")

if(NOT EXISTS "${TIRTC_LIB}")
    message(FATAL_ERROR "TiRTC static library not found: ${TIRTC_LIB}")
endif()

add_library(tirtc_single_prebuilt STATIC IMPORTED)
set_target_properties(tirtc_single_prebuilt PROPERTIES IMPORTED_LOCATION "${TIRTC_LIB}")

add_library(tirtc_link_bundle INTERFACE)
target_link_libraries(tirtc_link_bundle INTERFACE
    "-Wl,--start-group"
    tirtc_single_prebuilt
    $<TARGET_FILE:__idf_lwip>
    $<TARGET_FILE:__idf_mbedtls>
    $<TARGET_FILE:mbedcrypto>
    $<TARGET_FILE:mbedtls>
    $<TARGET_FILE:mbedx509>
    $<TARGET_FILE:everest>
    $<TARGET_FILE:p256m>
    $<TARGET_FILE:__idf_hal>
    $<TARGET_FILE:__idf_soc>
    "-Wl,--end-group")

target_link_libraries(${COMPONENT_LIB} INTERFACE tirtc_link_bundle)
