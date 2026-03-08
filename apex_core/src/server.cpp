#include <apex/core/server.hpp>
#include <apex/core/detail/math_utils.hpp>
#include <apex/core/error_sender.hpp>
#include <apex/core/tcp_binary_protocol.hpp>

#include <spdlog/spdlog.h>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace apex::core {

// --- PerCoreState ---

PerCoreState::PerCoreState(uint32_t id, uint32_t heartbeat_timeout_ticks,
                           size_t timer_wheel_slots, size_t recv_buf_capacity)
    : core_id(id)
    , session_mgr(id, heartbeat_timeout_ticks, timer_wheel_slots, recv_buf_capacity)
{
}

// --- Server ---

Server::Server(ServerConfig config)
    : config_(std::move(config))
{
    if (config_.recv_buf_capacity < TMP_BUF_SIZE) {
        throw std::invalid_argument(
            "ServerConfig::recv_buf_capacity must be >= " +
            std::to_string(TMP_BUF_SIZE));
    }

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
        .drain_interval = config_.drain_interval,
    };
    core_engine_ = std::make_unique<CoreEngine>(engine_config);

    // Sync num_cores with CoreEngine's resolved value.
    // CoreEngine normalizes 0 → hardware_concurrency, so we must use
    // the actual core count to avoid division-by-zero UB in on_accept().
    config_.num_cores = core_engine_->core_count();

    // PerCoreState
    for (uint32_t i = 0; i < config_.num_cores; ++i) {
        per_core_.push_back(std::make_unique<PerCoreState>(
            i, config_.heartbeat_timeout_ticks,
            config_.timer_wheel_slots, config_.recv_buf_capacity));
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

    stopping_.store(false);

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

    // Drain callback — SessionManager tick on each drain cycle
    core_engine_->set_drain_callback([this](uint32_t core_id) {
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
    if (active_sessions_.load(std::memory_order_acquire) == 0) {
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
                active_sessions_.load(std::memory_order_acquire));
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

void Server::on_accept(boost::asio::ip::tcp::socket socket) {
    // C-06: Set TCP_NODELAY before moving socket to core thread.
    // Disabling Nagle's algorithm eliminates up to 40ms coalescing delay.
    if (config_.tcp_nodelay) {
        boost::system::error_code ec;
        socket.set_option(boost::asio::ip::tcp::no_delay(true), ec);
        // Non-fatal: proceed even if option fails (e.g., non-TCP socket in tests)
    }

    // C-01 fix: Single fetch_add determines the target core for both the
    // socket move and the session creation. No separate context_provider,
    // so there is no TOCTOU window for core index mismatch.
    uint32_t core_id = next_core_.fetch_add(1, std::memory_order_relaxed)
                       % config_.num_cores;

    auto& core_io = core_engine_->io_context(core_id);
    boost::asio::post(core_io, [this, core_id,
                                s = std::move(socket)]() mutable {
        auto session = per_core_[core_id]->session_mgr.create_session(std::move(s));
        boost::asio::co_spawn(
            core_engine_->io_context(core_id),
            read_loop(std::move(session), core_id),
            boost::asio::detached);
    });
}

boost::asio::awaitable<void> Server::read_loop(SessionPtr session,
                                                uint32_t core_id) {
    active_sessions_.fetch_add(1, std::memory_order_relaxed);
    auto& session_mgr = per_core_[core_id]->session_mgr;
    std::array<uint8_t, TMP_BUF_SIZE> tmp_buf;

    while (session->is_open()) {
        auto [ec, n] = co_await session->socket().async_read_some(
            boost::asio::buffer(tmp_buf),
            boost::asio::as_tuple(boost::asio::use_awaitable));
        if (ec || n == 0) break;

        // I-4: Use RingBuffer::write() which handles wrap-around internally
        auto& rb = session->recv_buffer();
        if (!rb.write(std::span<const uint8_t>(tmp_buf.data(), n))) {
            session->close();
            break;
        }

        co_await process_frames(session, core_id);

        if (!session->is_open()) break;

        session_mgr.touch_session(session->id());
    }

    session_mgr.remove_session(session->id());

    // Safety note: fetch_sub must be the last operation in this coroutine.
    // poll_shutdown() checks active_sessions_==0 to decide when to call
    // core_engine_->stop(). Because this coroutine runs on a core io_context
    // and stop() is posted to accept_io_, the core's io_context continues
    // running long enough for this coroutine frame to be destroyed naturally
    // after co_return. The sequencing is: fetch_sub → co_return (frame
    // destroyed by executor) → poll_shutdown observes 0 → posts stop().
    active_sessions_.fetch_sub(1, std::memory_order_release);
}

boost::asio::awaitable<void> Server::process_frames(SessionPtr session,
                                                     uint32_t core_id) {
    auto& recv_buf = session->recv_buffer();
    auto& dispatcher = per_core_[core_id]->dispatcher;

    // C-05: Inline small-buffer optimization — avoid heap allocation for
    // payloads <= 4KB (the common case). Only fall back to std::vector
    // when the payload exceeds the stack buffer.
    constexpr size_t kSmallPayloadThreshold = 4096;
    std::array<uint8_t, kSmallPayloadThreshold> stack_buf;

    for (;;) {
        // C-3: Check decode result explicitly for error classification.
        // InsufficientData is normal (wait for more data), other errors
        // are protocol violations requiring session termination.
        auto decode_result = TcpBinaryProtocol::try_decode(recv_buf);
        if (!decode_result) {
            if (decode_result.error() != FrameError::InsufficientData) {
                session->close();
                co_return;
            }
            break;  // InsufficientData — wait for more data
        }
        auto& frame = *decode_result;

        // C-2: Copy payload before consume_frame() to avoid dangling span.
        // payload points into RingBuffer memory; consume_frame() advances
        // the read position, potentially allowing overwrite on next recv.
        auto header = frame.header;

        // C-05: SmallBuffer — stack for <= 4KB, heap for larger payloads
        std::vector<uint8_t> heap_buf;
        std::span<const uint8_t> payload_span;
        if (frame.payload.size() <= kSmallPayloadThreshold) {
            std::memcpy(stack_buf.data(), frame.payload.data(),
                        frame.payload.size());
            payload_span = {stack_buf.data(), frame.payload.size()};
        } else {
            heap_buf.assign(frame.payload.begin(), frame.payload.end());
            payload_span = heap_buf;
        }
        TcpBinaryProtocol::consume_frame(recv_buf, frame);

        auto result = co_await dispatcher.dispatch(
            session, header.msg_id, payload_span);

        if (!result.has_value()) {
            ErrorCode code = (result.error() == DispatchError::UnknownMessage)
                ? ErrorCode::HandlerNotFound
                : ErrorCode::Unknown;
            auto error_frame = ErrorSender::build_error_frame(
                header.msg_id, code);
            (void)co_await session->async_send_raw(error_frame);
        } else if (!result.value().has_value()) {
            auto error_frame = ErrorSender::build_error_frame(
                header.msg_id, result.value().error());
            (void)co_await session->async_send_raw(error_frame);
        }

        if (!session->is_open()) break;
    }
}

} // namespace apex::core
