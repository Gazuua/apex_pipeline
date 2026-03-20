// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/cross_core_op.hpp>

#include <cstdint>
#include <type_traits>

namespace apex::core
{

/// Trivially-copyable message for inter-core communication via SpscQueue/MpscQueue.
struct CoreMessage
{
    CrossCoreOp op{CrossCoreOp::Noop};
    uint32_t source_core{0};
    uintptr_t data{0};
};
static_assert(std::is_trivially_copyable_v<CoreMessage>);
static_assert(sizeof(CoreMessage) <= 16);

} // namespace apex::core
