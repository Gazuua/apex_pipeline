// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/gateway/file_watcher.hpp>

#include <boost/asio/io_context.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace apex::gateway;

TEST(FileWatcher, DetectsChange)
{
    // Create temp file
    auto tmp = std::filesystem::temp_directory_path() / "test_watcher.toml";
    {
        std::ofstream f(tmp);
        f << "initial = true\n";
    }

    // Pin initial timestamp to the past so rewrite always produces a newer time.
    // Windows NTFS can cache last_write_time, causing same-tick rewrites to go undetected.
    std::filesystem::last_write_time(tmp, std::filesystem::last_write_time(tmp) - std::chrono::seconds(2));

    boost::asio::io_context io;
    bool changed = false;

    FileWatcher watcher(io, tmp.string(), [&](const std::string&) { changed = true; }, std::chrono::milliseconds{100});
    watcher.start();

    // Modify file
    {
        std::ofstream f(tmp);
        f << "updated = true\n";
    }

    // Deterministic check — no sleep needed
    watcher.poll_now();

    EXPECT_TRUE(changed);

    watcher.stop();
    std::filesystem::remove(tmp);
}

TEST(FileWatcher, NoChangeNoCallback)
{
    auto tmp = std::filesystem::temp_directory_path() / "test_watcher_no_change.toml";
    {
        std::ofstream f(tmp);
        f << "stable = true\n";
    }

    boost::asio::io_context io;
    int change_count = 0;

    FileWatcher watcher(io, tmp.string(), [&](const std::string&) { ++change_count; }, std::chrono::milliseconds{50});
    watcher.start();

    // Poll multiple times without modifying file
    for (int i = 0; i < 5; ++i)
    {
        watcher.poll_now();
    }

    watcher.stop();
    EXPECT_EQ(change_count, 0);

    std::filesystem::remove(tmp);
}
