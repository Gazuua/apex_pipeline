# apex_services/gateway — Gateway MVP (v0.5.2.0)

add_executable(apex_gateway
    src/main.cpp
    src/gateway_service.cpp
    src/gateway_config_parser.cpp
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
        apex_protocols_tcp
        apex_protocols_websocket
        apex_protocols_kafka
        apex_shared_adapters_common
        apex_shared_adapters_kafka
        apex_shared_adapters_redis
        jwt-cpp::jwt-cpp
        OpenSSL::SSL
        OpenSSL::Crypto
        tomlplusplus::tomlplusplus
        spdlog::spdlog
)

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
        apex_core spdlog::spdlog GTest::gtest GTest::gtest_main)
    if(WIN32)
        target_compile_options(test_gateway_route_table PRIVATE /utf-8)
        target_compile_definitions(test_gateway_route_table PRIVATE _WIN32_WINNT=0x0A00)
    endif()
    add_test(NAME test_gateway_route_table COMMAND test_gateway_route_table)

    add_executable(test_gateway_jwt_verifier
        tests/test_jwt_verifier.cpp
        src/jwt_verifier.cpp
    )
    target_include_directories(test_gateway_jwt_verifier PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(test_gateway_jwt_verifier PRIVATE
        apex_core jwt-cpp::jwt-cpp OpenSSL::SSL OpenSSL::Crypto
        spdlog::spdlog GTest::gtest GTest::gtest_main)
    if(WIN32)
        target_compile_options(test_gateway_jwt_verifier PRIVATE /utf-8)
        target_compile_definitions(test_gateway_jwt_verifier PRIVATE _WIN32_WINNT=0x0A00)
    endif()
    add_test(NAME test_gateway_jwt_verifier COMMAND test_gateway_jwt_verifier)

    add_executable(test_gateway_file_watcher
        tests/test_file_watcher.cpp
        src/file_watcher.cpp
    )
    target_include_directories(test_gateway_file_watcher PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(test_gateway_file_watcher PRIVATE
        apex_core spdlog::spdlog GTest::gtest GTest::gtest_main
        Boost::headers)
    if(WIN32)
        target_compile_options(test_gateway_file_watcher PRIVATE /utf-8)
        target_compile_definitions(test_gateway_file_watcher PRIVATE _WIN32_WINNT=0x0A00)
        target_link_libraries(test_gateway_file_watcher PRIVATE ws2_32 mswsock)
    endif()
    add_test(NAME test_gateway_file_watcher COMMAND test_gateway_file_watcher)

    add_executable(test_gateway_pending_requests
        tests/test_pending_requests.cpp
        src/pending_requests.cpp
    )
    target_include_directories(test_gateway_pending_requests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(test_gateway_pending_requests PRIVATE
        apex_core spdlog::spdlog GTest::gtest GTest::gtest_main)
    if(WIN32)
        target_compile_options(test_gateway_pending_requests PRIVATE /utf-8)
        target_compile_definitions(test_gateway_pending_requests PRIVATE _WIN32_WINNT=0x0A00)
    endif()
    add_test(NAME test_gateway_pending_requests COMMAND test_gateway_pending_requests)

    add_executable(test_gateway_channel_session_map
        tests/test_channel_session_map.cpp
        src/channel_session_map.cpp
    )
    target_include_directories(test_gateway_channel_session_map PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(test_gateway_channel_session_map PRIVATE
        apex_core spdlog::spdlog GTest::gtest GTest::gtest_main)
    if(WIN32)
        target_compile_options(test_gateway_channel_session_map PRIVATE /utf-8)
        target_compile_definitions(test_gateway_channel_session_map PRIVATE _WIN32_WINNT=0x0A00)
    endif()
    add_test(NAME test_gateway_channel_session_map COMMAND test_gateway_channel_session_map)
endif()
