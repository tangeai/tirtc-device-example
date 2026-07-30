if(COMMAND idf_component_register)
    idf_component_register(
        SRCS
            "src/session_runtime.c"
            "src/session_ai.c"
            "src/session_ai_call.c"
            "src/session_call.c"
            "src/session_contact_resolver.c"
            "src/session_handoff_guard.c"
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "src"
        REQUIRES device_core media_runtime platform_client tirtc_adapter
        PRIV_REQUIRES json esp_system esp_timer
    )
else()
    cmake_minimum_required(VERSION 3.16)
    project(session_contact_resolver C)

    add_library(session_contact_resolver STATIC
        "src/session_contact_resolver.c"
    )
    target_include_directories(session_contact_resolver PUBLIC "src")
    target_compile_features(session_contact_resolver PUBLIC c_std_99)

    add_library(session_handoff_guard STATIC
        "src/session_handoff_guard.c"
    )
    target_include_directories(session_handoff_guard PUBLIC "src")
    target_compile_features(session_handoff_guard PUBLIC c_std_99)

    include(CTest)
    if(BUILD_TESTING)
        add_executable(session_contact_resolver_tests
            "tests/test_session_contact_resolver.c"
        )
        target_link_libraries(session_contact_resolver_tests
            PRIVATE session_contact_resolver
        )
        add_test(
            NAME session_contact_resolver_tests
            COMMAND session_contact_resolver_tests
        )
        add_executable(session_handoff_guard_tests
            "tests/test_session_handoff_guard.c"
        )
        target_link_libraries(session_handoff_guard_tests
            PRIVATE session_handoff_guard
        )
        add_test(
            NAME session_handoff_guard_tests
            COMMAND session_handoff_guard_tests
        )
    endif()
endif()
