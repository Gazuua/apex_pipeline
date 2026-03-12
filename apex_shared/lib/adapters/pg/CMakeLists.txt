# apex_shared/lib/adapters/pg/CMakeLists.txt
find_package(PostgreSQL REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_library(apex_shared_adapters_pg STATIC
    src/pg_connection.cpp
    src/pg_pool.cpp
    src/pg_adapter.cpp
)
add_library(apex::shared::adapters::pg ALIAS apex_shared_adapters_pg)

target_include_directories(apex_shared_adapters_pg
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(apex_shared_adapters_pg
    PUBLIC
        apex::shared::adapters::common
        PostgreSQL::PostgreSQL
        spdlog::spdlog
)

target_compile_features(apex_shared_adapters_pg PUBLIC cxx_std_23)

if(WIN32)
    target_compile_options(apex_shared_adapters_pg PUBLIC /utf-8)
endif()
