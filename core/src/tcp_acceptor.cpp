#include <apex/core/tcp_acceptor.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace apex::core {

TcpAcceptor::TcpAcceptor(boost::asio::io_context& io_ctx, uint16_t port,
                          AcceptCallback on_accept,
                          boost::asio::ip::tcp protocol)
    : io_ctx_(io_ctx)
    , acceptor_(io_ctx, boost::asio::ip::tcp::endpoint(protocol, port))
    , on_accept_(std::move(on_accept))
{
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
}

uint16_t TcpAcceptor::port() const noexcept {
    if (!acceptor_.is_open()) return 0;
    boost::system::error_code ec;
    auto ep = acceptor_.local_endpoint(ec);
    return ec ? 0 : ep.port();
}

// C-3: 코루틴 accept 루프 — 재귀 콜백 [this] 캡처 댕글링 문제 해결.
boost::asio::awaitable<void> TcpAcceptor::accept_loop() {
    while (running_.load(std::memory_order_relaxed)) {
        auto [ec, socket] = co_await acceptor_.async_accept(
            boost::asio::as_tuple(boost::asio::use_awaitable));
        if (ec) {
            if (ec == boost::asio::error::operation_aborted) break;
            // TODO: 일시적 에러(EMFILE 등) 시 exponential backoff 추가 고려
            continue;
        }
        if (on_accept_) on_accept_(std::move(socket));
    }
}

} // namespace apex::core
