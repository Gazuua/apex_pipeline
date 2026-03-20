# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

# Output directory: bin/{variant}/ (debug, release, asan, tsan)
if(APEX_BUILD_VARIANT)
    set(_bin_dir "${CMAKE_CURRENT_SOURCE_DIR}/../bin/${APEX_BUILD_VARIANT}")
else()
    set(_bin_dir "${CMAKE_CURRENT_SOURCE_DIR}/../bin/release")
endif()

# Helper function — add example + copy runtime DLLs
function(apex_add_example EXAMPLE_NAME EXAMPLE_SOURCE)
    add_executable(${EXAMPLE_NAME} ${EXAMPLE_SOURCE})
    target_link_libraries(${EXAMPLE_NAME} PRIVATE apex::core)
    apex_set_warnings(${EXAMPLE_NAME})
    set_target_properties(${EXAMPLE_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${_bin_dir}")
    if(WIN32)
        add_custom_command(TARGET ${EXAMPLE_NAME} POST_BUILD
            COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:${EXAMPLE_NAME}>>:${CMAKE_COMMAND}>"
                    "$<$<BOOL:$<TARGET_RUNTIME_DLLS:${EXAMPLE_NAME}>>:-E>"
                    "$<$<BOOL:$<TARGET_RUNTIME_DLLS:${EXAMPLE_NAME}>>:copy_if_different>"
                    $<TARGET_RUNTIME_DLLS:${EXAMPLE_NAME}>
                    "$<$<BOOL:$<TARGET_RUNTIME_DLLS:${EXAMPLE_NAME}>>:$<TARGET_FILE_DIR:${EXAMPLE_NAME}>>"
            COMMAND_EXPAND_LISTS
        )
    endif()
endfunction()

apex_add_example(echo_server echo_server.cpp)
apex_add_example(chat_server chat_server.cpp)
apex_add_example(multicore_echo_server multicore_echo_server.cpp)
apex_add_example(echo_loadtest echo_loadtest.cpp)
