#include <apex/core/slab_pool.hpp>
#include <algorithm>
#include <cstdlib>
#include <stdexcept>

#ifdef _MSC_VER
#include <malloc.h>  // _aligned_malloc, _aligned_free
#endif

namespace apex::core {

static size_t align_up(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

SlabPool::SlabPool(size_t slot_size, size_t initial_count)
    : free_list_(nullptr)
    , slot_size_(align_up(std::max(slot_size, sizeof(FreeNode)), alignof(std::max_align_t)))
    , total_count_(0)
    , free_count_(0)
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
        free_list_ = node;
    }

    total_count_ += count;
    free_count_ += count;
}

void* SlabPool::allocate() {
    if (!free_list_) return nullptr;

    FreeNode* node = free_list_;
    free_list_ = node->next;
    --free_count_;
    return static_cast<void*>(node);
}

void SlabPool::deallocate(void* ptr) noexcept {
    if (!ptr) return;

    // Debug: verify pointer belongs to this pool
    assert(owns(ptr) && "deallocate: pointer not owned by this pool");

    auto* node = static_cast<FreeNode*>(ptr);
    node->next = free_list_;
    free_list_ = node;
    ++free_count_;
}

bool SlabPool::owns(void* ptr) const noexcept {
    auto* p = static_cast<uint8_t*>(ptr);
    for (const auto& chunk : chunks_) {
        if (p >= chunk.data && p < chunk.data + slot_size_ * chunk.count) {
            return true;
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

} // namespace apex::core
