// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/cross_core_dispatcher.hpp>
#include <apex/core/scoped_logger.hpp>

#include <cassert>

namespace apex::core
{

namespace
{
const ScopedLogger& s_logger()
{
    static const ScopedLogger instance{"CrossCoreDispatcher", ScopedLogger::NO_CORE};
    return instance;
}
} // anonymous namespace

void CrossCoreDispatcher::register_handler(CrossCoreOp op, CrossCoreHandler handler)
{
    assert(handler != nullptr && "register_handler: handler must not be null");
    assert(!handlers_.contains(op) && "register_handler: duplicate registration for same op");
    s_logger().debug("register_handler op={}", static_cast<uint32_t>(op));
    handlers_.insert_or_assign(op, handler);
}

void CrossCoreDispatcher::dispatch(uint32_t core_id, uint32_t source_core, CrossCoreOp op, void* data) const
{
    auto it = handlers_.find(op);
    if (it != handlers_.end())
    {
        s_logger().trace("dispatch core={} src={} op={}", core_id, source_core, static_cast<uint32_t>(op));
        it->second(core_id, source_core, data);
    }
    else
    {
        s_logger().warn("dispatch core={} src={} op={} — no handler", core_id, source_core, static_cast<uint32_t>(op));
    }
}

bool CrossCoreDispatcher::has_handler(CrossCoreOp op) const noexcept
{
    return handlers_.contains(op);
}

} // namespace apex::core
