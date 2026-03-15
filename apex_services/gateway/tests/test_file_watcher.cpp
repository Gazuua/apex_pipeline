#include <apex/gateway/file_watcher.hpp>

#include <boost/asio/io_context.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace apex::gateway;

TEST(FileWatcher, DetectsChange) {
    // Create temp file
    auto tmp = std::filesystem::temp_directory_path() / "test_watcher.toml";
    {
        std::ofstream f(tmp);
        f << "initial = true\n";
    }

    boost::asio::io_context io;
    bool changed = false;

    FileWatcher watcher(io, tmp.string(),
        [&](const std::string&) { changed = true; },
        std::chrono::milliseconds{100});
    watcher.start();

    // Modify file
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    {
        std::ofstream f(tmp);
        f << "updated = true\n";
    }

    // Poll io_context briefly
    auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds{3};
    while (!changed && std::chrono::steady_clock::now() < deadline) {
        io.run_for(std::chrono::milliseconds{100});
    }

    watcher.stop();
    EXPECT_TRUE(changed);

    std::filesystem::remove(tmp);
}

TEST(FileWatcher, NoChangeNoCallback) {
    auto tmp = std::filesystem::temp_directory_path() / "test_watcher_no_change.toml";
    {
        std::ofstream f(tmp);
        f << "stable = true\n";
    }

    boost::asio::io_context io;
    int change_count = 0;

    FileWatcher watcher(io, tmp.string(),
        [&](const std::string&) { ++change_count; },
        std::chrono::milliseconds{50});
    watcher.start();

    // Run without modifying file
    io.run_for(std::chrono::milliseconds{300});

    watcher.stop();
    EXPECT_EQ(change_count, 0);

    std::filesystem::remove(tmp);
}
