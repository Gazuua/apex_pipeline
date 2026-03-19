cmake_minimum_required(VERSION 3.25...3.30)

# Standalone build (apex_core 단독) vs subdirectory (모노레포에서 포함)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(apex-core
        VERSION 0.4.0
        LANGUAGES CXX
        DESCRIPTION "Apex Core - High-performance real-time server framework"
    )

    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    option(APEX_USE_IO_URING "Enable io_uring backend for Boost.Asio" OFF)

    enable_testing()
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/ApexWarnings.cmake)

# --- Dependencies ---
find_package(Boost REQUIRED)  # asio, unordered (header-only)
find_package(flatbuffers CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)

# --- FlatBuffers 스키마 컴파일 ---
set(APEX_SCHEMAS
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/heartbeat.fbs
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/error_response.fbs
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/echo.fbs
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/chat_message.fbs
)

set(APEX_FBS_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${APEX_FBS_GENERATED_DIR})

foreach(SCHEMA ${APEX_SCHEMAS})
    get_filename_component(SCHEMA_NAME ${SCHEMA} NAME_WE)
    set(GENERATED_HEADER ${APEX_FBS_GENERATED_DIR}/${SCHEMA_NAME}_generated.h)
    add_custom_command(
        OUTPUT ${GENERATED_HEADER}
        COMMAND flatbuffers::flatc --cpp -o ${APEX_FBS_GENERATED_DIR} ${SCHEMA}
        DEPENDS ${SCHEMA}
        COMMENT "FlatBuffers: ${SCHEMA_NAME}.fbs"
    )
    list(APPEND APEX_FBS_GENERATED_HEADERS ${GENERATED_HEADER})
endforeach()

add_custom_target(apex_fbs_generate DEPENDS ${APEX_FBS_GENERATED_HEADERS})

# --- 라이브러리 ---
add_library(apex_core STATIC
    src/slab_allocator.cpp
    src/bump_allocator.cpp
    src/arena_allocator.cpp
    src/ring_buffer.cpp
    src/timing_wheel.cpp
    src/core_engine.cpp
    src/cross_core_dispatcher.cpp
    src/message_dispatcher.cpp
    src/wire_header.cpp
    src/frame_codec.cpp
    src/session.cpp
    src/session_manager.cpp
    src/error_sender.cpp
    src/tcp_acceptor.cpp
    src/config.cpp
    src/logging.cpp
    src/server.cpp
    src/periodic_task_scheduler.cpp
)
add_library(apex::core ALIAS apex_core)

add_dependencies(apex_core apex_fbs_generate)

target_include_directories(apex_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        # Forwarding headers → shared/protocols include paths
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../apex_shared/lib/protocols/tcp/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../apex_shared/lib/protocols/kafka/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(apex_core
    PUBLIC
        Boost::boost
        flatbuffers::flatbuffers
        spdlog::spdlog
        tomlplusplus::tomlplusplus
)

target_compile_features(apex_core PUBLIC cxx_std_23)
apex_set_warnings(apex_core)

# io_uring backend (Linux only)
if(APEX_USE_IO_URING)
    if(NOT UNIX OR APPLE)
        message(FATAL_ERROR "APEX_USE_IO_URING requires Linux")
    endif()
    target_compile_definitions(apex_core PUBLIC
        BOOST_ASIO_HAS_IO_URING
        BOOST_ASIO_DISABLE_EPOLL
    )
    find_library(URING_LIB uring REQUIRED)
    target_link_libraries(apex_core PUBLIC ${URING_LIB})
endif()

# Windows: UTF-8 source, suppress _WIN32_WINNT warning, link Winsock
if(WIN32)
    target_compile_options(apex_core PUBLIC /utf-8)
    target_compile_definitions(apex_core PUBLIC _WIN32_WINNT=0x0A00)
    target_link_libraries(apex_core PUBLIC ws2_32 mswsock)
endif()

add_subdirectory(tests)
add_subdirectory(examples)
add_subdirectory(benchmarks)

# Standalone build: include apex_shared for adapter abstractions + tests
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../apex_shared
                     ${CMAKE_CURRENT_BINARY_DIR}/apex_shared)
endif()
