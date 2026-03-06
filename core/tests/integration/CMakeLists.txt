function(apex_add_integration_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME}
        PRIVATE
            apex::core
            GTest::gtest_main
    )
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()

apex_add_integration_test(test_pipeline_integration test_pipeline_integration.cpp)
apex_add_integration_test(test_echo_integration test_echo_integration.cpp)
apex_add_integration_test(test_server_e2e test_server_e2e.cpp)
