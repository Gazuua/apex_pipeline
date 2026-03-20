// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <cstdint>

namespace apex::core
{

/// Operation codes for cross-core message dispatch.
/// Framework-reserved: 0x0000 ~ 0x00FF
/// Application-defined: 0x0100+
enum class CrossCoreOp : uint16_t
{
    /// Framework-internal operations
    Noop = 0,
    Shutdown,

    // Legacy compatibility (Tier 1 전환 기간 동안 유지)
    LegacyCrossCoreFn, // 기존 closure-based cross_core_post/call

    /// Generic user-defined message — message_handler_ 경로로 전달.
    /// 기존 CoreMessage::Type::Custom 대체.
    Custom,
};

/// Cross-core handler signature — function pointer for static dispatch (icache friendly).
/// @param core_id  The core that is processing this message
/// @param source_core  The core that sent this message
/// @param data  Opaque pointer to payload (handler casts to concrete type)
using CrossCoreHandler = void (*)(uint32_t core_id, uint32_t source_core, void* data);

} // namespace apex::core
