# Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

# apex_shared/lib/adapters/kafka/CMakeLists.txt
find_package(RdKafka CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_library(apex_shared_adapters_kafka STATIC
    src/kafka_config.cpp
    src/kafka_producer.cpp
    src/kafka_consumer.cpp
    src/kafka_adapter.cpp
    src/kafka_sink.cpp
    src/consumer_payload_pool.cpp
)
add_library(apex::shared::adapters::kafka ALIAS apex_shared_adapters_kafka)

target_include_directories(apex_shared_adapters_kafka
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(apex_shared_adapters_kafka
    PUBLIC
        apex::shared::adapters::common
        apex::shared::protocols::kafka   # [D2] KafkaDispatchBridge auto-wiring
        RdKafka::rdkafka        # C API
        RdKafka::rdkafka++      # C++ API
        spdlog::spdlog
)

target_compile_features(apex_shared_adapters_kafka PUBLIC cxx_std_23)

apex_set_warnings(apex_shared_adapters_kafka)

if(WIN32)
    target_compile_options(apex_shared_adapters_kafka PUBLIC /utf-8)
endif()
