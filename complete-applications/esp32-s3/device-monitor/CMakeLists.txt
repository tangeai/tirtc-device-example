# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# OTA firmware version rule: see docs/release/OTA_VERSIONING_CN.md.
set(PROJECT_VER "0.7.5")
project(sample_project)

if(TARGET lvgl_port_lib)
    target_compile_options(lvgl_port_lib PRIVATE -Wno-deprecated-declarations)
endif()
