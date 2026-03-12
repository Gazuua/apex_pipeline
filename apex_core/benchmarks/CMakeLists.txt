find_package(benchmark CONFIG QUIET)
if(NOT benchmark_FOUND)
    message(STATUS "Google Benchmark not found — benchmarks will not be built")
    return()
endif()

message(STATUS "Building benchmarks")

# Shared bench_main (custom main with SystemProfile)
add_library(apex_bench_main OBJECT bench_main.cpp)
target_include_directories(apex_bench_main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(apex_bench_main PRIVATE benchmark::benchmark)

# Output directory: bin/{variant}/ (debug, release, asan, tsan)
if(APEX_BUILD_VARIANT)
    set(_bench_bin_dir "${CMAKE_CURRENT_SOURCE_DIR}/../bin/${APEX_BUILD_VARIANT}")
else()
    set(_bench_bin_dir "${CMAKE_CURRENT_SOURCE_DIR}/../bin/release")
endif()

# Helper function — each benchmark is an independent executable
function(apex_add_benchmark BENCH_NAME BENCH_SOURCE)
    add_executable(${BENCH_NAME} ${BENCH_SOURCE} $<TARGET_OBJECTS:apex_bench_main>)
    target_include_directories(${BENCH_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${BENCH_NAME} PRIVATE apex::core benchmark::benchmark)
    set_target_properties(${BENCH_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${_bench_bin_dir}")
    if(WIN32)
        add_custom_command(TARGET ${BENCH_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${BENCH_NAME}>
                $<TARGET_FILE_DIR:${BENCH_NAME}>
            COMMAND_EXPAND_LISTS
        )
    endif()
endfunction()

# --- Micro benchmarks ---
apex_add_benchmark(bench_mpsc_queue    micro/bench_mpsc_queue.cpp)
apex_add_benchmark(bench_ring_buffer   micro/bench_ring_buffer.cpp)
apex_add_benchmark(bench_frame_codec   micro/bench_frame_codec.cpp)
apex_add_benchmark(bench_dispatcher    micro/bench_dispatcher.cpp)
apex_add_benchmark(bench_timing_wheel  micro/bench_timing_wheel.cpp)
apex_add_benchmark(bench_slab_pool     micro/bench_slab_pool.cpp)
apex_add_benchmark(bench_session_lifecycle micro/bench_session_lifecycle.cpp)

# --- Integration benchmarks ---
apex_add_benchmark(bench_cross_core_latency       integration/bench_cross_core_latency.cpp)
apex_add_benchmark(bench_frame_pipeline            integration/bench_frame_pipeline.cpp)
apex_add_benchmark(bench_session_throughput         integration/bench_session_throughput.cpp)
apex_add_benchmark(bench_cross_core_message_passing integration/bench_cross_core_message_passing.cpp)
