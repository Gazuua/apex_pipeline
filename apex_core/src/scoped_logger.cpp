// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/scoped_logger.hpp>

namespace apex::core
{

namespace
{

/// Extract basename from full file path (strips directory, cross-platform).
std::string_view basename(const char* path)
{
    std::string_view sv(path);
    auto pos = sv.find_last_of("/\\");
    return pos == std::string_view::npos ? sv : sv.substr(pos + 1);
}

/// Extract short function name: strip return type, calling convention, and namespace prefix.
/// "void __cdecl apex::core::CoreEngine::run(void)" → "CoreEngine::run"
/// "void apex::core::CoreEngine::run()"             → "CoreEngine::run"
std::string_view short_func(const char* name)
{
    std::string_view sv(name);
    // Strip parameters (everything from first '(')
    if (auto p = sv.find('('); p != std::string_view::npos)
        sv = sv.substr(0, p);
    // Strip return type + calling convention (everything before last space)
    if (auto s = sv.rfind(' '); s != std::string_view::npos)
        sv = sv.substr(s + 1);
    // Keep last two ::‐separated segments (Class::method)
    if (auto p = sv.rfind("::"); p != std::string_view::npos)
    {
        auto q = sv.substr(0, p).rfind("::");
        if (q != std::string_view::npos)
            sv = sv.substr(q + 2);
    }
    return sv;
}

/// Append common prefix: [file:line Func] [core=N][Component]
void format_prefix(fmt::memory_buffer& buf, const std::source_location& loc, uint32_t core_id,
                   const std::string& component)
{
    fmt::format_to(std::back_inserter(buf), "[{}:{} {}] ", basename(loc.file_name()), loc.line(),
                   short_func(loc.function_name()));
    if (core_id != ScopedLogger::NO_CORE)
        fmt::format_to(std::back_inserter(buf), "[core={}]", core_id);
    fmt::format_to(std::back_inserter(buf), "[{}]", component);
}

/// Append trace suffix if corr_id is set, then the user message.
void format_suffix(fmt::memory_buffer& buf, uint64_t corr_id, std::string_view message)
{
    if (corr_id != 0)
        fmt::format_to(std::back_inserter(buf), "[trace={:x}]", corr_id);
    fmt::format_to(std::back_inserter(buf), " {}", message);
}

} // anonymous namespace

ScopedLogger::ScopedLogger(std::string component, uint32_t core_id, std::string_view logger_name)
    : component_(std::move(component))
    , core_id_(core_id)
    , logger_(spdlog::get(std::string(logger_name)))
{}

ScopedLogger ScopedLogger::with_trace(uint64_t corr_id) const
{
    ScopedLogger copy = *this;
    copy.corr_id_ = corr_id;
    return copy;
}

void ScopedLogger::log_impl(spdlog::level::level_enum level, const std::source_location& loc, std::string_view message)
{
    if (!logger_)
        return;
    fmt::memory_buffer buf;
    format_prefix(buf, loc, core_id_, component_);
    format_suffix(buf, corr_id_, message);
    logger_->log(level, std::string_view(buf.data(), buf.size()));
}

void ScopedLogger::log_impl(spdlog::level::level_enum level, const std::source_location& loc, SessionId session_id,
                            std::string_view message)
{
    if (!logger_)
        return;
    fmt::memory_buffer buf;
    format_prefix(buf, loc, core_id_, component_);
    fmt::format_to(std::back_inserter(buf), "[sess={}]", session_id);
    format_suffix(buf, corr_id_, message);
    logger_->log(level, std::string_view(buf.data(), buf.size()));
}

void ScopedLogger::log_impl(spdlog::level::level_enum level, const std::source_location& loc, SessionId session_id,
                            uint32_t msg_id, std::string_view message)
{
    if (!logger_)
        return;
    fmt::memory_buffer buf;
    format_prefix(buf, loc, core_id_, component_);
    fmt::format_to(std::back_inserter(buf), "[sess={}][msg=0x{:04X}]", session_id, msg_id);
    format_suffix(buf, corr_id_, message);
    logger_->log(level, std::string_view(buf.data(), buf.size()));
}

} // namespace apex::core
