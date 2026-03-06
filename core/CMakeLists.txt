add_library(apex_core STATIC
    src/slab_pool.cpp
    src/ring_buffer.cpp
    src/timing_wheel.cpp
)
add_library(apex::core ALIAS apex_core)

target_include_directories(apex_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(apex_core PUBLIC cxx_std_23)

enable_testing()
add_subdirectory(tests)
