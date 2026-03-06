#include <apex/core/session.hpp>

#include <boost/asio/write.hpp>

#include <cstring>

namespace apex::core {

Session::Session(SessionId id, boost::asio::ip::tcp::socket socket,
                 uint32_t core_id, size_t recv_buf_capacity)
    : id_(id)
    , core_id_(core_id)
    , socket_(std::move(socket))
    , recv_buf_(recv_buf_capacity)
{
}

Session::~Session() {
    if (socket_.is_open()) {
        boost::system::error_code ec;
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }
}

bool Session::send(const WireHeader& header,
                   std::span<const uint8_t> payload) {
    if (!is_open()) return false;

    std::vector<uint8_t> frame(header.frame_size());
    auto hdr_bytes = header.serialize();
    std::memcpy(frame.data(), hdr_bytes.data(), WireHeader::SIZE);
    if (!payload.empty()) {
        std::memcpy(frame.data() + WireHeader::SIZE,
                    payload.data(), payload.size());
    }

    boost::system::error_code ec;
    boost::asio::write(socket_, boost::asio::buffer(frame), ec);
    if (ec) {
        close();
        return false;
    }
    return true;
}

bool Session::send_raw(std::span<const uint8_t> data) {
    if (!is_open()) return false;

    boost::system::error_code ec;
    boost::asio::write(socket_, boost::asio::buffer(data.data(), data.size()), ec);
    if (ec) {
        close();
        return false;
    }
    return true;
}

void Session::close() {
    if (state_ == State::Closed) return;
    state_ = State::Closed;

    boost::system::error_code ec;
    if (socket_.is_open()) {
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }
}

} // namespace apex::core
