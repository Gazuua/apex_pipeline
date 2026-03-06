#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>

#include <apex/core/detail/math_utils.hpp>

namespace apex::core {

enum class QueueError : uint8_t {
    Full,
};

/// Lock-free bounded MPSC (Multi-Producer, Single-Consumer) queue.
/// Designed for inter-core communication in shared-nothing architecture.
///
/// - Multiple producers can enqueue concurrently (lock-free via CAS).
/// - Single consumer dequeues (wait-free).
/// - Fixed capacity set at construction time.
/// - Cache-line aligned to prevent false sharing.
///
/// Template parameter T must be trivially copyable for lock-free guarantees.
template <typename T>
    requires std::is_trivially_copyable_v<T>
class alignas(64) MpscQueue {
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
    /// @return QueueError::Full if queue is at capacity (backpressure).
    [[nodiscard]] std::expected<void, QueueError> enqueue(const T& item);

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
    struct Slot {
        std::atomic<bool> ready{false};
        T data;
    };

    // head_ is first member: class is alignas(64), so head_ is auto-aligned to 64 bytes
    std::atomic<size_t> head_{0};  // producer CAS target

    Slot* slots_;
    size_t capacity_;
    size_t mask_;  // capacity_ - 1 (power of 2)

    alignas(64) std::atomic<size_t> tail_{0};  // consumer — separate cache line
};

// --- Implementation ---

template <typename T>
    requires std::is_trivially_copyable_v<T>
MpscQueue<T>::MpscQueue(size_t capacity)
    : capacity_(detail::next_power_of_2(capacity < 1 ? 1 : capacity))
    , mask_(capacity_ - 1)
{
    if (capacity_ == 0) {
        throw std::overflow_error("MpscQueue capacity overflow in next_power_of_2");
    }
    slots_ = new Slot[capacity_];
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
MpscQueue<T>::~MpscQueue() {
    delete[] slots_;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
std::expected<void, QueueError> MpscQueue<T>::enqueue(const T& item) {
    size_t head = head_.load(std::memory_order_relaxed);
    for (;;) {
        // Accurate full check: head - tail >= capacity
        size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= capacity_) {
            return std::unexpected(QueueError::Full);
        }

        if (head_.compare_exchange_weak(head, head + 1,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            Slot& slot = slots_[head & mask_];
            slot.data = item;
            slot.ready.store(true, std::memory_order_release);
            return {};
        }
        // CAS failed, head updated by compare_exchange_weak, retry
    }
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
std::optional<T> MpscQueue<T>::dequeue() {
    size_t tail = tail_.load(std::memory_order_relaxed);
    Slot& slot = slots_[tail & mask_];
    if (!slot.ready.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    T item = slot.data;
    slot.ready.store(false, std::memory_order_release);
    tail_.store(tail + 1, std::memory_order_release);
    return item;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
size_t MpscQueue<T>::size_approx() const noexcept {
    return head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_relaxed);
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
size_t MpscQueue<T>::capacity() const noexcept {
    return capacity_;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
bool MpscQueue<T>::empty() const noexcept {
    return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
}

} // namespace apex::core
