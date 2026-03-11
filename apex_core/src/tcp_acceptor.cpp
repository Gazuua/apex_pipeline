#include <apex/core/tcp_acceptor.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#if !defined(_WIN32)
#include <sys/socket.h>  // SOL_SOCKET, SO_REUSEPORT
#endif

namespace apex::core {

TcpAcceptor::TcpAcceptor(boost::asio::io_context& io_ctx, uint16_t port,
                          AcceptCallback on_accept,
                          boost::asio::ip::tcp protocol,
                          bool reuseport)
    : io_ctx_(io_ctx)
    , acceptor_(io_ctx)
    , backoff_timer_(io_ctx)
    , on_accept_(std::move(on_accept))
    , reuseport_(reuseport)
{
    acceptor_.open(protocol);
    acceptor_.set_option(boost::asio::socket_base::reuse_address(true));

#if !defined(_WIN32) && defined(SO_REUSEPORT)
    if (reuseport_) {
        using reuseport_option = boost::asio::detail::socket_option::boolean<
            SOL_SOCKET, SO_REUSEPORT>;
        acceptor_.set_option(reuseport_option(true));
    }
#endif

    acceptor_.bind(boost::asio::ip::tcp::endpoint(protocol, port));
    acceptor_.listen();
}

TcpAcceptor::~TcpAcceptor() { stop(); }

void TcpAcceptor::start() {
    if (running_.exchange(true)) return;
    boost::asio::co_spawn(io_ctx_, accept_loop(), boost::asio::detached);
}

void TcpAcceptor::stop() {
    if (!running_.exchange(false)) return;
    boost::system::error_code ec;
    acceptor_.close(ec);
    backoff_timer_.cancel();  // Cancel pending backoff timer for immediate exit
}

uint16_t TcpAcceptor::port() const noexcept {
    if (!acceptor_.is_open()) return 0;
    boost::system::error_code ec;
    auto ep = acceptor_.local_endpoint(ec);
    return ec ? 0 : ep.port();
}

boost::asio::awaitable<void> TcpAcceptor::accept_loop() {
    while (running_.load(std::memory_order_relaxed)) {
        // C-01: Always accept on the acceptor's own io_context. The
        // on_accept callback is responsible for moving the socket to the
        // target core's io_context via post(), ensuring a single atomic
        // operation determines the core assignment (no TOCTOU).
        auto [ec, socket] = co_await acceptor_.async_accept(
            boost::asio::as_tuple(boost::asio::use_awaitable));

        if (ec) {
            if (ec == boost::asio::error::operation_aborted) break;
            backoff_timer_.expires_after(std::chrono::milliseconds(100));
            auto [ec_timer] = co_await backoff_timer_.async_wait(
                boost::asio::as_tuple(boost::asio::use_awaitable));
            continue;
        }

        if (on_accept_) on_accept_(std::move(socket));
    }
}

} // namespace apex::core
