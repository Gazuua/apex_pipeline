// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <source_location>

namespace apex::core::detail
{

/// Assertion failure handler. Logs location + message, flushes, then aborts.
/// Called by APEX_ASSERT macro — not intended for direct use.
[[noreturn]] inline void assert_fail(const char* condition, const char* message,
                                     const std::source_location& loc = std::source_location::current())
{
    spdlog::critical("ASSERTION FAILED: {} — {} [{}:{}] in {}", condition, message, loc.file_name(), loc.line(),
                     loc.function_name());
    if (auto logger = spdlog::default_logger())
    {
        logger->flush();
    }
    std::abort();
}

} // namespace apex::core::detail

/// Runtime assertion that remains active in Release builds (unlike standard assert).
/// Logs file/function/line via spdlog before aborting, ensuring crash diagnostics.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) — macro required for source_location capture at call site
#define APEX_ASSERT(cond, msg)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond)) [[unlikely]]                                                                                      \
        {                                                                                                              \
            apex::core::detail::assert_fail(#cond, (msg));                                                             \
        }                                                                                                              \
    }                                                                                                                  \
    while (false)
