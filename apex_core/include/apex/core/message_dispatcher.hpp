#pragma once

#include <apex/core/result.hpp>
#include <apex/core/session.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

#include <cstdint>
#include <functional>
#include <span>

namespace apex::core {

/// Message dispatcher using boost::unordered_flat_map indexed by msg_id.
/// O(1) amortized lookup. Memory proportional to registered handler count only.
/// Thread-safe for concurrent reads after setup. NOT thread-safe for concurrent read-write.
class MessageDispatcher {
public:
    /// @note payload span은 co_await 시점까지만 유효합니다.
    ///       핸들러 내에서 co_await 이후에 payload 데이터에 접근하면
    ///       댕글링 참조가 발생합니다. 데이터를 보존하려면 co_await 전에 복사하세요.
    using Handler = std::function<
        boost::asio::awaitable<Result<void>>(SessionPtr, uint16_t, std::span<const uint8_t>)>;

    MessageDispatcher() = default;

    void register_handler(uint16_t msg_id, Handler handler);
    void unregister_handler(uint16_t msg_id);

    [[nodiscard]] boost::asio::awaitable<Result<void>>
    dispatch(SessionPtr session, uint16_t msg_id, std::span<const uint8_t> payload) const;

    [[nodiscard]] bool has_handler(uint16_t msg_id) const noexcept;
    [[nodiscard]] size_t handler_count() const noexcept;

private:
    boost::unordered_flat_map<uint16_t, Handler> handlers_;
};

} // namespace apex::core
