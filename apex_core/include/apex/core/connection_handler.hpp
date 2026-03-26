// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/error_code.hpp>
#include <apex/core/error_sender.hpp>
#include <apex/core/message_dispatcher.hpp>
#include <apex/core/protocol.hpp>
#include <apex/core/scoped_logger.hpp>
#include <apex/core/session.hpp>
#include <apex/core/session_manager.hpp>
#include <apex/core/socket_base.hpp>
#include <apex/core/transport.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>

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
    ConnectionHandler(SessionManager& session_mgr, MessageDispatcher& dispatcher, ConnectionHandlerConfig config,
                      uint32_t core_id = ScopedLogger::NO_CORE)
        : session_mgr_(session_mgr)
        , dispatcher_(dispatcher)
        , config_(config)
        , logger_("ConnectionHandler", core_id)
    {}

    using ConnectionClosedCallback = std::function<void(const std::string& remote_ip)>;

    /// Set callback invoked when a session's read_loop exits (before remove_session).
    /// Used by Listener to release per-IP connection counters.
    void set_connection_closed_callback(ConnectionClosedCallback cb)
    {
        connection_closed_cb_ = std::move(cb);
    }

    /// Accept a new connection -- create session + spawn read_loop.
    /// Must be called on the owning core's io_context thread.
    /// @param socket SocketBase 소유권. Listener가 Transport::wrap_socket()으로 생성.
    /// @param remote_ip 클라이언트 IP (Listener에서 accept 시점에 추출, close 시 release용)
    void accept_connection(std::unique_ptr<SocketBase> socket, boost::asio::io_context& io_ctx,
                           std::string remote_ip = {})
    {
        if (config_.tcp_nodelay)
        {
            socket->set_option_no_delay(true);
        }

        auto session = session_mgr_.create_session(std::move(socket));
        if (!remote_ip.empty())
            session->set_remote_ip(std::move(remote_ip));
        // Socket executor가 acceptor의 io_context(core 0)를 가리킬 수 있으므로,
        // 실제 실행 core의 executor를 명시적으로 설정.
        // Session 내부의 timer/write_pump가 올바른 io_context에서 동작하게 보장.
        session->set_core_executor(io_ctx.get_executor());
        boost::asio::co_spawn(io_ctx, read_loop(std::move(session)), boost::asio::detached);
    }

    /// Number of currently active read_loop coroutines.
    [[nodiscard]] uint32_t active_sessions() const noexcept
    {
        return active_sessions_->load(std::memory_order_acquire);
    }

  private:
    boost::asio::awaitable<void> read_loop(SessionPtr session)
    {
        struct ActiveSessionGuard
        {
            std::shared_ptr<std::atomic<uint32_t>> counter;
            ~ActiveSessionGuard()
            {
                counter->fetch_sub(1, std::memory_order_release);
            }
        };
        active_sessions_->fetch_add(1, std::memory_order_relaxed);
        ActiveSessionGuard guard{active_sessions_};

        // Handshake (TcpSocket: no-op, TlsSocket: TLS handshake)
        auto hs = co_await session->socket().async_handshake();
        if (!hs.has_value())
        {
            logger_.warn(session, "handshake failed");
            session->close();
            if (connection_closed_cb_ && !session->remote_ip().empty())
                connection_closed_cb_(session->remote_ip());
            session_mgr_.remove_session(session->id());
            co_return;
        }

        try
        {
            while (session->is_open())
            {
                auto& rb = session->recv_buffer();
                auto writable = rb.writable();
                if (writable.empty())
                {
                    logger_.warn(session, "recv_buffer full — closing connection");
                    session->close();
                    break;
                }

                auto [ec, n] =
                    co_await session->socket().async_read_some(boost::asio::buffer(writable.data(), writable.size()));
                if (ec || n == 0)
                {
                    if (ec && ec != boost::asio::error::eof)
                    {
                        logger_.warn(session, "abnormal disconnect: {}", ec.message());
                    }
                    else
                    {
                        logger_.debug(session, "disconnected (EOF)");
                    }
                    break;
                }

                rb.commit_write(n);
                co_await process_frames(session);
                if (!session->is_open())
                    break;

                session_mgr_.touch_session(session->id());
            }
        }
        catch (const std::exception& e)
        {
            logger_.error(session, "read_loop exception: {}", e.what());
        }
        catch (...)
        {
            logger_.error(session, "read_loop unknown exception");
        }

        if (connection_closed_cb_ && !session->remote_ip().empty())
            connection_closed_cb_(session->remote_ip());
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
                    logger_.warn(session, "frame too small for msg_id — closing");
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
                // enqueue_write_raw 사용 — fire-and-forget으로 충분.
                // (async_send_raw도 write_pump 경유이므로 안전하지만 co_await 불필요)
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
    ScopedLogger logger_;
    std::shared_ptr<std::atomic<uint32_t>> active_sessions_ = std::make_shared<std::atomic<uint32_t>>(0);
    ConnectionClosedCallback connection_closed_cb_;
};

} // namespace apex::core
