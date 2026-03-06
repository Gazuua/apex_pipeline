#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <new>
#include <optional>
#include <type_traits>

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
    struct alignas(64) Slot {
        std::atomic<bool> ready{false};
        T data;
    };

    Slot* slots_;
    size_t capacity_;
    size_t mask_;  // capacity_ - 1 (power of 2)

    alignas(64) std::atomic<size_t> head_{0};  // producer CAS target
    alignas(64) size_t tail_{0};               // consumer-only, no atomic needed
};

// --- Implementation ---

namespace detail {
    constexpr size_t next_power_of_2(size_t v) {
        v--;
        v |= v >> 1; v |= v >> 2; v |= v >> 4;
        v |= v >> 8; v |= v >> 16; v |= v >> 32;
        return v + 1;
    }
} // namespace detail

template <typename T>
    requires std::is_trivially_copyable_v<T>
MpscQueue<T>::MpscQueue(size_t capacity)
    : capacity_(detail::next_power_of_2(capacity < 1 ? 1 : capacity))
    , mask_(capacity_ - 1)
{
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
        Slot& slot = slots_[head & mask_];
        if (slot.ready.load(std::memory_order_acquire)) {
            // Slot still occupied -- might be full, or head may have advanced
            size_t new_head = head_.load(std::memory_order_relaxed);
            if (new_head == head) {
                return std::unexpected(QueueError::Full);
            }
            head = new_head;
            continue;
        }

        if (head_.compare_exchange_weak(head, head + 1,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
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
    Slot& slot = slots_[tail_ & mask_];
    if (!slot.ready.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    T item = slot.data;
    slot.ready.store(false, std::memory_order_release);
    ++tail_;
    return item;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
size_t MpscQueue<T>::size_approx() const noexcept {
    size_t count = 0;
    for (size_t i = 0; i < capacity_; ++i) {
        if (slots_[i].ready.load(std::memory_order_relaxed)) ++count;
    }
    return count;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
size_t MpscQueue<T>::capacity() const noexcept {
    return capacity_;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
bool MpscQueue<T>::empty() const noexcept {
    return size_approx() == 0;
}

} // namespace apex::core
