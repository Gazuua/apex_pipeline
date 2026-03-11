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
