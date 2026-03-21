// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <spdlog/spdlog.h>

#include <cstdint>

namespace apex::core::log
{

/// Standalone logging helpers for framework-internal code (CoreEngine, SpscMesh, etc.).
/// ServiceBase subclasses should use the built-in log_* methods instead.

template <typename... Args> void trace(uint32_t core_id, fmt::format_string<Args...> fmt, Args&&... args)
{
    if (spdlog::should_log(spdlog::level::trace))
        spdlog::trace("[core={}] {}", core_id, fmt::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void debug(uint32_t core_id, fmt::format_string<Args...> fmt, Args&&... args)
{
    if (spdlog::should_log(spdlog::level::debug))
        spdlog::debug("[core={}] {}", core_id, fmt::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void info(uint32_t core_id, fmt::format_string<Args...> fmt, Args&&... args)
{
    if (spdlog::should_log(spdlog::level::info))
        spdlog::info("[core={}] {}", core_id, fmt::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void warn(uint32_t core_id, fmt::format_string<Args...> fmt, Args&&... args)
{
    if (spdlog::should_log(spdlog::level::warn))
        spdlog::warn("[core={}] {}", core_id, fmt::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void error(uint32_t core_id, fmt::format_string<Args...> fmt, Args&&... args)
{
    if (spdlog::should_log(spdlog::level::err))
        spdlog::error("[core={}] {}", core_id, fmt::format(fmt, std::forward<Args>(args)...));
}

} // namespace apex::core::log
