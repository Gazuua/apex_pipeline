// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <boost/asio/cancellation_signal.hpp>

#include <atomic>
#include <cassert>
#include <memory>
#include <thread>
#include <vector>

namespace apex::shared::adapters
{

/// Per-core cancellation token for adapter coroutines.
/// Tracks outstanding coroutines and provides bulk cancellation.
///
/// Thread safety: all methods except outstanding() must be called from the owning core thread.
/// Debug builds enforce this with thread-id assertions.
///
/// Slot lifecycle: slots_ grows monotonically (no cleanup on completion).
/// This is intentional — adapter coroutine count is tiny (2-3 per multiplexer, ~24 total for 8 cores).
/// If future adapters spawn many coroutines, add slot pooling.
class CancellationToken
{
  public:
    CancellationToken() = default;
    ~CancellationToken() = default;

    // Non-copyable (std::atomic is non-copyable)
    CancellationToken(const CancellationToken&) = delete;
    CancellationToken& operator=(const CancellationToken&) = delete;

    // Movable (needed for vector storage)
    CancellationToken(CancellationToken&& other) noexcept
        : slots_(std::move(other.slots_))
        , outstanding_(other.outstanding_.load(std::memory_order_relaxed))
#ifndef NDEBUG
        , owner_thread_(other.owner_thread_)
#endif
    {}
    CancellationToken& operator=(CancellationToken&& other) noexcept
    {
        if (this != &other)
        {
            slots_ = std::move(other.slots_);
            outstanding_.store(other.outstanding_.load(std::memory_order_relaxed), std::memory_order_relaxed);
#ifndef NDEBUG
            owner_thread_ = other.owner_thread_;
#endif
        }
        return *this;
    }

    /// Allocates a new cancellation slot and increments outstanding counter.
    /// Returns the slot for binding to co_spawn via bind_cancellation_slot.
    [[nodiscard]] boost::asio::cancellation_slot new_slot();

    /// Emits terminal cancellation to all active slots.
    void cancel_all();

    /// Called by coroutine guard on completion (normal, cancelled, or exception).
    void on_complete();

    /// Returns current outstanding count. Thread-safe (atomic read).
    [[nodiscard]] uint32_t outstanding() const noexcept;

  private:
    struct Slot
    {
        boost::asio::cancellation_signal signal;
    };

    std::vector<std::unique_ptr<Slot>> slots_;
    std::atomic<uint32_t> outstanding_{0};

#ifndef NDEBUG
    std::thread::id owner_thread_{};
    void assert_owner_thread();
#else
    void assert_owner_thread() {}
#endif
};

} // namespace apex::shared::adapters
