cmake_minimum_required(VERSION 3.20)
project(apex-pipeline
    VERSION 0.2.3
    LANGUAGES CXX
    DESCRIPTION "Apex Pipeline - High-performance real-time server monorepo"
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(APEX_USE_IO_URING "Enable io_uring backend for Boost.Asio" OFF)

enable_testing()

# --- Sub-projects ---
add_subdirectory(apex_core)
add_subdirectory(apex_shared)
# add_subdirectory(apex_services/gateway)   # 향후 추가
