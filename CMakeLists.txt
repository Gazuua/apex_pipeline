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
