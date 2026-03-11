#include <apex/core/server.hpp>
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
                           ConnectionHandlerConfig handler_config)
    : core_id(id)
    , session_mgr(id, heartbeat_timeout_ticks, timer_wheel_slots, recv_buf_capacity)
    , handler(session_mgr, dispatcher, handler_config)
{
}

// --- Server ---

Server::Server(ServerConfig config)
    : config_(std::move(config))
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
    // the actual core count to avoid division-by-zero UB in on_accept().
    config_.num_cores = core_engine_->core_count();

    // PerCoreState
    ConnectionHandlerConfig handler_config{
        .tcp_nodelay = config_.tcp_nodelay,
    };
    for (uint32_t i = 0; i < config_.num_cores; ++i) {
        per_core_.push_back(std::make_unique<PerCoreState>(
            i, config_.heartbeat_timeout_ticks,
            config_.timer_wheel_slots, config_.recv_buf_capacity,
            handler_config));
    }

    // TcpAcceptor — bind immediately so port() works.
    // C-01 fix: No context_provider. Sockets are accepted on accept_io_ and
    // then moved to the target core's io_context in on_accept() via post().
    // This eliminates the TOCTOU race between context_provider's load() and
    // on_accept()'s fetch_add() that could cause core index mismatch.
    acceptor_ = std::make_unique<TcpAcceptor>(
        accept_io_, config_.port,
        [this](boost::asio::ip::tcp::socket socket) {
            on_accept(std::move(socket));
        });
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

    accept_io_.restart();

    // I-2: Clear leftover services from a previous run() cycle to prevent
    // double-start. Services are stopped in poll_shutdown(), but clearing
    // here guards against premature stop() before all services started.
    for (auto& state : per_core_) {
        state->services.clear();
    }

    // Per-core service instances
    for (uint32_t core_id = 0; core_id < config_.num_cores; ++core_id) {
        auto& state = *per_core_[core_id];
        for (auto& factory : service_factories_) {
            auto svc = factory(state);
            svc->start();
            state.services.push_back(std::move(svc));
        }
    }

    // Tick callback — SessionManager tick on each tick cycle (heartbeat, timing wheel)
    core_engine_->set_tick_callback([this](uint32_t core_id) {
        if (core_id < per_core_.size()) {
            per_core_[core_id]->session_mgr.tick();
        }
    });

    // Start CoreEngine (non-blocking)
    core_engine_->start();

    // Start accept loop
    acceptor_->start();

    // I-18: Set running_ after all initialization (services, CoreEngine,
    // acceptor) completes. External observers see running()==true only when
    // the server is fully ready to accept connections.
    running_.store(true, std::memory_order_release);

    // Work guard keeps accept_io_ alive until shutdown completes.
    // Sockets are accepted on accept_io_ then moved to per-core io_contexts
    // in on_accept(). accept_io_ still needs to run for:
    // - the accept loop coroutine itself
    // - signal handling
    // - shutdown coordination (begin_shutdown/poll_shutdown)
    auto work = boost::asio::make_work_guard(accept_io_);

    // Block on accept_io_ (with optional signal handling)
    if (config_.handle_signals) {
        boost::asio::signal_set signals(accept_io_, SIGINT, SIGTERM);
        signals.async_wait([this](auto, auto) { stop(); });
        accept_io_.run();
    } else {
        accept_io_.run();
    }

    // Shutdown already completed inside begin_shutdown()/poll_shutdown().
    running_.store(false);
}

void Server::stop() {
    if (stopping_.exchange(true)) return;

    if (acceptor_) acceptor_->stop();

    // Post shutdown to accept_io_ so it keeps processing IOCP completions
    // for session sockets while we drain active coroutines.
    boost::asio::post(accept_io_, [this] { begin_shutdown(); });
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
    shutdown_timer_ = std::make_unique<boost::asio::steady_timer>(accept_io_);
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

    // All coroutines exited (or timeout) — safe to stop core engine.
    // Order matters: stop() signals threads, join() waits for exit,
    // drain_remaining() cleans up leftover MPSC messages.
    core_engine_->stop();
    core_engine_->join();
    core_engine_->drain_remaining();

    // Stop services
    for (auto& state : per_core_) {
        for (auto& svc : state->services) {
            svc->stop();
        }
    }

    // Finally stop accept_io_ (causes run() to return)
    accept_io_.stop();
}

uint16_t Server::port() const noexcept {
    return acceptor_ ? acceptor_->port() : 0;
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
    for (const auto& state : per_core_) {
        total += state->handler.active_sessions();
    }
    return total;
}

void Server::on_accept(boost::asio::ip::tcp::socket socket) {
    uint32_t core_id = next_core_.fetch_add(1, std::memory_order_relaxed)
                       % config_.num_cores;

    auto& core_io = core_engine_->io_context(core_id);
    boost::asio::post(core_io, [this, core_id,
                                s = std::move(socket)]() mutable {
        per_core_[core_id]->handler.accept_connection(
            std::move(s), core_engine_->io_context(core_id));
    });
}

} // namespace apex::core
