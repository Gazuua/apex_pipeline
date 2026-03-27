// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/core_engine.hpp>
#include <apex/shared/adapters/adapter_base.hpp>
#include <boost/asio/awaitable.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::adapters;

class MockAdapter : public AdapterBase<MockAdapter>
{
  public:
    void do_init(apex::core::CoreEngine& /*engine*/)
    {
        init_called = true;
    }
    void do_drain()
    {
        drain_called = true;
    }
    void do_close()
    {
        close_called = true;
    }
    std::string_view do_name() const noexcept
    {
        return "mock";
    }

    // Expose protected spawn_adapter_coro for testing
    void try_spawn(uint32_t core_id, boost::asio::awaitable<void> coro)
    {
        spawn_adapter_coro(core_id, std::move(coro));
    }

    bool init_called = false;
    bool drain_called = false;
    bool close_called = false;
};

TEST(AdapterBase, NotReadyBeforeInit)
{
    MockAdapter adapter;
    EXPECT_FALSE(adapter.is_ready());
}

TEST(AdapterBase, ReadyAfterInit)
{
    MockAdapter adapter;
    apex::core::CoreEngineConfig config{.num_cores = 1,
                                        .spsc_queue_capacity = 64,
                                        .tick_interval = std::chrono::milliseconds{100},
                                        .drain_batch_limit = 1024,
                                        .core_assignments = {},
                                        .numa_aware = true};
    apex::core::CoreEngine engine(config);
    adapter.init(engine);
    EXPECT_TRUE(adapter.is_ready());
    EXPECT_TRUE(adapter.init_called);
}

TEST(AdapterBase, NotReadyAfterDrain)
{
    MockAdapter adapter;
    apex::core::CoreEngineConfig config{.num_cores = 1,
                                        .spsc_queue_capacity = 64,
                                        .tick_interval = std::chrono::milliseconds{100},
                                        .drain_batch_limit = 1024,
                                        .core_assignments = {},
                                        .numa_aware = true};
    apex::core::CoreEngine engine(config);
    adapter.init(engine);
    adapter.drain();
    EXPECT_FALSE(adapter.is_ready());
    EXPECT_TRUE(adapter.drain_called);
}

TEST(AdapterBase, CloseCallsDerived)
{
    MockAdapter adapter;
    adapter.close();
    EXPECT_TRUE(adapter.close_called);
}

TEST(AdapterBase, NameReturnsCorrectly)
{
    MockAdapter adapter;
    EXPECT_EQ(adapter.name(), "mock");
}

TEST(AdapterWrapper, TypeErasureWorks)
{
    auto wrapper = std::make_unique<AdapterWrapper<MockAdapter>>();
    apex::core::AdapterInterface* iface = wrapper.get();

    EXPECT_EQ(iface->name(), "mock");
    EXPECT_FALSE(iface->is_ready());
}

TEST(AdapterWrapper, LifecycleDelegation)
{
    auto wrapper = std::make_unique<AdapterWrapper<MockAdapter>>();
    apex::core::AdapterInterface* iface = wrapper.get();
    auto& mock = wrapper->get();

    // init delegation
    apex::core::CoreEngineConfig config{.num_cores = 1,
                                        .spsc_queue_capacity = 64,
                                        .tick_interval = std::chrono::milliseconds{100},
                                        .drain_batch_limit = 1024,
                                        .core_assignments = {},
                                        .numa_aware = true};
    apex::core::CoreEngine engine(config);
    iface->init(engine);
    EXPECT_TRUE(mock.init_called);
    EXPECT_TRUE(iface->is_ready());

    // drain delegation
    iface->drain();
    EXPECT_TRUE(mock.drain_called);
    EXPECT_FALSE(iface->is_ready());

    // close delegation
    iface->close();
    EXPECT_TRUE(mock.close_called);
}

// --- spawn_adapter_coro rejection tests (#143) ---

namespace
{
boost::asio::awaitable<void> noop_coro()
{
    co_return;
}
} // namespace

TEST(AdapterBase, SpawnRejectsBeforeInit)
{
    // CLOSED 상태(초기값) — spawn 거부, outstanding 0 유지
    MockAdapter adapter;
    adapter.try_spawn(0, noop_coro());
    EXPECT_EQ(adapter.outstanding_adapter_coros(), 0u);
}

TEST(AdapterBase, SpawnRejectsInDrainingState)
{
    // DRAINING 상태 — spawn 거부, outstanding 0 유지
    MockAdapter adapter;
    apex::core::CoreEngineConfig config{.num_cores = 1,
                                        .spsc_queue_capacity = 64,
                                        .tick_interval = std::chrono::milliseconds{100},
                                        .drain_batch_limit = 1024,
                                        .core_assignments = {},
                                        .numa_aware = true};
    apex::core::CoreEngine engine(config);
    adapter.init(engine);
    adapter.drain();
    EXPECT_FALSE(adapter.is_ready());
    adapter.try_spawn(0, noop_coro());
    EXPECT_EQ(adapter.outstanding_adapter_coros(), 0u);
}

TEST(AdapterBase, SpawnRejectsAfterClose)
{
    // CLOSED 상태(close 후) — spawn 거부, outstanding 0 유지
    MockAdapter adapter;
    apex::core::CoreEngineConfig config{.num_cores = 1,
                                        .spsc_queue_capacity = 64,
                                        .tick_interval = std::chrono::milliseconds{100},
                                        .drain_batch_limit = 1024,
                                        .core_assignments = {},
                                        .numa_aware = true};
    apex::core::CoreEngine engine(config);
    adapter.init(engine);
    adapter.close();
    EXPECT_FALSE(adapter.is_ready());
    adapter.try_spawn(0, noop_coro());
    EXPECT_EQ(adapter.outstanding_adapter_coros(), 0u);
}
