// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <cstdint>
#include <type_traits>

namespace apex::core
{

/// CPU-bound 블로킹 작업을 별도 스레드 풀로 offload하는 awaitable executor.
///
/// 코어 IO 스레드에서 CPU-intensive 작업(bcrypt, JWT 서명 등)을 실행하면
/// 해당 코어의 모든 비동기 작업이 블로킹된다. 이 클래스는 작업을 thread pool로
/// offload하고, 완료 후 호출자의 코어 executor로 resume하여 shared-nothing
/// 모델을 유지한다.
///
/// Usage:
///   auto result = co_await blocking_executor().run([&] {
///       return password_hasher_.verify(password, hash);
///   });
class BlockingTaskExecutor
{
  public:
    explicit BlockingTaskExecutor(uint32_t thread_count)
        : pool_(thread_count)
        , thread_count_(thread_count)
    {}

    ~BlockingTaskExecutor()
    {
        shutdown();
    }

    BlockingTaskExecutor(const BlockingTaskExecutor&) = delete;
    BlockingTaskExecutor& operator=(const BlockingTaskExecutor&) = delete;

    /// CPU-bound 작업을 스레드 풀에서 실행하고 결과를 호출자 코어로 반환.
    /// 호출자 코루틴은 suspend되어 코어 스레드를 반환하고,
    /// 작업 완료 후 원래 코어 스레드에서 resume된다.
    template <typename F>
        requires(!std::is_void_v<std::invoke_result_t<F>>)
    auto run(F&& fn) -> boost::asio::awaitable<std::invoke_result_t<F>>
    {
        using R = std::invoke_result_t<F>;

        auto caller_executor = co_await boost::asio::this_coro::executor;

        // promise를 통해 thread pool 작업 결과를 전달
        // co_spawn으로 thread pool에서 실행 후 caller executor로 resume
        R result = co_await boost::asio::co_spawn(
            pool_, [f = std::forward<F>(fn)]() -> boost::asio::awaitable<R> { co_return f(); },
            boost::asio::use_awaitable);

        co_return result;
    }

    /// void 반환 작업용 오버로드.
    template <typename F>
        requires(std::is_void_v<std::invoke_result_t<F>>)
    auto run(F&& fn) -> boost::asio::awaitable<void>
    {
        co_await boost::asio::co_spawn(
            pool_,
            [f = std::forward<F>(fn)]() -> boost::asio::awaitable<void> {
                f();
                co_return;
            },
            boost::asio::use_awaitable);
    }

    /// 스레드 풀 종료 — 진행 중인 작업 완료 대기 후 반환.
    void shutdown()
    {
        pool_.join();
    }

    [[nodiscard]] uint32_t thread_count() const noexcept
    {
        return thread_count_;
    }

  private:
    boost::asio::thread_pool pool_;
    uint32_t thread_count_;
};

} // namespace apex::core
