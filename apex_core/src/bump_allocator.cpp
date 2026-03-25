// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/bump_allocator.hpp>

#include <cstdint>
#include <cstring>
#include <utility>

namespace apex::core
{

BumpAllocator::BumpAllocator(std::size_t capacity)
{
    if (capacity == 0)
    {
        logger_.warn("created with zero capacity — all allocations will return nullptr");
        return;
    }

    // malloc guarantees alignment suitable for any fundamental type (typically
    // 16 bytes on 64-bit). For over-aligned requests, allocate() handles
    // internal pointer alignment within the chunk.
    base_ = static_cast<char*>(std::malloc(capacity));
    if (!base_)
    {
        throw std::bad_alloc();
    }
    cursor_ = base_;
    end_ = base_ + capacity;
}

BumpAllocator::~BumpAllocator()
{
    std::free(base_);
}

BumpAllocator::BumpAllocator(BumpAllocator&& other) noexcept
    : base_(std::exchange(other.base_, nullptr))
    , cursor_(std::exchange(other.cursor_, nullptr))
    , end_(std::exchange(other.end_, nullptr))
    , logger_(std::move(other.logger_))
{}

BumpAllocator& BumpAllocator::operator=(BumpAllocator&& other) noexcept
{
    if (this != &other)
    {
        std::free(base_);
        base_ = std::exchange(other.base_, nullptr);
        cursor_ = std::exchange(other.cursor_, nullptr);
        end_ = std::exchange(other.end_, nullptr);
    }
    return *this;
}

void* BumpAllocator::allocate(std::size_t size, std::size_t align)
{
    if (size == 0)
        return nullptr;
    if (align == 0 || (align & (align - 1)) != 0)
        return nullptr;
    if (!base_)
        return nullptr; // zero-capacity allocator

    // Align cursor up to the requested alignment boundary.
    // Formula: (addr + align - 1) & ~(align - 1)
    auto addr = reinterpret_cast<std::uintptr_t>(cursor_);
    auto aligned = (addr + align - 1) & ~(align - 1);
    auto* result = reinterpret_cast<char*>(aligned);

    // Check overflow: result + size must not exceed end_.
    if (result + size > end_)
    {
        return nullptr;
    }

    cursor_ = result + size;
    return result;
}

void BumpAllocator::reset() noexcept
{
    cursor_ = base_;
}

bool BumpAllocator::owns(void* ptr) const noexcept
{
    auto* p = static_cast<char*>(ptr);
    return p >= base_ && p < end_;
}

std::size_t BumpAllocator::used_bytes() const noexcept
{
    return static_cast<std::size_t>(cursor_ - base_);
}

std::size_t BumpAllocator::capacity() const noexcept
{
    return static_cast<std::size_t>(end_ - base_);
}

} // namespace apex::core
