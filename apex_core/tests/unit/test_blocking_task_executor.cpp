// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/blocking_task_executor.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

using namespace apex::core;

// ── 기본 실행 ────────────────────────────────────────────────────

TEST(BlockingTaskExecutorTest, RunReturnsValue)
{
    BlockingTaskExecutor executor(1);
    boost::asio::io_context io;

    auto future = boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<int> {
            auto result = co_await executor.run([] { return 42; });
            co_return result;
        },
        boost::asio::use_future);

    io.run();
    EXPECT_EQ(future.get(), 42);
}

TEST(BlockingTaskExecutorTest, RunReturnsString)
{
    BlockingTaskExecutor executor(1);
    boost::asio::io_context io;

    auto future = boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<std::string> {
            auto result = co_await executor.run([] { return std::string("hello"); });
            co_return result;
        },
        boost::asio::use_future);

    io.run();
    EXPECT_EQ(future.get(), "hello");
}

// ── void 반환 ────────────────────────────────────────────────────

TEST(BlockingTaskExecutorTest, RunVoidTask)
{
    BlockingTaskExecutor executor(1);
    boost::asio::io_context io;
    bool executed = false;

    auto future = boost::asio::co_spawn(
        io, [&]() -> boost::asio::awaitable<void> { co_await executor.run([&] { executed = true; }); },
        boost::asio::use_future);

    io.run();
    future.get();
    EXPECT_TRUE(executed);
}

// ── 동시 실행 ────────────────────────────────────────────────────

TEST(BlockingTaskExecutorTest, ConcurrentTasks)
{
    BlockingTaskExecutor executor(2);
    boost::asio::io_context io;

    auto future1 = boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<int> {
            co_return co_await executor.run([] {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return 1;
            });
        },
        boost::asio::use_future);

    auto future2 = boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<int> {
            co_return co_await executor.run([] {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return 2;
            });
        },
        boost::asio::use_future);

    io.run();
    EXPECT_EQ(future1.get(), 1);
    EXPECT_EQ(future2.get(), 2);
}

// ── executor 복귀 확인 ───────────────────────────────────────────

TEST(BlockingTaskExecutorTest, ResumesOnCallerExecutor)
{
    BlockingTaskExecutor executor(1);
    boost::asio::io_context io;

    auto caller_thread_id = std::this_thread::get_id();
    std::thread::id resume_thread_id;

    auto future = boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            co_await executor.run([] {
                // 다른 스레드에서 실행
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            });
            // resume 후 — io_context 스레드에서 실행되어야 함
            resume_thread_id = std::this_thread::get_id();
        },
        boost::asio::use_future);

    io.run();
    future.get();
    // io.run()을 호출한 스레드 = caller_thread_id
    // resume도 같은 io_context에서 실행되므로 같은 스레드
    EXPECT_EQ(resume_thread_id, caller_thread_id);
}

// ── 예외 전파 ────────────────────────────────────────────────────

TEST(BlockingTaskExecutorTest, ExceptionPropagation)
{
    BlockingTaskExecutor executor(1);
    boost::asio::io_context io;

    auto future = boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<int> {
            co_return co_await executor.run([]() -> int { throw std::runtime_error("test error"); });
        },
        boost::asio::use_future);

    io.run();
    EXPECT_THROW(future.get(), std::runtime_error);
}

// ── thread_count 접근자 ──────────────────────────────────────────

TEST(BlockingTaskExecutorTest, ThreadCount)
{
    BlockingTaskExecutor executor(4);
    EXPECT_EQ(executor.thread_count(), 4);
}
