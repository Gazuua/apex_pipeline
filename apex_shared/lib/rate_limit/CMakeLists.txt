# apex_shared/lib/rate_limit/CMakeLists.txt
add_library(apex_shared_rate_limit STATIC
    src/sliding_window_counter.cpp
    src/per_ip_rate_limiter.cpp
)
add_library(apex::shared::rate_limit ALIAS apex_shared_rate_limit)

find_package(Boost REQUIRED)

target_include_directories(apex_shared_rate_limit
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(apex_shared_rate_limit
    PUBLIC
        apex::core
        Boost::boost
)

target_compile_features(apex_shared_rate_limit PUBLIC cxx_std_23)

if(WIN32)
    target_compile_options(apex_shared_rate_limit PUBLIC /utf-8)
endif()
