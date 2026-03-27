# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

# apex_services/gateway — Gateway MVP (v0.5.2.0)

add_executable(apex_gateway
    src/main.cpp
    src/gateway_service.cpp
    src/gateway_config_parser.cpp
    src/gateway_pipeline.cpp
    src/jwt_verifier.cpp
    src/jwt_blacklist.cpp
    src/route_table.cpp
    src/message_router.cpp
    src/file_watcher.cpp
    src/config_reloader.cpp
    src/pending_requests.cpp
    src/response_dispatcher.cpp
    src/pubsub_listener.cpp
    src/broadcast_fanout.cpp
    src/channel_session_map.cpp
)

target_include_directories(apex_gateway
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# toml++
find_package(tomlplusplus CONFIG REQUIRED)

# spdlog
find_package(spdlog CONFIG REQUIRED)

# jwt-cpp
find_package(jwt-cpp CONFIG REQUIRED)

# OpenSSL (for TLS + jwt-cpp)
find_package(OpenSSL REQUIRED)

# Boost (headers only — for file_watcher tests)
find_package(Boost REQUIRED)

target_link_libraries(apex_gateway
    PRIVATE
        apex_core
        apex_shared
        apex_protocols_tcp
        apex_protocols_websocket
        apex_protocols_kafka
        apex_shared_adapters_common
        apex_shared_adapters_kafka
        apex_shared_adapters_redis
        apex_shared_rate_limit
        jwt-cpp::jwt-cpp
        OpenSSL::SSL
        OpenSSL::Crypto
        tomlplusplus::tomlplusplus
        spdlog::spdlog
)

apex_set_warnings(apex_gateway)

# GCC 14 -O3: picojson (jwt-cpp 의존) false positive -Wmaybe-uninitialized.
# 템플릿 인라이닝 경로에서 발생하는 외부 라이브러리 경고 — 코드 수정 불가.
if(NOT MSVC)
    set_source_files_properties(src/jwt_verifier.cpp PROPERTIES
        COMPILE_OPTIONS "-Wno-maybe-uninitialized")
endif()

if(WIN32)
    target_compile_options(apex_gateway PRIVATE /utf-8)
    target_compile_definitions(apex_gateway PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(apex_gateway PRIVATE ws2_32 mswsock)
endif()

# --- Tests ---
if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)

    add_executable(test_gateway_route_table
        tests/test_route_table.cpp
        src/route_table.cpp
    )
    target_include_directories(test_gateway_route_table PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(test_gateway_route_table PRIVATE
        apex_core apex_shared spdlog::spdlog GTest::gtest GTest::gtest_main)
    if(WIN32)
        target_compile_options(test_gateway_route_table PRIVATE /utf-8)
        target_compile_definitions(test_gateway_route_table PRIVATE _WIN32_WINNT=0x0A00)
    endif()
    apex_set_warnings(test_gateway_route_table)
    add_test(NAME test_gateway_route_table COMMAND test_gateway_route_table)

    add_executable(test_gateway_jwt_verifier
        tests/test_jwt_verifier.cpp
        src/jwt_verifier.cpp
    )
    target_include_directories(test_gateway_jwt_verifier PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(test_gateway_jwt_verifier PRIVATE
        apex_core apex_shared jwt-cpp::jwt-cpp OpenSSL::SSL OpenSSL::Crypto
        spdlog::spdlog GTest::gtest GTest::gtest_main)
    if(WIN32)
        target_compile_options(test_gateway_jwt_verifier PRIVATE /utf-8)
        target_compile_definitions(test_gateway_jwt_verifier PRIVATE _WIN32_WINNT=0x0A00)
    endif()
    apex_set_warnings(test_gateway_jwt_verifier)
    add_test(NAME test_gateway_jwt_verifier COMMAND test_gateway_jwt_verifier)

    add_executable(test_gateway_file_watcher
        tests/test_file_watcher.cpp
        src/file_watcher.cpp
    )
    target_include_directories(test_gateway_file_watcher PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(test_gateway_file_watcher PRIVATE
        apex_core apex_shared spdlog::spdlog GTest::gtest GTest::gtest_main
        Boost::headers)
    if(WIN32)
        target_compile_options(test_gateway_file_watcher PRIVATE /utf-8)
        target_compile_definitions(test_gateway_file_watcher PRIVATE _WIN32_WINNT=0x0A00)
        target_link_libraries(test_gateway_file_watcher PRIVATE ws2_32 mswsock)
    endif()
    apex_set_warnings(test_gateway_file_watcher)
    add_test(NAME test_gateway_file_watcher COMMAND test_gateway_file_watcher)

    add_executable(test_gateway_pending_requests
        tests/test_pending_requests.cpp
        src/pending_requests.cpp
    )
    target_include_directories(test_gateway_pending_requests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(test_gateway_pending_requests PRIVATE
        apex_core apex_shared spdlog::spdlog GTest::gtest GTest::gtest_main)
    if(WIN32)
        target_compile_options(test_gateway_pending_requests PRIVATE /utf-8)
        target_compile_definitions(test_gateway_pending_requests PRIVATE _WIN32_WINNT=0x0A00)
    endif()
    apex_set_warnings(test_gateway_pending_requests)
    add_test(NAME test_gateway_pending_requests COMMAND test_gateway_pending_requests)

    add_executable(test_gateway_channel_session_map
        tests/test_channel_session_map.cpp
        src/channel_session_map.cpp
    )
    target_include_directories(test_gateway_channel_session_map PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(test_gateway_channel_session_map PRIVATE
        apex_core apex_shared spdlog::spdlog GTest::gtest GTest::gtest_main)
    if(WIN32)
        target_compile_options(test_gateway_channel_session_map PRIVATE /utf-8)
        target_compile_definitions(test_gateway_channel_session_map PRIVATE _WIN32_WINNT=0x0A00)
    endif()
    apex_set_warnings(test_gateway_channel_session_map)
    add_test(NAME test_gateway_channel_session_map COMMAND test_gateway_channel_session_map)

    # --- Message Router tests (RouteTable + Envelope) ---
    add_executable(test_gateway_message_router
        tests/test_message_router.cpp
        src/route_table.cpp
    )
    target_include_directories(test_gateway_message_router PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/apex_services)
    target_link_libraries(test_gateway_message_router PRIVATE
        apex_core apex_shared apex_protocols_kafka apex_shared_adapters_kafka
        spdlog::spdlog GTest::gtest GTest::gtest_main)
    if(WIN32)
        target_compile_options(test_gateway_message_router PRIVATE /utf-8)
        target_compile_definitions(test_gateway_message_router PRIVATE _WIN32_WINNT=0x0A00)
    endif()
    apex_set_warnings(test_gateway_message_router)
    add_test(NAME test_gateway_message_router COMMAND test_gateway_message_router)

    # --- Gateway Pipeline tests (rate limit, auth state) ---
    add_executable(test_gateway_pipeline
        tests/test_gateway_pipeline.cpp
        src/gateway_pipeline.cpp
        src/jwt_verifier.cpp
        src/jwt_blacklist.cpp
    )
    target_include_directories(test_gateway_pipeline PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(test_gateway_pipeline PRIVATE
        apex_core apex_shared apex_shared_rate_limit apex_shared_adapters_redis
        jwt-cpp::jwt-cpp OpenSSL::SSL OpenSSL::Crypto
        spdlog::spdlog Boost::headers GTest::gtest GTest::gtest_main)
    if(WIN32)
        target_compile_options(test_gateway_pipeline PRIVATE /utf-8)
        target_compile_definitions(test_gateway_pipeline PRIVATE _WIN32_WINNT=0x0A00)
    endif()
    apex_set_warnings(test_gateway_pipeline)
    add_test(NAME test_gateway_pipeline COMMAND test_gateway_pipeline)

    # --- Gateway Pipeline Error tests (template policy + mock + coroutine) ---
    add_executable(test_gateway_pipeline_errors
        tests/test_gateway_pipeline_errors.cpp
    )
    target_include_directories(test_gateway_pipeline_errors PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(test_gateway_pipeline_errors PRIVATE
        apex_core apex_shared
        spdlog::spdlog Boost::headers GTest::gtest GTest::gtest_main)
    if(WIN32)
        target_compile_options(test_gateway_pipeline_errors PRIVATE /utf-8)
        target_compile_definitions(test_gateway_pipeline_errors PRIVATE _WIN32_WINNT=0x0A00)
    endif()
    apex_set_warnings(test_gateway_pipeline_errors)
    add_test(NAME test_gateway_pipeline_errors COMMAND test_gateway_pipeline_errors)

    # --- JWT Blacklist tests (JTI input validation) ---
    add_executable(test_gateway_jwt_blacklist
        tests/test_jwt_blacklist.cpp
        src/jwt_blacklist.cpp
    )
    target_include_directories(test_gateway_jwt_blacklist PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(test_gateway_jwt_blacklist PRIVATE
        apex_core apex_shared apex_shared_adapters_redis
        spdlog::spdlog GTest::gtest GTest::gtest_main)
    if(WIN32)
        target_compile_options(test_gateway_jwt_blacklist PRIVATE /utf-8)
        target_compile_definitions(test_gateway_jwt_blacklist PRIVATE _WIN32_WINNT=0x0A00)
    endif()
    apex_set_warnings(test_gateway_jwt_blacklist)
    add_test(NAME test_gateway_jwt_blacklist COMMAND test_gateway_jwt_blacklist)

    # --- Config Parser tests (parse_gateway_config TOML field coverage) ---
    add_executable(test_gateway_config_parser
        tests/test_gateway_config_parser.cpp
        src/gateway_config_parser.cpp
    )
    target_include_directories(test_gateway_config_parser PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(test_gateway_config_parser PRIVATE
        apex_core apex_shared tomlplusplus::tomlplusplus
        spdlog::spdlog GTest::gtest GTest::gtest_main)
    if(WIN32)
        target_compile_options(test_gateway_config_parser PRIVATE /utf-8)
        target_compile_definitions(test_gateway_config_parser PRIVATE _WIN32_WINNT=0x0A00)
    endif()
    apex_set_warnings(test_gateway_config_parser)
    add_test(NAME test_gateway_config_parser COMMAND test_gateway_config_parser)

    # --- Config Reloader tests (TOML parse + RouteTable build) ---
    add_executable(test_gateway_config_reloader
        tests/test_config_reloader.cpp
        src/gateway_config_parser.cpp
        src/route_table.cpp
    )
    target_include_directories(test_gateway_config_reloader PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(test_gateway_config_reloader PRIVATE
        apex_core apex_shared apex_shared_rate_limit tomlplusplus::tomlplusplus
        spdlog::spdlog Boost::headers GTest::gtest GTest::gtest_main)
    if(WIN32)
        target_compile_options(test_gateway_config_reloader PRIVATE /utf-8)
        target_compile_definitions(test_gateway_config_reloader PRIVATE _WIN32_WINNT=0x0A00)
    endif()
    apex_set_warnings(test_gateway_config_reloader)
    add_test(NAME test_gateway_config_reloader COMMAND test_gateway_config_reloader)

    # --- BroadcastFanout tests (build_wire_frame pure static function) ---
    add_executable(test_gateway_broadcast_fanout
        tests/test_broadcast_fanout.cpp
        src/broadcast_fanout.cpp
        src/channel_session_map.cpp
    )
    target_include_directories(test_gateway_broadcast_fanout PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(test_gateway_broadcast_fanout PRIVATE
        apex_core apex_shared spdlog::spdlog GTest::gtest GTest::gtest_main)
    if(WIN32)
        target_compile_options(test_gateway_broadcast_fanout PRIVATE /utf-8)
        target_compile_definitions(test_gateway_broadcast_fanout PRIVATE _WIN32_WINNT=0x0A00)
    endif()
    apex_set_warnings(test_gateway_broadcast_fanout)
    add_test(NAME test_gateway_broadcast_fanout COMMAND test_gateway_broadcast_fanout)

    # Gateway 단위 테스트 타임아웃 (30초)
    set_tests_properties(
        test_gateway_route_table
        test_gateway_jwt_verifier
        test_gateway_jwt_blacklist
        test_gateway_file_watcher
        test_gateway_pending_requests
        test_gateway_channel_session_map
        test_gateway_message_router
        test_gateway_pipeline
        test_gateway_pipeline_errors
        test_gateway_config_parser
        test_gateway_config_reloader
        test_gateway_broadcast_fanout
        PROPERTIES TIMEOUT 30
    )
endif()
