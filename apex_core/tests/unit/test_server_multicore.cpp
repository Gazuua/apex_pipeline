#include <apex/core/server.hpp>

#include "../test_helpers.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace apex::core;
using namespace std::chrono_literals;

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

    ASSERT_TRUE(apex::test::wait_for([&] { return server.running(); }));

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
    // NOTE: static counters — 모든 CountingService 테스트는 반드시 CountingServiceFixture를 사용할 것
    static inline std::atomic<uint32_t> instance_count{0};
    static inline std::atomic<uint32_t> start_count{0};
    static inline std::atomic<uint32_t> stop_count{0};

    CountingService() : ServiceBase("counting") {
        instance_count.fetch_add(1);
    }

    void on_start() override { start_count.fetch_add(1); }
    void on_stop() override { stop_count.fetch_add(1); }
};

// CountingService counter isolation fixture — resets static counters in SetUp
class CountingServiceFixture : public ::testing::Test {
protected:
    void SetUp() override {
        CountingService::instance_count.store(0);
        CountingService::start_count.store(0);
        CountingService::stop_count.store(0);
    }
};

TEST_F(CountingServiceFixture, ServicePerCoreInstance) {
    Server server({.port = 0, .num_cores = 4, .handle_signals = false});
    server.add_service<CountingService>();

    std::thread t([&] { server.run(); });

    // Wait for all 4 per-core service instances to be created.
    // running() becomes true at the start of run(), but factory calls
    // happen synchronously right after, so poll instance_count directly.
    ASSERT_TRUE(apex::test::wait_for([&] { return CountingService::instance_count.load() >= 4u; }));
    EXPECT_EQ(CountingService::instance_count.load(), 4u);

    // Also verify services were started
    ASSERT_TRUE(apex::test::wait_for([&] { return CountingService::start_count.load() >= 4u; }));
    EXPECT_EQ(CountingService::start_count.load(), 4u);

    server.stop();
    t.join();

    // Verify on_stop() was called on all cores
    EXPECT_EQ(CountingService::stop_count.load(), 4u);
}

TEST_F(CountingServiceFixture, AddServiceChaining) {
    Server server({.port = 0, .num_cores = 2, .handle_signals = false});

    // Chaining compiles and works
    server
        .add_service<CountingService>()
        .add_service<CountingService>();

    std::thread t([&] { server.run(); });

    // 2 services x 2 cores = 4 instances
    ASSERT_TRUE(apex::test::wait_for([&] { return CountingService::instance_count.load() >= 4u; }));
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

// CoreAwareService counter isolation fixture — resets static counters in SetUp
class CoreAwareServiceFixture : public ::testing::Test {
protected:
    void SetUp() override {
        CoreAwareService::factory_call_count.store(0);
        CoreAwareService::start_count.store(0);
        CoreAwareService::stop_count.store(0);
    }
};

TEST_F(CoreAwareServiceFixture, AddServiceFactoryCreatesPerCoreInstances) {
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
    ASSERT_TRUE(apex::test::wait_for([&] { return CoreAwareService::factory_call_count.load() >= 2u; }));

    // Factory was called exactly 2 times (once per core)
    EXPECT_EQ(CoreAwareService::factory_call_count.load(), 2u);

    // Both core_ids 0 and 1 were observed
    EXPECT_EQ(core_id_bits.load(), 0b11u);

    // Wait for services to start
    ASSERT_TRUE(apex::test::wait_for([&] { return CoreAwareService::start_count.load() >= 2u; }));
    EXPECT_EQ(CoreAwareService::start_count.load(), 2u);

    server.stop();
    t.join();

    // Services were stopped
    EXPECT_EQ(CoreAwareService::stop_count.load(), 2u);
}

// --- Constructor validation tests ---

TEST(ServerMulticoreTest, HeartbeatExceedsTimerWheelThrows) {
    // heartbeat_timeout_ticks must be < effective timer_wheel_slots
    // timer_wheel_slots=8 (power of 2), so heartbeat_timeout_ticks >= 8 throws
    EXPECT_THROW(
        Server({.port = 0, .heartbeat_timeout_ticks = 100,
                .timer_wheel_slots = 8, .handle_signals = false}),
        std::invalid_argument);
}

// --- CountingService counter isolation (additional test) ---

TEST_F(CountingServiceFixture, CounterIsolationBetweenTests) {
    // Verify counters start at zero after SetUp reset
    EXPECT_EQ(CountingService::instance_count.load(), 0u);
    EXPECT_EQ(CountingService::start_count.load(), 0u);
    EXPECT_EQ(CountingService::stop_count.load(), 0u);

    Server server({.port = 0, .num_cores = 2, .handle_signals = false});
    server.add_service<CountingService>();

    std::thread t([&] { server.run(); });
    ASSERT_TRUE(apex::test::wait_for([&] { return CountingService::instance_count.load() >= 2u; }));
    EXPECT_EQ(CountingService::instance_count.load(), 2u);

    server.stop();
    t.join();
}

// --- Double run() test ---

TEST(ServerMulticoreTest, DoubleRunThrows) {
    Server server({
        .port = 0,
        .num_cores = 1,
        .handle_signals = false,
    });

    std::thread t1([&] { server.run(); });

    ASSERT_TRUE(apex::test::wait_for([&] { return server.running(); }));

    // Second run() must throw — run() is single-use (I-21)
    EXPECT_THROW(server.run(), std::logic_error);

    // Server is still running from the first call
    EXPECT_TRUE(server.running());

    server.stop();
    t1.join();
}
