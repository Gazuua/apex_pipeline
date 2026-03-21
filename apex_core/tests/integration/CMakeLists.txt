# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

function(apex_add_integration_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME}
        PRIVATE
            apex::core
            GTest::gtest_main
    )
    apex_set_warnings(${TEST_NAME})
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()

apex_add_integration_test(test_pipeline_integration test_pipeline_integration.cpp)
apex_add_integration_test(test_echo_integration test_echo_integration.cpp)
apex_add_integration_test(test_server_e2e test_server_e2e.cpp)
target_link_libraries(test_server_e2e PRIVATE apex::shared::protocols::tcp)

# Phase 5: Graceful Shutdown
apex_add_integration_test(test_shutdown_timeout test_shutdown_timeout.cpp)
target_link_libraries(test_shutdown_timeout PRIVATE apex::shared::protocols::tcp)

# 통합 테스트 타임아웃
set_tests_properties(test_server_e2e PROPERTIES TIMEOUT 60)
set_tests_properties(test_shutdown_timeout PROPERTIES TIMEOUT 60)
set_tests_properties(test_echo_integration PROPERTIES TIMEOUT 30)
set_tests_properties(test_pipeline_integration PROPERTIES TIMEOUT 30)
