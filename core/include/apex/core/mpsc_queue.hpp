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

} // namespace apex::core
