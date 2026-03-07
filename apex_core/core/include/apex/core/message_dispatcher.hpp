#pragma once

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
/// Designed for per-service use. NOT thread-safe (per-core, single-thread).
class MessageDispatcher {
public:
    // 핸들러가 코루틴을 반환 (방안 A 핵심)
    using Handler = std::function<
        boost::asio::awaitable<void>(SessionPtr, uint16_t, std::span<const uint8_t>)>;

    MessageDispatcher() = default;

    void register_handler(uint16_t msg_id, Handler handler);
    void unregister_handler(uint16_t msg_id);

    // dispatch도 코루틴 (내부에서 co_await handler 호출)
    [[nodiscard]] boost::asio::awaitable<std::expected<void, DispatchError>>
    dispatch(SessionPtr session, uint16_t msg_id, std::span<const uint8_t> payload) const;

    [[nodiscard]] bool has_handler(uint16_t msg_id) const noexcept;
    [[nodiscard]] size_t handler_count() const noexcept;

private:
    std::unique_ptr<std::array<Handler, 65536>> handlers_
        = std::make_unique<std::array<Handler, 65536>>();
    size_t handler_count_{0};
};

} // namespace apex::core
