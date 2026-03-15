# apex_shared protocols/tcp — WireHeader + FrameCodec + TcpBinaryProtocol
#
# INTERFACE library: 헤더만 제공. 실제 .cpp 컴파일은 apex_core에서 수행.
# 이유: apex_core ↔ protocols_tcp 순환 의존 방지. apex_core가 wire_header.cpp,
# frame_codec.cpp를 컴파일하되 헤더는 shared namespace 정의를 사용한다.

add_library(apex_protocols_tcp INTERFACE)
add_library(apex::shared::protocols::tcp ALIAS apex_protocols_tcp)

target_include_directories(apex_protocols_tcp
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(apex_protocols_tcp
    INTERFACE
        apex::core
)
