// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/core_message.hpp>
#include <apex/core/cross_core_dispatcher.hpp>
#include <apex/core/cross_core_op.hpp>
#include <apex/core/result.hpp>
#include <apex/core/scoped_logger.hpp>
#include <apex/core/spsc_mesh.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace apex::core
{

/// Per-core metric counters for inter-core messaging diagnostics.
/// All counters use relaxed ordering — values are approximate but low-overhead.
struct CoreMetrics
{
    std::atomic<uint64_t> post_total{0};
    std::atomic<uint64_t> post_failures{0};
};

/// Per-core execution context. Each core owns its own io_context.
/// NOT thread-safe -- only accessed by the owning core thread.
struct CoreContext
{
    uint32_t core_id;
    boost::asio::io_context io_ctx{1}; // concurrency_hint=1 (single thread)
    std::unique_ptr<boost::asio::steady_timer> tick_timer;
    CoreMetrics metrics;

    explicit CoreContext(uint32_t id);
    ~CoreContext();

    CoreContext(const CoreContext&) = delete;
    CoreContext& operator=(const CoreContext&) = delete;
};

/// Per-worker core assignment for CPU affinity.
struct CoreAssignment
{
    uint32_t logical_core_id; ///< OS logical processor ID for SetThreadAffinityMask / pthread_setaffinity_np
    uint32_t numa_node{0};    ///< NUMA node for set_mempolicy
};

/// Configuration for CoreEngine.
struct CoreEngineConfig
{
    uint32_t num_cores{0}; // 0 = auto-detect (hardware_concurrency)
    size_t spsc_queue_capacity{1024};
    std::chrono::milliseconds tick_interval{100}; // per-core tick timer interval
    size_t drain_batch_limit{1024};               // max messages per drain cycle

    /// Per-worker affinity assignments. Empty = no affinity (legacy behavior).
    /// Size must equal num_cores when non-empty; assignments[i] pins worker i.
    std::vector<CoreAssignment> core_assignments;
    bool numa_aware{true}; ///< Apply NUMA memory policy (Linux only)
};

/// io_context-per-core engine. Creates N cores, each with its own
/// io_context + thread + SPSC mesh. Provides inter-core messaging.
///
/// Drain is event-driven: post_to() triggers immediate drain via post().
/// Tick is independent: periodic timer for heartbeat/timing wheel.
///
/// Usage:
///   CoreEngine engine({.num_cores = 4});
///   engine.set_message_handler([](uint32_t core_id, const CoreMessage& msg) { ... });
///   engine.run();   // blocks until stop()
///   // from another thread:
///   engine.stop();
class CoreEngine
{
  public:
    using MessageHandler = std::function<void(uint32_t core_id, const CoreMessage& msg)>;
    using TickCallback = std::function<void(uint32_t core_id)>;

    explicit CoreEngine(CoreEngineConfig config = {});
    ~CoreEngine();

    CoreEngine(const CoreEngine&) = delete;
    CoreEngine& operator=(const CoreEngine&) = delete;

    /// Set handler for inter-core messages. Must be called before start().
    void set_message_handler(MessageHandler handler);

    /// Set callback invoked on each tick cycle per core (heartbeat, timing wheel, etc.).
    void set_tick_callback(TickCallback callback);

    /// Register a cross-core message handler by op code. Must be called before start().
    void register_cross_core_handler(CrossCoreOp op, CrossCoreHandler handler);

    /// Drain remaining messages, cleaning up heap pointers. Call after stop() + join().
    void drain_remaining();

    /// Start all core threads (non-blocking).
    void start();

    /// Wait for all core threads to finish (blocking).
    void join();

    /// Start all core threads and block until stop() is called.
    void run();

    /// Signal all cores to stop. Thread-safe.
    void stop();

    /// Post a message to a specific core. Thread-safe.
    /// From core thread: uses SPSC mesh (fast path).
    /// From non-core thread: uses asio::post (fallback).
    /// @return ErrorCode::CrossCoreQueueFull if SPSC queue is full.
    [[nodiscard]] Result<void> post_to(uint32_t target_core, CoreMessage msg);

    /// Awaitable post — core thread only, with backpressure.
    /// Suspends if SPSC queue is full, resumes when space available.
    [[nodiscard]] boost::asio::awaitable<void> co_post_to(uint32_t target_core, CoreMessage msg);

    /// Broadcast a message to all cores via asio::post. Thread-safe. Best-effort.
    /// LegacyCrossCoreFn is NOT supported (assert).
    void broadcast(CoreMessage msg);

    [[nodiscard]] uint32_t core_count() const noexcept;
    [[nodiscard]] boost::asio::io_context& io_context(uint32_t core_id);
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] const CoreMetrics& metrics(uint32_t core_id) const;

    /// Current core ID via thread-local. Returns UINT32_MAX if not on a core thread.
    [[nodiscard]] static uint32_t current_core_id() noexcept;

    /// [D7] Tracked 인프라 코루틴 스폰.
    template <typename F> void spawn_tracked(uint32_t core_id, F&& coro_factory)
    {
        assert(core_id < core_count() && "Invalid core_id for spawn_tracked");
        logger_.debug("spawn_tracked infra coroutine core={}", core_id);
        outstanding_infra_coros_.fetch_add(1, std::memory_order_acq_rel);
        boost::asio::co_spawn(
            io_context(core_id),
            [this, core_id, f = std::forward<F>(coro_factory)]() -> boost::asio::awaitable<void> {
                try
                {
                    co_await f();
                }
                catch (const std::exception& e)
                {
                    logger_.error("spawn_tracked coroutine exception core={}: {}", core_id, e.what());
                }
                catch (...)
                {
                    logger_.error("spawn_tracked coroutine unknown exception core={}", core_id);
                }
                outstanding_infra_coros_.fetch_sub(1, std::memory_order_acq_rel);
            },
            boost::asio::detached);
    }

    [[nodiscard]] uint32_t outstanding_infra_coroutines() const noexcept
    {
        return outstanding_infra_coros_.load(std::memory_order_acquire);
    }

  private:
    void run_core(uint32_t core_id);
    void drain_inbox(uint32_t core_id);
    void start_tick_timer(uint32_t core_id);
    void schedule_drain(uint32_t target_core);
    void dispatch_message(uint32_t core_id, const CoreMessage& msg);

    ScopedLogger logger_{"CoreEngine", ScopedLogger::NO_CORE};
    CoreEngineConfig config_;
    std::vector<std::unique_ptr<CoreContext>> cores_;
    std::vector<std::thread> threads_;
    MessageHandler message_handler_;
    TickCallback tick_callback_;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> outstanding_infra_coros_{0};
    std::unique_ptr<std::atomic<bool>[]> drain_pending_;
    std::unique_ptr<SpscMesh> mesh_;
    CrossCoreDispatcher cross_core_dispatcher_;
    static thread_local uint32_t tls_core_id_;
};

} // namespace apex::core
