add_library(apex_core INTERFACE)
add_library(apex::core ALIAS apex_core)

target_include_directories(apex_core
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(apex_core INTERFACE cxx_std_20)

# Phase 2 components are header-only for now
# Source files will be added as implementation progresses

enable_testing()
add_subdirectory(tests)
