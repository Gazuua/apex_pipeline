# apex_shared/lib/adapters/common/CMakeLists.txt
add_library(apex_shared_adapters_common STATIC
    src/adapter_error.cpp
)
add_library(apex::shared::adapters::common ALIAS apex_shared_adapters_common)

target_include_directories(apex_shared_adapters_common
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(apex_shared_adapters_common
    PUBLIC
        apex::core
)

target_compile_features(apex_shared_adapters_common PUBLIC cxx_std_23)

if(WIN32)
    target_compile_options(apex_shared_adapters_common PUBLIC /utf-8)
endif()
