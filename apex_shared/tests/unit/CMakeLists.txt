find_package(GTest CONFIG REQUIRED)

function(apex_shared_add_unit_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME}
        PRIVATE
            apex::shared::adapters::common
            GTest::gtest_main
    )
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 30)
endfunction()

apex_shared_add_unit_test(test_adapter_error test_adapter_error.cpp)
apex_shared_add_unit_test(test_adapter_base test_adapter_base.cpp)
apex_shared_add_unit_test(test_connection_pool test_connection_pool.cpp)

# --- Kafka adapter tests ---
function(apex_shared_add_kafka_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME}
        PRIVATE
            apex::shared::adapters::kafka
            GTest::gtest_main
    )
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 30)
endfunction()

apex_shared_add_kafka_test(test_kafka_producer test_kafka_producer.cpp)
apex_shared_add_kafka_test(test_kafka_consumer test_kafka_consumer.cpp)
apex_shared_add_kafka_test(test_kafka_adapter test_kafka_adapter.cpp)
