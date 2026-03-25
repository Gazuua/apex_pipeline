// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/configure_context.hpp>
#include <apex/core/crash_handler.hpp>
#include <apex/core/detail/math_utils.hpp>
#include <apex/core/server.hpp>
#include <apex/core/wire_context.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>

#include <stdexcept>
#include <string>
#include <thread>

namespace apex::core
{

// --- PerCoreState ---

PerCoreState::PerCoreState(uint32_t id, uint32_t heartbeat_timeout_ticks, size_t timer_wheel_slots,
                           size_t recv_buf_capacity, size_t max_queue_depth, size_t bump_capacity, size_t arena_block,
                           size_t arena_max)
    : core_id(id)
    , session_mgr(id, heartbeat_timeout_ticks, timer_wheel_slots, recv_buf_capacity, max_queue_depth)
    , bump_allocator(bump_capacity)
    , arena_allocator(arena_block, arena_max)
{}

// --- Server ---

Server::Server(ServerConfig config)
    : config_(config)
{
    if (config_.heartbeat_timeout_ticks > 0)
    {
        size_t actual_slots = detail::next_power_of_2(config_.timer_wheel_slots < 1 ? 1 : config_.timer_wheel_slots);
        if (config_.heartbeat_timeout_ticks >= actual_slots)
        {
            throw std::invalid_argument(
                "ServerConfig::heartbeat_timeout_ticks (" + std::to_string(config_.heartbeat_timeout_ticks) +
                ") must be < timer_wheel effective slots (" + std::to_string(actual_slots) + ")");
        }
    }

    // CoreEngine
    CoreEngineConfig engine_config{
        .num_cores = config_.num_cores,
        .spsc_queue_capacity = config_.spsc_queue_capacity,
        .tick_interval = config_.tick_interval,
        .drain_batch_limit = 1024,
    };
    core_engine_ = std::make_unique<CoreEngine>(engine_config);

    // Sync num_cores with CoreEngine's resolved value.
    // CoreEngine normalizes 0 → hardware_concurrency, so we must use
    // the actual core count to avoid division-by-zero UB.
    config_.num_cores = core_engine_->core_count();

    // PerCoreState (no longer contains ConnectionHandler/MessageDispatcher)
    for (uint32_t i = 0; i < config_.num_cores; ++i)
    {
        per_core_.push_back(std::make_unique<PerCoreState>(i, config_.heartbeat_timeout_ticks,
                                                           config_.timer_wheel_slots, config_.recv_buf_capacity,
                                                           config_.session_max_queue_depth, config_.bump_capacity_bytes,
                                                           config_.arena_block_bytes, config_.arena_max_bytes));
    }

    // Acceptor binding moved to Listener<P>::start()
}

Server::~Server()
{
    // finalize_shutdown이 미호출된 비정상 경로 감지:
    // running_이 true면 run() 진입 후 정상 종료 경로를 거치지 않은 것
    if (running_.load(std::memory_order_acquire))
    {
        logger_.warn("Destructor called while still running - "
                     "finalize_shutdown may not have been called");
    }
    for (auto& state : per_core_)
    {
        for (auto& svc : state->services)
        {
            if (svc->started())
            {
                logger_.warn("Service '{}' still running in destructor", svc->name());
                svc->stop();
            }
        }
    }

    // 명시적 파괴 순서 — io_context 의존성 역순 정리.
    //
    //   0) 잔여 핸들러 drain: core_engine_->stop()이 io_context::stop()을 호출하면
    //      pending completion handler(예: Session::enqueue_and_await의 timer cancel)가
    //      미처리된 채 남음. 모든 객체(listener, session, timer)가 아직 유효한 상태에서
    //      restart+poll로 잔여 핸들러를 소진해야 ~io_context() 시 UAF를 방지.
    //      ※ listeners_.clear() 이후로 이동 금지 — poll이 처리하는 핸들러가
    //        TcpAcceptor 등 listener 소유 객체를 참조할 수 있음.
    //   1) listeners_ : TcpAcceptor 소켓이 io_context에 등록됨
    //   2) per_core_ schedulers : 타이머가 io_context에 등록됨
    //   3) core_engine_ : io_context 소유. per_core_의 Session slab 메모리가
    //      유효한 상태에서 실행되어야 함.
    if (core_engine_)
    {
        for (uint32_t i = 0; i < config_.num_cores; ++i)
        {
            auto& io = core_engine_->io_context(i);
            io.restart();
            try
            {
                io.poll();
            }
            catch (const std::exception&)
            {
                // Best-effort drain during destruction — suppress handler exceptions.
            }
        }
    }

    // 0.5) control_io_ 잔여 핸들러 drain — MetricsHttpServer cancel 콜백 등 소진.
    {
        control_io_.restart();
        try
        {
            control_io_.poll();
        }
        catch (const std::exception&)
        {}
    }

    listeners_.clear();
    for (auto& state : per_core_)
    {
        state->scheduler.reset();
    }
    core_engine_.reset();
}

void Server::run()
{
    // I-21: run() is single-use. Reject re-entry after first call.
    if (run_count_.fetch_add(1, std::memory_order_acq_rel) != 0)
    {
        throw std::logic_error("Server::run() must not be called more than once");
    }

    stopping_.store(false, std::memory_order_relaxed); // run() init, single thread

    // Install crash signal handlers (SIGABRT, SIGSEGV, etc.) for diagnostics.
    // Independent of handle_signals — crash handlers are always active.
    install_crash_handlers();

    control_io_.restart();

    // I-2: Clear leftover services from a previous run() cycle to prevent
    // double-start. Services are stopped in poll_shutdown(), but clearing
    // here guards against premature stop() before all services started.
    for (auto& state : per_core_)
    {
        state->services.clear();
    }

    logger_.info("starting with {} cores", config_.num_cores);

    // 어댑터 초기화 — 서비스보다 먼저 (어댑터는 서비스의 인프라 의존성)
    for (auto& adapter : adapters_)
    {
        adapter->init(*core_engine_);
    }
    logger_.debug("adapters initialized ({})", adapters_.size());

    // Register adapter metrics
    for (auto& adapter : adapters_)
    {
        adapter->register_metrics(metrics_registry_);
    }

    // PeriodicTaskScheduler 초기화 — per-core io_context 기반
    for (uint32_t core_id = 0; core_id < config_.num_cores; ++core_id)
    {
        per_core_[core_id]->scheduler = std::make_unique<PeriodicTaskScheduler>(core_engine_->io_context(core_id));
    }

    // Per-core 서비스 인스턴스 생성
    // 리스너가 있으면 리스너의 dispatcher를, 없으면 fallback_dispatcher를 사용 (Kafka-only 서비스).
    for (uint32_t core_id = 0; core_id < config_.num_cores; ++core_id)
    {
        auto& state = *per_core_[core_id];
        auto& dispatcher = listeners_.empty() ? state.fallback_dispatcher : listeners_[0]->dispatcher(core_id);

        for (auto& factory : service_factories_)
        {
            auto svc = factory(state, dispatcher);
            state.services.push_back(std::move(svc));
        }

        // [D1] ServiceRegistry에 서비스 참조 등록 — on_wire에서 타입 기반 조회 가능
        for (auto& svc : state.services)
        {
            state.registry.register_ref(*svc);
        }
    }

    logger_.debug("Phase 1: on_configure");
    // ── Phase 1: on_configure — 어댑터/설정 접근 단계 ──────────────────
    // 서비스가 어댑터, 설정, per-core 상태를 받아 초기화한다.
    // 다른 서비스에 접근 불가 (ServiceRegistry 미제공).
    // io_context는 ConfigureContext에 노출하지 않고 bind_io_context()로 별도 주입.
    for (uint32_t core_id = 0; core_id < config_.num_cores; ++core_id)
    {
        auto& state = *per_core_[core_id];
        auto& io = core_engine_->io_context(core_id);
        ConfigureContext ctx{*this, core_id, state};
        for (auto& svc : state.services)
        {
            svc->bind_io_context(io); // [D7] spawn()용 io_context 주입
            svc->internal_configure(ctx);
        }
    }

    logger_.debug("Phase 2: on_wire");
    // ── Phase 2: on_wire — 서비스 간 와이어링 단계 ─────────────────────
    // 모든 코어의 서비스 인스턴스가 생성 완료. 코어 스레드 시작 전이므로
    // ServiceRegistryView를 통한 전 코어 읽기 접근이 데이터 레이스 없이 안전.
    {
        // ServiceRegistryView용 per-core 레지스트리 포인터 수집
        std::vector<ServiceRegistry*> registries;
        registries.reserve(config_.num_cores);
        for (uint32_t i = 0; i < config_.num_cores; ++i)
        {
            registries.push_back(&per_core_[i]->registry);
        }
        ServiceRegistryView global_view(std::move(registries));

        for (uint32_t core_id = 0; core_id < config_.num_cores; ++core_id)
        {
            auto& state = *per_core_[core_id];
            WireContext ctx{*this, core_id, state.registry, global_view, *state.scheduler};
            for (auto& svc : state.services)
            {
                svc->internal_wire(ctx);
            }
        }
    }

    logger_.debug("Phase 3: on_start");
    // ── Phase 3: on_start — 서비스 시작 ────────────────────────────────
    for (auto& state : per_core_)
    {
        for (auto& svc : state->services)
        {
            svc->start();
        }
    }

    // ── Phase 3.5: multi-listener handler 동기화 ─────────────────────
    // on_start()에서 등록된 모든 핸들러(default + msg_id)를 보조 리스너에 전파.
    // Phase 0(서비스 생성 직후)에서는 핸들러 미등록 → 반드시 on_start 이후 수행.
    if (listeners_.size() > 1)
    {
        for (uint32_t core_id = 0; core_id < config_.num_cores; ++core_id)
        {
            auto& dispatcher = listeners_[0]->dispatcher(core_id);
            for (size_t li = 1; li < listeners_.size(); ++li)
            {
                listeners_[li]->sync_all_handlers(core_id, dispatcher);
            }
        }
    }

    // ── Phase 3.75: Adapter-service auto-wiring [D2] ────────────────────
    // 어댑터가 서비스의 핸들러를 감지하여 자동 배선 (예: KafkaDispatchBridge)
    for (auto& adapter : adapters_)
    {
        // core 0의 서비스에 대해 와이어링 (Kafka는 단일 consumer → core 0 디스패치)
        adapter->wire_services(per_core_[0]->services, *core_engine_);
    }

    // ── 세션 종료 콜백 와이어링 ────────────────────────────────────────
    // 세션 제거 시 해당 코어의 모든 서비스에 on_session_closed 통지.
    for (auto& state : per_core_)
    {
        state->session_mgr.set_remove_callback([&services = state->services](SessionId sid) {
            for (auto& svc : services)
            {
                svc->on_session_closed(sid);
            }
        });
    }

    // Post-init callback — 서비스 라이프사이클 외부의 cross-cutting concerns 와이어링.
    // Gateway는 Task 8에서 라이프사이클 훅으로 이전 완료.
    // Auth/Chat 서비스도 마이그레이션 완료 시 제거 예정.
    if (post_init_cb_)
    {
        post_init_cb_(*this);
    }

    // Tick callback — SessionManager tick on each tick cycle (heartbeat, timing wheel)
    core_engine_->set_tick_callback([this](uint32_t core_id) {
        if (core_id < per_core_.size())
        {
            per_core_[core_id]->session_mgr.tick();
        }
    });

    logger_.debug("starting CoreEngine");
    // Start CoreEngine (non-blocking)
    core_engine_->start();

    // Start listeners (accept loops)
    for (auto& listener : listeners_)
        listener->start();

    // Register framework metrics (per-core) — after all init phases, before freeze
    for (uint32_t core_id = 0; core_id < config_.num_cores; ++core_id)
    {
        auto core_label = Labels{{"core", std::to_string(core_id)}};
        auto& sm = per_core_[core_id]->session_mgr;
        auto& cm = core_engine_->metrics(core_id);
        auto& disp = listeners_.empty() ? per_core_[core_id]->fallback_dispatcher : listeners_[0]->dispatcher(core_id);

        // SessionManager
        metrics_registry_.gauge_fn("apex_sessions_active", "Current active session count", core_label,
                                   [&sm]() { return static_cast<int64_t>(sm.active_session_count()); });
        metrics_registry_.counter_from("apex_sessions_created_total", "Total sessions created", core_label,
                                       sm.metric_sessions_created());
        metrics_registry_.counter_from("apex_sessions_timeout_total", "Total session timeouts", core_label,
                                       sm.metric_sessions_timeout());
        metrics_registry_.counter_from("apex_session_pool_heap_fallback_total", "Session pool heap fallback count",
                                       core_label, sm.metric_heap_fallback());

        // MessageDispatcher
        metrics_registry_.counter_from("apex_messages_dispatched_total", "Total messages dispatched", core_label,
                                       disp.metric_dispatched());
        metrics_registry_.counter_from("apex_handler_exceptions_total", "Total handler exceptions", core_label,
                                       disp.metric_exceptions());

        // CoreMetrics (cross-core messaging)
        metrics_registry_.counter_from("apex_crosscore_post_total", "Total cross-core posts", core_label,
                                       cm.post_total);
        metrics_registry_.counter_from("apex_crosscore_post_failures_total", "Total cross-core post failures",
                                       core_label, cm.post_failures);
    }

    // Freeze metrics registry — no more registrations allowed after init phase
    metrics_registry_.freeze();

    // Start metrics HTTP server if enabled
    if (config_.metrics.enabled)
    {
        metrics_http_server_.start(control_io_, config_.metrics.port, metrics_registry_, running_);
    }

    // I-18: Set running_ after all initialization (services, CoreEngine,
    // listeners) completes. External observers see running()==true only when
    // the server is fully ready to accept connections.
    running_.store(true, std::memory_order_release);
    logger_.info("ready — {} cores, {} listeners, {} adapters", config_.num_cores, listeners_.size(), adapters_.size());

    // Work guard keeps control_io_ alive until shutdown completes.
    auto work = boost::asio::make_work_guard(control_io_);

    // Block on control_io_ (with optional signal handling)
    if (config_.handle_signals)
    {
        boost::asio::signal_set signals(control_io_, SIGINT, SIGTERM);
        signals.async_wait([this](auto, auto) { stop(); });
        control_io_.run();
    }
    else
    {
        control_io_.run();
    }

    // Shutdown already completed inside begin_shutdown()/poll_shutdown().
    running_.store(false);
}

void Server::stop()
{
    if (stopping_.exchange(true))
        return;

    // Stop accepting new connections
    for (auto& listener : listeners_)
        listener->drain();

    // Post shutdown to control_io_ so it keeps processing IOCP completions
    // for session sockets while we drain active coroutines.
    boost::asio::post(control_io_, [this] { begin_shutdown(); });
}

void Server::begin_shutdown()
{
    logger_.info("shutdown initiated, drain_timeout={}s", config_.drain_timeout.count());
    shutdown_deadline_ = std::chrono::steady_clock::now() + config_.drain_timeout;

    // close() is posted to core threads. Since each core runs a single-threaded
    // io_context, for_each and read_loop's remove_session never execute concurrently.
    // poll_shutdown polls active_sessions_ until all read_loops exit.

    // Close all sessions on each core thread
    for (uint32_t i = 0; i < config_.num_cores; ++i)
    {
        boost::asio::post(core_engine_->io_context(i), [this, i] {
            per_core_[i]->session_mgr.for_each([](SessionPtr session) { session->close(); });
        });
    }

    // Poll until all read_loop coroutines have exited
    shutdown_timer_ = std::make_unique<boost::asio::steady_timer>(control_io_);
    poll_shutdown();
}

void Server::poll_shutdown()
{
    if (total_active_sessions() == 0)
    {
        logger_.info("All sessions drained, shutting down");
        finalize_shutdown();
        return;
    }

    // Timeout check
    if (std::chrono::steady_clock::now() >= shutdown_deadline_)
    {
        logger_.warn("Drain timeout ({}s) expired, {} sessions remaining - forcing shutdown",
                     config_.drain_timeout.count(), total_active_sessions());
        finalize_shutdown();
        return;
    }

    // Re-poll after 1ms
    shutdown_timer_->expires_after(std::chrono::milliseconds(1));
    shutdown_timer_->async_wait([this](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
            return;
        poll_shutdown();
    });
}

void Server::finalize_shutdown()
{
    logger_.debug("finalize_shutdown");
    shutdown_timer_.reset();

    // 0. Stop metrics HTTP server (no more scrape requests)
    metrics_http_server_.stop();

    // 1. Stop listeners completely
    for (auto& listener : listeners_)
        listener->stop();

    // 2. Adapter drain (새 요청 거부)
    for (auto& adapter : adapters_)
    {
        adapter->drain();
    }

    // 3. 주기적 작업 스케줄러 정지 (서비스 정지 전에 타이머 해제)
    for (auto& state : per_core_)
    {
        if (state->scheduler)
        {
            state->scheduler->stop_all();
        }
    }

    // 4. Stop services
    for (auto& state : per_core_)
    {
        for (auto& svc : state->services)
        {
            svc->stop();
        }
    }

    // 4.5 [D7] Outstanding 코루틴 drain 대기 (drain_timeout 설정 사용)
    // 서비스 코루틴(spawn()), 인프라 코루틴(spawn_tracked()), 어댑터 코루틴(spawn_adapter_coro()) 모두 대기.
    // drain(step 2)에서 어댑터 코루틴에 cancellation signal이 발행됨 → operation_aborted로 종료 중.
    {
        auto coro_deadline = std::chrono::steady_clock::now() + config_.drain_timeout;
        uint32_t total = 0;
        while (std::chrono::steady_clock::now() < coro_deadline)
        {
            total = 0;
            for (auto& state : per_core_)
            {
                for (auto& svc : state->services)
                {
                    total += svc->outstanding_coroutines();
                }
            }
            total += core_engine_->outstanding_infra_coroutines();
            for (const auto& adapter : adapters_)
                total += adapter->outstanding_adapter_coros();
            if (total == 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        if (total > 0)
        {
            logger_.warn("shutdown step 4.5: drain timeout ({} ms) expired with {} outstanding coroutines",
                         std::chrono::duration_cast<std::chrono::milliseconds>(config_.drain_timeout).count(), total);
        }
    }

    // 5. Adapter close (flush + 커넥션 정리)
    // INVARIANT: outstanding_adapter_coros == 0 (Step 4.5에서 확인됨)
    // INVARIANT: io_context 아직 실행 중 (CoreEngine stop은 Step 6)
    // INVARIANT: 새 요청 없음 (Step 2에서 DRAINING)
    // WARNING: Step 6 이후로 이동 금지 — close()의 per-core phase가 io_context에 post함
    logger_.info("shutdown step 5: closing adapters");
    for (auto& adapter : adapters_)
    {
        adapter->close();
    }

    // 6. CoreEngine stop + join + drain
    // INVARIANT: 어댑터 리소스 정리 완료 — io_context에 어댑터 참조 핸들러 없음
    // Order matters: stop() signals threads, join() waits for exit,
    // drain_remaining() cleans up leftover SPSC messages.
    logger_.info("shutdown step 6: stopping core engine");
    core_engine_->stop();
    core_engine_->join();
    core_engine_->drain_remaining();

    // 7. [D3] Global resources 정리
    globals_.clear();

    // Restore default signal handlers before caller tears down spdlog.
    // Prevents crash handler from accessing destroyed spdlog internals.
    uninstall_crash_handlers();

    // Finally stop control_io_ (causes run() to return)
    control_io_.stop();
}

uint16_t Server::port() const noexcept
{
    return listeners_.empty() ? 0 : listeners_[0]->port();
}

uint32_t Server::core_count() const noexcept
{
    return config_.num_cores;
}

bool Server::running() const noexcept
{
    return running_.load(std::memory_order_acquire);
}

boost::asio::io_context& Server::core_io_context(uint32_t core_id)
{
    return core_engine_->io_context(core_id);
}

uint32_t Server::total_active_sessions() const noexcept
{
    uint32_t total = 0;
    for (const auto& listener : listeners_)
    {
        total += listener->active_sessions();
    }
    return total;
}

} // namespace apex::core
