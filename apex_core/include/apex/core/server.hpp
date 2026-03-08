#pragma once

#include <apex/core/core_engine.hpp>
#include <apex/core/cross_core_call.hpp>
#include <apex/core/message_dispatcher.hpp>
#include <apex/core/session_manager.hpp>
#include <apex/core/service_base.hpp>
#include <apex/core/tcp_acceptor.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace apex::core {

/// Server configuration. Fields are ordered for designated-initializer convenience.
struct ServerConfig {
    // Network (bind_address deferred — TcpAcceptor defaults to 0.0.0.0)
    uint16_t port = 9000;

    // Multicore
    uint32_t num_cores = 1;
    size_t mpsc_queue_capacity = 65536;
    std::chrono::microseconds drain_interval{100};

    // Session
    uint32_t heartbeat_timeout_ticks = 300;  // 0 = disabled
    size_t recv_buf_capacity = 8192;
    size_t timer_wheel_slots = 1024;

    // Lifecycle
    bool handle_signals = true;
};

/// Per-core isolated state (shared-nothing). Each core owns its own
/// SessionManager, MessageDispatcher, and service instances.
struct PerCoreState {
    uint32_t core_id;
    SessionManager session_mgr;
    MessageDispatcher dispatcher;
    std::vector<std::unique_ptr<ServiceBaseInterface>> services;

    explicit PerCoreState(uint32_t id, uint32_t heartbeat_timeout_ticks,
                          size_t timer_wheel_slots, size_t recv_buf_capacity);
};

/// Multicore server — io_context-per-core architecture.
///
/// Usage:
///   Server({.port = 9000, .num_cores = 4})
///       .add_service<EchoService>()
///       .run();   // blocks until stop() or signal
class Server {
public:
    using Config = ServerConfig;

    explicit Server(ServerConfig config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Register a service type to be instantiated once per core.
    /// Args are copy-captured for per-core construction. Supports chaining.
    /// Note: Args are copied for each core. For move-only arguments,
    /// use add_service_factory() instead.
    template <typename T, typename... Args>
    Server& add_service(Args&&... args) {
        service_factories_.push_back(
            [args...](PerCoreState& state)
                -> std::unique_ptr<ServiceBaseInterface> {
                auto svc = std::make_unique<T>(args...);
                svc->bind_dispatcher(state.dispatcher);
                return svc;
            }
        );
        return *this;
    }

    /// Register a factory that receives PerCoreState for per-core injection.
    /// Use when services need SessionManager or other per-core state.
    template <typename Factory>
    Server& add_service_factory(Factory&& factory) {
        service_factories_.push_back(
            [f = std::forward<Factory>(factory)](PerCoreState& state)
                -> std::unique_ptr<ServiceBaseInterface> {
                auto svc = f(state);
                svc->bind_dispatcher(state.dispatcher);
                return svc;
            }
        );
        return *this;
    }

    /// Blocking run. Owns all io_contexts and threads internally.
    void run();

    /// Thread-safe. Safe to call from another thread or from signal_set
    /// completion handler. Not async-signal-safe (do not call from raw
    /// POSIX signal handler).
    /// Note: running() returns true until run() fully exits (including shutdown).
    /// Use stopping_ internally to prevent re-entry.
    void stop();

    /// Actual bound port (after constructor binds).
    [[nodiscard]] uint16_t port() const noexcept;

    [[nodiscard]] uint32_t core_count() const noexcept;
    [[nodiscard]] bool running() const noexcept;

    /// Access core's io_context (for cross_core_call / tests).
    [[nodiscard]] boost::asio::io_context& core_io_context(uint32_t core_id);

    /// Execute func on target_core and co_await the result (coroutine).
    template <typename F>
    auto cross_core_call(uint32_t target_core, F&& func,
                         std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) {
        return apex::core::cross_core_call(
            *core_engine_, target_core, std::forward<F>(func), timeout);
    }

    /// Fire-and-forget execution on target core.
    template <typename F>
    bool cross_core_post(uint32_t target_core, F&& func) {
        return apex::core::cross_core_post(
            *core_engine_, target_core, std::forward<F>(func));
    }

private:
    using ServiceFactory = std::function<
        std::unique_ptr<ServiceBaseInterface>(PerCoreState&)>;

    boost::asio::awaitable<void> read_loop(SessionPtr session, uint32_t core_id);
    boost::asio::awaitable<void> process_frames(SessionPtr session, uint32_t core_id);
    void on_accept(boost::asio::ip::tcp::socket socket);
    void begin_shutdown();
    void poll_shutdown();

    ServerConfig config_;
    boost::asio::io_context accept_io_;
    std::unique_ptr<CoreEngine> core_engine_;
    std::vector<std::unique_ptr<PerCoreState>> per_core_;
    std::unique_ptr<TcpAcceptor> acceptor_;
    std::vector<ServiceFactory> service_factories_;

    std::atomic<uint32_t> next_core_{0};
    std::atomic<uint32_t> active_sessions_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::unique_ptr<boost::asio::steady_timer> shutdown_timer_;

    static constexpr size_t TMP_BUF_SIZE = 4096;
};

} // namespace apex::core
