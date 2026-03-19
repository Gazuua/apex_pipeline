#pragma once

#include <apex/core/cross_core_op.hpp>

#include <boost/unordered/unordered_flat_map.hpp>

#include <cstdint>

namespace apex::core
{

/// Dispatches cross-core messages by CrossCoreOp → handler lookup.
/// Handlers are function pointers for static dispatch (no virtual, icache friendly).
/// Thread-safe for concurrent reads after setup (register before start).
class CrossCoreDispatcher
{
  public:
    CrossCoreDispatcher() = default;

    void register_handler(CrossCoreOp op, CrossCoreHandler handler);

    /// Dispatch a message. No-op if handler not registered.
    void dispatch(uint32_t core_id, uint32_t source_core, CrossCoreOp op, void* data) const;

    [[nodiscard]] bool has_handler(CrossCoreOp op) const noexcept;

  private:
    boost::unordered_flat_map<CrossCoreOp, CrossCoreHandler> handlers_;
};

} // namespace apex::core
