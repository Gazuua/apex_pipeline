#include <apex/core/tcp_acceptor.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace apex::core {

TcpAcceptor::TcpAcceptor(boost::asio::io_context& io_ctx, uint16_t port,
                          AcceptCallback on_accept,
                          boost::asio::ip::tcp protocol)
    : io_ctx_(io_ctx)
    , acceptor_(io_ctx, boost::asio::ip::tcp::endpoint(protocol, port))
    , backoff_timer_(io_ctx)
    , on_accept_(std::move(on_accept))
{
}

TcpAcceptor::~TcpAcceptor() { stop(); }

void TcpAcceptor::set_context_provider(ContextProvider provider) {
    context_provider_ = std::move(provider);
}

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
        // I-3: Accept with branching only on context_provider_ presence,
        // unified error handling and callback invocation below.
        auto [ec, socket] = context_provider_
            ? co_await acceptor_.async_accept(
                  context_provider_(),
                  boost::asio::as_tuple(boost::asio::use_awaitable))
            : co_await acceptor_.async_accept(
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
