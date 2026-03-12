#include <apex/core/server.hpp>
#include <apex/shared/adapters/adapter_base.hpp>
#include <gtest/gtest.h>

using namespace apex::core;
using namespace apex::shared::adapters;

class TestAdapter : public AdapterBase<TestAdapter> {
public:
    void do_init(CoreEngine& /*engine*/) { init_called = true; }
    void do_drain() { drain_called = true; }
    void do_close() { close_called = true; }
    std::string_view do_name() const noexcept { return "test"; }

    bool init_called = false;
    bool drain_called = false;
    bool close_called = false;
};

TEST(ServerAdapter, AddAdapterChaining) {
    ServerConfig config{.port = 0, .num_cores = 1};
    Server server(config);
    auto& ref = server.add_adapter<TestAdapter>();
    EXPECT_EQ(&ref, &server);  // 체이닝 반환
}

TEST(ServerAdapter, AdapterAccessible) {
    ServerConfig config{.port = 0, .num_cores = 1};
    Server server(config);
    server.add_adapter<TestAdapter>();
    auto& adapter = server.adapter<TestAdapter>();
    EXPECT_EQ(adapter.name(), "test");
}
