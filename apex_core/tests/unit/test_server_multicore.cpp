#include <apex/core/server.hpp>

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
    std::this_thread::sleep_for(100ms);

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

    CountingService() : ServiceBase("counting") {
        instance_count.fetch_add(1);
    }

    void on_start() override {}
};

TEST(ServerMulticoreTest, ServicePerCoreInstance) {
    CountingService::instance_count.store(0);

    Server server({.port = 0, .num_cores = 4, .handle_signals = false});
    server.add_service<CountingService>();

    std::thread t([&] { server.run(); });
    std::this_thread::sleep_for(100ms);

    EXPECT_EQ(CountingService::instance_count.load(), 4u);

    server.stop();
    t.join();
}

TEST(ServerMulticoreTest, AddServiceChaining) {
    CountingService::instance_count.store(0);

    Server server({.port = 0, .num_cores = 2, .handle_signals = false});

    // Chaining compiles and works
    server
        .add_service<CountingService>()
        .add_service<CountingService>();

    std::thread t([&] { server.run(); });
    std::this_thread::sleep_for(100ms);

    // 2 services × 2 cores = 4 instances
    EXPECT_EQ(CountingService::instance_count.load(), 4u);

    server.stop();
    t.join();
}
