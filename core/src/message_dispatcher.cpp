#include <apex/core/message_dispatcher.hpp>

namespace apex::core {

void MessageDispatcher::register_handler(uint16_t msg_id, Handler handler) {
    if (!(*handlers_)[msg_id]) {
        ++handler_count_;
    }
    (*handlers_)[msg_id] = std::move(handler);
}

void MessageDispatcher::unregister_handler(uint16_t msg_id) {
    if ((*handlers_)[msg_id]) {
        (*handlers_)[msg_id] = nullptr;
        --handler_count_;
    }
}

std::expected<void, DispatchError>
MessageDispatcher::dispatch(SessionPtr session, uint16_t msg_id, std::span<const uint8_t> payload) const {
    if (!(*handlers_)[msg_id]) {
        return std::unexpected(DispatchError::UnknownMessage);
    }
    try {
        (*handlers_)[msg_id](session, msg_id, payload);
    } catch (...) {
        return std::unexpected(DispatchError::HandlerFailed);
    }
    return {};
}

bool MessageDispatcher::has_handler(uint16_t msg_id) const noexcept {
    return static_cast<bool>((*handlers_)[msg_id]);
}

size_t MessageDispatcher::handler_count() const noexcept {
    return handler_count_;
}

} // namespace apex::core
