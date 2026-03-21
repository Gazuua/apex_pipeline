// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <cstdint>
#include <functional>
#include <ostream>

#include <fmt/format.h>

namespace apex::core
{

/// 고유 세션 식별자 (코어별 단조 증가)
enum class SessionId : uint64_t
{
};

/// SessionId 생성 헬퍼
constexpr SessionId make_session_id(uint64_t v) noexcept
{
    return static_cast<SessionId>(v);
}

/// SessionId → uint64_t 변환
constexpr uint64_t to_underlying(SessionId id) noexcept
{
    return static_cast<uint64_t>(id);
}

inline std::ostream& operator<<(std::ostream& os, SessionId id)
{
    return os << to_underlying(id);
}

} // namespace apex::core

template <> struct std::hash<apex::core::SessionId>
{
    std::size_t operator()(apex::core::SessionId id) const noexcept
    {
        return std::hash<uint64_t>{}(apex::core::to_underlying(id));
    }
};

template <> struct fmt::formatter<apex::core::SessionId> : fmt::formatter<uint64_t>
{
    auto format(apex::core::SessionId id, fmt::format_context& ctx) const
    {
        return fmt::formatter<uint64_t>::format(apex::core::to_underlying(id), ctx);
    }
};
