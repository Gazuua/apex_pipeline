// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/message_dispatcher.hpp>

namespace apex::core
{

void MessageDispatcher::register_handler(uint32_t msg_id, Handler handler)
{
    handlers_.insert_or_assign(msg_id, std::move(handler));
}

void MessageDispatcher::unregister_handler(uint32_t msg_id)
{
    handlers_.erase(msg_id);
}

void MessageDispatcher::set_default_handler(Handler handler)
{
    default_handler_ = std::move(handler);
}

void MessageDispatcher::clear_default_handler()
{
    default_handler_ = nullptr;
}

boost::asio::awaitable<Result<void>> MessageDispatcher::dispatch(SessionPtr session, uint32_t msg_id,
                                                                 std::span<const uint8_t> payload) const
{
    logger_.trace("dispatch msg_id=0x{:04X} payload_size={}", msg_id, payload.size());

    auto it = handlers_.find(msg_id);
    if (it == handlers_.end())
    {
        if (default_handler_)
        {
            metric_dispatched_.fetch_add(1, std::memory_order_relaxed);
            try
            {
                co_return co_await default_handler_(std::move(session), msg_id, payload);
            }
            catch (const std::exception& e)
            {
                metric_exceptions_.fetch_add(1, std::memory_order_relaxed);
                logger_.error("default handler for msg_id 0x{:08x} threw: {}", static_cast<unsigned>(msg_id), e.what());
                co_return error(ErrorCode::HandlerException);
            }
            catch (...)
            {
                metric_exceptions_.fetch_add(1, std::memory_order_relaxed);
                logger_.error("default handler for msg_id 0x{:08x} threw unknown exception",
                              static_cast<unsigned>(msg_id));
                co_return error(ErrorCode::HandlerException);
            }
        }
        co_return error(ErrorCode::HandlerNotFound);
    }
    metric_dispatched_.fetch_add(1, std::memory_order_relaxed);
    try
    {
        co_return co_await it->second(std::move(session), msg_id, payload);
    }
    catch (const std::exception& e)
    {
        metric_exceptions_.fetch_add(1, std::memory_order_relaxed);
        logger_.error("handler for msg_id 0x{:08x} threw: {}", static_cast<unsigned>(msg_id), e.what());
        co_return error(ErrorCode::HandlerException);
    }
    catch (...)
    {
        metric_exceptions_.fetch_add(1, std::memory_order_relaxed);
        logger_.error("handler for msg_id 0x{:08x} threw unknown exception", static_cast<unsigned>(msg_id));
        co_return error(ErrorCode::HandlerException);
    }
}

bool MessageDispatcher::has_handler(uint32_t msg_id) const noexcept
{
    return handlers_.contains(msg_id);
}

bool MessageDispatcher::has_default_handler() const noexcept
{
    return static_cast<bool>(default_handler_);
}

size_t MessageDispatcher::handler_count() const noexcept
{
    return handlers_.size();
}

} // namespace apex::core
