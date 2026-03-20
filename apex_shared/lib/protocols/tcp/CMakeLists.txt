# apex_shared protocols/tcp — TcpBinaryProtocol
#
# INTERFACE library: 헤더만 제공.
# WireHeader/FrameCodec는 apex_core로 복원됨. TcpBinaryProtocol만 shared에 잔류.

add_library(apex_protocols_tcp INTERFACE)
add_library(apex::shared::protocols::tcp ALIAS apex_protocols_tcp)

target_include_directories(apex_protocols_tcp
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

find_package(OpenSSL QUIET)

target_link_libraries(apex_protocols_tcp
    INTERFACE
        apex::core
        $<$<TARGET_EXISTS:OpenSSL::SSL>:OpenSSL::SSL>
        $<$<TARGET_EXISTS:OpenSSL::Crypto>:OpenSSL::Crypto>
)
