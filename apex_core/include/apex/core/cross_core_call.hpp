#pragma once

#include <apex/core/core_engine.hpp>
#include <apex/core/error_code.hpp>
#include <apex/core/result.hpp>

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

namespace apex::core {

/// Execute func on target_core and co_await the result.
/// Returns CrossCoreTimeout if timeout expires before completion.
/// Returns CrossCoreQueueFull if target core's MPSC queue is full.
///
/// This function is designed for infrequent cross-core RPC patterns.
/// Each call incurs 3 heap allocations (State, timer, task). For high-frequency
/// inter-core messaging, use cross_core_post() instead.
/// TODO: If profiling shows this is a bottleneck, consider arena allocation
/// or a pre-allocated cross-core channel (Phase 8.5 benchmark).
///
/// @pre func MUST NOT capture coroutine-local variables by reference.
/// On timeout, a narrow race window exists where func may still execute after
/// the caller's coroutine resumes and its stack frame is destroyed. The early
/// check (status != 0) covers the common case, but the window between the
/// check and std::invoke cannot be fully eliminated without a CAS-first design
/// that would complicate result visibility. Value-capture or shared_ptr capture only.
///
/// @pre R must be nothrow-destructible and thread-safe to destroy. If timeout
/// wins the CAS race, the result object is destroyed on the target core's thread,
/// not on the caller's thread. Avoid R types with thread-affine RAII semantics.
///
/// @warning On timeout, the target core may still be executing or about to
/// execute the task. If the caller's shared_ptr to State is the last
/// reference, ~State() runs on the target core's thread. Ensure R has
/// no thread-affine RAII (e.g., thread-local cleanup, mutex ownership).
template <typename F>
    requires (!std::is_void_v<std::invoke_result_t<F>>)
auto cross_core_call(CoreEngine& engine, uint32_t target_core, F func,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds{5000})
    -> boost::asio::awaitable<Result<std::invoke_result_t<F>>>
{
    using R = std::invoke_result_t<F>;
    static_assert(std::is_nothrow_destructible_v<R>,
        "cross_core_call<R>: R must be nothrow destructible "
        "(may be destroyed on the target core's thread after timeout)");

    // Shared state between caller coroutine and target core task.
    // status: 0 = pending, 1 = completed, 2 = timed out
    struct State {
        std::atomic<int> status{0};
        std::optional<R> result;
    };
    auto state = std::make_shared<State>();

    // Timer on caller's executor — heap-allocated so target core can
    // safely cancel it even if the coroutine has already resumed.
    auto executor = co_await boost::asio::this_coro::executor;
    auto timer = std::make_shared<boost::asio::steady_timer>(executor);
    timer->expires_after(timeout);

    // Type-erased task to execute on target core.
    // Captures shared_ptr to state and timer for safe cross-thread access.
    // Race between func execution and timeout is inherent and intentional.
    // If timeout wins the CAS race, func may still execute (already past the
    // early check) but its result is discarded. This matches Seastar's model.
    // Side-effectful funcs must be idempotent or accept at-most-once semantics.
    auto* task = new std::function<void()>(
        [state, timer, f = std::move(func)]() mutable {
            // Early check: skip execution if already timed out.
            // Prevents dangling-reference access when func captures by ref.
            if (state->status.load(std::memory_order_acquire) != 0) {
                return;
            }
            auto r = std::invoke(std::move(f));
            // Write result BEFORE CAS so caller sees it after observing status==1.
            state->result.emplace(std::move(r));
            int expected = 0;
            // seq_cst is intentional: cross_core_call is infrequent (RPC pattern),
            // and seq_cst provides the strongest ordering guarantee for correctness.
            if (state->status.compare_exchange_strong(expected, 1)) {
                boost::asio::post(timer->get_executor(),
                    [timer] { timer->cancel(); });
            }
            // CAS failed → timeout won. Result was written but will never be read;
            // it is destroyed when the shared State is released (on this thread).
        });

    CoreMessage msg;
    msg.type = CoreMessage::Type::CrossCoreRequest;
    msg.data = reinterpret_cast<uint64_t>(task);

    if (!engine.post_to(target_core, msg)) {
        delete task;
        co_return apex::core::error(ErrorCode::CrossCoreQueueFull);
    }

    // Wait for either task completion (timer cancel) or timeout
    (void)co_await timer->async_wait(
        boost::asio::as_tuple(boost::asio::use_awaitable));

    // CAS resolves the race between completion and timeout
    int expected = 0;
    if (state->status.compare_exchange_strong(expected, 2)) {
        co_return apex::core::error(ErrorCode::CrossCoreTimeout);
    }

    co_return std::move(*state->result);
}

/// Execute void func on target_core and co_await completion.
///
/// @pre func MUST NOT capture coroutine-local variables by reference.
/// See non-void overload documentation for the rationale.
template <typename F>
    requires std::is_void_v<std::invoke_result_t<F>>
auto cross_core_call(CoreEngine& engine, uint32_t target_core, F func,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds{5000})
    -> boost::asio::awaitable<Result<void>>
{
    struct State {
        std::atomic<int> status{0};
    };
    auto state = std::make_shared<State>();

    auto executor = co_await boost::asio::this_coro::executor;
    auto timer = std::make_shared<boost::asio::steady_timer>(executor);
    timer->expires_after(timeout);

    // Race between func execution and timeout is inherent and intentional.
    // If timeout wins the CAS race, func may still execute (already past the
    // early check) but its side effects are fire-and-forget. Seastar's model.
    // Side-effectful funcs must be idempotent or accept at-most-once semantics.
    auto* task = new std::function<void()>(
        [state, timer, f = std::move(func)]() mutable {
            if (state->status.load(std::memory_order_acquire) != 0) {
                return;
            }
            std::invoke(std::move(f));
            int expected = 0;
            // seq_cst is intentional: cross_core_call is infrequent (RPC pattern),
            // and seq_cst provides the strongest ordering guarantee for correctness.
            if (state->status.compare_exchange_strong(expected, 1)) {
                boost::asio::post(timer->get_executor(),
                    [timer] { timer->cancel(); });
            }
        });

    CoreMessage msg;
    msg.type = CoreMessage::Type::CrossCoreRequest;
    msg.data = reinterpret_cast<uint64_t>(task);

    if (!engine.post_to(target_core, msg)) {
        delete task;
        co_return apex::core::error(ErrorCode::CrossCoreQueueFull);
    }

    (void)co_await timer->async_wait(
        boost::asio::as_tuple(boost::asio::use_awaitable));

    int expected = 0;
    if (state->status.compare_exchange_strong(expected, 2)) {
        co_return apex::core::error(ErrorCode::CrossCoreTimeout);
    }

    co_return apex::core::ok();
}

/// Fire-and-forget execution on target core (no timeout, no result).
/// Preferred over cross_core_call() for high-frequency inter-core messaging
/// where the caller does not need a response.
/// Returns false if queue is full.
template <typename F>
bool cross_core_post(CoreEngine& engine, uint32_t target_core, F&& func) {
    auto* task = new std::function<void()>(std::forward<F>(func));
    CoreMessage msg;
    msg.type = CoreMessage::Type::CrossCorePost;
    msg.data = reinterpret_cast<uint64_t>(task);
    if (!engine.post_to(target_core, msg)) {
        delete task;
        return false;
    }
    return true;
}

} // namespace apex::core
