#pragma once

#include <apex/core/result.hpp>
#include <apex/core/session.hpp>

#include <boost/asio/awaitable.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>

namespace apex::core {

enum class DispatchError : uint8_t {
    UnknownMessage,
    HandlerFailed,
};

/// O(1) message dispatcher using std::array indexed by msg_id (uint16_t).
/// Designed for per-service use. Thread-safe for concurrent reads after setup
/// (핸들러 등록은 on_start() 전에만). NOT thread-safe for concurrent read-write.
class MessageDispatcher {
public:
    // 핸들러가 Result<void>를 반환하는 코루틴
    using Handler = std::function<
        boost::asio::awaitable<Result<void>>(SessionPtr, uint16_t, std::span<const uint8_t>)>;

    MessageDispatcher() = default;

    void register_handler(uint16_t msg_id, Handler handler);
    void unregister_handler(uint16_t msg_id);

    // dispatch도 코루틴 (내부에서 co_await handler 호출)
    // 핸들러 에러(ErrorCode)가 Result에 포함되어 전달됨
    [[nodiscard]] boost::asio::awaitable<std::expected<Result<void>, DispatchError>>
    dispatch(SessionPtr session, uint16_t msg_id, std::span<const uint8_t> payload) const;

    [[nodiscard]] bool has_handler(uint16_t msg_id) const noexcept;
    [[nodiscard]] size_t handler_count() const noexcept;

private:
    std::unique_ptr<std::array<Handler, 65536>> handlers_
        = std::make_unique<std::array<Handler, 65536>>();
    size_t handler_count_{0};
};

} // namespace apex::core
