cmake_minimum_required(VERSION 3.20)
project(apex-pipeline
    VERSION 0.1.0
    LANGUAGES CXX
    DESCRIPTION "Apex Pipeline - High-performance real-time server framework"
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# io_uring option (default OFF, Linux only)
option(APEX_USE_IO_URING "Enable io_uring backend for Boost.Asio" OFF)

enable_testing()
add_subdirectory(core)

# Copy compile_commands.json to project root for clangd (works in VS, build.bat, CLI)
add_custom_target(copy_compile_commands ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_BINARY_DIR}/compile_commands.json"
        "${CMAKE_SOURCE_DIR}/compile_commands.json"
    COMMENT "Copying compile_commands.json to project root"
)
