#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>

namespace apex::core {

/// Base class for immutable, refcounted cross-core payloads.
/// Subclass to add concrete data fields.
///
/// Usage pattern:
///   auto* p = new ConcretePayload{...};
///   p->set_refcount(N);          // broadcast: N receivers
///   engine.post_to(core, msg);   // each receiver calls release() after processing
///
/// Thread safety: refcount operations are atomic. Payload data is immutable
/// after construction — no synchronization needed for reads.
class SharedPayload {
public:
    SharedPayload() = default;
    virtual ~SharedPayload() = default;

    SharedPayload(const SharedPayload&) = delete;
    SharedPayload& operator=(const SharedPayload&) = delete;

    /// Increment refcount (for point-to-point: call once after new).
    void add_ref() noexcept {
        refcount_.fetch_add(1, std::memory_order_relaxed);
    }

    /// Set refcount directly (for broadcast: set to receiver count).
    void set_refcount(uint32_t count) noexcept {
        refcount_.store(count, std::memory_order_relaxed);
    }

    /// Decrement refcount. Deletes this when refcount reaches 0.
    void release() noexcept {
        auto prev = refcount_.fetch_sub(1, std::memory_order_acq_rel);
        assert(prev > 0 && "SharedPayload::release() called with refcount 0");
        if (prev == 1) {
            delete this;
        }
    }

    [[nodiscard]] uint32_t refcount() const noexcept {
        return refcount_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<uint32_t> refcount_{0};
};

} // namespace apex::core
