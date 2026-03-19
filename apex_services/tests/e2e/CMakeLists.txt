# apex_services/tests/e2e — E2E integration tests
# Requires: Docker-based infrastructure (services running in containers)
# Run with: ctest -L e2e
# Run stress only: ctest -L e2e_stress

find_package(GTest CONFIG REQUIRED)
include(GoogleTest)
find_package(flatbuffers CONFIG REQUIRED)
find_package(Boost REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(apex_e2e_tests
    e2e_test_fixture.cpp
    e2e_auth_test.cpp
    e2e_chat_test.cpp
    e2e_ratelimit_test.cpp
    e2e_timeout_test.cpp
)

# Depend on chat-svc FlatBuffers generation (chat_room_generated.h, chat_message_generated.h)
add_dependencies(apex_e2e_tests chat_svc_fbs_generate)

target_include_directories(apex_e2e_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        # Chat Service FlatBuffers generated headers
        ${CMAKE_BINARY_DIR}/apex_services/chat-svc/generated
)

target_link_libraries(apex_e2e_tests
    PRIVATE
        apex_core
        apex_protocols_tcp
        flatbuffers::flatbuffers
        Boost::headers
        OpenSSL::SSL
        OpenSSL::Crypto
        GTest::gtest
)

apex_set_warnings(apex_e2e_tests)

if(WIN32)
    target_compile_options(apex_e2e_tests PRIVATE /utf-8)
    target_compile_definitions(apex_e2e_tests PRIVATE _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(apex_e2e_tests PRIVATE ws2_32 mswsock)
endif()

# 스트레스 E2E 테스트 (nightly/Valgrind용)
add_executable(apex_e2e_stress_tests
    e2e_test_fixture.cpp
    e2e_stress_connection.cpp
    e2e_stress_protocol.cpp
    e2e_stress_infra.cpp
    e2e_stress_concurrency.cpp
)

# Depend on chat-svc FlatBuffers generation (chat_room_generated.h, chat_message_generated.h)
add_dependencies(apex_e2e_stress_tests chat_svc_fbs_generate)

target_include_directories(apex_e2e_stress_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        # Chat Service FlatBuffers generated headers
        ${CMAKE_BINARY_DIR}/apex_services/chat-svc/generated
)

target_link_libraries(apex_e2e_stress_tests
    PRIVATE
        apex_core
        apex_protocols_tcp
        flatbuffers::flatbuffers
        Boost::headers
        OpenSSL::SSL
        OpenSSL::Crypto
        GTest::gtest
)

apex_set_warnings(apex_e2e_stress_tests)

if(WIN32)
    target_compile_options(apex_e2e_stress_tests PRIVATE /utf-8)
    target_compile_definitions(apex_e2e_stress_tests PRIVATE _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(apex_e2e_stress_tests PRIVATE ws2_32 mswsock)
endif()

# E2E tests are labeled separately from unit tests
# Run only E2E: ctest -L e2e
# Exclude E2E: ctest -LE e2e
gtest_discover_tests(apex_e2e_tests
    PROPERTIES LABELS "e2e" TIMEOUT 120
    DISCOVERY_TIMEOUT 60
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

gtest_discover_tests(apex_e2e_stress_tests
    PROPERTIES LABELS "e2e;e2e_stress" TIMEOUT 300
    DISCOVERY_TIMEOUT 60
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
