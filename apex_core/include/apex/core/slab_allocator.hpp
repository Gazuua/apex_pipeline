#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <vector>

namespace apex::core {

/// Configuration for SlabAllocator auto-grow and metrics behavior.
struct SlabAllocatorConfig {
    bool auto_grow = false;      ///< true: allocate() grows pool on exhaustion
    size_t grow_chunk_size = 0;  ///< 0: use initial_count as grow size
    size_t max_total_count = 0;  ///< 0: unlimited growth
};

/// Fixed-size slab memory pool for O(1) allocation/deallocation.
/// Designed for per-core use (no thread synchronization).
///
/// Memory is pre-allocated in chunks. Each chunk contains N slots of
/// fixed size. Free slots are tracked via an intrusive free-list.
///
/// Supports optional auto-grow: when exhausted, allocate() grows the pool
/// by grow_chunk_size slots (up to max_total_count). Default is fixed-size.
///
/// Usage:
///   SlabAllocator pool(sizeof(MyObject), 1024);  // fixed 1024 slots
///   SlabAllocator pool(sizeof(MyObject), 256, {.auto_grow = true, .max_total_count = 4096});
///   void* p = pool.allocate();
///   pool.deallocate(p);
class SlabAllocator {
public:
    /// Fixed-size constructor (backward compatible, auto_grow=false).
    SlabAllocator(size_t slot_size, size_t initial_count);

    /// Extended constructor with config for auto-grow and metrics.
    SlabAllocator(size_t slot_size, size_t initial_count, SlabAllocatorConfig config);

    ~SlabAllocator();

    // Non-copyable, non-movable
    SlabAllocator(const SlabAllocator&) = delete;
    SlabAllocator& operator=(const SlabAllocator&) = delete;
    SlabAllocator(SlabAllocator&&) = delete;
    SlabAllocator& operator=(SlabAllocator&&) = delete;

    /// O(1) allocation from the free-list. NOT thread-safe (per-core use).
    /// @return nullptr if pool is exhausted.
    [[nodiscard]] void* allocate();

    /// CoreAllocator concept 충족용 overload.
    /// size > slot_size_ 이면 nullptr 반환. align은 생성 시 보장되므로 무시.
    [[nodiscard]] void* allocate(std::size_t size, std::size_t align);

    /// O(1) deallocation. Returns the slot to the free-list. NOT thread-safe.
    /// @pre ptr은 반드시 이 풀에서 allocate()로 할당된 포인터여야 한다.
    /// Double-free is detected via magic marker and increments double_free_count_.
    /// In both Debug and Release builds, double-free is safely handled (no corruption).
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

    /// Number of times the pool has auto-grown (0 = never grown).
    [[nodiscard]] size_t grow_count() const noexcept;

    /// Peak number of simultaneously allocated slots (high water mark).
    [[nodiscard]] size_t peak_allocated() const noexcept;

    /// Current allocated bytes = allocated_count() * slot_size_.
    [[nodiscard]] std::size_t used_bytes() const noexcept;

    /// Total capacity bytes = total_count() * slot_size_.
    [[nodiscard]] std::size_t capacity() const noexcept;

    /// Number of double-free attempts detected (best-effort lower bound).
    [[nodiscard]] uint64_t double_free_count() const noexcept { return double_free_count_; }

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
    SlabAllocatorConfig config_;
    size_t initial_count_;   // grow_chunk_size=0 시 fallback
    size_t grow_count_{0};
    size_t peak_allocated_{0};
    uint64_t double_free_count_{0};
};

/// Typed wrapper around SlabAllocator for type-safe allocation.
///
/// NOTE: Double-free detection relies on a magic value overlaid on freed slots.
/// If T's first sizeof(FreeNode) bytes overwrite this region during use,
/// detection becomes best-effort.
///
/// Usage:
///   TypedSlabAllocator<Session> pool(1024);
///   Session* s = pool.construct(args...);
///   pool.destroy(s);
template <typename T>
class TypedSlabAllocator {
    static_assert(alignof(T) <= alignof(std::max_align_t),
        "TypedSlabAllocator does not support over-aligned types. "
        "Use SlabAllocator directly with custom alignment.");

public:
    explicit TypedSlabAllocator(size_t initial_count)
        : pool_(sizeof(T), initial_count) {}

    TypedSlabAllocator(size_t initial_count, SlabAllocatorConfig config)
        : pool_(sizeof(T), initial_count, config) {}

    template <typename... Args>
    [[nodiscard]] T* construct(Args&&... args) {
        void* p = pool_.allocate();
        if (!p) return nullptr;
        try {
            return new (p) T(std::forward<Args>(args)...);
        } catch (...) {
            pool_.deallocate(p);
            throw;
        }
    }

    void destroy(T* ptr) noexcept {
        static_assert(std::is_nothrow_destructible_v<T>,
            "T destructor must be noexcept for TypedSlabAllocator::destroy");
        if (ptr) {
            ptr->~T();
            pool_.deallocate(ptr);
        }
    }

    [[nodiscard]] size_t allocated_count() const noexcept { return pool_.allocated_count(); }
    [[nodiscard]] size_t free_count() const noexcept { return pool_.free_count(); }
    [[nodiscard]] size_t grow_count() const noexcept { return pool_.grow_count(); }
    [[nodiscard]] size_t peak_allocated() const noexcept { return pool_.peak_allocated(); }

private:
    SlabAllocator pool_;
};

} // namespace apex::core
