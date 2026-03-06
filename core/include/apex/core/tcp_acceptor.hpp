#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <cstdint>
#include <functional>

namespace apex::core {

/// 재사용 가능한 비동기 TCP accept 루프.
class TcpAcceptor {
public:
    using AcceptCallback = std::function<void(boost::asio::ip::tcp::socket)>;

    TcpAcceptor(boost::asio::io_context& io_ctx, uint16_t port,
                AcceptCallback on_accept);
    ~TcpAcceptor();

    TcpAcceptor(const TcpAcceptor&) = delete;
    TcpAcceptor& operator=(const TcpAcceptor&) = delete;

    /// 비동기 accept 루프 시작.
    void start();

    /// accept 루프 중단.
    void stop();

    /// 바인딩된 실제 포트 (port=0 전달 시 OS 할당).
    [[nodiscard]] uint16_t port() const noexcept;

    /// 실행 중 여부.
    [[nodiscard]] bool running() const noexcept { return running_; }

private:
    void do_accept();

    boost::asio::ip::tcp::acceptor acceptor_;
    AcceptCallback on_accept_;
    bool running_{false};
};

} // namespace apex::core
