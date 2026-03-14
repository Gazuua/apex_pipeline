#pragma once

#include <apex/core/core_allocator.hpp>
#include <cstddef>
#include <cstdlib>
#include <vector>

namespace apex::core {

/// Arena (block-chaining + bump) allocator for transaction-lifetime data.
///
/// Allocates by bumping a cursor within the current block. When the current
/// block is exhausted, a new block is chained. reset() deallocates all blocks
/// except the first, making it ideal for request/transaction scoped allocation
/// where individual deallocation is unnecessary.
///
/// Satisfies CoreAllocator and Resettable concepts but NOT Freeable — there is
/// no per-object deallocate(). Call reset() to bulk-release all allocations.
///
/// NOT thread-safe. Designed for per-core / per-coroutine use.
class ArenaAllocator {
public:
    static constexpr std::size_t kDefaultBlockSize = 4096;
    static constexpr std::size_t kDefaultMaxBytes = 1024 * 1024;

    explicit ArenaAllocator(std::size_t block_size = kDefaultBlockSize,
                            std::size_t max_bytes = kDefaultMaxBytes);
    ~ArenaAllocator();

    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;
    ArenaAllocator(ArenaAllocator&&) noexcept;
    ArenaAllocator& operator=(ArenaAllocator&&) noexcept;

    /// Bump-allocate within the current block, chaining a new block on overflow.
    /// @param size  Allocation size in bytes. 0 returns nullptr.
    /// @param align Alignment requirement (must be power of 2).
    /// @return Aligned pointer, or nullptr if size==0 or max_bytes exceeded.
    [[nodiscard]] void* allocate(std::size_t size,
                                 std::size_t align = alignof(std::max_align_t));

    /// Bulk-deallocate: free all blocks except the first, reset first block's cursor.
    void reset() noexcept;

    /// Check if ptr falls within any owned block.
    [[nodiscard]] bool owns(void* ptr) const noexcept;

    /// Total bytes consumed by user allocations (including alignment padding).
    [[nodiscard]] std::size_t used_bytes() const noexcept;

    /// Total bytes allocated from the system (sum of all block sizes).
    [[nodiscard]] std::size_t capacity() const noexcept;

private:
    struct Block {
        char* base;
        char* cursor;
        char* end;
        std::size_t size;
    };

    Block make_block(std::size_t size);
    void free_block(Block& block);

    std::vector<Block> blocks_;
    std::size_t block_size_;
    std::size_t max_bytes_;
    std::size_t total_allocated_{0};
};

} // namespace apex::core
