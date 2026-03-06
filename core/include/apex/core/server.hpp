#pragma once

#include <apex/core/message_dispatcher.hpp>
#include <apex/core/session.hpp>
#include <apex/core/session_manager.hpp>
#include <apex/core/service_base.hpp>
#include <apex/core/tcp_acceptor.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace apex::core {

/// 모든 프레임워크 컴포넌트를 엮는 최상위 서버 클래스.
///
/// Usage:
///   boost::asio::io_context io_ctx;
///   Server server(io_ctx, {.port = 9000});
///   server.add_service<EchoService>();
///   server.start();
///   io_ctx.run();
class Server {
public:
    struct Config {
        uint16_t port = 9000;
        uint32_t heartbeat_timeout_ticks = 300;  // 0 = 비활성화
        uint32_t tick_interval_ms = 100;
        size_t recv_buf_capacity = 8192;
        size_t timer_wheel_slots = 1024;
    };

    explicit Server(boost::asio::io_context& io_ctx, Config config = {});
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// 서비스 등록 (start 전에 호출).
    template <typename T, typename... Args>
    T& add_service(Args&&... args) {
        auto svc = std::make_unique<T>(std::forward<Args>(args)...);
        svc->bind_dispatcher(dispatcher_);
        auto& ref = *svc;
        services_.push_back(std::move(svc));
        return ref;
    }

    /// 서버 시작 (accept + timer tick + 서비스 시작).
    void start();

    /// 서버 중지 (모든 세션/서비스 정리).
    void stop();

    /// 바인딩된 실제 포트.
    [[nodiscard]] uint16_t port() const noexcept;

    /// 접근자.
    [[nodiscard]] SessionManager& session_manager() noexcept { return session_mgr_; }
    [[nodiscard]] MessageDispatcher& dispatcher() noexcept { return dispatcher_; }
    [[nodiscard]] bool running() const noexcept { return running_; }

private:
    void on_accept(boost::asio::ip::tcp::socket socket);
    void start_read(SessionPtr session);
    void do_read(SessionPtr session);
    void process_frames(SessionPtr session);
    void start_tick_timer();

    boost::asio::io_context& io_ctx_;
    Config config_;
    MessageDispatcher dispatcher_;
    SessionManager session_mgr_;
    TcpAcceptor acceptor_;
    boost::asio::steady_timer tick_timer_;
    bool running_{false};

    std::vector<std::unique_ptr<ServiceBaseInterface>> services_;

    static constexpr size_t TMP_BUF_SIZE = 4096;
};

} // namespace apex::core
