#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <vector>

namespace apex::core {

/// Configuration for SlabPool auto-grow and metrics behavior.
struct SlabPoolConfig {
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
///   SlabPool pool(sizeof(MyObject), 1024);  // fixed 1024 slots
///   SlabPool pool(sizeof(MyObject), 256, {.auto_grow = true, .max_total_count = 4096});
///   void* p = pool.allocate();
///   pool.deallocate(p);
class SlabPool {
public:
    /// Fixed-size constructor (backward compatible, auto_grow=false).
    SlabPool(size_t slot_size, size_t initial_count);

    /// Extended constructor with config for auto-grow and metrics.
    SlabPool(size_t slot_size, size_t initial_count, SlabPoolConfig config);

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
    /// In Release builds, double-free is silently ignored (no-op) without modifying
    /// free_count_. This means allocated_count() + free_count() may not equal total_count()
    /// if double-free occurred.
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
    SlabPoolConfig config_;
    size_t initial_count_;   // grow_chunk_size=0 시 fallback
    size_t grow_count_{0};
    size_t peak_allocated_{0};
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

    TypedSlabPool(size_t initial_count, SlabPoolConfig config)
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
            "T destructor must be noexcept for TypedSlabPool::destroy");
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
    SlabPool pool_;
};

} // namespace apex::core
