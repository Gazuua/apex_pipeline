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
    : chunk_(nullptr)
    , free_list_(nullptr)
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
#ifdef _MSC_VER
    _aligned_free(chunk_);
#else
    std::free(chunk_);
#endif
}

void SlabPool::grow(size_t count) {
    constexpr size_t kAlignment = 64;  // cache-line alignment

#ifdef _MSC_VER
    chunk_ = static_cast<uint8_t*>(_aligned_malloc(slot_size_ * count, kAlignment));
#else
    // std::aligned_alloc requires size to be a multiple of alignment
    size_t alloc_size = align_up(slot_size_ * count, kAlignment);
    chunk_ = static_cast<uint8_t*>(std::aligned_alloc(kAlignment, alloc_size));
#endif

    if (!chunk_) {
        throw std::bad_alloc();
    }

    // Build free-list from back to front so first allocate returns first slot
    for (size_t i = count; i > 0; --i) {
        auto* node = reinterpret_cast<FreeNode*>(chunk_ + (i - 1) * slot_size_);
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
    auto* node = static_cast<FreeNode*>(ptr);
    node->next = free_list_;
    free_list_ = node;
    ++free_count_;
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
