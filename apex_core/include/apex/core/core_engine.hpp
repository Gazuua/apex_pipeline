#pragma once

#include <apex/core/mpsc_queue.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace apex::core {

/// Trivially-copyable message for inter-core communication via MpscQueue.
struct CoreMessage {
    enum class Type : uint8_t {
        Shutdown = 0,
        DrainComplete,
        Custom,
        CrossCoreRequest,   // cross_core_call request (data = CrossCoreTask*)
        CrossCorePost,      // cross_core_post fire-and-forget (data = CrossCoreTask*)
    };

    Type type{Type::Custom};
    uint32_t source_core{0};
    uint64_t data{0};
};
static_assert(std::is_trivially_copyable_v<CoreMessage>);

/// Per-core execution context. Each core owns its own io_context and MPSC inbox.
/// NOT thread-safe -- only accessed by the owning core thread.
struct CoreContext {
    uint32_t core_id;
    boost::asio::io_context io_ctx{1};  // concurrency_hint=1 (single thread)
    std::unique_ptr<MpscQueue<CoreMessage>> inbox;
    std::unique_ptr<boost::asio::steady_timer> drain_timer;

    CoreContext(uint32_t id, size_t queue_capacity);
    ~CoreContext();

    CoreContext(const CoreContext&) = delete;
    CoreContext& operator=(const CoreContext&) = delete;
};

/// Configuration for CoreEngine.
struct CoreEngineConfig {
    uint32_t num_cores{0};              // 0 = auto-detect (hardware_concurrency)
    size_t mpsc_queue_capacity{65536};
    std::chrono::microseconds drain_interval{100};  // MPSC poll interval
};

/// io_context-per-core engine. Creates N cores, each with its own
/// io_context + thread + MPSC inbox. Provides inter-core messaging.
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
    using DrainCallback = std::function<void(uint32_t core_id)>;

    explicit CoreEngine(CoreEngineConfig config = {});
    ~CoreEngine();

    CoreEngine(const CoreEngine&) = delete;
    CoreEngine& operator=(const CoreEngine&) = delete;

    /// Set handler for inter-core messages. Must be called before start().
    void set_message_handler(MessageHandler handler);

    /// Set callback invoked on each drain cycle per core (for tick, etc.).
    void set_drain_callback(DrainCallback callback);

    /// Drain remaining messages from all inboxes, cleaning up heap pointers
    /// for CrossCoreRequest/CrossCorePost messages. Call after stop() + join().
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
    /// @return false if the target core's queue is full (backpressure).
    [[nodiscard]] bool post_to(uint32_t target_core, CoreMessage msg);

    /// Broadcast a message to all cores. Thread-safe. Best-effort.
    void broadcast(CoreMessage msg);

    [[nodiscard]] uint32_t core_count() const noexcept;
    [[nodiscard]] boost::asio::io_context& io_context(uint32_t core_id);
    [[nodiscard]] bool running() const noexcept;

private:
    void run_core(uint32_t core_id);
    void start_drain_timer(uint32_t core_id);

    CoreEngineConfig config_;
    std::vector<std::unique_ptr<CoreContext>> cores_;
    std::vector<std::thread> threads_;
    MessageHandler message_handler_;
    DrainCallback drain_callback_;
    std::atomic<bool> running_{false};
};

} // namespace apex::core
