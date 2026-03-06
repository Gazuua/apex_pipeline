#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <cstdint>
#include <functional>

namespace apex::core {

/// 재사용 가능한 비동기 TCP accept 루프 (코루틴 기반).
class TcpAcceptor {
public:
    using AcceptCallback = std::function<void(boost::asio::ip::tcp::socket)>;

    /// @param protocol  I-6: IPv4/IPv6 선택 (기본값 v4, 하위 호환).
    TcpAcceptor(boost::asio::io_context& io_ctx, uint16_t port,
                AcceptCallback on_accept,
                boost::asio::ip::tcp protocol = boost::asio::ip::tcp::v4());
    ~TcpAcceptor();

    TcpAcceptor(const TcpAcceptor&) = delete;
    TcpAcceptor& operator=(const TcpAcceptor&) = delete;

    /// 비동기 accept 루프 시작 (co_spawn).
    void start();

    /// accept 루프 중단.
    void stop();

    /// 바인딩된 실제 포트 (port=0 전달 시 OS 할당).
    [[nodiscard]] uint16_t port() const noexcept;

    /// 실행 중 여부.
    [[nodiscard]] bool running() const noexcept { return running_; }

private:
    /// C-3: 재귀 콜백 대신 코루틴 루프 — [this] 캡처 댕글링 제거.
    boost::asio::awaitable<void> accept_loop();

    boost::asio::io_context& io_ctx_;
    boost::asio::ip::tcp::acceptor acceptor_;
    AcceptCallback on_accept_;
    bool running_{false};
};

} // namespace apex::core
