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
apex_shared_add_unit_test(test_pool_concept test_pool_concept.cpp)

# CircuitBreaker test needs Boost.Asio for coroutine testing
add_executable(test_circuit_breaker test_circuit_breaker.cpp)
target_link_libraries(test_circuit_breaker
    PRIVATE
        apex::shared::adapters::common
        GTest::gtest_main
)
add_test(NAME test_circuit_breaker COMMAND test_circuit_breaker)
set_tests_properties(test_circuit_breaker PROPERTIES TIMEOUT 30)

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
apex_shared_add_redis_test(test_redis_multiplexer test_redis_multiplexer.cpp)
apex_shared_add_redis_test(test_redis_adapter test_redis_adapter.cpp)
apex_shared_add_redis_test(test_redis_reply test_redis_reply.cpp)

# --- PostgreSQL adapter tests ---
function(apex_shared_add_pg_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME}
        PRIVATE
            apex::shared::adapters::pg
            GTest::gtest_main
    )
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 30)
endfunction()

apex_shared_add_pg_test(test_pg_connection test_pg_connection.cpp)
apex_shared_add_pg_test(test_pg_pool test_pg_pool.cpp)
apex_shared_add_pg_test(test_pg_pool_retry test_pg_pool_retry.cpp)
apex_shared_add_pg_test(test_pg_adapter test_pg_adapter.cpp)
apex_shared_add_pg_test(test_pg_transaction test_pg_transaction.cpp)
