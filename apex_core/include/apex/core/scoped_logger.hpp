// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/session_id.hpp>

#include <spdlog/spdlog.h>

#include <cstdint>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>

namespace apex::core
{

/// Format string wrapper that captures std::source_location at the call site.
/// The constructor's default parameter evaluates source_location::current()
/// at the point where the log method is called, not where log_loc is defined.
/// consteval ensures format string validation at compile time and keeps
/// source_location::current() evaluation at the call site.
/// The string_view requires-clause prevents non-string types (e.g., SessionId)
/// from matching, which was causing MSVC C7595 / C2666 ambiguity.
template <typename... Args> struct log_loc
{
    fmt::format_string<Args...> fmt;
    std::source_location loc;

    template <typename T>
        requires std::is_convertible_v<const std::remove_reference_t<T>&, std::string_view>
    consteval log_loc(T&& s, std::source_location l = std::source_location::current())
        : fmt(std::forward<T>(s))
        , loc(l)
    {}
};

/// type_identity_t alias prevents template argument deduction from the format string,
/// forcing deduction from the trailing Args only.
template <typename... Args> using log_fmt = log_loc<std::type_identity_t<Args>...>;

namespace detail
{

/// Concept for types that behave like a session smart pointer (has ->id() returning SessionId).
/// Allows ScopedLogger to accept SessionPtr without including session.hpp.
template <typename T>
concept HasSessionId = requires(const T& t) {
    { static_cast<bool>(t) };
    { t->id() } -> std::convertible_to<SessionId>;
};

} // namespace detail

class ScopedLogger
{
  public:
    static constexpr uint32_t NO_CORE = UINT32_MAX;

    /// Default constructor — inert logger (all log calls no-op).
    /// Use for delayed initialization (e.g., ServiceBase where core_id is available after configure).
    ScopedLogger() = default;

    /// @param component   Component name (e.g., "CoreEngine", "AuthService")
    /// @param core_id     Core ID. Use NO_CORE outside per-core context
    /// @param logger_name spdlog logger name: "apex" (framework) or "app" (service)
    ScopedLogger(std::string component, uint32_t core_id, std::string_view logger_name = "apex");

    /// Return a copy with corr_id bound (for request-scoped tracing).
    /// Lightweight value copy — use as a stack-local variable.
    [[nodiscard]] ScopedLogger with_trace(uint64_t corr_id) const;

    [[nodiscard]] bool should_log(spdlog::level::level_enum level) const noexcept
    {
        return logger_ && logger_->should_log(level);
    }

    // ── Log methods: 6 levels × 3 overloads ─────────────────────────────
    // Overload 1: basic (component context only)
    // Overload 2: + session (adds [sess=N])
    // Overload 3: + session + msg_id (adds [sess=N][msg=0xHHHH])

    // NOLINTBEGIN(cppcoreguidelines-macro-usage) — internal code-gen, immediately #undef'd
#define APEX_SCOPED_LOG_(level_name, spdlog_level)                                                                     \
    template <typename... Args> void level_name(log_fmt<Args...> fmt, Args&&... args)                                  \
    {                                                                                                                  \
        if (!should_log(spdlog::level::spdlog_level))                                                                  \
            return;                                                                                                    \
        log_impl(spdlog::level::spdlog_level, fmt.loc, fmt::format(fmt.fmt, std::forward<Args>(args)...));             \
    }                                                                                                                  \
    template <typename... Args> void level_name(SessionId sid, log_fmt<Args...> fmt, Args&&... args)                   \
    {                                                                                                                  \
        if (!should_log(spdlog::level::spdlog_level))                                                                  \
            return;                                                                                                    \
        log_impl(spdlog::level::spdlog_level, fmt.loc, sid, fmt::format(fmt.fmt, std::forward<Args>(args)...));        \
    }                                                                                                                  \
    template <typename... Args> void level_name(SessionId sid, uint32_t msg_id, log_fmt<Args...> fmt, Args&&... args)  \
    {                                                                                                                  \
        if (!should_log(spdlog::level::spdlog_level))                                                                  \
            return;                                                                                                    \
        log_impl(spdlog::level::spdlog_level, fmt.loc, sid, msg_id,                                                    \
                 fmt::format(fmt.fmt, std::forward<Args>(args)...));                                                   \
    }                                                                                                                  \
    template <typename S, typename... Args>                                                                            \
        requires detail::HasSessionId<S>                                                                               \
    void level_name(const S& session, log_fmt<Args...> fmt, Args&&... args)                                            \
    {                                                                                                                  \
        if (!should_log(spdlog::level::spdlog_level))                                                                  \
            return;                                                                                                    \
        log_impl(spdlog::level::spdlog_level, fmt.loc, session ? session->id() : make_session_id(0),                   \
                 fmt::format(fmt.fmt, std::forward<Args>(args)...));                                                   \
    }                                                                                                                  \
    template <typename S, typename... Args>                                                                            \
        requires detail::HasSessionId<S>                                                                               \
    void level_name(const S& session, uint32_t msg_id, log_fmt<Args...> fmt, Args&&... args)                           \
    {                                                                                                                  \
        if (!should_log(spdlog::level::spdlog_level))                                                                  \
            return;                                                                                                    \
        log_impl(spdlog::level::spdlog_level, fmt.loc, session ? session->id() : make_session_id(0), msg_id,           \
                 fmt::format(fmt.fmt, std::forward<Args>(args)...));                                                   \
    }

    APEX_SCOPED_LOG_(trace, trace)
    APEX_SCOPED_LOG_(debug, debug)
    APEX_SCOPED_LOG_(info, info)
    APEX_SCOPED_LOG_(warn, warn)
    APEX_SCOPED_LOG_(error, err)
    APEX_SCOPED_LOG_(critical, critical)

#undef APEX_SCOPED_LOG_
    // NOLINTEND(cppcoreguidelines-macro-usage)

  private:
    std::string component_;
    uint32_t core_id_{0};
    uint64_t corr_id_{0};
    std::shared_ptr<spdlog::logger> logger_;

    /// Format context prefix + message and log via spdlog.
    /// Output: [file:line Func] [core=N][Component][trace=hex] message
    void log_impl(spdlog::level::level_enum level, const std::source_location& loc, std::string_view message);

    void log_impl(spdlog::level::level_enum level, const std::source_location& loc, SessionId session_id,
                  std::string_view message);

    void log_impl(spdlog::level::level_enum level, const std::source_location& loc, SessionId session_id,
                  uint32_t msg_id, std::string_view message);
};

} // namespace apex::core
