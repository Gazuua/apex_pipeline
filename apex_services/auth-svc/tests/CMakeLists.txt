# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

find_package(GTest CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

# --- Unit Tests ---
add_executable(auth_svc_unit_tests
    unit/test_password_hasher.cpp
    unit/test_jwt_manager.cpp
    unit/test_session_store.cpp
    unit/test_envelope_codec.cpp
    unit/test_auth_handlers.cpp
    unit/test_crypto_util.cpp
    unit/test_auth_logic.cpp
)

target_link_libraries(auth_svc_unit_tests
    PRIVATE
        apex_auth_svc
        GTest::gtest
        GTest::gtest_main
        OpenSSL::SSL
        OpenSSL::Crypto
)

target_include_directories(auth_svc_unit_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_SOURCE_DIR}/apex_services
)

if(WIN32)
    target_compile_options(auth_svc_unit_tests PRIVATE /utf-8)
    target_compile_definitions(auth_svc_unit_tests PRIVATE _WIN32_WINNT=0x0A00)
endif()

apex_set_warnings(auth_svc_unit_tests)
add_test(NAME auth_svc_unit_tests COMMAND auth_svc_unit_tests)
set_tests_properties(auth_svc_unit_tests PROPERTIES TIMEOUT 30)

# --- Integration Tests (requires external infrastructure) ---
# add_executable(auth_svc_integration_tests
#     integration/test_login_flow.cpp
# )
# target_link_libraries(auth_svc_integration_tests PRIVATE
#     apex_auth_svc GTest::gtest GTest::gtest_main)
# add_test(NAME auth_svc_integration_tests COMMAND auth_svc_integration_tests)
