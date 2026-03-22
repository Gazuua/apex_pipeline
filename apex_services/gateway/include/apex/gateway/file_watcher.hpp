// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace apex::gateway
{

/// File change detector.
/// Cross-platform implementation:
///   - Windows: polling-based (stat comparison, period: 1s)
///   - Linux: inotify (future)
///
/// Initial implementation uses polling for both platforms.
/// OS-specific optimization is lower priority.
class FileWatcher
{
  public:
    using Callback = std::function<void(const std::string& path)>;

    /// @param io_ctx Timer io_context
    /// @param path File path to watch
    /// @param callback Called on change detection
    /// @param interval Polling interval (default 1s)
    FileWatcher(boost::asio::io_context& io_ctx, std::string path, Callback callback,
                std::chrono::milliseconds interval = std::chrono::milliseconds{1000});
    ~FileWatcher();

    /// Start watching.
    void start();

    /// Stop watching.
    void stop();

    /// Immediately check the file for changes (no timer wait).
    /// Useful for deterministic testing without sleep.
    void poll_now();

  private:
    void schedule_check();
    void check_file();

    boost::asio::io_context& io_ctx_;
    std::string path_;
    Callback callback_;
    std::chrono::milliseconds interval_;
    std::unique_ptr<boost::asio::steady_timer> timer_;
    std::filesystem::file_time_type last_write_time_;
    std::atomic<bool> running_{false};
};

} // namespace apex::gateway
