option(APEX_BUILD_INTEGRATION_TESTS "Build integration tests (requires docker-compose)" OFF)

if(NOT APEX_BUILD_INTEGRATION_TESTS)
    message(STATUS "Integration tests disabled (set -DAPEX_BUILD_INTEGRATION_TESTS=ON to enable)")
    return()
endif()

find_package(GTest CONFIG REQUIRED)

function(apex_shared_add_integration_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME}
        PRIVATE
            apex::shared::adapters::kafka
            apex::shared::adapters::redis
            apex::shared::adapters::pg
            GTest::gtest_main
    )
    target_compile_features(${TEST_NAME} PRIVATE cxx_std_23)
    apex_set_warnings(${TEST_NAME})
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    set_tests_properties(${TEST_NAME} PROPERTIES
        TIMEOUT 60
        LABELS "integration"
    )
    if(WIN32)
        target_compile_options(${TEST_NAME} PRIVATE /utf-8)
    endif()
endfunction()

apex_shared_add_integration_test(test_kafka_roundtrip test_kafka_roundtrip.cpp)
apex_shared_add_integration_test(test_redis_roundtrip test_redis_roundtrip.cpp)
apex_shared_add_integration_test(test_pg_roundtrip test_pg_roundtrip.cpp)
apex_shared_add_integration_test(test_kafka_sink test_kafka_sink.cpp)
