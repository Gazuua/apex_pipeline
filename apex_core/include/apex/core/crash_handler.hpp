// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

namespace apex::core
{

/// Install crash signal handlers (SIGABRT, SIGSEGV, SIGFPE, SIGBUS).
/// On Windows, also installs SetUnhandledExceptionFilter and suppresses
/// the abort() dialog via _set_abort_behavior().
///
/// Handlers write minimal diagnostics to stderr (signal-safe) and perform
/// a best-effort spdlog flush before re-raising the signal with SIG_DFL
/// to allow core dump generation.
///
/// Should be called once at Server::run() startup. Independent of
/// ServerConfig::handle_signals (crash handlers are always active).
void install_crash_handlers();

/// Restore default signal handlers. Called during orderly shutdown if needed.
void uninstall_crash_handlers();

} // namespace apex::core
