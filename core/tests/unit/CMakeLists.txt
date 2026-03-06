# Each test file is added as components are implemented

function(apex_add_unit_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME}
        PRIVATE
            apex::core
            GTest::gtest_main
    )
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()

# Phase 2 tests (uncomment as implemented):
# apex_add_unit_test(test_mpsc_queue test_mpsc_queue.cpp)
# apex_add_unit_test(test_slab_pool test_slab_pool.cpp)
# apex_add_unit_test(test_ring_buffer test_ring_buffer.cpp)
# apex_add_unit_test(test_timing_wheel test_timing_wheel.cpp)
