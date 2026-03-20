// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <cstdint>

namespace apex::core
{

/// Kafka 메시지 메타데이터 — core 레이어용 경량 구조체.
/// shared의 MetadataPrefix와 1:1 대응. core가 kafka_envelope.hpp에 의존하지 않도록 분리.
struct KafkaMessageMeta
{
    uint32_t meta_version{0};
    uint16_t core_id{0};
    uint64_t corr_id{0};
    uint16_t source_id{0};
    uint64_t session_id{0};
    uint64_t user_id{0};
    uint64_t timestamp{0};
};

} // namespace apex::core
