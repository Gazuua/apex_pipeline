// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/core_allocator.hpp>
#include <apex/core/scoped_logger.hpp>

#include <cstddef>
#include <cstdlib>
#include <new>

namespace apex::core
{

/// Bump (linear) allocator for request/coroutine lifetime temporary data.
///
/// Single contiguous chunk allocated on construction. Allocation advances a
/// cursor forward; individual deallocation is not supported. reset() restores
/// the cursor to the beginning, reclaiming all memory in O(1).
///
/// Overflow returns nullptr (no auto-grow).
///
/// Satisfies CoreAllocator and Resettable concepts, but NOT Freeable.
class BumpAllocator
{
  public:
    explicit BumpAllocator(std::size_t capacity);
    ~BumpAllocator();

    BumpAllocator(const BumpAllocator&) = delete;
    BumpAllocator& operator=(const BumpAllocator&) = delete;

    BumpAllocator(BumpAllocator&& other) noexcept;
    BumpAllocator& operator=(BumpAllocator&& other) noexcept;

    /// Allocate size bytes with the given alignment.
    /// @return Aligned pointer, or nullptr on overflow or size==0.
    [[nodiscard]] void* allocate(std::size_t size, std::size_t align = alignof(std::max_align_t));

    /// Reset cursor to base — reclaim all memory in O(1).
    void reset() noexcept;

    /// Check if ptr falls within this allocator's chunk.
    [[nodiscard]] bool owns(void* ptr) const noexcept;

    /// Number of bytes currently in use (cursor - base).
    [[nodiscard]] std::size_t used_bytes() const noexcept;

    /// Total capacity in bytes.
    [[nodiscard]] std::size_t capacity() const noexcept;

    /// Free and re-allocate the backing memory on the current thread's NUMA node.
    /// Must be called when the allocator is empty (used_bytes() == 0).
    /// Used by CoreEngine::run_core() after set_mempolicy() to ensure NUMA locality.
    void rebind_memory();

  private:
    ScopedLogger logger_{"BumpAllocator", ScopedLogger::NO_CORE};
    char* base_{nullptr};
    char* cursor_{nullptr};
    char* end_{nullptr};
};

} // namespace apex::core
