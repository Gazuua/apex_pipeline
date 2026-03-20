# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

# apex_shared/lib/rate_limit/CMakeLists.txt
add_library(apex_shared_rate_limit STATIC
    src/sliding_window_counter.cpp
    src/per_ip_rate_limiter.cpp
    src/redis_rate_limiter.cpp
    src/rate_limit_facade.cpp
)
add_library(apex::shared::rate_limit ALIAS apex_shared_rate_limit)

find_package(Boost REQUIRED)
find_package(spdlog CONFIG REQUIRED)

target_include_directories(apex_shared_rate_limit
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(apex_shared_rate_limit
    PUBLIC
        apex::core
        apex::shared::adapters::redis
        Boost::boost
        spdlog::spdlog
)

target_compile_features(apex_shared_rate_limit PUBLIC cxx_std_23)

apex_set_warnings(apex_shared_rate_limit)

if(WIN32)
    target_compile_options(apex_shared_rate_limit PUBLIC /utf-8)
endif()
