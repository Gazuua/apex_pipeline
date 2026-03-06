find_package(Boost REQUIRED)

add_library(apex_core STATIC
    src/slab_pool.cpp
    src/ring_buffer.cpp
    src/timing_wheel.cpp
    src/core_engine.cpp
    src/message_dispatcher.cpp
    src/wire_header.cpp
    src/frame_codec.cpp
)
add_library(apex::core ALIAS apex_core)

target_include_directories(apex_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(apex_core
    PUBLIC
        Boost::boost
)

target_compile_features(apex_core PUBLIC cxx_std_23)

# Windows: UTF-8 source, suppress _WIN32_WINNT warning, link Winsock
if(WIN32)
    target_compile_options(apex_core PUBLIC /utf-8)
    target_compile_definitions(apex_core PUBLIC _WIN32_WINNT=0x0A00)
    target_link_libraries(apex_core PUBLIC ws2_32 mswsock)
endif()

enable_testing()
add_subdirectory(tests)
add_subdirectory(examples)
