// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/core_engine.hpp>
#include <apex/core/thread_affinity.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>

#include <cassert>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <thread>

namespace apex::core
{

thread_local uint32_t CoreEngine::tls_core_id_ = UINT32_MAX;

// --- CoreContext ---

CoreContext::CoreContext(uint32_t id)
    : core_id(id)
{}

CoreContext::~CoreContext() = default;

// --- CoreEngine ---

CoreEngine::CoreEngine(CoreEngineConfig config)
    : config_(config)
{
    if (config_.num_cores == 0)
    {
        auto hw = std::thread::hardware_concurrency();
        config_.num_cores = (hw > 0) ? hw : 1;
    }

    // Per-core drain coalescing flags
    drain_pending_ = std::make_unique<std::atomic<bool>[]>(config_.num_cores);
    for (uint32_t i = 0; i < config_.num_cores; ++i)
    {
        drain_pending_[i].store(false, std::memory_order_relaxed);
    }

    cores_.reserve(config_.num_cores);
    for (uint32_t i = 0; i < config_.num_cores; ++i)
    {
        cores_.push_back(std::make_unique<CoreContext>(i));
    }

    // SpscMesh: N>=2 일 때만 생성 (단일 코어는 자기 자신에게 보낼 수 없음)
    if (config_.num_cores > 1)
    {
        std::vector<boost::asio::io_context*> io_ptrs;
        io_ptrs.reserve(config_.num_cores);
        for (auto& ctx : cores_)
            io_ptrs.push_back(&ctx->io_ctx);

        mesh_ = std::make_unique<SpscMesh>(config_.num_cores, config_.spsc_queue_capacity, io_ptrs);
    }
}

CoreEngine::~CoreEngine()
{
    stop();
    join();
    drain_remaining();
}

void CoreEngine::set_message_handler(MessageHandler handler)
{
    assert(!running_.load(std::memory_order_acquire) && "set_message_handler must be called before start()");
    message_handler_ = std::move(handler);
}

void CoreEngine::set_tick_callback(TickCallback callback)
{
    assert(!running_.load(std::memory_order_acquire) && "set_tick_callback must be called before start()");
    tick_callback_ = std::move(callback);
}

void CoreEngine::register_cross_core_handler(CrossCoreOp op, CrossCoreHandler handler)
{
    assert(!running_.load(std::memory_order_acquire) && "register_cross_core_handler must be called before start()");
    cross_core_dispatcher_.register_handler(op, handler);
}

void CoreEngine::drain_remaining()
{
    assert(!running_.load(std::memory_order_relaxed) && threads_.empty() &&
           "drain_remaining() must be called after stop() + join()");

    if (mesh_)
        mesh_->shutdown();
}

void CoreEngine::start()
{
    if (running_.exchange(true, std::memory_order_acq_rel))
    {
        throw std::logic_error("CoreEngine::start() called while already running");
    }

    if (!threads_.empty())
    {
        running_.store(false, std::memory_order_release);
        throw std::logic_error("CoreEngine::start() called before join()");
    }

    // Reset io_contexts and drain flags for re-run
    for (uint32_t i = 0; i < config_.num_cores; ++i)
    {
        cores_[i]->io_ctx.restart();
        drain_pending_[i].store(false, std::memory_order_relaxed);
    }

    threads_.reserve(config_.num_cores);
    for (uint32_t i = 0; i < config_.num_cores; ++i)
    {
        threads_.emplace_back([this, i]() { run_core(i); });
    }
}

void CoreEngine::join()
{
    for (auto& t : threads_)
    {
        if (t.joinable())
        {
            t.join();
        }
    }
    threads_.clear();
    running_.store(false, std::memory_order_release);
}

void CoreEngine::run()
{
    start();
    join();
}

void CoreEngine::stop()
{
    running_.store(false, std::memory_order_release);
    for (auto& ctx : cores_)
    {
        ctx->io_ctx.stop();
    }
}

Result<void> CoreEngine::post_to(uint32_t target_core, CoreMessage msg)
{
    if (target_core >= cores_.size())
    {
        // No dedicated InvalidCoreId error code — adding one requires schema changes
        // across error_code.hpp, FlatBuffers ErrorResponse, etc. ErrorCode::Unknown is
        // acceptable here; callers already handle Unknown as a generic failure.
        return error(ErrorCode::Unknown);
    }
    auto& target = *cores_[target_core];
    target.metrics.post_total.fetch_add(1, std::memory_order_relaxed);

    auto src = tls_core_id_;
    if (mesh_ && src != UINT32_MAX && src != target_core)
    {
        // Core thread → SPSC mesh (fast path, no CAS)
        logger_.trace("post_to src={} dst={} op={}", src, target_core, static_cast<uint32_t>(msg.op));
        if (!mesh_->queue(src, target_core).try_enqueue(msg))
        {
            target.metrics.post_failures.fetch_add(1, std::memory_order_relaxed);
            static thread_local auto last_log = std::chrono::steady_clock::time_point{};
            auto now = std::chrono::steady_clock::now();
            if (now - last_log > std::chrono::seconds{1})
            {
                logger_.warn("post_to src={} dst={} failed: SPSC queue full", src, target_core);
                last_log = now;
            }
            return error(ErrorCode::CrossCoreQueueFull);
        }
        schedule_drain(target_core);
        return ok();
    }

    // Non-core thread, self-post, or single-core → asio::post fallback
    logger_.trace("post_to dst={} via asio::post fallback", target_core);
    boost::asio::post(target.io_ctx, [this, target_core, msg] { dispatch_message(target_core, msg); });
    return ok();
}

boost::asio::awaitable<void> CoreEngine::co_post_to(uint32_t target_core, CoreMessage msg)
{
    if (target_core >= cores_.size())
    {
        throw std::out_of_range("co_post_to: target_core out of range");
    }
    auto src = tls_core_id_;
    assert(src != UINT32_MAX && "co_post_to must be called from a core thread");
    if (src == target_core)
    {
        throw std::logic_error("co_post_to: cannot post to self");
    }
    assert(mesh_ && "co_post_to requires SPSC mesh (num_cores >= 2)");

    cores_[target_core]->metrics.post_total.fetch_add(1, std::memory_order_relaxed);

    co_await mesh_->queue(src, target_core).enqueue(msg);
    schedule_drain(target_core);
}

void CoreEngine::schedule_drain(uint32_t target_core)
{
    if (!drain_pending_[target_core].exchange(true, std::memory_order_acq_rel))
    {
        boost::asio::post(cores_[target_core]->io_ctx, [this, target_core] {
            drain_inbox(target_core);
            drain_pending_[target_core].store(false, std::memory_order_release);
            // Re-check: messages may have arrived during drain
            if (mesh_)
            {
                for (uint32_t src = 0; src < core_count(); ++src)
                {
                    if (src == target_core)
                        continue;
                    if (!mesh_->queue(src, target_core).empty())
                    {
                        schedule_drain(target_core);
                        return;
                    }
                }
            }
        });
    }
}

void CoreEngine::broadcast(CoreMessage msg)
{
    assert(msg.op != CrossCoreOp::LegacyCrossCoreFn &&
           "broadcast() cannot be used with LegacyCrossCoreFn (raw pointer ownership)");

    for (uint32_t i = 0; i < cores_.size(); ++i)
    {
        boost::asio::post(cores_[i]->io_ctx, [this, i, msg] { dispatch_message(i, msg); });
    }
}

void CoreEngine::dispatch_message(uint32_t core_id, const CoreMessage& msg)
{
    logger_.trace("dispatch_message core={} op={} from_core={}", core_id, static_cast<uint32_t>(msg.op),
                  msg.source_core);

    if (msg.op == CrossCoreOp::LegacyCrossCoreFn)
    {
        auto* task = reinterpret_cast<std::function<void()>*>(msg.data);
        if (task)
        {
            try
            {
                (*task)();
            }
            catch (const std::exception& e)
            {
                logger_.error("cross-core task exception core={}: {}", core_id, e.what());
            }
            catch (...)
            {
                logger_.error("cross-core task unknown exception core={}", core_id);
            }
            delete task;
        }
    }
    else if (cross_core_dispatcher_.has_handler(msg.op))
    {
        cross_core_dispatcher_.dispatch(core_id, msg.source_core, msg.op, reinterpret_cast<void*>(msg.data));
    }
    else if (message_handler_)
    {
        message_handler_(core_id, msg);
    }
}

uint32_t CoreEngine::core_count() const noexcept
{
    return static_cast<uint32_t>(cores_.size());
}

boost::asio::io_context& CoreEngine::io_context(uint32_t core_id)
{
    if (core_id >= cores_.size())
    {
        throw std::out_of_range("core_id out of range");
    }
    return cores_[core_id]->io_ctx;
}

bool CoreEngine::running() const noexcept
{
    return running_.load(std::memory_order_acquire);
}

const CoreMetrics& CoreEngine::metrics(uint32_t core_id) const
{
    if (core_id >= cores_.size())
    {
        throw std::out_of_range("core_id out of range");
    }
    return cores_[core_id]->metrics;
}

uint32_t CoreEngine::current_core_id() noexcept
{
    return tls_core_id_;
}

void CoreEngine::run_core(uint32_t core_id)
{
    tls_core_id_ = core_id;

    // Apply CPU affinity and NUMA memory policy before any allocations.
    if (!config_.core_assignments.empty() && core_id < config_.core_assignments.size())
    {
        const auto& assignment = config_.core_assignments[core_id];
        if (apply_thread_affinity(assignment.logical_core_id))
        {
            logger_.info("worker[{}] pinned to logical core {}", core_id, assignment.logical_core_id);
        }

        if (config_.numa_aware)
        {
            if (apply_numa_memory_policy(assignment.numa_node))
            {
                logger_.info("worker[{}] bound to NUMA node {}", core_id, assignment.numa_node);
            }
            else
            {
                logger_.warn("worker[{}] NUMA bind failed for node {}", core_id, assignment.numa_node);
            }
        }
    }

    auto& ctx = *cores_[core_id];

    ctx.tick_timer = std::make_unique<boost::asio::steady_timer>(ctx.io_ctx);
    start_tick_timer(core_id);

    auto work_guard = boost::asio::make_work_guard(ctx.io_ctx);

    ctx.io_ctx.run();
}

void CoreEngine::drain_inbox(uint32_t core_id)
{
    if (mesh_)
    {
        mesh_->drain_all_for(core_id,
                             std::function<void(uint32_t, const CoreMessage&)>{
                                 [this](uint32_t cid, const CoreMessage& msg) { dispatch_message(cid, msg); }},
                             config_.drain_batch_limit);
    }
}

void CoreEngine::start_tick_timer(uint32_t core_id)
{
    auto& ctx = *cores_[core_id];
    ctx.tick_timer->expires_after(config_.tick_interval);
    ctx.tick_timer->async_wait([this, core_id](const boost::system::error_code& ec) {
        if (ec)
            return;

        if (tick_callback_)
        {
            try
            {
                tick_callback_(core_id);
            }
            catch (const std::exception& e)
            {
                logger_.error("tick_callback exception core={}: {}", core_id, e.what());
            }
            catch (...)
            {
                logger_.error("tick_callback unknown exception core={}", core_id);
            }
        }

        if (running_.load(std::memory_order_acquire))
        {
            start_tick_timer(core_id);
        }
    });
}

} // namespace apex::core
