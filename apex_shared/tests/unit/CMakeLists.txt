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
# Timeout 60s: librdkafka background threads need time to shut down
# when connecting to non-existent brokers (metadata request timeout).
function(apex_shared_add_kafka_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME}
        PRIVATE
            apex::shared::adapters::kafka
            GTest::gtest_main
    )
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 60)
endfunction()

apex_shared_add_kafka_test(test_kafka_producer test_kafka_producer.cpp)
apex_shared_add_kafka_test(test_kafka_consumer test_kafka_consumer.cpp)
apex_shared_add_kafka_test(test_kafka_adapter test_kafka_adapter.cpp)
apex_shared_add_kafka_test(test_kafka_sink test_kafka_sink.cpp)

# --- Redis adapter tests ---
function(apex_shared_add_redis_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME}
        PRIVATE
            apex::shared::adapters::redis
            GTest::gtest_main
    )
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 30)
endfunction()

apex_shared_add_redis_test(test_hiredis_asio_adapter test_hiredis_asio_adapter.cpp)
apex_shared_add_redis_test(test_redis_pool test_redis_pool.cpp)
