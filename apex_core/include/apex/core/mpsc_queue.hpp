// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>

#include <apex/core/detail/math_utils.hpp>
#include <apex/core/result.hpp>

namespace apex::core
{

/// Lock-free bounded MPSC (Multi-Producer, Single-Consumer) queue.
/// Designed for inter-core communication in shared-nothing architecture.
///
/// - Multiple producers can enqueue concurrently (lock-free via CAS).
/// - Single consumer dequeues (wait-free).
/// - Fixed capacity set at construction time.
/// - Cache-line aligned to prevent false sharing.
///
/// Template parameter T must be trivially copyable for lock-free guarantees.
///
/// ## Ordering guarantee
///
/// Per-producer FIFO only. Global FIFO is NOT guaranteed in multi-producer
/// scenarios. Two producers P1 and P2 may reserve slots s1 < s2 respectively,
/// but P2 may complete its write (ready=true) before P1, making P2's item
/// dequeue-able first — once P1's slot becomes ready.
///
/// ## Head-of-Line (HOL) blocking
///
/// A slow producer holding a reserved slot (head advanced, ready still false)
/// stalls all subsequent dequeues, because the single consumer processes slots
/// strictly in tail order. This is a structural property of bounded MPSC queues
/// with in-order consumption.
///
/// ## Thread safety invariant (MPSC only — NOT safe for MPMC)
///
/// The ready flag on each slot acts as a per-slot publication barrier:
/// - Producer: writes data, then sets ready=true (release).
/// - Consumer: reads ready (acquire), then reads data.
/// Consumer never observes an incomplete write because it only reads slots
/// where ready==true. Slot reuse is safe because the consumer resets
/// ready=false and advances tail before the slot index can be reached again
/// by a new producer (requires head to advance by capacity).
///
/// WARNING: This queue is NOT safe for multiple consumers (MPMC). An MPMC
/// variant would require a sequence counter per slot instead of a bool ready
/// flag, plus CAS-based tail advancement.
template <typename T>
    requires std::is_trivially_copyable_v<T>
class alignas(64) MpscQueue
{
  public:
    /// Constructs a queue with the given maximum capacity.
    /// @param capacity Must be > 0. Rounded up to next power of 2 internally.
    explicit MpscQueue(size_t capacity);

    ~MpscQueue();

    // Non-copyable, non-movable (aligned, owns memory)
    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;
    MpscQueue(MpscQueue&&) = delete;
    MpscQueue& operator=(MpscQueue&&) = delete;

    /// Thread-safe. Lock-free. Called by any producer core.
    /// @return ErrorCode::BufferFull if queue is at capacity (backpressure).
    [[nodiscard]] Result<void> enqueue(const T& item);

    /// NOT thread-safe. Called only by the owning consumer core.
    /// @return std::nullopt if queue is empty.
    [[nodiscard]] std::optional<T> dequeue();

    /// Thread-safe. Approximate count (may be stale).
    [[nodiscard]] size_t size_approx() const noexcept;

    /// Thread-safe.
    [[nodiscard]] size_t capacity() const noexcept;

    /// Thread-safe. Approximate check.
    [[nodiscard]] bool empty() const noexcept;

  private:
    // NOTE: Adjacent slots may share a cache line when T is small.
    // For CoreMessage (~16 bytes), sizeof(Slot) ≈ 17-24 bytes, meaning
    // ~3-4 slots fit in one 64-byte cache line. This is acceptable because
    // the current drain_interval (>15ms) makes producer-consumer contention
    // rare. If sub-millisecond drain intervals are ever needed, consider
    // alignas(64) padding per slot (at the cost of 3-4x memory).
    struct Slot
    {
        std::atomic<bool> ready{false};
        T data;
    };

    // --- Cache-line isolation for false-sharing prevention ---
    // Producer-only: CAS 대상. 클래스 alignas(64)에 의해 캐시라인 선두 정렬.
    alignas(64) std::atomic<size_t> head_{0};

    // Immutable after construction — 별도 캐시라인으로 분리하여
    // producer CAS가 이 필드들의 캐시라인을 무효화하지 않도록 함.
    // capacity_/mask_를 slots_ 앞에 선언하여 초기화 리스트 순서와 일치시킴.
    alignas(64) size_t capacity_;
    size_t mask_; // capacity_ - 1 (power of 2)
    Slot* slots_;

    static_assert(sizeof(size_t) >= 8, "MpscQueue requires 64-bit size_t to prevent index overflow");

    // Consumer-only — 별도 캐시라인.
    alignas(64) std::atomic<size_t> tail_{0};
};

// --- Implementation ---

template <typename T>
    requires std::is_trivially_copyable_v<T>
MpscQueue<T>::MpscQueue(size_t capacity)
    : capacity_(detail::next_power_of_2(capacity < 1 ? 1 : capacity))
    , mask_(capacity_ - 1)
{
    if (capacity_ == 0)
    {
        throw std::overflow_error("MpscQueue capacity overflow in next_power_of_2");
    }
    slots_ = new Slot[capacity_];
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
MpscQueue<T>::~MpscQueue()
{
    // Note: items may legitimately remain during shutdown (e.g., backpressure
    // tests, graceful shutdown with pending cross-core messages). This is not
    // an error — trivially_copyable items need no destructor cleanup.
    delete[] slots_;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
Result<void> MpscQueue<T>::enqueue(const T& item)
{
    size_t head = head_.load(std::memory_order_relaxed);
    size_t tail = tail_.load(std::memory_order_acquire);
    for (;;)
    {
        if (head - tail >= capacity_)
        {
            return error(ErrorCode::BufferFull);
        }

        if (head_.compare_exchange_weak(head, head + 1, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            Slot& slot = slots_[head & mask_];
            slot.data = item;
            slot.ready.store(true, std::memory_order_release);
            return ok();
        }
        // CAS failed — head was updated by compare_exchange_weak.
        // Reload tail: consumer may have advanced since our last read,
        // freeing slots that would otherwise cause a spurious Full return.
        tail = tail_.load(std::memory_order_acquire);
    }
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
std::optional<T> MpscQueue<T>::dequeue()
{
    size_t tail = tail_.load(std::memory_order_relaxed);
    Slot& slot = slots_[tail & mask_];
    if (!slot.ready.load(std::memory_order_acquire))
    {
        return std::nullopt;
    }
    T item = slot.data;
    slot.ready.store(false, std::memory_order_release);
    tail_.store(tail + 1, std::memory_order_release);
    return item;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
size_t MpscQueue<T>::size_approx() const noexcept
{
    return head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_relaxed);
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
size_t MpscQueue<T>::capacity() const noexcept
{
    return capacity_;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
bool MpscQueue<T>::empty() const noexcept
{
    return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
}

} // namespace apex::core
