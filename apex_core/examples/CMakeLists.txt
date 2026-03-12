# Output directory: bin/{variant}/ (debug, release, asan, tsan)
if(APEX_BUILD_VARIANT)
    set(_bin_dir "${CMAKE_CURRENT_SOURCE_DIR}/../bin/${APEX_BUILD_VARIANT}")
else()
    set(_bin_dir "${CMAKE_CURRENT_SOURCE_DIR}/../bin/release")
endif()

add_executable(echo_server echo_server.cpp)
target_link_libraries(echo_server PRIVATE apex::core)
set_target_properties(echo_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${_bin_dir}"
)

add_executable(chat_server chat_server.cpp)
target_link_libraries(chat_server PRIVATE apex::core)
set_target_properties(chat_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${_bin_dir}"
)

add_executable(multicore_echo_server multicore_echo_server.cpp)
target_link_libraries(multicore_echo_server PRIVATE apex::core)
set_target_properties(multicore_echo_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${_bin_dir}"
)

add_executable(echo_loadtest echo_loadtest.cpp)
target_link_libraries(echo_loadtest PRIVATE apex::core)
set_target_properties(echo_loadtest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${_bin_dir}"
)

# Copy runtime DLLs next to executables (Windows only)
if(WIN32)
    foreach(_target echo_server chat_server multicore_echo_server echo_loadtest)
        add_custom_command(TARGET ${_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${_target}>
                $<TARGET_FILE_DIR:${_target}>
            COMMAND_EXPAND_LISTS
        )
    endforeach()
endif()
