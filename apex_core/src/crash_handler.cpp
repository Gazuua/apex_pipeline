// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/crash_handler.hpp>

#include <spdlog/spdlog.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <io.h> // _write
#include <Windows.h>
#else
#include <unistd.h> // write
#endif

namespace apex::core
{

// Cached logger pointer — avoids spdlog registry mutex in signal handler.
// Stored at install_crash_handlers(), cleared at uninstall_crash_handlers().
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static std::atomic<spdlog::logger*> g_cached_logger{nullptr};

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
    (void)_write(2, msg, static_cast<unsigned int>(len));
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

    // Step 2: Best-effort spdlog flush via cached pointer (bypasses registry mutex).
    // WARNING: Not strictly async-signal-safe — flush() uses internal mutex.
    // If spdlog is mid-write when the signal fires, this may deadlock.
    // The stderr output above guarantees minimal diagnostics even if flush deadlocks.
    if (auto* logger = g_cached_logger.load(std::memory_order_acquire))
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

    if (auto* logger = g_cached_logger.load(std::memory_order_acquire))
    {
        logger->flush();
    }

    return EXCEPTION_CONTINUE_SEARCH; // Allow default crash behavior (dump/debugger)
}
#endif

} // anonymous namespace

/// Detect sanitizer builds. Sanitizers (ASAN, TSAN, MSAN) install their own
/// signal handlers; overwriting them causes false positives or missed detections.
static bool is_sanitizer_active() noexcept
{
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
    return true;
#endif
#endif
    return false;
}

void install_crash_handlers()
{
    // Sanitizers use their own signal handlers (especially SIGSEGV for shadow memory).
    // Installing ours would interfere with ASAN/TSAN/MSAN detection mechanisms.
    if (is_sanitizer_active())
        return;

    // Cache logger pointer to bypass spdlog registry mutex in signal handler
    g_cached_logger.store(spdlog::default_logger_raw(), std::memory_order_release);

    std::signal(SIGABRT, crash_signal_handler);
    std::signal(SIGSEGV, crash_signal_handler);
    std::signal(SIGFPE, crash_signal_handler);

#if !defined(_WIN32)
    std::signal(SIGBUS, crash_signal_handler);
#endif

#ifdef _WIN32
    SetUnhandledExceptionFilter(unhandled_exception_filter);
    // Suppress the Windows abort() dialog ("Debug Error!") to allow clean crash.
    // _WRITE_ABORT_MSG controls the "abnormal program termination" message box.
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
#endif
}

void uninstall_crash_handlers()
{
    if (is_sanitizer_active())
        return;

    g_cached_logger.store(nullptr, std::memory_order_release);

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
