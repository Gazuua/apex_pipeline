#include <apex/core/server.hpp>
#include <apex/core/configure_context.hpp>
#include <apex/core/wire_context.hpp>
#include <apex/core/detail/math_utils.hpp>

#include <spdlog/spdlog.h>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>

#include <stdexcept>
#include <string>

namespace apex::core {

// --- PerCoreState ---

PerCoreState::PerCoreState(uint32_t id, uint32_t heartbeat_timeout_ticks,
                           size_t timer_wheel_slots, size_t recv_buf_capacity,
                           size_t bump_capacity, size_t arena_block, size_t arena_max)
    : core_id(id)
    , session_mgr(id, heartbeat_timeout_ticks, timer_wheel_slots, recv_buf_capacity)
    , bump_allocator(bump_capacity)
    , arena_allocator(arena_block, arena_max)
{
}

// --- Server ---

Server::Server(ServerConfig config)
    : config_(config)
{
    if (config_.heartbeat_timeout_ticks > 0) {
        size_t actual_slots = detail::next_power_of_2(
            config_.timer_wheel_slots < 1 ? 1 : config_.timer_wheel_slots);
        if (config_.heartbeat_timeout_ticks >= actual_slots) {
            throw std::invalid_argument(
                "ServerConfig::heartbeat_timeout_ticks (" +
                std::to_string(config_.heartbeat_timeout_ticks) +
                ") must be < timer_wheel effective slots (" +
                std::to_string(actual_slots) + ")");
        }
    }

    // CoreEngine
    CoreEngineConfig engine_config{
        .num_cores = config_.num_cores,
        .mpsc_queue_capacity = config_.mpsc_queue_capacity,
        .tick_interval = config_.tick_interval,
    };
    core_engine_ = std::make_unique<CoreEngine>(engine_config);

    // Sync num_cores with CoreEngine's resolved value.
    // CoreEngine normalizes 0 → hardware_concurrency, so we must use
    // the actual core count to avoid division-by-zero UB.
    config_.num_cores = core_engine_->core_count();

    // PerCoreState (no longer contains ConnectionHandler/MessageDispatcher)
    for (uint32_t i = 0; i < config_.num_cores; ++i) {
        per_core_.push_back(std::make_unique<PerCoreState>(
            i, config_.heartbeat_timeout_ticks,
            config_.timer_wheel_slots, config_.recv_buf_capacity,
            config_.bump_capacity_bytes, config_.arena_block_bytes,
            config_.arena_max_bytes));
    }

    // Acceptor binding moved to Listener<P>::start()
}

Server::~Server() {
    for (auto& state : per_core_) {
        for (auto& svc : state->services) {
            if (svc->started()) {
                svc->stop();
            }
        }
    }
}

void Server::run() {
    // I-21: run() is single-use. Reject re-entry after first call.
    if (run_count_.fetch_add(1, std::memory_order_acq_rel) != 0) {
        throw std::logic_error("Server::run() must not be called more than once");
    }

    stopping_.store(false, std::memory_order_relaxed);  // run() init, single thread

    // I-09: Cache spdlog logger for hot-path use (avoid mutex per call)
    logger_ = spdlog::get("apex");

    control_io_.restart();

    // I-2: Clear leftover services from a previous run() cycle to prevent
    // double-start. Services are stopped in poll_shutdown(), but clearing
    // here guards against premature stop() before all services started.
    for (auto& state : per_core_) {
        state->services.clear();
    }

    // 어댑터 초기화 — 서비스보다 먼저 (어댑터는 서비스의 인프라 의존성)
    for (auto& adapter : adapters_) {
        adapter->init(*core_engine_);
    }

    // PeriodicTaskScheduler 초기화 — per-core io_context 기반
    for (uint32_t core_id = 0; core_id < config_.num_cores; ++core_id) {
        per_core_[core_id]->scheduler = std::make_unique<PeriodicTaskScheduler>(
            core_engine_->io_context(core_id));
    }

    // Per-core 서비스 인스턴스 생성
    // 리스너가 있으면 리스너의 dispatcher를, 없으면 fallback_dispatcher를 사용 (Kafka-only 서비스).
    for (uint32_t core_id = 0; core_id < config_.num_cores; ++core_id) {
        auto& state = *per_core_[core_id];
        auto& dispatcher = listeners_.empty()
            ? state.fallback_dispatcher
            : listeners_[0]->dispatcher(core_id);

        for (auto& factory : service_factories_) {
            auto svc = factory(state, dispatcher);
            state.services.push_back(std::move(svc));
        }
    }

    // ── Phase 1: on_configure — 어댑터/설정 접근 단계 ──────────────────
    // 서비스가 어댑터, 설정, per-core 상태를 받아 초기화한다.
    // 다른 서비스에 접근 불가 (ServiceRegistry 미제공).
    for (uint32_t core_id = 0; core_id < config_.num_cores; ++core_id) {
        auto& state = *per_core_[core_id];
        ConfigureContext ctx{*this, core_id, state};
        for (auto& svc : state.services) {
            svc->internal_configure(ctx);
        }
    }

    // ── Phase 2: on_wire — 서비스 간 와이어링 단계 ─────────────────────
    // 모든 코어의 서비스 인스턴스가 생성 완료. 코어 스레드 시작 전이므로
    // ServiceRegistryView를 통한 전 코어 읽기 접근이 데이터 레이스 없이 안전.
    {
        // ServiceRegistryView용 per-core 레지스트리 포인터 수집
        std::vector<ServiceRegistry*> registries;
        registries.reserve(config_.num_cores);
        for (uint32_t i = 0; i < config_.num_cores; ++i) {
            registries.push_back(&per_core_[i]->registry);
        }
        ServiceRegistryView global_view(std::move(registries));

        for (uint32_t core_id = 0; core_id < config_.num_cores; ++core_id) {
            auto& state = *per_core_[core_id];
            WireContext ctx{
                *this, core_id,
                state.registry, global_view,
                *state.scheduler
            };
            for (auto& svc : state.services) {
                svc->internal_wire(ctx);
            }
        }
    }

    // ── Phase 3: on_start — 서비스 시작 ────────────────────────────────
    for (auto& state : per_core_) {
        for (auto& svc : state->services) {
            svc->start();
        }
    }

    // ── Phase 3.5: multi-listener handler 동기화 ─────────────────────
    // on_start()에서 등록된 default_handler를 보조 리스너에 전파.
    // Phase 0(서비스 생성 직후)에서는 핸들러 미등록 → 반드시 on_start 이후 수행.
    if (listeners_.size() > 1) {
        for (uint32_t core_id = 0; core_id < config_.num_cores; ++core_id) {
            auto& dispatcher = listeners_[0]->dispatcher(core_id);
            for (size_t li = 1; li < listeners_.size(); ++li) {
                listeners_[li]->sync_default_handler(core_id, dispatcher);
            }
        }
    }

    // ── 세션 종료 콜백 와이어링 ────────────────────────────────────────
    // 세션 제거 시 해당 코어의 모든 서비스에 on_session_closed 통지.
    for (auto& state : per_core_) {
        state->session_mgr.set_remove_callback(
            [&services = state->services](SessionId sid) {
                for (auto& svc : services) {
                    svc->on_session_closed(sid);
                }
            });
    }

    // Post-init callback — 서비스 라이프사이클 외부의 cross-cutting concerns 와이어링.
    // Gateway는 Task 8에서 라이프사이클 훅으로 이전 완료.
    // Auth/Chat 서비스도 마이그레이션 완료 시 제거 예정.
    if (post_init_cb_) {
        post_init_cb_(*this);
    }

    // Tick callback — SessionManager tick on each tick cycle (heartbeat, timing wheel)
    core_engine_->set_tick_callback([this](uint32_t core_id) {
        if (core_id < per_core_.size()) {
            per_core_[core_id]->session_mgr.tick();
        }
    });

    // Start CoreEngine (non-blocking)
    core_engine_->start();

    // Start listeners (accept loops)
    for (auto& listener : listeners_) listener->start();

    // I-18: Set running_ after all initialization (services, CoreEngine,
    // listeners) completes. External observers see running()==true only when
    // the server is fully ready to accept connections.
    running_.store(true, std::memory_order_release);

    // Work guard keeps control_io_ alive until shutdown completes.
    auto work = boost::asio::make_work_guard(control_io_);

    // Block on control_io_ (with optional signal handling)
    if (config_.handle_signals) {
        boost::asio::signal_set signals(control_io_, SIGINT, SIGTERM);
        signals.async_wait([this](auto, auto) { stop(); });
        control_io_.run();
    } else {
        control_io_.run();
    }

    // Shutdown already completed inside begin_shutdown()/poll_shutdown().
    running_.store(false);
}

void Server::stop() {
    if (stopping_.exchange(true)) return;

    // Stop accepting new connections
    for (auto& listener : listeners_) listener->drain();

    // Post shutdown to control_io_ so it keeps processing IOCP completions
    // for session sockets while we drain active coroutines.
    boost::asio::post(control_io_, [this] { begin_shutdown(); });
}

void Server::begin_shutdown() {
    shutdown_deadline_ = std::chrono::steady_clock::now() + config_.drain_timeout;

    // close() is posted to core threads. Since each core runs a single-threaded
    // io_context, for_each and read_loop's remove_session never execute concurrently.
    // poll_shutdown polls active_sessions_ until all read_loops exit.

    // Close all sessions on each core thread
    for (uint32_t i = 0; i < config_.num_cores; ++i) {
        boost::asio::post(core_engine_->io_context(i),
            [this, i] {
                per_core_[i]->session_mgr.for_each([](SessionPtr session) {
                    session->close();
                });
            });
    }

    // Poll until all read_loop coroutines have exited
    shutdown_timer_ = std::make_unique<boost::asio::steady_timer>(control_io_);
    poll_shutdown();
}

void Server::poll_shutdown() {
    if (total_active_sessions() == 0) {
        if (logger_) {
            logger_->info("All sessions drained, shutting down");
        }
        finalize_shutdown();
        return;
    }

    // Timeout check
    if (std::chrono::steady_clock::now() >= shutdown_deadline_) {
        if (logger_) {
            logger_->warn(
                "Drain timeout ({}s) expired, {} sessions remaining - forcing shutdown",
                config_.drain_timeout.count(),
                total_active_sessions());
        }
        finalize_shutdown();
        return;
    }

    // Re-poll after 1ms
    shutdown_timer_->expires_after(std::chrono::milliseconds(1));
    shutdown_timer_->async_wait([this](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted) return;
        poll_shutdown();
    });
}

void Server::finalize_shutdown() {
    shutdown_timer_.reset();

    // 1. Stop listeners completely
    for (auto& listener : listeners_) listener->stop();

    // 2. Adapter drain (새 요청 거부)
    for (auto& adapter : adapters_) {
        adapter->drain();
    }

    // 3. 주기적 작업 스케줄러 정지 (서비스 정지 전에 타이머 해제)
    for (auto& state : per_core_) {
        if (state->scheduler) {
            state->scheduler->stop_all();
        }
    }

    // 4. Stop services
    for (auto& state : per_core_) {
        for (auto& svc : state->services) {
            svc->stop();
        }
    }

    // 5. CoreEngine stop + join + drain
    // Order matters: stop() signals threads, join() waits for exit,
    // drain_remaining() cleans up leftover MPSC messages.
    // CoreEngine must stop before adapter close -- pending completion handlers
    // on core threads may reference adapter resources (e.g. KafkaConsumer).
    core_engine_->stop();
    core_engine_->join();
    core_engine_->drain_remaining();

    // 6. Adapter close (flush + 커넥션 정리)
    // Safe now: all core threads have exited, no pending handlers.
    for (auto& adapter : adapters_) {
        adapter->close();
    }

    // Finally stop control_io_ (causes run() to return)
    control_io_.stop();
}

uint16_t Server::port() const noexcept {
    return listeners_.empty() ? 0 : listeners_[0]->port();
}

uint32_t Server::core_count() const noexcept {
    return config_.num_cores;
}

bool Server::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

boost::asio::io_context& Server::core_io_context(uint32_t core_id) {
    return core_engine_->io_context(core_id);
}

uint32_t Server::total_active_sessions() const noexcept {
    uint32_t total = 0;
    for (const auto& listener : listeners_) {
        total += listener->active_sessions();
    }
    return total;
}

} // namespace apex::core
