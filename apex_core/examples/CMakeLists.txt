# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

# Output directory: APEX_CORE_BIN_DIR (상위 CMakeLists.txt에서 정의)
set(_bin_dir "${APEX_CORE_BIN_DIR}")

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
target_link_libraries(echo_server PRIVATE apex::shared::protocols::tcp)
apex_add_example(chat_server chat_server.cpp)
target_link_libraries(chat_server PRIVATE apex::shared::protocols::tcp)
apex_add_example(multicore_echo_server multicore_echo_server.cpp)
target_link_libraries(multicore_echo_server PRIVATE apex::shared::protocols::tcp)
apex_add_example(echo_loadtest echo_loadtest.cpp)
