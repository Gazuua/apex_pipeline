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

class AnotherTestAdapter : public AdapterBase<AnotherTestAdapter> {
public:
    explicit AnotherTestAdapter(int value = 0) : value_(value) {}

    void do_init(CoreEngine& /*engine*/) { init_called = true; }
    void do_drain() { drain_called = true; }
    void do_close() { close_called = true; }
    std::string_view do_name() const noexcept { return "another"; }

    int value() const noexcept { return value_; }

    bool init_called = false;
    bool drain_called = false;
    bool close_called = false;

private:
    int value_;
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

TEST(ServerAdapter, MultipleAdapters) {
    ServerConfig config{.port = 0, .num_cores = 1};
    Server server(config);
    server.add_adapter<TestAdapter>();
    server.add_adapter<AnotherTestAdapter>();

    auto& test_adapter = server.adapter<TestAdapter>();
    auto& another_adapter = server.adapter<AnotherTestAdapter>();

    EXPECT_EQ(test_adapter.name(), "test");
    EXPECT_EQ(another_adapter.name(), "another");
}

TEST(ServerAdapter, AddAdapterWithArgs) {
    ServerConfig config{.port = 0, .num_cores = 1};
    Server server(config);
    server.add_adapter<AnotherTestAdapter>(42);

    auto& adapter = server.adapter<AnotherTestAdapter>();
    EXPECT_EQ(adapter.name(), "another");
    EXPECT_EQ(adapter.value(), 42);
}
