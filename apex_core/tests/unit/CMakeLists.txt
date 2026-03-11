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

# 기반 컴포넌트 테스트
apex_add_unit_test(test_mpsc_queue test_mpsc_queue.cpp)
apex_add_unit_test(test_slab_pool test_slab_pool.cpp)
apex_add_unit_test(test_ring_buffer test_ring_buffer.cpp)
apex_add_unit_test(test_timing_wheel test_timing_wheel.cpp)

# 프레임워크 테스트
apex_add_unit_test(test_core_engine test_core_engine.cpp)
apex_add_unit_test(test_message_dispatcher test_message_dispatcher.cpp)
apex_add_unit_test(test_service_base test_service_base.cpp)
apex_add_unit_test(test_wire_header test_wire_header.cpp)
apex_add_unit_test(test_frame_codec test_frame_codec.cpp)

# 크로스코어 인프라 테스트
apex_add_unit_test(test_shared_payload test_shared_payload.cpp)
apex_add_unit_test(test_cross_core_dispatcher test_cross_core_dispatcher.cpp)

# 세션 / 에러 처리 테스트
apex_add_unit_test(test_session test_session.cpp)
apex_add_unit_test(test_session_manager test_session_manager.cpp)
apex_add_unit_test(test_error_propagation test_error_propagation.cpp)
apex_add_unit_test(test_flatbuffers_dispatch test_flatbuffers_dispatch.cpp)

# 네트워크 / 프로토콜 테스트
apex_add_unit_test(test_tcp_acceptor test_tcp_acceptor.cpp)
apex_add_unit_test(test_tcp_binary_protocol test_tcp_binary_protocol.cpp)

# 멀티코어 / 서버 통합 테스트
apex_add_unit_test(test_server_multicore test_server_multicore.cpp)
apex_add_unit_test(test_cross_core_call test_cross_core_call.cpp)
apex_add_unit_test(test_drain_tick test_drain_tick.cpp)

# Phase 5: Config, Logging
apex_add_unit_test(test_config test_config.cpp)
apex_add_unit_test(test_logging test_logging.cpp)

# 단위 테스트 일괄 타임아웃 (30초)
set_tests_properties(
    test_mpsc_queue
    test_slab_pool
    test_ring_buffer
    test_timing_wheel
    test_core_engine
    test_message_dispatcher
    test_service_base
    test_wire_header
    test_frame_codec
    test_session
    test_session_manager
    test_error_propagation
    test_flatbuffers_dispatch
    test_tcp_acceptor
    test_tcp_binary_protocol
    test_server_multicore
    test_cross_core_call
    test_shared_payload
    test_cross_core_dispatcher
    test_drain_tick
    test_config
    test_logging
    PROPERTIES TIMEOUT 30
)
