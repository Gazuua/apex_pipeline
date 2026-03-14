#pragma once

#include <apex/core/adapter_interface.hpp>
#include <apex/core/arena_allocator.hpp>
#include <apex/core/bump_allocator.hpp>
#include <apex/core/connection_handler.hpp>
#include <apex/core/core_engine.hpp>
#include <apex/core/cross_core_call.hpp>
#include <apex/core/message_dispatcher.hpp>
#include <apex/core/session_manager.hpp>
#include <apex/core/service_base.hpp>
#include <apex/core/tcp_acceptor.hpp>

#include <spdlog/spdlog.h>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <vector>

// Forward declaration — template instantiation happens at the call site
// where the caller includes <apex/shared/adapters/adapter_base.hpp>.
namespace apex::shared::adapters {
template <typename Derived> class AdapterWrapper;
} // namespace apex::shared::adapters

namespace apex::core {

/// Server configuration. Fields are ordered for designated-initializer convenience.
struct ServerConfig {
    // Network (bind_address deferred — TcpAcceptor defaults to 0.0.0.0)
    uint16_t port = 9000;
    bool tcp_nodelay = true;  // Disable Nagle's algorithm for low-latency

    // Multicore
    uint32_t num_cores = 1;
    size_t mpsc_queue_capacity = 65536;
    std::chrono::milliseconds tick_interval{100};

    // Session
    uint32_t heartbeat_timeout_ticks = 300;  // 0 = disabled
    size_t recv_buf_capacity = 8192;
    size_t timer_wheel_slots = 1024;

    // Platform I/O
    bool reuseport = false;  // Linux: per-core SO_REUSEPORT, Windows: ignored

    // Lifecycle
    bool handle_signals = true;
    std::chrono::seconds drain_timeout{25};  // Graceful Shutdown drain timeout

    // Cross-core call
    std::chrono::milliseconds cross_core_call_timeout{5000};

    // Memory allocators (per-core)
    std::size_t bump_capacity_bytes = 64 * 1024;    // 64KB
    std::size_t arena_block_bytes = 4096;            // 4KB
    std::size_t arena_max_bytes = 1024 * 1024;       // 1MB
};

/// Per-core isolated state (shared-nothing). Each core owns its own
/// SessionManager, MessageDispatcher, and service instances.
///
/// Members are destroyed in reverse declaration order. 'services' is declared
/// after 'dispatcher', so services are destroyed first — ensuring dispatcher_
/// pointers remain valid during ServiceBase::stop() calls in ~Server().
struct PerCoreState {
    uint32_t core_id;
    SessionManager session_mgr;
    MessageDispatcher dispatcher;
    ConnectionHandler handler;
    std::vector<std::unique_ptr<ServiceBaseInterface>> services;

    // Per-core memory allocators
    BumpAllocator bump_allocator;
    ArenaAllocator arena_allocator;

    explicit PerCoreState(uint32_t id, uint32_t heartbeat_timeout_ticks,
                          size_t timer_wheel_slots, size_t recv_buf_capacity,
                          ConnectionHandlerConfig handler_config,
                          size_t bump_capacity, size_t arena_block, size_t arena_max);
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

    /// 어댑터(인프라 컴포넌트) 등록. 체이닝 지원.
    /// 등록 순서는 무관 — run()에서 서비스보다 먼저 초기화됨.
    /// 주의: 호출자가 <apex/shared/adapters/adapter_base.hpp>를 include해야 함.
    ///       server.hpp는 순환 의존 방지를 위해 이를 포함하지 않음.
    template <typename T, typename... Args>
    Server& add_adapter(Args&&... args) {
        auto wrapper = std::make_unique<apex::shared::adapters::AdapterWrapper<T>>(
            std::forward<Args>(args)...);
        auto* raw = wrapper.get();
        auto key = std::type_index(typeid(T));
        adapter_map_[key] = raw;
        adapters_.push_back(std::move(wrapper));
        return *this;
    }

    /// 등록된 어댑터 접근 (타입 기반).
    /// 주의: 호출자가 <apex/shared/adapters/adapter_base.hpp>를 include해야 함.
    template <typename T>
    T& adapter() const {
        auto key = std::type_index(typeid(T));
        auto it = adapter_map_.find(key);
        assert(it != adapter_map_.end() && "Adapter not registered");
        return static_cast<apex::shared::adapters::AdapterWrapper<T>*>(it->second)->get();
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

    /// Total active sessions across all cores.
    [[nodiscard]] uint32_t total_active_sessions() const noexcept;

    /// Execute func on target_core and co_await the result (coroutine).
    /// Overload without timeout uses config_.cross_core_call_timeout.
    template <typename F>
    auto cross_core_call(uint32_t target_core, F&& func) {
        return apex::core::cross_core_call(
            *core_engine_, target_core, std::forward<F>(func),
            config_.cross_core_call_timeout);
    }

    /// Execute func on target_core with explicit timeout.
    template <typename F>
    auto cross_core_call(uint32_t target_core, F&& func,
                         std::chrono::milliseconds timeout) {
        return apex::core::cross_core_call(
            *core_engine_, target_core, std::forward<F>(func), timeout);
    }

    /// Fire-and-forget execution on target core.
    template <typename F>
    Result<void> cross_core_post(uint32_t target_core, F&& func) {
        return apex::core::cross_core_post(
            *core_engine_, target_core, std::forward<F>(func));
    }

private:
    using ServiceFactory = std::function<
        std::unique_ptr<ServiceBaseInterface>(PerCoreState&)>;

    void on_accept(boost::asio::ip::tcp::socket socket);
    void begin_shutdown();
    void poll_shutdown();
    void finalize_shutdown();

    ServerConfig config_;
    boost::asio::io_context control_io_;
    std::unique_ptr<CoreEngine> core_engine_;
    std::vector<std::unique_ptr<PerCoreState>> per_core_;
    std::vector<std::unique_ptr<TcpAcceptor>> acceptors_;
    std::vector<ServiceFactory> service_factories_;
    std::vector<std::unique_ptr<AdapterInterface>> adapters_;
    std::unordered_map<std::type_index, AdapterInterface*> adapter_map_;

    std::atomic<uint32_t> next_core_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<uint32_t> run_count_{0};  // I-21: prevent re-entry
    std::unique_ptr<boost::asio::steady_timer> shutdown_timer_;
    std::chrono::steady_clock::time_point shutdown_deadline_;
    std::shared_ptr<spdlog::logger> logger_;  // I-09: cached logger
};

} // namespace apex::core
