// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/adapter_interface.hpp>
#include <apex/core/admin_http_server.hpp>
#include <apex/core/arena_allocator.hpp>
#include <apex/core/assert.hpp>
#include <apex/core/blocking_task_executor.hpp>
#include <apex/core/bump_allocator.hpp>
#include <apex/core/core_engine.hpp>
#include <apex/core/cross_core_call.hpp>
#include <apex/core/listener.hpp>
#include <apex/core/message_dispatcher.hpp>
#include <apex/core/metrics_http_server.hpp>
#include <apex/core/metrics_registry.hpp>
#include <apex/core/periodic_task_scheduler.hpp>
#include <apex/core/scoped_logger.hpp>
#include <apex/core/server_config.hpp>
#include <apex/core/service_base.hpp>
#include <apex/core/service_registry.hpp>
#include <apex/core/session_manager.hpp>
#include <apex/core/transport.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <vector>

// Forward declaration — template instantiation happens at the call site
// where the caller includes <apex/shared/adapters/adapter_base.hpp>.
namespace apex::shared::adapters
{
template <typename Derived> class AdapterWrapper;
} // namespace apex::shared::adapters

namespace apex::core
{

/// 어댑터 다중 등록을 위한 복합 키 (type + role).
/// 동일 타입의 어댑터를 역할(role)로 구분하여 다중 등록할 수 있다.
struct AdapterKey
{
    std::type_index type;
    std::string role;

    bool operator==(const AdapterKey&) const = default;
};

/// AdapterKey용 해시 함수.
struct AdapterKeyHash
{
    size_t operator()(const AdapterKey& k) const
    {
        auto h1 = std::hash<std::type_index>{}(k.type);
        auto h2 = std::hash<std::string>{}(k.role);
        return h1 ^ (h2 * 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

// ServerConfig is defined in server_config.hpp (lightweight header).

/// Per-core isolated state (shared-nothing). Each core owns its own
/// SessionManager and allocators. MessageDispatcher and ConnectionHandler
/// are owned by Listener<P> (per-protocol).
///
/// Members are destroyed in reverse declaration order (last declared = first destroyed):
///   arena_allocator → bump_allocator → services → scheduler → registry →
///   fallback_dispatcher → session_mgr
///
/// allocator가 services보다 먼저 소멸되지만, 서비스 소멸자에서 allocator에 접근하지
/// 않으므로 안전하다 (bump/arena는 요청 범위 임시 데이터 전용).
/// services가 scheduler/registry보다 먼저 소멸되어 안전한 정리 보장.
/// session_mgr가 가장 마지막에 소멸 — 코루틴 프레임의 SessionPtr이 해제될 때
/// session_pool_ 메모리가 유효해야 하므로 Server::~Server()에서 core_engine을
/// per_core보다 먼저 명시적으로 파괴한다.
struct PerCoreState
{
    uint32_t core_id;
    SessionManager session_mgr;
    // Fallback dispatcher — 리스너 없는 서비스(Kafka-only)용.
    // Listener가 있으면 Listener의 dispatcher를 사용하고, 없으면 이것을 사용.
    MessageDispatcher fallback_dispatcher;

    // 서비스 레지스트리 — 타입 기반 서비스 조회 (services 벡터와 공존, 점진적 마이그레이션)
    ServiceRegistry registry;

    // 주기적 작업 스케줄러 — per-core io_context 기반
    // services보다 먼저 선언 → services 소멸 후 스케줄러 소멸 (안전한 역순 파괴)
    std::unique_ptr<PeriodicTaskScheduler> scheduler;

    std::vector<std::unique_ptr<ServiceBaseInterface>> services;

    // Per-core memory allocators
    BumpAllocator bump_allocator;
    ArenaAllocator arena_allocator;

    explicit PerCoreState(uint32_t id, uint32_t heartbeat_timeout_ticks, size_t timer_wheel_slots,
                          size_t recv_buf_capacity, size_t max_queue_depth, size_t bump_capacity, size_t arena_block,
                          size_t arena_max);
};

/// Multicore server — io_context-per-core architecture.
///
/// Usage:
///   Server({.num_cores = 4})
///       .listen<TcpBinaryProtocol>(9000, TcpBinaryProtocol::Config{
///           .max_frame_size = 64 * 1024
///       })
///       .add_service<EchoService>()
///       .run();   // blocks until stop() or signal
class Server
{
  public:
    using Config = ServerConfig;

    explicit Server(ServerConfig config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// 프로토콜별 리스너 등록. 포트 분리.
    /// listen<P>()는 run() 전에 호출해야 한다.
    template <Protocol P, Transport T = DefaultTransport>
    Server& listen(uint16_t port, typename P::Config config = {}, typename T::Config transport_config = {})
    {
        std::vector<SessionManager*> mgrs;
        for (auto& state : per_core_)
        {
            mgrs.push_back(&state->session_mgr);
        }
        ConnectionHandlerConfig handler_config{.tcp_nodelay = config_.tcp_nodelay};
        auto listener = std::make_unique<Listener<P, T>>(
            port, std::move(config), std::move(transport_config), *core_engine_, std::move(mgrs), handler_config,
            config_.bind_address, config_.max_connections, config_.reuseport);

        // 서비스 바인딩: 기존에 등록된 서비스 팩토리는 첫 번째 listener의
        // dispatcher에 바인딩된다. 다중 프로토콜 시 서비스별 dispatcher 분리는
        // v0.6에서 처리.
        listeners_.push_back(std::move(listener));
        return *this;
    }

    /// Register a service type to be instantiated once per core.
    /// Args are copy-captured for per-core construction. Supports chaining.
    /// Note: Args are copied for each core. For move-only arguments,
    /// use add_service_factory() instead.
    template <typename T, typename... Args> Server& add_service(Args&&... args)
    {
        service_factories_.push_back(
            [args...](PerCoreState& /*state*/, MessageDispatcher& dispatcher) -> std::unique_ptr<ServiceBaseInterface> {
                auto svc = std::make_unique<T>(args...);
                svc->bind_dispatcher(dispatcher);
                return svc;
            });
        return *this;
    }

    /// Register a factory that receives PerCoreState for per-core injection.
    /// Use when services need SessionManager or other per-core state.
    /// Factory signature: (PerCoreState&) -> unique_ptr<ServiceBaseInterface>
    template <typename Factory> Server& add_service_factory(Factory&& factory)
    {
        service_factories_.push_back(
            [f = std::forward<Factory>(factory)](
                PerCoreState& state, MessageDispatcher& dispatcher) -> std::unique_ptr<ServiceBaseInterface> {
                auto svc = f(state);
                svc->bind_dispatcher(dispatcher);
                return svc;
            });
        return *this;
    }

    /// 역할(role)을 지정하여 어댑터 등록. 동일 타입을 역할별로 다중 등록 가능.
    /// 체이닝 지원. 등록 순서는 무관 — run()에서 서비스보다 먼저 초기화됨.
    /// 주의: 호출자가 <apex/shared/adapters/adapter_base.hpp>를 include해야 함.
    ///       server.hpp는 순환 의존 방지를 위해 이를 포함하지 않음.
    template <typename T, typename... Args> Server& add_adapter(std::string role, Args&&... args)
    {
        auto wrapper = std::make_unique<apex::shared::adapters::AdapterWrapper<T>>(std::forward<Args>(args)...);
        auto* raw = wrapper.get();
        AdapterKey key{std::type_index(typeid(T)), std::move(role)};
        adapter_map_[key] = raw;
        adapters_.push_back(std::move(wrapper));
        return *this;
    }

    /// 어댑터 등록 (role = "default"). 기존 API 호환.
    /// 체이닝 지원. 등록 순서는 무관 — run()에서 서비스보다 먼저 초기화됨.
    /// 주의: 호출자가 <apex/shared/adapters/adapter_base.hpp>를 include해야 함.
    ///       server.hpp는 순환 의존 방지를 위해 이를 포함하지 않음.
    /// @note role 오버로드와의 모호성 방지: 첫 번째 인자가 std::string 변환 가능하면 비활성화.
    template <typename T, typename First, typename... Rest>
        requires(!std::is_convertible_v<std::decay_t<First>, std::string>)
    Server& add_adapter(First&& first, Rest&&... rest)
    {
        return add_adapter<T>(std::string("default"), std::forward<First>(first), std::forward<Rest>(rest)...);
    }

    /// 어댑터 등록 (인자 없음, role = "default"). 기존 API 호환.
    template <typename T> Server& add_adapter()
    {
        return add_adapter<T>(std::string("default"));
    }

    /// 등록된 어댑터 접근 (타입 + 역할 기반). 미등록 시 assert 실패.
    /// 주의: 호출자가 <apex/shared/adapters/adapter_base.hpp>를 include해야 함.
    template <typename T> T& adapter(std::string_view role = "default") const
    {
        AdapterKey key{std::type_index(typeid(T)), std::string(role)};
        auto it = adapter_map_.find(key);
        APEX_ASSERT(it != adapter_map_.end(), "Adapter not registered");
        return static_cast<apex::shared::adapters::AdapterWrapper<T>*>(it->second)->get();
    }

    /// Register a callback invoked after adapters + services are initialized
    /// but before CoreEngine starts. Use for wiring cross-cutting concerns
    /// (e.g., ResponseDispatcher) that need both service and adapter refs.
    using PostInitCallback = std::function<void(Server&)>;
    Server& set_post_init_callback(PostInitCallback cb)
    {
        post_init_cb_ = std::move(cb);
        return *this;
    }

    /// Access CoreEngine (available after run() enters init phase).
    [[nodiscard]] CoreEngine& core_engine() noexcept
    {
        return *core_engine_;
    }

    /// Access BlockingTaskExecutor for CPU-bound work offload.
    [[nodiscard]] BlockingTaskExecutor& blocking_executor() noexcept
    {
        return *blocking_executor_;
    }

    /// Access MetricsRegistry for Prometheus metric registration.
    [[nodiscard]] MetricsRegistry& metrics_registry() noexcept
    {
        return metrics_registry_;
    }

    /// Access per-core state (for ResponseDispatcher wiring etc.)
    [[nodiscard]] PerCoreState& per_core_state(uint32_t core_id)
    {
        return *per_core_[core_id];
    }

    /// Blocking run. Owns all io_contexts and threads internally.
    void run();

    /// Thread-safe. Safe to call from another thread or from signal_set
    /// completion handler. Not async-signal-safe (do not call from raw
    /// POSIX signal handler).
    /// Note: running() returns true until run() fully exits (including shutdown).
    /// Use stopping_ internally to prevent re-entry.
    void stop();

    /// Actual bound port of the first listener (0 if no listeners).
    /// Useful for tests that bind to port 0.
    [[nodiscard]] uint16_t port() const noexcept;

    [[nodiscard]] uint32_t core_count() const noexcept;
    [[nodiscard]] bool running() const noexcept;

    /// Access core's io_context (for cross_core_call / tests).
    [[nodiscard]] boost::asio::io_context& core_io_context(uint32_t core_id);

    /// Total active sessions across all listeners.
    [[nodiscard]] uint32_t total_active_sessions() const noexcept;

    /// [D3] Cross-core 공유 자원 등록/접근. 최초 호출 시 factory 실행, 이후 동일 T 반환.
    /// on_wire() 단계에서 사용. Server가 수명 관리, 서비스는 raw pointer로 참조.
    /// @note on_wire는 메인 스레드에서 순차 실행되므로 동기화 불필요.
    template <typename T, typename Factory> T& global(Factory&& factory)
    {
        auto key = std::type_index(typeid(T));
        auto it = globals_.find(key);
        if (it == globals_.end())
        {
            auto holder = std::make_unique<TypedGlobalHolder<T>>(std::forward<Factory>(factory));
            auto& ref = holder->value;
            globals_.emplace(key, std::move(holder));
            return ref;
        }
        return static_cast<TypedGlobalHolder<T>*>(it->second.get())->value;
    }

    /// Execute func on target_core and co_await the result (coroutine).
    /// Overload without timeout uses config_.cross_core_call_timeout.
    template <typename F> auto cross_core_call(uint32_t target_core, F&& func)
    {
        return apex::core::cross_core_call(*core_engine_, target_core, std::forward<F>(func),
                                           config_.cross_core_call_timeout);
    }

    /// Execute func on target_core with explicit timeout.
    template <typename F> auto cross_core_call(uint32_t target_core, F&& func, std::chrono::milliseconds timeout)
    {
        return apex::core::cross_core_call(*core_engine_, target_core, std::forward<F>(func), timeout);
    }

    /// Fire-and-forget execution on target core. Awaitable, core thread only.
    template <typename F> boost::asio::awaitable<void> cross_core_post(uint32_t target_core, F&& func)
    {
        co_await apex::core::cross_core_post(*core_engine_, target_core, std::forward<F>(func));
    }

  private:
    // [D3] Cross-core global resource holder (type-erased).
    struct GlobalHolder
    {
        virtual ~GlobalHolder() = default;
    };

    template <typename T> struct TypedGlobalHolder : GlobalHolder
    {
        T value;
        template <typename Factory>
        explicit TypedGlobalHolder(Factory&& f)
            : value(std::forward<Factory>(f)())
        {}
    };

    using ServiceFactory = std::function<std::unique_ptr<ServiceBaseInterface>(PerCoreState&, MessageDispatcher&)>;

    void begin_shutdown();
    void poll_shutdown();
    void finalize_shutdown();

    ServerConfig config_;
    boost::asio::io_context control_io_;
    std::unique_ptr<CoreEngine> core_engine_;
    std::unique_ptr<BlockingTaskExecutor> blocking_executor_;

    // [D3] Cross-core global resources. 선언 위치 중요: per_core_ 앞.
    // 소멸 순서에서 per_core_(서비스) → globals_ 순으로 소멸되어
    // 서비스가 보유한 globals raw pointer가 서비스 소멸 시점까지 유효.
    std::unordered_map<std::type_index, std::unique_ptr<GlobalHolder>> globals_;

    std::vector<std::unique_ptr<PerCoreState>> per_core_;
    std::vector<std::unique_ptr<ListenerBase>> listeners_;
    std::vector<ServiceFactory> service_factories_;
    std::vector<std::unique_ptr<AdapterInterface>> adapters_;
    std::unordered_map<AdapterKey, AdapterInterface*, AdapterKeyHash> adapter_map_;

    PostInitCallback post_init_cb_;

    MetricsRegistry metrics_registry_;
    MetricsHttpServer metrics_http_server_;
    AdminHttpServer admin_http_server_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<uint32_t> run_count_{0}; // I-21: prevent re-entry
    std::unique_ptr<boost::asio::steady_timer> shutdown_timer_;
    std::chrono::steady_clock::time_point shutdown_deadline_;
    ScopedLogger logger_{"Server", ScopedLogger::NO_CORE}; // I-09: ScopedLogger caches logger internally
};

// ── ServiceBase per-core 접근자 out-of-line 정의 ──────────────────────────
// PerCoreState complete type이 필요하므로 service_base.hpp가 아닌 여기서 정의.
template <typename Derived> inline BumpAllocator& ServiceBase<Derived>::bump()
{
    APEX_ASSERT(per_core_ != nullptr, "bump() called before internal_configure");
    return per_core_->bump_allocator;
}

template <typename Derived> inline ArenaAllocator& ServiceBase<Derived>::arena()
{
    APEX_ASSERT(per_core_ != nullptr, "arena() called before internal_configure");
    return per_core_->arena_allocator;
}

template <typename Derived> inline uint32_t ServiceBase<Derived>::core_id() const
{
    APEX_ASSERT(per_core_ != nullptr, "core_id() called before internal_configure");
    return per_core_->core_id;
}

template <typename Derived> inline uint32_t ServiceBase<Derived>::core_id_for_log() const noexcept
{
    return per_core_ ? per_core_->core_id : UINT32_MAX;
}

} // namespace apex::core
