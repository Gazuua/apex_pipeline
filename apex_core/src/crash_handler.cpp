// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/crash_handler.hpp>

#include <spdlog/spdlog.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <io.h> // _write
#else
#include <unistd.h> // write
#endif

namespace apex::core
{

namespace
{

/// Write a string to stderr in a signal-safe manner.
void safe_stderr(const char* msg) noexcept
{
    if (!msg)
        return;
    // Compute length manually (strlen is not guaranteed signal-safe on all platforms)
    size_t len = 0;
    while (msg[len] != '\0')
        ++len;
#ifdef _WIN32
    _write(2, msg, static_cast<unsigned int>(len));
#else
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) — write() is signal-safe
    [[maybe_unused]] auto _ = ::write(STDERR_FILENO, msg, len);
#endif
}

/// Map signal number to a human-readable name.
const char* signal_name(int sig) noexcept
{
    switch (sig)
    {
    case SIGABRT:
        return "SIGABRT";
    case SIGSEGV:
        return "SIGSEGV";
    case SIGFPE:
        return "SIGFPE";
#if !defined(_WIN32)
    case SIGBUS:
        return "SIGBUS";
#endif
    default:
        return "UNKNOWN";
    }
}

/// Crash signal handler. Writes minimal info to stderr, attempts spdlog flush,
/// then re-raises with default handler to allow core dump.
void crash_signal_handler(int sig) noexcept
{
    // Step 1: Signal-safe stderr output
    safe_stderr("\n[APEX CRASH] Signal: ");
    safe_stderr(signal_name(sig));
    safe_stderr("\n[APEX CRASH] Attempting log flush before exit...\n");

    // Step 2: Best-effort spdlog flush.
    // WARNING: Not strictly async-signal-safe. If spdlog is mid-write when the
    // signal fires, this may deadlock on spdlog's internal mutex. The stderr
    // output above guarantees minimal diagnostics even if flush deadlocks.
    if (auto logger = spdlog::default_logger())
    {
        logger->flush();
    }

    // Step 3: Restore default handler and re-raise for core dump
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

#ifdef _WIN32
/// Windows SEH filter for unhandled exceptions (access violations, etc.)
LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* info) noexcept
{
    safe_stderr("\n[APEX CRASH] Unhandled Windows exception: 0x");

    // Convert exception code to hex string manually (signal-safe)
    if (info && info->ExceptionRecord)
    {
        auto code = info->ExceptionRecord->ExceptionCode;
        char hex[9];
        for (int i = 7; i >= 0; --i)
        {
            auto nibble = code & 0xF;
            hex[i] = static_cast<char>(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
            code >>= 4;
        }
        hex[8] = '\0';
        safe_stderr(hex);
    }
    safe_stderr("\n");

    if (auto logger = spdlog::default_logger())
    {
        logger->flush();
    }

    return EXCEPTION_CONTINUE_SEARCH; // Allow default crash behavior (dump/debugger)
}
#endif

} // anonymous namespace

void install_crash_handlers()
{
    std::signal(SIGABRT, crash_signal_handler);
    std::signal(SIGSEGV, crash_signal_handler);
    std::signal(SIGFPE, crash_signal_handler);

#if !defined(_WIN32)
    std::signal(SIGBUS, crash_signal_handler);
#endif

#ifdef _WIN32
    SetUnhandledExceptionFilter(unhandled_exception_filter);
    // Suppress the Windows abort() dialog ("Debug Error!") to allow clean crash
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTMODE);
#endif
}

void uninstall_crash_handlers()
{
    std::signal(SIGABRT, SIG_DFL);
    std::signal(SIGSEGV, SIG_DFL);
    std::signal(SIGFPE, SIG_DFL);

#if !defined(_WIN32)
    std::signal(SIGBUS, SIG_DFL);
#endif

#ifdef _WIN32
    SetUnhandledExceptionFilter(nullptr);
#endif
}

} // namespace apex::core
