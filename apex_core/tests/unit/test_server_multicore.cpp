#include <apex/core/server.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace apex::core;
using namespace std::chrono_literals;

namespace {

/// Helper: poll until server.running() is true, with a deadline.
void wait_until_running(Server& server,
                        std::chrono::milliseconds timeout = 5000ms) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!server.running() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
}

/// Helper: poll until an atomic counter reaches expected value, with a deadline.
void wait_until_count(const std::atomic<uint32_t>& counter, uint32_t expected,
                      std::chrono::milliseconds timeout = 5000ms) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (counter.load() < expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
}

} // anonymous namespace

TEST(ServerMulticoreTest, CreateAndDestroy) {
    Server server({.port = 0, .num_cores = 2});
    // Create/destroy without crash
}

TEST(ServerMulticoreTest, RunAndStop) {
    Server server({
        .port = 0,
        .num_cores = 2,
        .handle_signals = false,
    });

    std::thread t([&] { server.run(); });

    wait_until_running(server);
    ASSERT_TRUE(server.running());

    server.stop();
    t.join();
}

TEST(ServerMulticoreTest, CoreCount) {
    Server server({.port = 0, .num_cores = 4, .handle_signals = false});
    EXPECT_EQ(server.core_count(), 4u);
}

// --- Task 5: Service per-core instantiation tests ---

class CountingService : public ServiceBase<CountingService> {
public:
    static inline std::atomic<uint32_t> instance_count{0};
    static inline std::atomic<uint32_t> start_count{0};
    static inline std::atomic<uint32_t> stop_count{0};

    CountingService() : ServiceBase("counting") {
        instance_count.fetch_add(1);
    }

    void on_start() override { start_count.fetch_add(1); }
    void on_stop() override { stop_count.fetch_add(1); }
};

TEST(ServerMulticoreTest, ServicePerCoreInstance) {
    CountingService::instance_count.store(0);
    CountingService::start_count.store(0);
    CountingService::stop_count.store(0);

    Server server({.port = 0, .num_cores = 4, .handle_signals = false});
    server.add_service<CountingService>();

    std::thread t([&] { server.run(); });

    // Wait for all 4 per-core service instances to be created.
    // running() becomes true at the start of run(), but factory calls
    // happen synchronously right after, so poll instance_count directly.
    wait_until_count(CountingService::instance_count, 4u);
    EXPECT_EQ(CountingService::instance_count.load(), 4u);

    // Also verify services were started
    wait_until_count(CountingService::start_count, 4u);
    EXPECT_EQ(CountingService::start_count.load(), 4u);

    server.stop();
    t.join();
}

TEST(ServerMulticoreTest, AddServiceChaining) {
    CountingService::instance_count.store(0);
    CountingService::start_count.store(0);
    CountingService::stop_count.store(0);

    Server server({.port = 0, .num_cores = 2, .handle_signals = false});

    // Chaining compiles and works
    server
        .add_service<CountingService>()
        .add_service<CountingService>();

    std::thread t([&] { server.run(); });

    // 2 services x 2 cores = 4 instances
    wait_until_count(CountingService::instance_count, 4u);
    EXPECT_EQ(CountingService::instance_count.load(), 4u);

    server.stop();
    t.join();
}

// --- Task 2: add_service_factory tests ---

/// Service that records which core_id it was created on.
class CoreAwareService : public ServiceBase<CoreAwareService> {
public:
    static inline std::atomic<uint32_t> factory_call_count{0};
    static inline std::atomic<uint32_t> start_count{0};
    static inline std::atomic<uint32_t> stop_count{0};

    explicit CoreAwareService(uint32_t core_id)
        : ServiceBase("core_aware"), core_id_(core_id) {}

    void on_start() override { start_count.fetch_add(1); }
    void on_stop() override { stop_count.fetch_add(1); }

    [[nodiscard]] uint32_t core_id() const noexcept { return core_id_; }

private:
    uint32_t core_id_;
};

TEST(ServerMulticoreTest, AddServiceFactoryCreatesPerCoreInstances) {
    CoreAwareService::factory_call_count.store(0);
    CoreAwareService::start_count.store(0);
    CoreAwareService::stop_count.store(0);

    // Track core_ids assigned by the factory (bitfield for 2 cores: bits 0,1)
    std::atomic<uint32_t> core_id_bits{0};

    Server server({.port = 0, .num_cores = 2, .handle_signals = false});

    server.add_service_factory(
        [&core_id_bits](PerCoreState& state)
            -> std::unique_ptr<ServiceBaseInterface> {
            CoreAwareService::factory_call_count.fetch_add(1);
            core_id_bits.fetch_or(1u << state.core_id);
            return std::make_unique<CoreAwareService>(state.core_id);
        });

    std::thread t([&] { server.run(); });

    // Wait for factory to be called for both cores
    wait_until_count(CoreAwareService::factory_call_count, 2u);

    // Factory was called exactly 2 times (once per core)
    EXPECT_EQ(CoreAwareService::factory_call_count.load(), 2u);

    // Both core_ids 0 and 1 were observed
    EXPECT_EQ(core_id_bits.load(), 0b11u);

    // Wait for services to start
    wait_until_count(CoreAwareService::start_count, 2u);
    EXPECT_EQ(CoreAwareService::start_count.load(), 2u);

    server.stop();
    t.join();

    // Services were stopped
    EXPECT_EQ(CoreAwareService::stop_count.load(), 2u);
}
