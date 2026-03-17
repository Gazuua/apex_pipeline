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
        boost::asio::awaitable<Result<void>>(SessionPtr, uint32_t, std::span<const uint8_t>)>;

    MessageDispatcher() = default;

    void register_handler(uint32_t msg_id, Handler handler);
    void unregister_handler(uint32_t msg_id);

    /// Default handler for unmatched msg_ids.
    /// Used by proxy services (e.g., Gateway) that route all messages generically.
    void set_default_handler(Handler handler);
    void clear_default_handler();

    [[nodiscard]] boost::asio::awaitable<Result<void>>
    dispatch(SessionPtr session, uint32_t msg_id, std::span<const uint8_t> payload) const;

    [[nodiscard]] bool has_handler(uint32_t msg_id) const noexcept;
    [[nodiscard]] bool has_default_handler() const noexcept;
    [[nodiscard]] const Handler& default_handler() const noexcept { return default_handler_; }
    [[nodiscard]] size_t handler_count() const noexcept;

private:
    boost::unordered_flat_map<uint32_t, Handler> handlers_;
    Handler default_handler_;  // fallback for unmatched msg_ids
};

} // namespace apex::core
