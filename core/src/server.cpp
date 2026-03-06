#include <apex/core/server.hpp>
#include <apex/core/error_sender.hpp>
#include <apex/core/frame_codec.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/use_awaitable.hpp>
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

Server::~Server() {
    // 소멸자에서는 직접 정리 (post 불필요 — io_ctx가 이미 멈춘 상태)
    if (!running_) return;
    running_ = false;
    acceptor_.stop();
    tick_timer_.cancel();
    session_mgr_.for_each([](SessionPtr session) {
        session->close();
    });
    for (auto& svc : services_) {
        svc->stop();
    }
}

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

// I-5: 스레드 안전 — 내부 post 가드.
void Server::stop() {
    boost::asio::post(io_ctx_, [this]() {
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
    });
}

uint16_t Server::port() const noexcept {
    return acceptor_.port();
}

void Server::on_accept(boost::asio::ip::tcp::socket socket) {
    auto session = session_mgr_.create_session(std::move(socket));
    // create_session 내부에서 이미 Active로 전이됨
    boost::asio::co_spawn(io_ctx_, read_loop(session), boost::asio::detached);
}

// C-3: 재귀 콜백 대신 코루틴 루프 — [this] 캡처 댕글링 제거.
// C-4: process_frames 후 is_open() 재확인 — while 조건에서 자연스럽게 해결.
// C-5: RingBuffer 가득 참 시 session close + break.
boost::asio::awaitable<void> Server::read_loop(SessionPtr session) {
    std::array<uint8_t, TMP_BUF_SIZE> tmp_buf;
    while (session->is_open()) {
        auto [ec, n] = co_await session->socket().async_read_some(
            boost::asio::buffer(tmp_buf),
            boost::asio::as_tuple(boost::asio::use_awaitable));
        if (ec) break;
        if (n == 0) break;

        // C-5: RingBuffer overflow 시 session close + break
        auto& rb = session->recv_buffer();
        if (rb.writable_size() < n) {
            session->close();
            break;
        }

        // RingBuffer에 복사 (wrap-around 시 분할 복사)
        auto w = rb.writable();
        if (w.size() >= n) {
            std::memcpy(w.data(), tmp_buf.data(), n);
            rb.commit_write(n);
        } else {
            auto first = w.size();
            std::memcpy(w.data(), tmp_buf.data(), first);
            rb.commit_write(first);
            auto w2 = rb.writable();
            std::memcpy(w2.data(), tmp_buf.data() + first, n - first);
            rb.commit_write(n - first);
        }

        // 프레임 파싱 + 디스패치
        co_await process_frames(session);

        // 하트비트 리셋
        session_mgr_.touch_session(session->id());
    }
    // 세션 정리
    session_mgr_.remove_session(session->id());
}

boost::asio::awaitable<void> Server::process_frames(SessionPtr session) {
    auto& recv_buf = session->recv_buffer();

    while (auto frame = FrameCodec::try_decode(recv_buf)) {
        // SAFETY: read_loop이 co_await 중이므로 recv_buf에 새 데이터가 쓰이지 않으며,
        // linearize()가 재호출되지 않아 frame->payload span이 유효함.
        auto result = co_await dispatcher_.dispatch(
            session, frame->header.msg_id, frame->payload);

        if (!result.has_value() &&
            result.error() == DispatchError::UnknownMessage) {
            auto error_frame = ErrorSender::build_error_frame(
                frame->header.msg_id, ErrorCode::HandlerNotFound);
            co_await session->async_send_raw(error_frame);
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
