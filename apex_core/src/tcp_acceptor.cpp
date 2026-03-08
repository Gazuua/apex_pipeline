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

void TcpAcceptor::start() {
    if (running_.exchange(true)) return;
    boost::asio::co_spawn(io_ctx_, accept_loop(), boost::asio::detached);
}

void TcpAcceptor::stop() {
    if (!running_.exchange(false)) return;
    boost::system::error_code ec;
    acceptor_.close(ec);
    backoff_timer_.cancel();  // I-2: 대기 중인 백오프 타이머 취소 → 코루틴 즉시 재개/종료
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
            // I-6: 일시적 에러 시 100ms 백오프 (EMFILE 등 busy-loop 방지)
            // I-2: 멤버 backoff_timer_ 사용 — stop()에서 cancel 가능
            backoff_timer_.expires_after(std::chrono::milliseconds(100));
            auto [ec_timer] = co_await backoff_timer_.async_wait(
                boost::asio::as_tuple(boost::asio::use_awaitable));
            // timer cancel 시에도 예외 없이 안전하게 loop 재진입
            continue;
        }

        if (on_accept_) on_accept_(std::move(socket));
    }
}

} // namespace apex::core
