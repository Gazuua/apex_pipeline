#include <apex/core/cross_core_dispatcher.hpp>

namespace apex::core {

void CrossCoreDispatcher::register_handler(CrossCoreOp op, CrossCoreHandler handler) {
    handlers_.insert_or_assign(op, handler);
}

void CrossCoreDispatcher::dispatch(uint32_t core_id, uint32_t source_core,
                                   CrossCoreOp op, void* data) const {
    auto it = handlers_.find(op);
    if (it != handlers_.end()) {
        it->second(core_id, source_core, data);
    }
}

bool CrossCoreDispatcher::has_handler(CrossCoreOp op) const noexcept {
    return handlers_.contains(op);
}

} // namespace apex::core
