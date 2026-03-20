// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/error_code.hpp>
#include <apex/core/error_sender.hpp>
#include <apex/core/message_dispatcher.hpp>
#include <apex/core/protocol.hpp>
#include <apex/core/session.hpp>
#include <apex/core/session_manager.hpp>
#include <apex/core/transport.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

namespace apex::core
{

/// Configuration for ConnectionHandler behavior.
struct ConnectionHandlerConfig
{
    bool tcp_nodelay = true;
};

/// Handles the lifecycle of a single TCP connection:
/// read_loop (recv -> parse -> dispatch) and process_frames.
///
/// Templated on Protocol concept for zero-overhead dispatch.
/// Each ConnectionHandler<P> is bound to a single core's PerCoreState.
template <Protocol P, Transport T = DefaultTransport> class ConnectionHandler
{
  public:
    ConnectionHandler(SessionManager& session_mgr, MessageDispatcher& dispatcher, ConnectionHandlerConfig config)
        : session_mgr_(session_mgr)
        , dispatcher_(dispatcher)
        , config_(config)
        , logger_(spdlog::default_logger())
    {}

    /// Accept a new connection -- create session + spawn read_loop.
    /// Must be called on the owning core's io_context thread.
    void accept_connection(boost::asio::ip::tcp::socket socket, boost::asio::io_context& io_ctx)
    {
        if (config_.tcp_nodelay)
        {
            boost::system::error_code ec;
            socket.set_option(boost::asio::ip::tcp::no_delay(true), ec);
        }

        auto session = session_mgr_.create_session(std::move(socket));
        boost::asio::co_spawn(io_ctx, read_loop(std::move(session)), boost::asio::detached);
    }

    /// Number of currently active read_loop coroutines.
    [[nodiscard]] uint32_t active_sessions() const noexcept
    {
        return active_sessions_.load(std::memory_order_acquire);
    }

  private:
    boost::asio::awaitable<void> read_loop(SessionPtr session)
    {
        struct ActiveSessionGuard
        {
            std::atomic<uint32_t>& counter;
            ~ActiveSessionGuard()
            {
                counter.fetch_sub(1, std::memory_order_release);
            }
        };
        active_sessions_.fetch_add(1, std::memory_order_relaxed);
        ActiveSessionGuard guard{active_sessions_};

        while (session->is_open())
        {
            auto& rb = session->recv_buffer();
            auto writable = rb.writable();
            if (writable.empty())
            {
                if (logger_)
                    logger_->warn("session {} recv_buffer full — closing connection", session->id());
                session->close();
                break;
            }

            auto [ec, n] =
                co_await session->socket().async_read_some(boost::asio::buffer(writable.data(), writable.size()),
                                                           boost::asio::as_tuple(boost::asio::use_awaitable));
            if (ec || n == 0)
            {
                if (ec && ec != boost::asio::error::eof)
                {
                    if (logger_)
                        logger_->warn("session {} abnormal disconnect: {}", session->id(), ec.message());
                }
                else
                {
                    if (logger_)
                        logger_->debug("session {} disconnected (EOF)", session->id());
                }
                break;
            }

            rb.commit_write(n);
            co_await process_frames(session);
            if (!session->is_open())
                break;

            session_mgr_.touch_session(session->id());
        }

        session_mgr_.remove_session(session->id());
    }

    boost::asio::awaitable<void> process_frames(SessionPtr session)
    {
        auto& recv_buf = session->recv_buffer();

        for (;;)
        {
            auto decode_result = P::try_decode(recv_buf);
            if (!decode_result)
            {
                if (decode_result.error() == ErrorCode::InsufficientData)
                {
                    break; // 더 읽기
                }
                // 그 외 에러 (InvalidMessage 등) → 연결 종료
                session->close();
                co_return;
            }
            auto& frame = *decode_result;

            // Protocol Frame 구조에 따라 msg_id와 payload를 추출.
            // TCP Frame: header.msg_id + span payload
            // WebSocket Frame: payload 첫 4바이트 = msg_id, 나머지 = payload
            uint32_t msg_id{};
            std::span<const uint8_t> payload_span;

            if constexpr (requires { frame.header.msg_id; })
            {
                // TCP-style frame (header + payload)
                msg_id = frame.header.msg_id;
                payload_span = frame.payload();
            }
            else
            {
                // WebSocket-style frame (payload only, msg_id in first 4 bytes)
                const auto& raw = frame.payload();
                if (raw.size() < sizeof(uint32_t))
                {
                    if (logger_)
                        logger_->warn("session {} frame too small for msg_id — closing", session->id());
                    session->close();
                    co_return;
                }
                uint32_t raw_id = 0;
                std::memcpy(&raw_id, raw.data(), sizeof(uint32_t));
                msg_id = ntohl(raw_id); // big-endian → host (TCP WireHeader와 통일)
                payload_span = std::span<const uint8_t>(raw.data() + sizeof(uint32_t), raw.size() - sizeof(uint32_t));
            }

            auto result = co_await dispatcher_.dispatch(session, msg_id, payload_span);

            P::consume_frame(recv_buf, frame);

            if (!result.has_value() && result.error() != ErrorCode::ServiceError)
            {
                // ServiceError는 핸들러가 이미 에러 프레임을 클라이언트에 전송한 경우.
                // 그 외 에러만 connection_handler에서 프레임을 보낸다.
                // enqueue_write_raw 사용 — async_send_raw는 write_pump와 동시 실행 시
                // 같은 소켓에 concurrent async_write가 발생하여 UB.
                auto error_frame = ErrorSender::build_error_frame(msg_id, result.error());
                (void)session->enqueue_write_raw(error_frame);
            }

            if (!session->is_open())
                break;
        }
    }

    SessionManager& session_mgr_;
    MessageDispatcher& dispatcher_;
    ConnectionHandlerConfig config_;
    std::shared_ptr<spdlog::logger> logger_; // Cached to survive spdlog::shutdown()
    std::atomic<uint32_t> active_sessions_{0};
};

} // namespace apex::core
