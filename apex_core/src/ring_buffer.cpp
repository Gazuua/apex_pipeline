#include <apex/core/ring_buffer.hpp>
#include <apex/core/detail/math_utils.hpp>
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <new>
#include <stdexcept>

#ifdef _MSC_VER
#include <malloc.h>
#endif

namespace apex::core {

RingBuffer::RingBuffer(size_t capacity)
    : capacity_(detail::next_power_of_2(capacity < 1 ? 1 : capacity))
    , mask_(capacity_ - 1)
{
    if (capacity_ == 0) {
        throw std::overflow_error("RingBuffer capacity overflow in next_power_of_2");
    }
#ifdef _MSC_VER
    buffer_ = static_cast<uint8_t*>(_aligned_malloc(capacity_, 64));
#else
    buffer_ = static_cast<uint8_t*>(std::aligned_alloc(64, capacity_));
#endif
    if (!buffer_) throw std::bad_alloc();
}

RingBuffer::~RingBuffer() {
#ifdef _MSC_VER
    _aligned_free(buffer_);
#else
    std::free(buffer_);
#endif
    // linear_buf_ is allocated via std::realloc, so std::free is the correct pair
    // (not _aligned_free, which pairs with _aligned_malloc for buffer_)
    std::free(linear_buf_);
}

std::span<uint8_t> RingBuffer::writable() noexcept {
    size_t avail = writable_size();
    if (avail == 0) return {};

    size_t w = write_pos_ & mask_;
    size_t to_end = capacity_ - w;
    return {buffer_ + w, std::min(to_end, avail)};
}

void RingBuffer::commit_write(size_t n) noexcept {
    n = std::min(n, writable_size());
    write_pos_ += n;
}

std::span<const uint8_t> RingBuffer::contiguous_read() const noexcept {
    size_t avail = readable_size();
    if (avail == 0) return {};

    size_t r = read_pos_ & mask_;
    size_t to_end = capacity_ - r;
    return {buffer_ + r, std::min(to_end, avail)};
}

size_t RingBuffer::readable_size() const noexcept {
    return write_pos_ - read_pos_;
}

void RingBuffer::consume(size_t n) noexcept {
    n = std::min(n, readable_size());
    read_pos_ += n;
}

std::span<const uint8_t> RingBuffer::linearize(size_t n) {
    if (readable_size() < n) return {};

    size_t r = read_pos_ & mask_;
    size_t to_end = capacity_ - r;

    // Data is contiguous — zero-copy path
    if (to_end >= n) {
        return {buffer_ + r, n};
    }

    // Wrap-around — copy into linear scratch buffer
    if (linear_buf_size_ < n) {
        auto* new_buf = static_cast<uint8_t*>(std::realloc(linear_buf_, n));
        if (!new_buf) {
            // linear_buf_ preserved on realloc failure
            return {};
        }
        linear_buf_ = new_buf;
        linear_buf_size_ = n;
    }

    std::memcpy(linear_buf_, buffer_ + r, to_end);
    std::memcpy(linear_buf_ + to_end, buffer_, n - to_end);

    return {linear_buf_, n};
}

bool RingBuffer::write(std::span<const uint8_t> data) noexcept {
    if (data.empty()) return true;
    if (data.size() > writable_size()) return false;
    auto w = writable();
    if (w.size() >= data.size()) {
        std::memcpy(w.data(), data.data(), data.size());
        commit_write(data.size());
    } else {
        auto first = w.size();
        std::memcpy(w.data(), data.data(), first);
        commit_write(first);
        auto w2 = writable();
        std::memcpy(w2.data(), data.data() + first, data.size() - first);
        commit_write(data.size() - first);
    }
    return true;
}

size_t RingBuffer::capacity() const noexcept {
    return capacity_;
}

size_t RingBuffer::writable_size() const noexcept {
    return capacity_ - readable_size();
}

void RingBuffer::reset() noexcept {
    read_pos_ = 0;
    write_pos_ = 0;
    std::free(linear_buf_);
    linear_buf_ = nullptr;
    linear_buf_size_ = 0;
}

} // namespace apex::core
