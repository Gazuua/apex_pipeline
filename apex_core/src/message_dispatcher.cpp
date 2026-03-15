#include <apex/core/message_dispatcher.hpp>

#include <spdlog/spdlog.h>

namespace apex::core {

void MessageDispatcher::register_handler(uint32_t msg_id, Handler handler) {
    handlers_.insert_or_assign(msg_id, std::move(handler));
}

void MessageDispatcher::unregister_handler(uint32_t msg_id) {
    handlers_.erase(msg_id);
}

boost::asio::awaitable<Result<void>>
MessageDispatcher::dispatch(SessionPtr session, uint32_t msg_id, std::span<const uint8_t> payload) const {
    auto it = handlers_.find(msg_id);
    if (it == handlers_.end()) {
        co_return error(ErrorCode::HandlerNotFound);
    }
    try {
        co_return co_await it->second(std::move(session), msg_id, payload);
    } catch (const std::exception& e) {
        if (auto logger = spdlog::get("apex")) {
            logger->error("MessageDispatcher: handler for msg_id 0x{:08x} threw: {}",
                static_cast<unsigned>(msg_id), e.what());
        }
        co_return error(ErrorCode::HandlerException);
    } catch (...) {
        if (auto logger = spdlog::get("apex")) {
            logger->error("MessageDispatcher: handler for msg_id 0x{:08x} threw unknown exception",
                static_cast<unsigned>(msg_id));
        }
        co_return error(ErrorCode::HandlerException);
    }
}

bool MessageDispatcher::has_handler(uint32_t msg_id) const noexcept {
    return handlers_.contains(msg_id);
}

size_t MessageDispatcher::handler_count() const noexcept {
    return handlers_.size();
}

} // namespace apex::core
