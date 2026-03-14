#include <apex/core/slab_pool.hpp>
#include <algorithm>
#include <cstdlib>
#include <stdexcept>

#ifdef _MSC_VER
#include <malloc.h>  // _aligned_malloc, _aligned_free
#endif

namespace apex::core {

// 슬롯 상태 마커 (allocated/freed 구분)
static constexpr uint32_t SLAB_MAGIC_ALLOCATED = 0xA110CA7E;
static constexpr uint32_t SLAB_MAGIC_FREED     = 0xF2EED000;

static size_t align_up(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

SlabPool::SlabPool(size_t slot_size, size_t initial_count)
    : SlabPool(slot_size, initial_count, SlabPoolConfig{})
{
}

SlabPool::SlabPool(size_t slot_size, size_t initial_count, SlabPoolConfig config)
    : free_list_(nullptr)
    , slot_size_(align_up(std::max(slot_size, sizeof(FreeNode)), alignof(std::max_align_t)))
    , total_count_(0)
    , free_count_(0)
    , config_(config)
    , initial_count_(initial_count)
{
    if (initial_count == 0) {
        throw std::invalid_argument("SlabPool: initial_count must be > 0");
    }
    grow(initial_count);
}

SlabPool::~SlabPool() {
    for (auto& chunk : chunks_) {
#ifdef _MSC_VER
        _aligned_free(chunk.data);
#else
        std::free(chunk.data);
#endif
    }
}

void SlabPool::grow(size_t count) {
    constexpr size_t kAlignment = 64;  // cache-line alignment

    // Overflow check for slot_size_ * count
    if (count > 0 && slot_size_ > SIZE_MAX / count) {
        throw std::bad_alloc();
    }

    uint8_t* chunk = nullptr;
#ifdef _MSC_VER
    chunk = static_cast<uint8_t*>(_aligned_malloc(slot_size_ * count, kAlignment));
#else
    // std::aligned_alloc requires size to be a multiple of alignment
    size_t alloc_size = align_up(slot_size_ * count, kAlignment);
    chunk = static_cast<uint8_t*>(std::aligned_alloc(kAlignment, alloc_size));
#endif

    if (!chunk) {
        throw std::bad_alloc();
    }

    chunks_.push_back({chunk, count});

    // Build free-list from back to front so first allocate returns first slot
    for (size_t i = count; i > 0; --i) {
        auto* node = reinterpret_cast<FreeNode*>(chunk + (i - 1) * slot_size_);
        node->next = free_list_;
        node->magic = SLAB_MAGIC_FREED;
        free_list_ = node;
    }

    total_count_ += count;
    free_count_ += count;
}

void* SlabPool::allocate() {
    if (!free_list_) {
        if (!config_.auto_grow) return nullptr;

        // max_total check
        if (config_.max_total_count > 0 && total_count_ >= config_.max_total_count) {
            return nullptr;
        }

        size_t chunk = config_.grow_chunk_size > 0
            ? config_.grow_chunk_size
            : initial_count_;

        // Clamp to max_total
        if (config_.max_total_count > 0) {
            chunk = std::min(chunk, config_.max_total_count - total_count_);
        }

        if (chunk == 0) return nullptr;
        grow(chunk);
        ++grow_count_;
    }

    FreeNode* node = free_list_;
    free_list_ = node->next;
    --free_count_;
    node->magic = SLAB_MAGIC_ALLOCATED;

    // Peak tracking
    size_t current = allocated_count();
    if (current > peak_allocated_) {
        peak_allocated_ = current;
    }

    return static_cast<void*>(node);
}

void SlabPool::deallocate(void* ptr) noexcept {
    if (!ptr) return;

    // Debug: verify pointer belongs to this pool
    assert(owns(ptr) && "deallocate: pointer not owned by this pool");

    auto* node = static_cast<FreeNode*>(ptr);

    // NOTE: Magic-based detection is best-effort. If the user overwrites the
    // entire slot (sizeof(T) >= sizeof(FreeNode)), the magic value is destroyed
    // and double-free detection becomes ineffective in Release builds.

    // Double-free 감지: magic이 FREED이면 이미 반환된 슬롯
    if (node->magic == SLAB_MAGIC_FREED) {
        assert(false && "deallocate: double-free detected");
        return;  // Release에서는 silent return으로 corruption 방지
    }

    node->magic = SLAB_MAGIC_FREED;
    node->next = free_list_;
    free_list_ = node;
    ++free_count_;
}

bool SlabPool::owns(void* ptr) const noexcept {
    // Linear scan over chunks. O(1) when pool has a single chunk (typical case).
    // With auto-grow, chunk count grows slowly (e.g., 4→8→12→16 = 4 chunks).
    auto* p = static_cast<uint8_t*>(ptr);
    for (const auto& chunk : chunks_) {
        if (p >= chunk.data && p < chunk.data + slot_size_ * chunk.count) {
            // 슬롯 정렬 검증: 포인터가 슬롯 경계에 정확히 맞아야 함
            size_t offset = static_cast<size_t>(p - chunk.data);
            return (offset % slot_size_) == 0;
        }
    }
    return false;
}

size_t SlabPool::allocated_count() const noexcept {
    return total_count_ - free_count_;
}

size_t SlabPool::free_count() const noexcept {
    return free_count_;
}

size_t SlabPool::total_count() const noexcept {
    return total_count_;
}

size_t SlabPool::slot_size() const noexcept {
    return slot_size_;
}

size_t SlabPool::grow_count() const noexcept {
    return grow_count_;
}

size_t SlabPool::peak_allocated() const noexcept {
    return peak_allocated_;
}

} // namespace apex::core
