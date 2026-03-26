// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/blocking_task_executor.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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
    std::thread::id pool_thread_id;
    std::thread::id resume_thread_id;

    auto future = boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            co_await executor.run([&] {
                // thread pool 스레드 ID 캡처
                pool_thread_id = std::this_thread::get_id();
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
    // thread pool에서 실행된 스레드는 io_context 스레드와 달라야 함
    EXPECT_NE(pool_thread_id, caller_thread_id);
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
    try
    {
        future.get();
        FAIL() << "Expected std::runtime_error";
    }
    catch (const std::runtime_error& e)
    {
        EXPECT_STREQ(e.what(), "test error");
    }
}

// ── thread_count 접근자 ──────────────────────────────────────────

TEST(BlockingTaskExecutorTest, ThreadCount)
{
    BlockingTaskExecutor executor(4);
    EXPECT_EQ(executor.thread_count(), 4);
}

// ── pool saturation ─────────────────────────────────────────────

TEST(BlockingTaskExecutorTest, PoolSaturationQueuesExcessTasks)
{
    // thread_count=2에 task 8개 동시 제출 — 풀 포화 시 큐잉 후 전체 정상 완료
    BlockingTaskExecutor executor(2);
    boost::asio::io_context io;

    constexpr int task_count = 8;
    std::atomic<int> completed{0};
    std::vector<std::future<int>> futures;
    futures.reserve(task_count);

    for (int i = 0; i < task_count; ++i)
    {
        futures.push_back(boost::asio::co_spawn(
            io,
            [&, i]() -> boost::asio::awaitable<int> {
                co_return co_await executor.run([&completed, i] {
                    // 각 작업이 짧은 CPU 작업 수행
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    completed.fetch_add(1, std::memory_order_relaxed);
                    return i;
                });
            },
            boost::asio::use_future));
    }

    io.run();

    for (int i = 0; i < task_count; ++i)
    {
        EXPECT_EQ(futures[static_cast<size_t>(i)].get(), i);
    }
    EXPECT_EQ(completed.load(), task_count);
}

// ── shutdown ────────────────────────────────────────────────────

TEST(BlockingTaskExecutorTest, ShutdownWaitsForRunningTasks)
{
    // 진행 중인 작업이 완료된 후에 shutdown이 반환되는지 확인
    auto executor = std::make_unique<BlockingTaskExecutor>(1);
    boost::asio::io_context io;

    std::atomic<bool> task_started{false};
    std::atomic<bool> task_finished{false};

    auto future = boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            co_await executor->run([&] {
                task_started.store(true, std::memory_order_release);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                task_finished.store(true, std::memory_order_release);
            });
        },
        boost::asio::use_future);

    io.run();
    future.get();

    // 작업 완료 후 shutdown — join이 정상 반환해야 함
    executor->shutdown();
    EXPECT_TRUE(task_started.load());
    EXPECT_TRUE(task_finished.load());
}

TEST(BlockingTaskExecutorTest, DoubleShutdownIsSafe)
{
    // 소멸자에서도 shutdown()을 호출하므로 double shutdown 안전성 확인
    BlockingTaskExecutor executor(1);
    executor.shutdown();
    executor.shutdown(); // 두 번째 호출도 UB 없이 안전해야 함
}

// ── move-only 반환 타입 ─────────────────────────────────────────

TEST(BlockingTaskExecutorTest, RunReturnsMoveOnlyType)
{
    BlockingTaskExecutor executor(1);
    boost::asio::io_context io;

    auto future = boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<std::unique_ptr<int>> {
            co_return co_await executor.run([] { return std::make_unique<int>(99); });
        },
        boost::asio::use_future);

    io.run();
    auto result = future.get();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(*result, 99);
}
