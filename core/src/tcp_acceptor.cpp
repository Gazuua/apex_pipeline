#include <apex/core/tcp_acceptor.hpp>

namespace apex::core {

TcpAcceptor::TcpAcceptor(boost::asio::io_context& io_ctx, uint16_t port,
                          AcceptCallback on_accept)
    : acceptor_(io_ctx, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port))
    , on_accept_(std::move(on_accept))
{
}

TcpAcceptor::~TcpAcceptor() { stop(); }

void TcpAcceptor::start() {
    if (running_) return;
    running_ = true;
    do_accept();
}

void TcpAcceptor::stop() {
    if (!running_) return;
    running_ = false;
    boost::system::error_code ec;
    acceptor_.close(ec);
}

uint16_t TcpAcceptor::port() const noexcept {
    if (!acceptor_.is_open()) return 0;
    boost::system::error_code ec;
    auto ep = acceptor_.local_endpoint(ec);
    return ec ? 0 : ep.port();
}

void TcpAcceptor::do_accept() {
    if (!running_) return;
    acceptor_.async_accept([this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
        if (ec || !running_) return;
        if (on_accept_) on_accept_(std::move(socket));
        do_accept();
    });
}

} // namespace apex::core
