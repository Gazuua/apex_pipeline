# Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

# apex_shared protocols/websocket — WebSocketProtocol (MVP)

add_library(apex_protocols_websocket STATIC
    src/websocket_protocol.cpp
)
add_library(apex::shared::protocols::websocket ALIAS apex_protocols_websocket)

target_include_directories(apex_protocols_websocket
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(apex_protocols_websocket
    PUBLIC
        apex::core
)

target_compile_features(apex_protocols_websocket PUBLIC cxx_std_23)

apex_set_warnings(apex_protocols_websocket)

if(WIN32)
    target_compile_options(apex_protocols_websocket PUBLIC /utf-8)
endif()
