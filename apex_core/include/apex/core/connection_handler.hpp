#pragma once

#include <apex/core/message_dispatcher.hpp>
#include <apex/core/session.hpp>
#include <apex/core/session_manager.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <cstdint>

namespace apex::core {

/// Configuration for ConnectionHandler behavior.
struct ConnectionHandlerConfig {
    bool tcp_nodelay = true;
};

/// Handles the lifecycle of a single TCP connection:
/// read_loop (recv -> parse -> dispatch) and process_frames.
///
/// Extracted from Server to separate connection handling from server lifecycle.
/// Each ConnectionHandler is bound to a single core's PerCoreState.
class ConnectionHandler {
public:
    ConnectionHandler(SessionManager& session_mgr,
                      MessageDispatcher& dispatcher,
                      ConnectionHandlerConfig config);

    /// Accept a new connection -- create session + spawn read_loop.
    /// Must be called on the owning core's io_context thread.
    void accept_connection(boost::asio::ip::tcp::socket socket,
                           boost::asio::io_context& io_ctx);

    /// Number of currently active read_loop coroutines.
    [[nodiscard]] uint32_t active_sessions() const noexcept {
        return active_sessions_.load(std::memory_order_acquire);
    }

private:
    boost::asio::awaitable<void> read_loop(SessionPtr session);
    boost::asio::awaitable<void> process_frames(SessionPtr session);

    SessionManager& session_mgr_;
    MessageDispatcher& dispatcher_;
    ConnectionHandlerConfig config_;
    std::atomic<uint32_t> active_sessions_{0};
};

} // namespace apex::core
