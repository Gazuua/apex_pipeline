#include <apex/core/arena_allocator.hpp>
#include <algorithm>
#include <cstdlib>
#include <utility>

namespace apex::core {

ArenaAllocator::ArenaAllocator(std::size_t block_size, std::size_t max_bytes)
    : block_size_(block_size), max_bytes_(max_bytes) {
    blocks_.push_back(make_block(block_size_));
    total_allocated_ = block_size_;
}

ArenaAllocator::~ArenaAllocator() {
    for (auto& block : blocks_) {
        free_block(block);
    }
}

ArenaAllocator::ArenaAllocator(ArenaAllocator&& other) noexcept
    : blocks_(std::move(other.blocks_))
    , block_size_(other.block_size_)
    , max_bytes_(other.max_bytes_)
    , total_allocated_(other.total_allocated_) {
    other.total_allocated_ = 0;
}

ArenaAllocator& ArenaAllocator::operator=(ArenaAllocator&& other) noexcept {
    if (this != &other) {
        for (auto& block : blocks_) free_block(block);
        blocks_ = std::move(other.blocks_);
        block_size_ = other.block_size_;
        max_bytes_ = other.max_bytes_;
        total_allocated_ = other.total_allocated_;
        other.total_allocated_ = 0;
    }
    return *this;
}

void* ArenaAllocator::allocate(std::size_t size, std::size_t align) {
    if (size == 0) return nullptr;
    if (align == 0 || (align & (align - 1)) != 0) return nullptr;

    // Try current block first.
    auto& current = blocks_.back();
    auto addr = reinterpret_cast<std::uintptr_t>(current.cursor);
    auto aligned = (addr + align - 1) & ~(align - 1);
    auto* result = reinterpret_cast<char*>(aligned);
    auto* new_cursor = result + size;

    if (new_cursor <= current.end) {
        current.cursor = new_cursor;
        return result;
    }

    // Need a new block. Size must fit the allocation + worst-case alignment padding.
    // Alignment padding is at most (align - 1) bytes.
    auto needed = size + (align > 1 ? align - 1 : 0);
    auto new_block_size = std::max(block_size_, needed);
    if (total_allocated_ + new_block_size > max_bytes_) {
        return nullptr;  // max_bytes limit exceeded
    }

    blocks_.push_back(make_block(new_block_size));
    total_allocated_ += new_block_size;

    auto& fresh = blocks_.back();
    addr = reinterpret_cast<std::uintptr_t>(fresh.cursor);
    aligned = (addr + align - 1) & ~(align - 1);
    result = reinterpret_cast<char*>(aligned);
    fresh.cursor = result + size;
    return result;
}

void ArenaAllocator::reset() noexcept {
    if (blocks_.empty()) return;  // moved-from guard

    // Keep only the first block, free the rest.
    for (std::size_t i = 1; i < blocks_.size(); ++i) {
        free_block(blocks_[i]);
    }
    blocks_.resize(1);
    blocks_[0].cursor = blocks_[0].base;
    total_allocated_ = blocks_[0].size;
}

bool ArenaAllocator::owns(void* ptr) const noexcept {
    auto* p = static_cast<char*>(ptr);
    for (const auto& block : blocks_) {
        if (p >= block.base && p < block.end) return true;
    }
    return false;
}

std::size_t ArenaAllocator::used_bytes() const noexcept {
    std::size_t total = 0;
    for (const auto& block : blocks_) {
        total += static_cast<std::size_t>(block.cursor - block.base);
    }
    return total;
}

std::size_t ArenaAllocator::capacity() const noexcept {
    return total_allocated_;
}

ArenaAllocator::Block ArenaAllocator::make_block(std::size_t size) {
    auto* base = static_cast<char*>(std::malloc(size));
    if (!base) {
        throw std::bad_alloc();
    }
    return {base, base, base + size, size};
}

void ArenaAllocator::free_block(Block& block) {
    std::free(block.base);
    block.base = nullptr;
}

} // namespace apex::core
