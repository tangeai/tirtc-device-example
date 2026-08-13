set(DEVICE_CORE_SRCS
    "src/device_media.c"
    "src/device_media_file.c"
    "src/device_session.c"
    "src/device_utf8.c"
)

if(COMMAND idf_component_register)
    idf_component_register(
        SRCS ${DEVICE_CORE_SRCS}
        INCLUDE_DIRS "include"
    )
else()
    cmake_minimum_required(VERSION 3.16)
    project(device_core C)

    add_library(device_core STATIC ${DEVICE_CORE_SRCS})
    target_include_directories(device_core PUBLIC "include")
    target_compile_features(device_core PUBLIC c_std_99)

    include(CTest)
    if(BUILD_TESTING)
        add_executable(device_core_tests tests/test_device_core.c)
        target_link_libraries(device_core_tests PRIVATE device_core)
        add_test(NAME device_core_tests COMMAND device_core_tests)
    endif()
endif()
