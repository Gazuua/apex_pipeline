// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/gateway/file_watcher.hpp>

#include <spdlog/spdlog.h>

namespace apex::gateway
{

FileWatcher::FileWatcher(boost::asio::io_context& io_ctx, std::string path, Callback callback,
                         std::chrono::milliseconds interval)
    : io_ctx_(io_ctx)
    , path_(std::move(path))
    , callback_(std::move(callback))
    , interval_(interval)
    , timer_(std::make_unique<boost::asio::steady_timer>(io_ctx))
{
    // Record initial modification time
    std::error_code ec;
    last_write_time_ = std::filesystem::last_write_time(path_, ec);
    if (ec)
    {
        spdlog::warn("FileWatcher: cannot stat {}: {}", path_, ec.message());
    }
}

FileWatcher::~FileWatcher()
{
    stop();
}

void FileWatcher::start()
{
    running_ = true;
    schedule_check();
    spdlog::info("FileWatcher started: {}", path_);
}

void FileWatcher::stop()
{
    running_ = false;
    if (timer_)
    {
        timer_->cancel();
    }
}

void FileWatcher::poll_now()
{
    check_file();
}

void FileWatcher::schedule_check()
{
    if (!running_)
        return;
    timer_->expires_after(interval_);
    timer_->async_wait([this](boost::system::error_code ec) {
        if (ec || !running_)
            return;
        check_file();
        schedule_check();
    });
}

void FileWatcher::check_file()
{
    std::error_code ec;
    auto current = std::filesystem::last_write_time(path_, ec);
    if (ec)
    {
        spdlog::debug("FileWatcher: stat failed for {}: {}", path_, ec.message());
        return;
    }

    if (current != last_write_time_)
    {
        last_write_time_ = current;
        spdlog::info("FileWatcher: change detected in {}", path_);
        if (callback_)
        {
            callback_(path_);
        }
    }
}

} // namespace apex::gateway
