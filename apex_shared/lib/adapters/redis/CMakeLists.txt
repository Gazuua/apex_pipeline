# apex_shared/lib/adapters/redis/CMakeLists.txt
find_package(hiredis CONFIG REQUIRED)
find_package(redis++ CONFIG REQUIRED)

add_library(apex_shared_adapters_redis STATIC
    src/redis_config.cpp
    # src/hiredis_asio_adapter.cpp   # Task 2
    # src/redis_connection.cpp       # Task 2
    # src/redis_pool.cpp             # Task 3
    # src/redis_adapter.cpp          # Task 4
)
add_library(apex::shared::adapters::redis ALIAS apex_shared_adapters_redis)

target_include_directories(apex_shared_adapters_redis
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(apex_shared_adapters_redis
    PUBLIC
        apex::shared::adapters::common
        hiredis::hiredis
        redis++::redis++
)

target_compile_features(apex_shared_adapters_redis PUBLIC cxx_std_23)

if(WIN32)
    target_compile_options(apex_shared_adapters_redis PUBLIC /utf-8)
    target_link_libraries(apex_shared_adapters_redis PUBLIC ws2_32)
endif()
