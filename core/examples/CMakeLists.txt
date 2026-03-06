find_package(Boost REQUIRED)

add_executable(echo_server echo_server.cpp)
target_link_libraries(echo_server PRIVATE apex::core)
