#include <apex/core/connection_handler.hpp>
#include <apex/core/error_sender.hpp>
#include <apex/core/tcp_binary_protocol.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>

#include <span>

namespace apex::core {

ConnectionHandler::ConnectionHandler(SessionManager& session_mgr,
                                     MessageDispatcher& dispatcher,
                                     ConnectionHandlerConfig config)
    : session_mgr_(session_mgr)
    , dispatcher_(dispatcher)
    , config_(config)
{
}

void ConnectionHandler::accept_connection(
    boost::asio::ip::tcp::socket socket,
    boost::asio::io_context& io_ctx)
{
    if (config_.tcp_nodelay) {
        boost::system::error_code ec;
        socket.set_option(boost::asio::ip::tcp::no_delay(true), ec);
    }

    auto session = session_mgr_.create_session(std::move(socket));
    boost::asio::co_spawn(io_ctx,
        read_loop(std::move(session)),
        boost::asio::detached);
}

boost::asio::awaitable<void> ConnectionHandler::read_loop(SessionPtr session) {
    struct ActiveSessionGuard {
        std::atomic<uint32_t>& counter;
        ~ActiveSessionGuard() { counter.fetch_sub(1, std::memory_order_release); }
    };
    active_sessions_.fetch_add(1, std::memory_order_relaxed);
    ActiveSessionGuard guard{active_sessions_};

    while (session->is_open()) {
        auto& rb = session->recv_buffer();
        auto writable = rb.writable();
        if (writable.empty()) {
            session->close();
            break;
        }

        auto [ec, n] = co_await session->socket().async_read_some(
            boost::asio::buffer(writable.data(), writable.size()),
            boost::asio::as_tuple(boost::asio::use_awaitable));
        if (ec || n == 0) {
            if (ec && ec != boost::asio::error::eof) {
                spdlog::warn("session {} abnormal disconnect: {}",
                             session->id(), ec.message());
            } else {
                spdlog::debug("session {} disconnected (EOF)", session->id());
            }
            break;
        }

        rb.commit_write(n);
        co_await process_frames(session);
        if (!session->is_open()) break;

        session_mgr_.touch_session(session->id());
    }

    session_mgr_.remove_session(session->id());
}

boost::asio::awaitable<void> ConnectionHandler::process_frames(SessionPtr session) {
    auto& recv_buf = session->recv_buffer();

    for (;;) {
        auto decode_result = TcpBinaryProtocol::try_decode(recv_buf);
        if (!decode_result) {
            if (decode_result.error() != FrameError::InsufficientData) {
                session->close();
                co_return;
            }
            break;
        }
        auto& frame = *decode_result;

        auto header = frame.header;
        auto payload_span = frame.payload;

        auto result = co_await dispatcher_.dispatch(
            session, header.msg_id, payload_span);

        TcpBinaryProtocol::consume_frame(recv_buf, frame);

        if (!result.has_value()) {
            auto error_frame = ErrorSender::build_error_frame(
                header.msg_id, result.error());
            (void)co_await session->async_send_raw(error_frame);
        }

        if (!session->is_open()) break;
    }
}

} // namespace apex::core
