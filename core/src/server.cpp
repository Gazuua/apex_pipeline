#include <apex/core/server.hpp>
#include <apex/core/error_sender.hpp>
#include <apex/core/frame_codec.hpp>

#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <cstring>

namespace apex::core {

Server::Server(boost::asio::io_context& io_ctx, Config config)
    : io_ctx_(io_ctx)
    , config_(config)
    , session_mgr_(0, config.heartbeat_timeout_ticks, config.timer_wheel_slots)
    , acceptor_(io_ctx, config.port, [this](boost::asio::ip::tcp::socket sock) {
          on_accept(std::move(sock));
      })
    , tick_timer_(io_ctx)
{
}

Server::~Server() { stop(); }

void Server::start() {
    if (running_) return;
    running_ = true;

    for (auto& svc : services_) {
        svc->start();
    }

    acceptor_.start();

    if (config_.heartbeat_timeout_ticks > 0) {
        start_tick_timer();
    }
}

void Server::stop() {
    if (!running_) return;
    running_ = false;

    acceptor_.stop();
    tick_timer_.cancel();

    // 모든 세션 닫기
    session_mgr_.for_each([](SessionPtr session) {
        session->close();
    });

    for (auto& svc : services_) {
        svc->stop();
    }
}

uint16_t Server::port() const noexcept {
    return acceptor_.port();
}

void Server::on_accept(boost::asio::ip::tcp::socket socket) {
    auto session = session_mgr_.create_session(std::move(socket));
    start_read(session);
}

void Server::start_read(SessionPtr session) {
    do_read(session);
}

void Server::do_read(SessionPtr session) {
    if (!running_ || !session->is_open()) return;

    auto buf = std::make_shared<std::array<uint8_t, TMP_BUF_SIZE>>();
    session->socket().async_read_some(
        boost::asio::buffer(*buf),
        [this, session, buf](boost::system::error_code ec, size_t bytes) {
            if (ec || bytes == 0) {
                session_mgr_.remove_session(session->id());
                return;
            }

            // RingBuffer에 복사
            auto& recv_buf = session->recv_buffer();
            size_t copied = 0;
            while (copied < bytes) {
                auto w = recv_buf.writable();
                if (w.empty()) break;
                size_t to_copy = std::min(w.size(), bytes - copied);
                std::memcpy(w.data(), buf->data() + copied, to_copy);
                recv_buf.commit_write(to_copy);
                copied += to_copy;
            }

            process_frames(session);

            // 하트비트 리셋
            session_mgr_.touch_session(session->id());

            // 다음 읽기
            do_read(session);
        });
}

void Server::process_frames(SessionPtr session) {
    auto& recv_buf = session->recv_buffer();

    while (auto frame = FrameCodec::try_decode(recv_buf)) {
        auto result = dispatcher_.dispatch(
            session, frame->header.msg_id, frame->payload);

        if (!result.has_value() &&
            result.error() == DispatchError::UnknownMessage) {
            auto error_frame = ErrorSender::build_error_frame(
                frame->header.msg_id, ErrorCode::HandlerNotFound);
            (void)session->send_raw(error_frame);
        }

        FrameCodec::consume_frame(recv_buf, *frame);
    }
}

void Server::start_tick_timer() {
    if (!running_) return;

    tick_timer_.expires_after(
        std::chrono::milliseconds(config_.tick_interval_ms));
    tick_timer_.async_wait([this](boost::system::error_code ec) {
        if (ec || !running_) return;
        session_mgr_.tick();
        start_tick_timer();
    });
}

} // namespace apex::core
