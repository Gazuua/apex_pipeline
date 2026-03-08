#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <vector>

namespace apex::core {

/// Fixed-size slab memory pool for O(1) allocation/deallocation.
/// Designed for per-core use (no thread synchronization).
///
/// Memory is pre-allocated in chunks. Each chunk contains N slots of
/// fixed size. Free slots are tracked via an intrusive free-list.
///
/// Usage:
///   SlabPool pool(sizeof(MyObject), 1024);  // 1024 slots
///   void* p = pool.allocate();
///   pool.deallocate(p);
class SlabPool {
public:
    /// @param slot_size Size of each slot in bytes (will be aligned up to alignof(max_align_t)).
    /// @param initial_count Number of slots to pre-allocate.
    SlabPool(size_t slot_size, size_t initial_count);

    ~SlabPool();

    // Non-copyable, non-movable
    SlabPool(const SlabPool&) = delete;
    SlabPool& operator=(const SlabPool&) = delete;
    SlabPool(SlabPool&&) = delete;
    SlabPool& operator=(SlabPool&&) = delete;

    /// O(1) allocation from the free-list. NOT thread-safe (per-core use).
    /// @return nullptr if pool is exhausted.
    [[nodiscard]] void* allocate();

    /// O(1) deallocation. Returns the slot to the free-list. NOT thread-safe.
    /// @pre ptr은 반드시 이 풀에서 allocate()로 할당된 포인터여야 한다.
    /// @warning Release 빌드에서는 소유권 검증이 비활성화됨 (assert 기반).
    ///          double-free 시 정의되지 않은 동작.
    void deallocate(void* ptr) noexcept;

    /// Number of currently allocated (in-use) slots.
    [[nodiscard]] size_t allocated_count() const noexcept;

    /// Number of free slots available.
    [[nodiscard]] size_t free_count() const noexcept;

    /// Total capacity (allocated + free).
    [[nodiscard]] size_t total_count() const noexcept;

    /// Size of each slot in bytes (after alignment).
    [[nodiscard]] size_t slot_size() const noexcept;

    /// Check if a pointer belongs to this pool and is slot-aligned.
    [[nodiscard]] bool owns(void* ptr) const noexcept;

private:
    struct FreeNode {
        FreeNode* next;
        uint32_t magic;  // double-free 감지용 마커
    };

    void grow(size_t count);

    struct ChunkInfo {
        uint8_t* data;
        size_t count;  // number of slots in this chunk
    };

    std::vector<ChunkInfo> chunks_;  // all allocated memory blocks
    FreeNode* free_list_;            // head of free-list
    size_t slot_size_;       // aligned slot size
    size_t total_count_;     // total slots ever created
    size_t free_count_;      // current free slots
};

/// Typed wrapper around SlabPool for type-safe allocation.
///
/// NOTE: Double-free detection relies on a magic value overlaid on freed slots.
/// If T's first sizeof(FreeNode) bytes overwrite this region during use,
/// detection becomes best-effort.
///
/// Usage:
///   TypedSlabPool<Session> pool(1024);
///   Session* s = pool.construct(args...);
///   pool.destroy(s);
template <typename T>
class TypedSlabPool {
    static_assert(alignof(T) <= alignof(std::max_align_t),
        "TypedSlabPool does not support over-aligned types. "
        "Use SlabPool directly with custom alignment.");

public:
    explicit TypedSlabPool(size_t initial_count)
        : pool_(sizeof(T), initial_count) {}

    template <typename... Args>
    [[nodiscard]] T* construct(Args&&... args) {
        void* p = pool_.allocate();
        if (!p) return nullptr;
        return new (p) T(std::forward<Args>(args)...);
    }

    void destroy(T* ptr) noexcept {
        static_assert(std::is_nothrow_destructible_v<T>,
            "T destructor must be noexcept for TypedSlabPool::destroy");
        if (ptr) {
            ptr->~T();
            pool_.deallocate(ptr);
        }
    }

    [[nodiscard]] size_t allocated_count() const noexcept { return pool_.allocated_count(); }
    [[nodiscard]] size_t free_count() const noexcept { return pool_.free_count(); }

private:
    SlabPool pool_;
};

} // namespace apex::core
