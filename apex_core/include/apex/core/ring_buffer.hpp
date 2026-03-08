#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace apex::core {

/// Circular buffer for network receive buffering.
/// Designed for per-core use (no thread synchronization).
///
/// Key design:
/// - No memmove: data stays in place, read/write positions advance.
/// - contiguous_read() returns the largest contiguous readable span.
/// - For FlatBuffers zero-copy: if the message fits in contiguous area, no copy.
///   If it wraps around, linearize() copies to make it contiguous.
class RingBuffer {
public:
    /// @param capacity Buffer size in bytes. Rounded up to next power of 2.
    explicit RingBuffer(size_t capacity);

    ~RingBuffer();

    // Non-copyable, non-movable
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    /// Returns a contiguous writable span from the current write position
    /// up to the physical end of the buffer.
    /// On wrap-around, only the space to the end of the buffer is returned;
    /// caller must commit and call writable() again for the remaining space.
    /// Use commit_write() after writing data to advance the write position.
    [[nodiscard]] std::span<uint8_t> writable() noexcept;

    /// Advances write position by `n` bytes. Must be <= writable().size().
    void commit_write(size_t n) noexcept;

    /// Returns the largest contiguous readable span starting from read position.
    /// May be less than readable_size() if data wraps around.
    [[nodiscard]] std::span<const uint8_t> contiguous_read() const noexcept;

    /// Total number of readable bytes (may span wrap-around boundary).
    [[nodiscard]] size_t readable_size() const noexcept;

    /// Advances read position by `n` bytes. Must be <= readable_size().
    void consume(size_t n) noexcept;

    /// If the next `n` bytes are contiguous, returns a span to them directly (zero-copy).
    /// If they wrap around, copies them into an internal linearization buffer
    /// and returns a span to that buffer.
    /// @return span of exactly `n` bytes, or empty span if readable_size() < n.
    [[nodiscard]] std::span<const uint8_t> linearize(size_t n);

    /// Total buffer capacity.
    [[nodiscard]] size_t capacity() const noexcept;

    /// Available space for writing.
    [[nodiscard]] size_t writable_size() const noexcept;

    /// Reset read/write positions to start.
    void reset() noexcept;

private:
    uint8_t* buffer_;
    // linear_buf_ is managed with malloc/realloc/free (not unique_ptr) because
    // std::realloc requires raw pointers. Ownership is guaranteed by destructor + reset().
    uint8_t* linear_buf_{nullptr};     // linearization scratch buffer (allocated on first use)
    size_t capacity_;
    size_t mask_;             // capacity_ - 1
    size_t read_pos_{0};
    size_t write_pos_{0};
    size_t linear_buf_size_{0};
};

} // namespace apex::core
