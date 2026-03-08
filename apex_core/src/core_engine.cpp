#include <apex/core/core_engine.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>

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

    cores_.reserve(config_.num_cores);
    for (uint32_t i = 0; i < config_.num_cores; ++i) {
        cores_.push_back(std::make_unique<CoreContext>(i, config_.mpsc_queue_capacity));
    }
}

CoreEngine::~CoreEngine() {
    stop();
    join();
}

void CoreEngine::set_message_handler(MessageHandler handler) {
    assert(!running_.load(std::memory_order_acquire) && "set_message_handler must be called before start()");
    message_handler_ = std::move(handler);
}

void CoreEngine::set_drain_callback(DrainCallback callback) {
    assert(!running_.load(std::memory_order_acquire) && "set_drain_callback must be called before start()");
    drain_callback_ = std::move(callback);
}

void CoreEngine::drain_remaining() {
    for (auto& ctx : cores_) {
        while (auto msg = ctx->inbox->dequeue()) {
            if (msg->type == CoreMessage::Type::CrossCoreRequest ||
                msg->type == CoreMessage::Type::CrossCorePost) {
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

    // 재호출 시 io_context 리셋
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

bool CoreEngine::post_to(uint32_t target_core, CoreMessage msg) {
    if (target_core >= cores_.size()) {
        return false;
    }
    auto result = cores_[target_core]->inbox->enqueue(msg);
    return result.has_value();
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

    // Create drain timer
    ctx.drain_timer = std::make_unique<boost::asio::steady_timer>(ctx.io_ctx);
    start_drain_timer(core_id);

    // Work guard keeps io_context alive even when no pending work
    auto work_guard = boost::asio::make_work_guard(ctx.io_ctx);

    ctx.io_ctx.run();
}

void CoreEngine::start_drain_timer(uint32_t core_id) {
    auto& ctx = *cores_[core_id];
    ctx.drain_timer->expires_after(config_.drain_interval);
    ctx.drain_timer->async_wait([this, core_id](const boost::system::error_code& ec) {
        if (ec) {
            return;  // timer cancelled or error
        }

        auto& core_ctx = *cores_[core_id];

        // Drain all pending messages from the inbox
        while (auto msg = core_ctx.inbox->dequeue()) {
            // Built-in handling for cross-core tasks
            if (msg->type == CoreMessage::Type::CrossCoreRequest ||
                msg->type == CoreMessage::Type::CrossCorePost) {
                auto* task = reinterpret_cast<std::function<void()>*>(msg->data);
                if (task) {
                    (*task)();
                    delete task;
                }
                continue;
            }
            if (message_handler_) {
                message_handler_(core_id, *msg);
            }
        }

        // Drain callback (tick, etc.)
        if (drain_callback_) {
            drain_callback_(core_id);
        }

        // Re-arm the timer
        if (running_.load(std::memory_order_acquire)) {
            start_drain_timer(core_id);
        }
    });
}

} // namespace apex::core
