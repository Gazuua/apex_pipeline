// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/detail/math_utils.hpp>

#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace apex::core
{

/// Wait-free bounded SPSC (Single-Producer, Single-Consumer) queue.
/// Designed for inter-core communication in SPSC all-to-all mesh.
///
/// - Single producer enqueues (wait-free, no CAS).
/// - Single consumer dequeues (wait-free).
/// - Fixed capacity set at construction time (power-of-2).
/// - Cache-line aligned to prevent false sharing.
/// - Awaitable enqueue() for backpressure (큐 full → coroutine suspend).
///
/// Template parameter T must be trivially copyable.
///
/// ## Ordering guarantee
/// Strict FIFO — single producer, single consumer.
///
/// ## Thread safety
/// Exactly one producer thread and one consumer thread.
/// NOT safe for multiple producers or multiple consumers.
template <typename T>
    requires std::is_trivially_copyable_v<T>
class alignas(64) SpscQueue
{
  public:
    /// @param capacity Queue capacity (rounded up to power-of-2).
    /// @param producer_io Producer core's io_context (for await resume).
    explicit SpscQueue(size_t capacity, boost::asio::io_context& producer_io);
    ~SpscQueue();

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;
    SpscQueue(SpscQueue&&) = delete;
    SpscQueue& operator=(SpscQueue&&) = delete;

    // === Producer API (단일 스레드) ===

    /// Non-blocking enqueue. Returns false if queue is full.
    bool try_enqueue(const T& item) noexcept;

    /// Awaitable enqueue. Suspends if queue is full, resumes when space available.
    /// Takes item by value so that it survives in the coroutine frame across suspend points.
    boost::asio::awaitable<void> enqueue(T item);

    // === Consumer API (단일 스레드) ===

    /// Non-blocking dequeue. Returns nullopt if queue is empty.
    [[nodiscard]] std::optional<T> try_dequeue() noexcept;

    /// Batch drain into caller-provided buffer. Returns count drained.
    size_t drain(std::span<T> batch) noexcept;

    /// Call after drain — resumes waiting producer if any.
    void notify_producer_if_waiting() noexcept;

    /// Shutdown — cancels waiting producer with operation_aborted.
    void cancel_waiting_producer() noexcept;

    /// Thread-safe approximate count.
    [[nodiscard]] size_t size_approx() const noexcept;

    /// Thread-safe.
    [[nodiscard]] size_t capacity() const noexcept;

    /// Thread-safe approximate check.
    [[nodiscard]] bool empty() const noexcept;

  private:
    struct Slot
    {
        T data;
    };

    // Producer-only — 클래스 alignas(64)에 의해 캐시라인 선두 정렬.
    alignas(64) size_t head_{0};

    // Consumer-only
    alignas(64) size_t tail_{0};

    // Cross-thread coordination (acquire-release)
    alignas(64) std::atomic<size_t> published_{0}; // producer writes (release), consumer reads (acquire)
    alignas(64) std::atomic<size_t> consumed_{0};  // consumer writes (release), producer reads (acquire)

    // Immutable after construction
    size_t capacity_;
    size_t mask_;
    std::unique_ptr<Slot[]> slots_;

    // Await backpressure — separate cache line for cross-thread atomic
    boost::asio::io_context& producer_io_;
    alignas(64) std::atomic<bool> producer_waiting_{false};
    std::move_only_function<void(boost::system::error_code)> pending_handler_;
};

// --- Implementation ---

template <typename T>
    requires std::is_trivially_copyable_v<T>
SpscQueue<T>::SpscQueue(size_t capacity, boost::asio::io_context& producer_io)
    : capacity_(detail::next_power_of_2(capacity < 1 ? 1 : capacity))
    , mask_(capacity_ - 1)
    , slots_(std::make_unique<Slot[]>(capacity_))
    , producer_io_(producer_io)
{
    if (capacity_ == 0)
    {
        throw std::overflow_error("SpscQueue capacity overflow in next_power_of_2");
    }
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
SpscQueue<T>::~SpscQueue() = default;

template <typename T>
    requires std::is_trivially_copyable_v<T>
bool SpscQueue<T>::try_enqueue(const T& item) noexcept
{
    auto consumed = consumed_.load(std::memory_order_acquire);
    if (head_ - consumed >= capacity_)
    {
        return false; // full
    }
    slots_[head_ & mask_].data = item;
    ++head_;
    published_.store(head_, std::memory_order_release);
    return true;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
boost::asio::awaitable<void> SpscQueue<T>::enqueue(T item)
{
    if (try_enqueue(item))
        co_return;

    // Queue full → suspend via async_initiate
    co_await boost::asio::async_initiate<decltype(boost::asio::use_awaitable), void(boost::system::error_code)>(
        [this, &item](auto handler) {
            // Re-check: consumer may have drained between our full check and now
            auto consumed = consumed_.load(std::memory_order_acquire);
            if (head_ - consumed < capacity_)
            {
                // Space available — don't suspend
                auto ex = boost::asio::get_associated_executor(handler, producer_io_.get_executor());
                boost::asio::post(ex, [h = std::move(handler)]() mutable { h(boost::system::error_code{}); });
                return;
            }

            // Still full — store handler FIRST, then set waiting flag (release).
            // Release fence ensures handler write is visible to consumer before
            // the consumer reads producer_waiting_ (acquire).
            pending_handler_ = std::move(handler);
            producer_waiting_.store(true, std::memory_order_release);
        },
        boost::asio::use_awaitable);

    // Resumed — enqueue must succeed now
    bool ok = try_enqueue(item);
    assert(ok && "SpscQueue::enqueue: try_enqueue after resume must succeed");
    (void)ok;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
std::optional<T> SpscQueue<T>::try_dequeue() noexcept
{
    auto published = published_.load(std::memory_order_acquire);
    if (tail_ >= published)
    {
        return std::nullopt; // empty
    }
    T item = slots_[tail_ & mask_].data;
    ++tail_;
    consumed_.store(tail_, std::memory_order_release);
    return item;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
size_t SpscQueue<T>::drain(std::span<T> batch) noexcept
{
    auto published = published_.load(std::memory_order_acquire);
    size_t available = published - tail_;
    size_t count = std::min(available, batch.size());

    for (size_t i = 0; i < count; ++i)
    {
        batch[i] = slots_[(tail_ + i) & mask_].data;
    }
    tail_ += count;
    if (count > 0)
    {
        consumed_.store(tail_, std::memory_order_release);
    }
    return count;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
void SpscQueue<T>::notify_producer_if_waiting() noexcept
{
    if (!producer_waiting_.load(std::memory_order_acquire))
        return;

    producer_waiting_.store(false, std::memory_order_relaxed);

    if (pending_handler_)
    {
        auto handler = std::move(pending_handler_);
        boost::asio::post(producer_io_, [h = std::move(handler)]() mutable { h(boost::system::error_code{}); });
    }
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
void SpscQueue<T>::cancel_waiting_producer() noexcept
{
    if (!producer_waiting_.load(std::memory_order_acquire))
        return;

    producer_waiting_.store(false, std::memory_order_relaxed);

    if (pending_handler_)
    {
        auto handler = std::move(pending_handler_);
        boost::asio::post(producer_io_,
                          [h = std::move(handler)]() mutable { h(boost::asio::error::operation_aborted); });
    }
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
size_t SpscQueue<T>::size_approx() const noexcept
{
    auto pub = published_.load(std::memory_order_relaxed);
    auto con = consumed_.load(std::memory_order_relaxed);
    return (pub >= con) ? (pub - con) : 0;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
size_t SpscQueue<T>::capacity() const noexcept
{
    return capacity_;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
bool SpscQueue<T>::empty() const noexcept
{
    return published_.load(std::memory_order_relaxed) == consumed_.load(std::memory_order_relaxed);
}

} // namespace apex::core
