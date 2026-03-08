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

    /// Provides a target io_context for each accepted socket. When set,
    /// async_accept binds the new socket directly to the returned io_context's
    /// IOCP/epoll, avoiding the default binding to the acceptor's io_context.
    using ContextProvider = std::function<boost::asio::io_context&()>;

    /// @param protocol  IPv4/IPv6 selection (default v4, backward compatible).
    TcpAcceptor(boost::asio::io_context& io_ctx, uint16_t port,
                AcceptCallback on_accept,
                boost::asio::ip::tcp protocol = boost::asio::ip::tcp::v4());
    ~TcpAcceptor();

    TcpAcceptor(const TcpAcceptor&) = delete;
    TcpAcceptor& operator=(const TcpAcceptor&) = delete;

    /// Set a context provider for IOCP-correct socket binding.
    /// Must be called before start(). When set, each accepted socket is
    /// bound to the io_context returned by the provider (e.g., round-robin
    /// across per-core io_contexts), instead of the acceptor's own io_context.
    void set_context_provider(ContextProvider provider);

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
    ContextProvider context_provider_;
    std::atomic<bool> running_{false};
};

} // namespace apex::core
