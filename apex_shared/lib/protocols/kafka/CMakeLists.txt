# apex_shared protocols/kafka — Kafka Envelope (RoutingHeader + MetadataPrefix)

add_library(apex_protocols_kafka STATIC
    src/kafka_envelope.cpp
)
add_library(apex::shared::protocols::kafka ALIAS apex_protocols_kafka)

target_include_directories(apex_protocols_kafka
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Byte-order 유틸은 자체 포함, 외부 의존 없음
if(WIN32)
    target_compile_options(apex_protocols_kafka PUBLIC /utf-8)
    target_compile_definitions(apex_protocols_kafka PUBLIC _WIN32_WINNT=0x0A00)
endif()
