#pragma once

#include <apex/core/cross_core_dispatcher.hpp>
#include <apex/core/cross_core_op.hpp>
#include <apex/core/mpsc_queue.hpp>
#include <apex/core/result.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace apex::core {

/// Per-core metric counters for inter-core messaging diagnostics.
/// All counters use relaxed ordering — values are approximate but low-overhead.
struct CoreMetrics {
    std::atomic<uint64_t> post_total{0};
    std::atomic<uint64_t> post_failures{0};
};

/// Trivially-copyable message for inter-core communication via MpscQueue.
struct CoreMessage {
    CrossCoreOp op{CrossCoreOp::Noop};
    uint32_t source_core{0};
    uintptr_t data{0};
};
static_assert(std::is_trivially_copyable_v<CoreMessage>);
static_assert(sizeof(CoreMessage) <= 16);

/// Per-core execution context. Each core owns its own io_context and MPSC inbox.
/// NOT thread-safe -- only accessed by the owning core thread.
struct CoreContext {
    uint32_t core_id;
    boost::asio::io_context io_ctx{1};  // concurrency_hint=1 (single thread)
    std::unique_ptr<MpscQueue<CoreMessage>> inbox;
    std::unique_ptr<boost::asio::steady_timer> tick_timer;
    CoreMetrics metrics;

    CoreContext(uint32_t id, size_t queue_capacity);
    ~CoreContext();

    CoreContext(const CoreContext&) = delete;
    CoreContext& operator=(const CoreContext&) = delete;
};

/// Configuration for CoreEngine.
struct CoreEngineConfig {
    uint32_t num_cores{0};              // 0 = auto-detect (hardware_concurrency)
    size_t mpsc_queue_capacity{65536};
    std::chrono::milliseconds tick_interval{100};  // per-core tick timer interval
    size_t drain_batch_limit{1024};                // max messages per drain cycle
};

/// io_context-per-core engine. Creates N cores, each with its own
/// io_context + thread + MPSC inbox. Provides inter-core messaging.
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
class CoreEngine {
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

    /// Drain remaining messages from all inboxes, cleaning up heap pointers
    /// for LegacyCrossCoreFn messages. Call after stop() + join().
    void drain_remaining();

    /// Start all core threads (non-blocking).
    /// Must call join() before destruction.
    void start();

    /// Wait for all core threads to finish (blocking).
    /// Call stop() first to signal threads to exit.
    void join();

    /// Start all core threads and block until stop() is called.
    /// Equivalent to start() + join().
    void run();

    /// Signal all cores to stop. Thread-safe.
    void stop();

    /// Post a message to a specific core's MPSC inbox. Thread-safe.
    /// Triggers immediate event-driven drain on the target core.
    /// @return ErrorCode::CrossCoreQueueFull if target core's queue is full.
    [[nodiscard]] Result<void> post_to(uint32_t target_core, CoreMessage msg);

    /// Broadcast a message to all cores. Thread-safe. Best-effort.
    void broadcast(CoreMessage msg);

    [[nodiscard]] uint32_t core_count() const noexcept;
    [[nodiscard]] boost::asio::io_context& io_context(uint32_t core_id);
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] const CoreMetrics& metrics(uint32_t core_id) const;

    /// Current core ID via thread-local. Must be called from a core thread.
    [[nodiscard]] static uint32_t current_core_id() noexcept;

private:
    void run_core(uint32_t core_id);
    void drain_inbox(uint32_t core_id);
    void start_tick_timer(uint32_t core_id);
    void schedule_drain(uint32_t target_core);

    CoreEngineConfig config_;
    std::vector<std::unique_ptr<CoreContext>> cores_;
    std::vector<std::thread> threads_;
    MessageHandler message_handler_;
    TickCallback tick_callback_;
    std::atomic<bool> running_{false};
    std::unique_ptr<std::atomic<bool>[]> drain_pending_;
    CrossCoreDispatcher cross_core_dispatcher_;
    static thread_local uint32_t tls_core_id_;
};

} // namespace apex::core
