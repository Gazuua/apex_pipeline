// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/cross_core_dispatcher.hpp>

#include <cassert>

namespace apex::core
{

void CrossCoreDispatcher::register_handler(CrossCoreOp op, CrossCoreHandler handler)
{
    assert(handler != nullptr && "register_handler: handler must not be null");
    assert(!handlers_.contains(op) && "register_handler: duplicate registration for same op");
    handlers_.insert_or_assign(op, handler);
}

void CrossCoreDispatcher::dispatch(uint32_t core_id, uint32_t source_core, CrossCoreOp op, void* data) const
{
    auto it = handlers_.find(op);
    if (it != handlers_.end())
    {
        it->second(core_id, source_core, data);
    }
}

bool CrossCoreDispatcher::has_handler(CrossCoreOp op) const noexcept
{
    return handlers_.contains(op);
}

} // namespace apex::core
