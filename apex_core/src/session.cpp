#include <apex/core/session.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

namespace apex::core {

using boost::asio::awaitable;

Session::Session(SessionId id, boost::asio::ip::tcp::socket socket,
                 uint32_t core_id, size_t recv_buf_capacity)
    : id_(id)
    , core_id_(core_id)
    , socket_(std::move(socket))
    , recv_buf_(recv_buf_capacity)
{
}

// M-2: Simplified — close() already checks Closed state and is idempotent
Session::~Session() {
    close();
}

awaitable<Result<void>> Session::async_send(const WireHeader& header,
                                            std::span<const uint8_t> payload) {
    if (!is_open()) co_return error(ErrorCode::SessionClosed);

    std::array<uint8_t, WireHeader::SIZE> hdr_buf{};
    header.serialize(hdr_buf);

    std::array<boost::asio::const_buffer, 2> buffers{
        boost::asio::buffer(hdr_buf),
        boost::asio::buffer(payload.data(), payload.size())
    };

    auto [ec, bytes] = co_await boost::asio::async_write(
        socket_, buffers,
        boost::asio::as_tuple(boost::asio::use_awaitable));

    if (ec) {
        close();
        co_return error(ErrorCode::SendFailed);
    }
    co_return ok();
}

awaitable<Result<void>> Session::async_send_raw(std::span<const uint8_t> data) {
    if (!is_open()) co_return error(ErrorCode::SessionClosed);

    auto [ec, bytes] = co_await boost::asio::async_write(
        socket_, boost::asio::buffer(data.data(), data.size()),
        boost::asio::as_tuple(boost::asio::use_awaitable));

    if (ec) {
        close();
        co_return error(ErrorCode::SendFailed);
    }
    co_return ok();
}

void Session::close() {
    if (state_ == State::Closed) return;
    // Bypass set_state() which asserts on Closed->Closed transition
    // (already guarded by early return above)
    state_ = State::Closed;

    boost::system::error_code ec;
    if (socket_.is_open()) {
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }
}

} // namespace apex::core
