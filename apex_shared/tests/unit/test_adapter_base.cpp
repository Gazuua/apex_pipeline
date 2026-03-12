#include <apex/shared/adapters/adapter_base.hpp>
#include <apex/core/core_engine.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::adapters;

class MockAdapter : public AdapterBase<MockAdapter> {
public:
    void do_init(apex::core::CoreEngine& /*engine*/) { init_called = true; }
    void do_drain() { drain_called = true; }
    void do_close() { close_called = true; }
    std::string_view do_name() const noexcept { return "mock"; }

    bool init_called = false;
    bool drain_called = false;
    bool close_called = false;
};

TEST(AdapterBase, NotReadyBeforeInit) {
    MockAdapter adapter;
    EXPECT_FALSE(adapter.is_ready());
}

TEST(AdapterBase, ReadyAfterInit) {
    MockAdapter adapter;
    apex::core::CoreEngineConfig config{.num_cores = 1, .mpsc_queue_capacity = 64};
    apex::core::CoreEngine engine(config);
    adapter.init(engine);
    EXPECT_TRUE(adapter.is_ready());
    EXPECT_TRUE(adapter.init_called);
}

TEST(AdapterBase, NotReadyAfterDrain) {
    MockAdapter adapter;
    apex::core::CoreEngineConfig config{.num_cores = 1, .mpsc_queue_capacity = 64};
    apex::core::CoreEngine engine(config);
    adapter.init(engine);
    adapter.drain();
    EXPECT_FALSE(adapter.is_ready());
    EXPECT_TRUE(adapter.drain_called);
}

TEST(AdapterBase, CloseCallsDerived) {
    MockAdapter adapter;
    adapter.close();
    EXPECT_TRUE(adapter.close_called);
}

TEST(AdapterBase, NameReturnsCorrectly) {
    MockAdapter adapter;
    EXPECT_EQ(adapter.name(), "mock");
}

TEST(AdapterWrapper, TypeErasureWorks) {
    auto wrapper = std::make_unique<AdapterWrapper<MockAdapter>>();
    apex::core::AdapterInterface* iface = wrapper.get();

    EXPECT_EQ(iface->name(), "mock");
    EXPECT_FALSE(iface->is_ready());
}
