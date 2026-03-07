find_package(Boost REQUIRED)

# Build variant suffix: debug → _debug, asan → _asan, default(release) → no suffix
if(APEX_BUILD_VARIANT)
    set(_variant_suffix "_${APEX_BUILD_VARIANT}")
else()
    set(_variant_suffix "")
endif()

set(_bin_dir "${CMAKE_CURRENT_SOURCE_DIR}/../bin")

add_executable(echo_server echo_server.cpp)
target_link_libraries(echo_server PRIVATE apex::core)
set_target_properties(echo_server PROPERTIES
    OUTPUT_NAME "echo_server${_variant_suffix}"
    RUNTIME_OUTPUT_DIRECTORY "${_bin_dir}"
)

add_executable(chat_server chat_server.cpp)
target_link_libraries(chat_server PRIVATE apex::core)
set_target_properties(chat_server PROPERTIES
    OUTPUT_NAME "chat_server${_variant_suffix}"
    RUNTIME_OUTPUT_DIRECTORY "${_bin_dir}"
)
