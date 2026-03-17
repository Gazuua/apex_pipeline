# apex_services/tests/e2e — E2E integration tests
# Requires: docker-compose infrastructure + running Gateway/Service processes
# Run with: ctest -L e2e

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

if(WIN32)
    target_compile_options(apex_e2e_tests PRIVATE /utf-8)
    target_compile_definitions(apex_e2e_tests PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(apex_e2e_tests PRIVATE ws2_32 mswsock)
endif()

# Inject service executable paths so E2E fixture can locate binaries at runtime.
# E2E_PROJECT_ROOT is used to set the working directory for spawned service
# processes, so that relative paths in TOML configs (e.g. key files) resolve
# correctly regardless of where ctest runs from.
target_compile_definitions(apex_e2e_tests PRIVATE
    E2E_GATEWAY_EXE="$<TARGET_FILE:apex_gateway>"
    E2E_AUTH_SVC_EXE="$<TARGET_FILE:auth_svc_main>"
    E2E_CHAT_SVC_EXE="$<TARGET_FILE:chat_svc_main>"
    E2E_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
)

# E2E tests are labeled separately from unit tests
# Run only E2E: ctest -L e2e
# Exclude E2E: ctest -LE e2e
gtest_discover_tests(apex_e2e_tests
    PROPERTIES LABELS "e2e"
    DISCOVERY_TIMEOUT 60
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
