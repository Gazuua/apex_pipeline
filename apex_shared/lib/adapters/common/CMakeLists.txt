# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

# apex_shared/lib/adapters/common/CMakeLists.txt
add_library(apex_shared_adapters_common STATIC
    src/adapter_error.cpp
    src/cancellation_token.cpp
    src/circuit_breaker.cpp
)
add_library(apex::shared::adapters::common ALIAS apex_shared_adapters_common)

target_include_directories(apex_shared_adapters_common
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

find_package(Boost REQUIRED)

target_link_libraries(apex_shared_adapters_common
    PUBLIC
        apex::core
        Boost::boost
)

target_compile_features(apex_shared_adapters_common PUBLIC cxx_std_23)

apex_set_warnings(apex_shared_adapters_common)

if(WIN32)
    target_compile_options(apex_shared_adapters_common PUBLIC /utf-8)
endif()
