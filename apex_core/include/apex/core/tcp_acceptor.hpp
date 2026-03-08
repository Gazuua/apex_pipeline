#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <cstdint>
#include <functional>

namespace apex::core {

/// Reusable asynchronous TCP accept loop (coroutine-based).
class TcpAcceptor {
public:
    using AcceptCallback = std::function<void(boost::asio::ip::tcp::socket)>;

    /// @param protocol  IPv4/IPv6 selection (default v4, backward compatible).
    TcpAcceptor(boost::asio::io_context& io_ctx, uint16_t port,
                AcceptCallback on_accept,
                boost::asio::ip::tcp protocol = boost::asio::ip::tcp::v4());
    ~TcpAcceptor();

    TcpAcceptor(const TcpAcceptor&) = delete;
    TcpAcceptor& operator=(const TcpAcceptor&) = delete;

    /// Start the async accept loop (co_spawn).
    void start();

    /// Stop the accept loop.
    void stop();

    /// Actual bound port (useful when port=0 was passed for OS assignment).
    [[nodiscard]] uint16_t port() const noexcept;

    /// Whether the accept loop is running.
    [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_relaxed); }

private:
    /// Coroutine accept loop — replaces recursive callback to avoid
    /// [this] capture dangling.
    boost::asio::awaitable<void> accept_loop();

    boost::asio::io_context& io_ctx_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::steady_timer backoff_timer_;  // Member — stop() can cancel
    AcceptCallback on_accept_;
    std::atomic<bool> running_{false};
};

} // namespace apex::core
