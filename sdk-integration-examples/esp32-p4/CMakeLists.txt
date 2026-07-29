cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "1.1.1")
project(tirtc_esp32p4_wifi_link_demo)

set(FIRMWARE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/firmware")

add_custom_target(collect_firmware_outputs ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FIRMWARE_OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FIRMWARE_OUTPUT_DIR}/bootloader"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FIRMWARE_OUTPUT_DIR}/partition_table"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.bin"
            "${FIRMWARE_OUTPUT_DIR}/${PROJECT_NAME}.bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/bootloader/bootloader.bin"
            "${FIRMWARE_OUTPUT_DIR}/bootloader/bootloader.bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/partition_table/partition-table.bin"
            "${FIRMWARE_OUTPUT_DIR}/partition_table/partition-table.bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/ota_data_initial.bin"
            "${FIRMWARE_OUTPUT_DIR}/ota_data_initial.bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/storage.bin"
            "${FIRMWARE_OUTPUT_DIR}/storage.bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/flash_args"
            "${FIRMWARE_OUTPUT_DIR}/flash_args"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/flash_project_args"
            "${FIRMWARE_OUTPUT_DIR}/flash_project_args"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/flasher_args.json"
            "${FIRMWARE_OUTPUT_DIR}/flasher_args.json"
    COMMENT "Collecting firmware outputs into ${FIRMWARE_OUTPUT_DIR}"
)

add_dependencies(collect_firmware_outputs
    app
    bootloader
    partition_table_bin
    spiffs_storage_bin
)
