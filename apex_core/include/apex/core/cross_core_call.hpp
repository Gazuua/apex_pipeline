// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/core_engine.hpp>
#include <apex/core/error_code.hpp>
#include <apex/core/result.hpp>
#include <apex/core/shared_payload.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>

namespace apex::core
{

/// Execute func on target_core and co_await the result.
/// Returns CrossCoreTimeout if timeout expires before completion.
///
/// Uses co_post_to() for SPSC mesh backpressure.
///
/// @pre func MUST NOT capture coroutine-local variables by reference.
/// @pre R must be nothrow-destructible and thread-safe to destroy.
template <typename F>
    requires(!std::is_void_v<std::invoke_result_t<F>>)
[[nodiscard]] auto cross_core_call(CoreEngine& engine, uint32_t target_core, F func,
                                   std::chrono::milliseconds timeout = std::chrono::milliseconds{5000})
    -> boost::asio::awaitable<Result<std::invoke_result_t<F>>>
{
    using R = std::invoke_result_t<F>;
    // R의 소멸자는 thread-safe여야 함 — 타임아웃 시 타깃 코어 스레드에서 소멸될 수 있음.
    // 호출자 코어가 타임아웃(status CAS → 2)한 뒤에도 타깃 코어의 task 람다가 아직
    // State를 shared_ptr로 보유 중이므로, State::result (즉 R)의 최종 소멸은
    // 타깃 코어 스레드의 람다 소멸 시점에 발생한다.
    // 따라서 R의 소멸자가 특정 스레드에 종속된 리소스를 해제하면 UB가 된다.
    static_assert(std::is_nothrow_destructible_v<R>, "cross_core_call<R>: R must be nothrow destructible "
                                                     "(may be destroyed on the target core's thread after timeout)");

    struct State
    {
        std::atomic<int> status{0};
        std::optional<R> result;
    };
    auto state = std::make_shared<State>();

    auto executor = co_await boost::asio::this_coro::executor;
    auto timer = std::make_shared<boost::asio::steady_timer>(executor);
    timer->expires_after(timeout);

    auto* task = new std::function<void()>([state, timer, f = std::move(func)]() mutable {
        if (state->status.load(std::memory_order_acquire) != 0)
        {
            return;
        }
        try
        {
            auto r = std::invoke(std::move(f));
            state->result.emplace(std::move(r));
            int expected = 0;
            if (state->status.compare_exchange_strong(expected, 1))
            {
                boost::asio::post(timer->get_executor(), [timer] { timer->cancel(); });
            }
        }
        catch (...)
        {
            // func threw — signal error state (3) so caller doesn't wait until timeout
            int expected = 0;
            if (state->status.compare_exchange_strong(expected, 3))
            {
                boost::asio::post(timer->get_executor(), [timer] { timer->cancel(); });
            }
        }
    });

    CoreMessage msg;
    msg.op = CrossCoreOp::LegacyCrossCoreFn;
    msg.data = reinterpret_cast<uintptr_t>(task);

    try
    {
        co_await engine.co_post_to(target_core, msg);
    }
    catch (...)
    {
        delete task;
        throw;
    }

    (void)co_await timer->async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));

    int expected = 0;
    if (state->status.compare_exchange_strong(expected, 2))
    {
        co_return apex::core::error(ErrorCode::CrossCoreTimeout);
    }

    if (state->status.load(std::memory_order_acquire) == 3)
    {
        co_return apex::core::error(ErrorCode::CrossCoreFuncException);
    }

    assert(state->result.has_value() && "cross_core_call: result must be set before CAS(1)");
    co_return std::move(*state->result);
}

/// Execute void func on target_core and co_await completion.
template <typename F>
    requires std::is_void_v<std::invoke_result_t<F>>
[[nodiscard]] auto cross_core_call(CoreEngine& engine, uint32_t target_core, F func,
                                   std::chrono::milliseconds timeout = std::chrono::milliseconds{5000})
    -> boost::asio::awaitable<Result<void>>
{
    struct State
    {
        std::atomic<int> status{0};
    };
    auto state = std::make_shared<State>();

    auto executor = co_await boost::asio::this_coro::executor;
    auto timer = std::make_shared<boost::asio::steady_timer>(executor);
    timer->expires_after(timeout);

    auto* task = new std::function<void()>([state, timer, f = std::move(func)]() mutable {
        if (state->status.load(std::memory_order_acquire) != 0)
        {
            return;
        }
        try
        {
            std::invoke(std::move(f));
            int expected = 0;
            if (state->status.compare_exchange_strong(expected, 1))
            {
                boost::asio::post(timer->get_executor(), [timer] { timer->cancel(); });
            }
        }
        catch (...)
        {
            // func threw — signal error state (3) so caller doesn't wait until timeout
            int expected = 0;
            if (state->status.compare_exchange_strong(expected, 3))
            {
                boost::asio::post(timer->get_executor(), [timer] { timer->cancel(); });
            }
        }
    });

    CoreMessage msg;
    msg.op = CrossCoreOp::LegacyCrossCoreFn;
    msg.data = reinterpret_cast<uintptr_t>(task);

    try
    {
        co_await engine.co_post_to(target_core, msg);
    }
    catch (...)
    {
        delete task;
        throw;
    }

    (void)co_await timer->async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));

    int expected = 0;
    if (state->status.compare_exchange_strong(expected, 2))
    {
        co_return apex::core::error(ErrorCode::CrossCoreTimeout);
    }

    if (state->status.load(std::memory_order_acquire) == 3)
    {
        co_return apex::core::error(ErrorCode::CrossCoreFuncException);
    }

    co_return apex::core::ok();
}

/// Fire-and-forget closure execution on target core. Awaitable, core thread only.
/// For non-core threads, use asio::post(engine.io_context(core_id), callback) directly.
template <typename F>
[[nodiscard]] boost::asio::awaitable<void> cross_core_post(CoreEngine& engine, uint32_t target_core, F&& func)
{
    auto* task = new std::function<void()>(std::forward<F>(func));
    CoreMessage msg;
    msg.op = CrossCoreOp::LegacyCrossCoreFn;
    msg.data = reinterpret_cast<uintptr_t>(task);
    try
    {
        co_await engine.co_post_to(target_core, msg);
    }
    catch (...)
    {
        delete task;
        throw;
    }
}

/// Zero-allocation fire-and-forget message passing via CrossCoreOp.
/// Awaitable, core thread only.
[[nodiscard]] inline boost::asio::awaitable<void> cross_core_post_msg(CoreEngine& engine, uint32_t source_core,
                                                                      uint32_t target_core, CrossCoreOp op,
                                                                      void* data = nullptr)
{
    assert(source_core < engine.core_count() && "invalid source_core");
    CoreMessage msg{.op = op, .source_core = source_core, .data = reinterpret_cast<uintptr_t>(data)};
    co_await engine.co_post_to(target_core, msg);
}

/// Broadcast a shared payload to all cores except source. Awaitable, core thread only.
[[nodiscard]] inline boost::asio::awaitable<void> broadcast_cross_core(CoreEngine& engine, uint32_t source_core,
                                                                       CrossCoreOp op, SharedPayload* payload)
{
    assert(payload != nullptr && "broadcast_cross_core: payload must not be null");

    if (engine.core_count() <= 1)
    {
        delete payload;
        co_return;
    }

    assert(payload->refcount() > 0 && "broadcast_cross_core: refcount must be set before calling");

    const uint32_t total_receivers = engine.core_count() - 1; // excluding source_core
    uint32_t delivered = 0;
    try
    {
        for (uint32_t i = 0; i < engine.core_count(); ++i)
        {
            if (i == source_core)
                continue;
            CoreMessage msg{.op = op, .source_core = source_core, .data = reinterpret_cast<uintptr_t>(payload)};
            co_await engine.co_post_to(i, msg);
            ++delivered;
        }
    }
    catch (...)
    {
        // Release refcount for each undelivered core to prevent SharedPayload leak.
        // Already-delivered cores will release() normally on consumption.
        const uint32_t undelivered = total_receivers - delivered;
        for (uint32_t j = 0; j < undelivered; ++j)
        {
            payload->release();
        }
        throw;
    }
}

} // namespace apex::core
