#include <apex/core/core_engine.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>

#include <spdlog/spdlog.h>

#include <cassert>
#include <functional>
#include <stdexcept>
#include <thread>

namespace apex::core {

// --- CoreContext ---

CoreContext::CoreContext(uint32_t id, size_t queue_capacity)
    : core_id(id)
    , inbox(std::make_unique<MpscQueue<CoreMessage>>(queue_capacity))
{
}

CoreContext::~CoreContext() = default;

// --- CoreEngine ---

CoreEngine::CoreEngine(CoreEngineConfig config)
    : config_(config)
{
    if (config_.num_cores == 0) {
        auto hw = std::thread::hardware_concurrency();
        config_.num_cores = (hw > 0) ? hw : 1;
    }

    // Per-core drain coalescing flags
    drain_pending_ = std::make_unique<std::atomic<bool>[]>(config_.num_cores);
    for (uint32_t i = 0; i < config_.num_cores; ++i) {
        drain_pending_[i].store(false, std::memory_order_relaxed);
    }

    cores_.reserve(config_.num_cores);
    for (uint32_t i = 0; i < config_.num_cores; ++i) {
        cores_.push_back(std::make_unique<CoreContext>(i, config_.mpsc_queue_capacity));
    }
}

CoreEngine::~CoreEngine() {
    stop();
    join();
    // m-11: Drain remaining cross-core messages to prevent heap leaks.
    // After join(), no threads are running so drain_remaining() is safe.
    drain_remaining();
}

void CoreEngine::set_message_handler(MessageHandler handler) {
    assert(!running_.load(std::memory_order_acquire) && "set_message_handler must be called before start()");
    message_handler_ = std::move(handler);
}

void CoreEngine::set_tick_callback(TickCallback callback) {
    assert(!running_.load(std::memory_order_acquire) && "set_tick_callback must be called before start()");
    tick_callback_ = std::move(callback);
}

void CoreEngine::register_cross_core_handler(CrossCoreOp op, CrossCoreHandler handler) {
    assert(!running_.load(std::memory_order_acquire) && "register_cross_core_handler must be called before start()");
    cross_core_dispatcher_.register_handler(op, handler);
}

void CoreEngine::drain_remaining() {
    // I-9: Assert precondition — must be called after stop() + join()
    assert(!running_.load(std::memory_order_relaxed) && threads_.empty()
           && "drain_remaining() must be called after stop() + join()");

    for (auto& ctx : cores_) {
        while (auto msg = ctx->inbox->dequeue()) {
            if (msg->op == CrossCoreOp::LegacyCrossCoreFn) {
                auto* task = reinterpret_cast<std::function<void()>*>(msg->data);
                delete task;
            }
        }
    }
}

void CoreEngine::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        throw std::logic_error("CoreEngine::start() called while already running");
    }

    // I-1: Guard against calling start() before join() has cleared threads_
    if (!threads_.empty()) {
        running_.store(false, std::memory_order_release);
        throw std::logic_error("CoreEngine::start() called before join()");
    }

    // Reset io_contexts for re-run
    for (auto& ctx : cores_) {
        ctx->io_ctx.restart();
    }

    threads_.reserve(config_.num_cores);
    for (uint32_t i = 0; i < config_.num_cores; ++i) {
        threads_.emplace_back([this, i]() { run_core(i); });
    }
}

void CoreEngine::join() {
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    threads_.clear();
    running_.store(false, std::memory_order_release);
}

void CoreEngine::run() {
    start();
    join();
}

void CoreEngine::stop() {
    running_.store(false, std::memory_order_release);
    for (auto& ctx : cores_) {
        ctx->io_ctx.stop();
    }
}

Result<void> CoreEngine::post_to(uint32_t target_core, CoreMessage msg) {
    if (target_core >= cores_.size()) {
        return error(ErrorCode::Unknown);
    }
    auto result = cores_[target_core]->inbox->enqueue(msg);
    if (!result) {
        return error(ErrorCode::CrossCoreQueueFull);
    }
    schedule_drain(target_core);
    return ok();
}

void CoreEngine::schedule_drain(uint32_t target_core) {
    // Atomic coalescing: only one drain per batch. If drain is already pending,
    // the running drain_inbox will pick up the newly enqueued messages.
    if (!drain_pending_[target_core].exchange(true, std::memory_order_acq_rel)) {
        boost::asio::post(cores_[target_core]->io_ctx, [this, target_core] {
            drain_pending_[target_core].store(false, std::memory_order_release);
            drain_inbox(target_core);
        });
    }
}

void CoreEngine::broadcast(CoreMessage msg) {
    for (uint32_t i = 0; i < cores_.size(); ++i) {
        (void)post_to(i, msg);
    }
}

uint32_t CoreEngine::core_count() const noexcept {
    return static_cast<uint32_t>(cores_.size());
}

boost::asio::io_context& CoreEngine::io_context(uint32_t core_id) {
    if (core_id >= cores_.size()) {
        throw std::out_of_range("core_id out of range");
    }
    return cores_[core_id]->io_ctx;
}

bool CoreEngine::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

void CoreEngine::run_core(uint32_t core_id) {
    auto& ctx = *cores_[core_id];

    // Independent tick timer (heartbeat, timing wheel, etc.)
    ctx.tick_timer = std::make_unique<boost::asio::steady_timer>(ctx.io_ctx);
    start_tick_timer(core_id);

    // Work guard keeps io_context alive even when no pending work
    auto work_guard = boost::asio::make_work_guard(ctx.io_ctx);

    ctx.io_ctx.run();
}

void CoreEngine::drain_inbox(uint32_t core_id) {
    auto& core_ctx = *cores_[core_id];
    size_t processed = 0;

    while (processed < config_.drain_batch_limit) {
        auto msg = core_ctx.inbox->dequeue();
        if (!msg) break;

        if (msg->op == CrossCoreOp::LegacyCrossCoreFn) {
            // Legacy closure-based compatibility (remove after full migration)
            auto* task = reinterpret_cast<std::function<void()>*>(msg->data);
            if (task) {
                try {
                    (*task)();
                } catch (const std::exception& e) {
                    spdlog::error("Core {} cross-core task exception: {}", core_id, e.what());
                } catch (...) {
                    spdlog::error("Core {} cross-core task unknown exception", core_id);
                }
                delete task;
            }
        } else if (cross_core_dispatcher_.has_handler(msg->op)) {
            // Message passing dispatch (registered handler takes priority)
            cross_core_dispatcher_.dispatch(
                core_id, msg->source_core, msg->op,
                reinterpret_cast<void*>(msg->data));
        } else if (message_handler_) {
            // Fallback: unregistered op → legacy message_handler_ path
            message_handler_(core_id, *msg);
        }
        ++processed;
    }

    // If batch limit reached, there may be more — re-post to process remaining
    if (processed == config_.drain_batch_limit) {
        boost::asio::post(core_ctx.io_ctx, [this, core_id] {
            drain_inbox(core_id);
        });
    }
}

void CoreEngine::start_tick_timer(uint32_t core_id) {
    auto& ctx = *cores_[core_id];
    ctx.tick_timer->expires_after(config_.tick_interval);
    ctx.tick_timer->async_wait([this, core_id](const boost::system::error_code& ec) {
        if (ec) return;  // timer cancelled or error

        if (tick_callback_) {
            try {
                tick_callback_(core_id);
            } catch (const std::exception& e) {
                spdlog::error("Core {} tick_callback exception: {}", core_id, e.what());
            } catch (...) {
                spdlog::error("Core {} tick_callback unknown exception", core_id);
            }
        }

        // Re-arm the timer
        if (running_.load(std::memory_order_acquire)) {
            start_tick_timer(core_id);
        }
    });
}

} // namespace apex::core
