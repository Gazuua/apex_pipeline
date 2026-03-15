#include <apex/core/session.hpp>
#include <apex/core/slab_allocator.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#include <spdlog/spdlog.h>

namespace apex::core {

void intrusive_ptr_release(Session* s) noexcept {
    assert(s->refcount_ > 0 && "Session refcount underflow");
    if (--s->refcount_ == 0) {
        if (s->pool_owner_ != nullptr) {
            s->pool_owner_->destroy(s);
        } else {
            delete s;  // NOLINT(cppcoreguidelines-owning-memory)
        }
    }
}

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

// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
awaitable<Result<void>> Session::async_send(const WireHeader& header,
                                            std::span<const uint8_t> payload) {
    if (!is_open()) co_return error(ErrorCode::SessionClosed);

    std::array<uint8_t, WireHeader::SIZE> hdr_buf{};
    header.serialize(hdr_buf);

    std::array<boost::asio::const_buffer, 2> buffers{
        boost::asio::buffer(hdr_buf),
        boost::asio::buffer(payload.data(), payload.size())
    };

    auto [ec, bytes_sent] = co_await boost::asio::async_write(
        socket_, buffers,
        boost::asio::as_tuple(boost::asio::use_awaitable));
    (void)bytes_sent;

    if (ec) {
        close();
        co_return error(ErrorCode::SendFailed);
    }
    co_return ok();
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
awaitable<Result<void>> Session::async_send_raw(std::span<const uint8_t> data) {
    if (!is_open()) co_return error(ErrorCode::SessionClosed);

    auto [ec, bytes_sent] = co_await boost::asio::async_write(
        socket_, boost::asio::buffer(data.data(), data.size()),
        boost::asio::as_tuple(boost::asio::use_awaitable));
    (void)bytes_sent;

    if (ec) {
        close();
        co_return error(ErrorCode::SendFailed);
    }
    co_return ok();
}

void Session::close() noexcept {
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

// --- Write Queue (v0.5) ---

Result<void> Session::enqueue_write(std::vector<uint8_t> data) {
    if (!is_open()) return error(ErrorCode::SessionClosed);
    if (write_queue_.size() >= max_queue_depth_) {
        SPDLOG_WARN("Session {} write queue full (depth={})", id_, max_queue_depth_);
        return error(ErrorCode::BufferFull);
    }
    write_queue_.push_back(WriteRequest{std::move(data)});
    if (!pump_running_) {
        pump_running_ = true;
        // UAF 방어: co_spawn된 write_pump 코루틴이 실행되는 동안
        // Session이 소멸되지 않도록 intrusive_ptr로 refcount 증가.
        // 람다가 self를 캡처하여 코루틴 완료까지 생존 보장.
        SessionPtr self(this);
        boost::asio::co_spawn(socket_.get_executor(),
            [self]() { return self->write_pump(); },
            boost::asio::detached);
    }
    return ok();
}

Result<void> Session::enqueue_write_raw(std::span<const uint8_t> data) {
    std::vector<uint8_t> copy(data.begin(), data.end());
    return enqueue_write(std::move(copy));
}

awaitable<void> Session::write_pump() {
    while (!write_queue_.empty() && is_open()) {
        auto& front = write_queue_.front();
        auto [ec, bytes_written] = co_await boost::asio::async_write(
            socket_,
            boost::asio::buffer(front.data),
            boost::asio::as_tuple(boost::asio::use_awaitable));
        (void)bytes_written;

        write_queue_.pop_front();

        if (ec) {
            write_queue_.clear();  // 에러 시 잔여 큐 정리
            close();
            break;
        }
    }
    pump_running_ = false;
}

} // namespace apex::core
