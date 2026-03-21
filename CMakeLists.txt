# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

cmake_minimum_required(VERSION 3.25)
project(apex-pipeline
    VERSION 0.5.10
    LANGUAGES C CXX
    DESCRIPTION "Apex Pipeline - High-performance real-time server monorepo"
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# TODO: Linux io_uring 통합 시 활성화
option(APEX_USE_IO_URING "Enable io_uring backend for Boost.Asio" OFF)

enable_testing()
include(CTest)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/ApexWarnings.cmake)

# --- Sub-projects ---
add_subdirectory(apex_core)
# 실제 코드 추가 시 활성화 (현재 placeholder만 존재)
add_subdirectory(apex_shared)
add_subdirectory(apex_services)
