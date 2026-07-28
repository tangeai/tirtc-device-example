cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/tirtc_sdk")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "1.1.1")
project(tirtc_esp32s3_wifi_link_demo)
