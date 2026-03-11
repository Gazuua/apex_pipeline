#include <apex/core/message_dispatcher.hpp>

#include <spdlog/spdlog.h>

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

boost::asio::awaitable<Result<void>>
MessageDispatcher::dispatch(SessionPtr session, uint16_t msg_id, std::span<const uint8_t> payload) const {
    auto& handler = (*handlers_)[msg_id];
    if (!handler) {
        co_return error(ErrorCode::HandlerNotFound);
    }
    try {
        co_return co_await handler(std::move(session), msg_id, payload);
    } catch (const std::exception& e) {
        if (auto logger = spdlog::get("apex")) {
            logger->error("MessageDispatcher: handler for msg_id 0x{:04x} threw: {}",
                static_cast<unsigned>(msg_id), e.what());
        }
        co_return error(ErrorCode::HandlerException);
    } catch (...) {
        if (auto logger = spdlog::get("apex")) {
            logger->error("MessageDispatcher: handler for msg_id 0x{:04x} threw unknown exception",
                static_cast<unsigned>(msg_id));
        }
        co_return error(ErrorCode::HandlerException);
    }
}

bool MessageDispatcher::has_handler(uint16_t msg_id) const noexcept {
    return static_cast<bool>((*handlers_)[msg_id]);
}

size_t MessageDispatcher::handler_count() const noexcept {
    return handler_count_;
}

} // namespace apex::core
