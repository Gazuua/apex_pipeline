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

apex_add_unit_test(test_mpsc_queue test_mpsc_queue.cpp)
apex_add_unit_test(test_slab_pool test_slab_pool.cpp)
apex_add_unit_test(test_ring_buffer test_ring_buffer.cpp)
apex_add_unit_test(test_timing_wheel test_timing_wheel.cpp)

# Phase 3 tests (구현 완료 시 주석 해제)
apex_add_unit_test(test_core_engine test_core_engine.cpp)
apex_add_unit_test(test_message_dispatcher test_message_dispatcher.cpp)
apex_add_unit_test(test_service_base test_service_base.cpp)
apex_add_unit_test(test_wire_header test_wire_header.cpp)
apex_add_unit_test(test_frame_codec test_frame_codec.cpp)
