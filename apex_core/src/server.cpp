#include <apex/core/server.hpp>
#include <apex/core/error_sender.hpp>
#include <apex/core/frame_codec.hpp>
#include <apex/core/tcp_binary_protocol.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

namespace apex::core {

Server::Server(boost::asio::io_context& io_ctx, Config config)
    : io_ctx_(io_ctx)
    , config_(config)
    , session_mgr_(0, config.heartbeat_timeout_ticks,
                   config.timer_wheel_slots, config.recv_buf_capacity)
    , acceptor_(io_ctx, config.port, [this](boost::asio::ip::tcp::socket sock) {
          on_accept(std::move(sock));
      })
    , tick_timer_(io_ctx)
{
    // I-4: 런타임 체크 — assert 대신 throw (생성자, cold path)
    if (config_.recv_buf_capacity < TMP_BUF_SIZE) {
        throw std::invalid_argument(
            "ServerConfig::recv_buf_capacity must be >= " +
            std::to_string(TMP_BUF_SIZE));
    }
}

Server::~Server() {
    // 소멸자에서는 직접 정리 (post 불필요 — io_ctx가 이미 멈춘 상태)
    if (!running_.load(std::memory_order_relaxed)) return;
    running_.store(false, std::memory_order_relaxed);
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
    if (running_.exchange(true)) return;

    for (auto& svc : services_) {
        svc->start();
    }

    acceptor_.start();

    if (config_.heartbeat_timeout_ticks > 0) {
        start_tick_timer();
    }
}

// NOTE: post된 핸들러가 실행되기 전 Server가 소멸하면 UB.
// 전제 조건: io_ctx.run()이 완료된 후에만 Server를 소멸시킬 것.
// I-5: 스레드 안전 — 내부 post 가드.
void Server::stop() {
    boost::asio::post(io_ctx_, [this]() {
        if (!running_.exchange(false)) return;

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

    while (auto frame = TcpBinaryProtocol::try_decode(recv_buf)) {
        // SAFETY: 단일 io_context 스레드에서 read_loop -> process_frames가 순차 실행되므로,
        // co_await 중에도 이 세션의 recv_buf에 새 데이터가 쓰이지 않는다.
        // 따라서 linearize() 결과인 frame->payload span은 consume_frame() 전까지 유효하다.
        auto result = co_await dispatcher_.dispatch(
            session, frame->header.msg_id, frame->payload);

        if (!result.has_value()) {
            // DispatchError (UnknownMessage, HandlerFailed)
            ErrorCode code = (result.error() == DispatchError::UnknownMessage)
                ? ErrorCode::HandlerNotFound
                : ErrorCode::Unknown;
            auto error_frame = ErrorSender::build_error_frame(
                frame->header.msg_id, code);
            co_await session->async_send_raw(error_frame);
        } else if (!result.value().has_value()) {
            // 핸들러가 apex::error(ErrorCode::X)를 반환
            auto error_frame = ErrorSender::build_error_frame(
                frame->header.msg_id, result.value().error());
            co_await session->async_send_raw(error_frame);
        }

        TcpBinaryProtocol::consume_frame(recv_buf, *frame);

        // I-5: ErrorResponse 전송 실패 등으로 세션이 close된 경우 추가 프레임 처리 중단
        if (!session->is_open()) break;
    }
}

// NOTE: [this] 캡처 — Server가 tick_timer_보다 오래 살아야 함.
// 소멸자에서 tick_timer_.cancel()로 보장됨.
void Server::start_tick_timer() {
    if (!running_.load(std::memory_order_relaxed)) return;

    tick_timer_.expires_after(
        std::chrono::milliseconds(config_.tick_interval_ms));
    tick_timer_.async_wait([this](boost::system::error_code ec) {
        if (ec || !running_.load(std::memory_order_relaxed)) return;
        session_mgr_.tick();
        start_tick_timer();
    });
}

} // namespace apex::core
