// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/server.hpp>
#include <apex/shared/adapters/adapter_base.hpp>
#include <gtest/gtest.h>

using namespace apex::core;
using namespace apex::shared::adapters;

class TestAdapter : public AdapterBase<TestAdapter>
{
  public:
    void do_init(CoreEngine& /*engine*/)
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
        return "test";
    }

    bool init_called = false;
    bool drain_called = false;
    bool close_called = false;
};

class AnotherTestAdapter : public AdapterBase<AnotherTestAdapter>
{
  public:
    explicit AnotherTestAdapter(int value = 0)
        : value_(value)
    {}

    void do_init(CoreEngine& /*engine*/)
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
        return "another";
    }

    int value() const noexcept
    {
        return value_;
    }

    bool init_called = false;
    bool drain_called = false;
    bool close_called = false;

  private:
    int value_;
};

TEST(ServerAdapter, AddAdapterChaining)
{
    ServerConfig config;
    config.num_cores = 1;
    config.affinity.enabled = false;
    Server server(config);
    auto& ref = server.add_adapter<TestAdapter>();
    EXPECT_EQ(&ref, &server); // 체이닝 반환
}

TEST(ServerAdapter, AdapterAccessible)
{
    ServerConfig config;
    config.num_cores = 1;
    config.affinity.enabled = false;
    Server server(config);
    server.add_adapter<TestAdapter>();
    auto& adapter = server.adapter<TestAdapter>();
    EXPECT_EQ(adapter.name(), "test");
}

TEST(ServerAdapter, MultipleAdapters)
{
    ServerConfig config;
    config.num_cores = 1;
    config.affinity.enabled = false;
    Server server(config);
    server.add_adapter<TestAdapter>();
    server.add_adapter<AnotherTestAdapter>();

    auto& test_adapter = server.adapter<TestAdapter>();
    auto& another_adapter = server.adapter<AnotherTestAdapter>();

    EXPECT_EQ(test_adapter.name(), "test");
    EXPECT_EQ(another_adapter.name(), "another");
}

TEST(ServerAdapter, AddAdapterWithArgs)
{
    ServerConfig config;
    config.num_cores = 1;
    config.affinity.enabled = false;
    Server server(config);
    server.add_adapter<AnotherTestAdapter>(42);

    auto& adapter = server.adapter<AnotherTestAdapter>();
    EXPECT_EQ(adapter.name(), "another");
    EXPECT_EQ(adapter.value(), 42);
}

// ── 다중 등록 (role 기반) 테스트 ──────────────────────────────────────

TEST(ServerAdapter, MultiRegistrationWithRole)
{
    // 동일 타입을 역할별로 다중 등록
    ServerConfig config;
    config.num_cores = 1;
    config.affinity.enabled = false;
    Server server(config);
    server.add_adapter<AnotherTestAdapter>(std::string("primary"), 10);
    server.add_adapter<AnotherTestAdapter>(std::string("secondary"), 20);

    auto& primary = server.adapter<AnotherTestAdapter>("primary");
    auto& secondary = server.adapter<AnotherTestAdapter>("secondary");

    // 역할별로 다른 인스턴스
    EXPECT_EQ(primary.value(), 10);
    EXPECT_EQ(secondary.value(), 20);
    EXPECT_NE(&primary, &secondary);
}

TEST(ServerAdapter, DefaultRoleBackwardCompat)
{
    // role 없이 등록한 어댑터는 "default" 역할로 접근 가능
    ServerConfig config;
    config.num_cores = 1;
    config.affinity.enabled = false;
    Server server(config);
    server.add_adapter<TestAdapter>();

    // role 명시 없이 접근 (기존 API 호환)
    auto& a1 = server.adapter<TestAdapter>();
    // "default" 역할로 명시 접근
    auto& a2 = server.adapter<TestAdapter>("default");

    EXPECT_EQ(&a1, &a2);
    EXPECT_EQ(a1.name(), "test");
}

TEST(ServerAdapter, RoleAndDefaultCoexist)
{
    // 기본 역할 + 명시 역할 공존
    ServerConfig config;
    config.num_cores = 1;
    config.affinity.enabled = false;
    Server server(config);
    server.add_adapter<AnotherTestAdapter>(100); // default role
    server.add_adapter<AnotherTestAdapter>(std::string("custom"), 200);

    auto& def = server.adapter<AnotherTestAdapter>();
    auto& custom = server.adapter<AnotherTestAdapter>("custom");

    EXPECT_EQ(def.value(), 100);
    EXPECT_EQ(custom.value(), 200);
    EXPECT_NE(&def, &custom);
}

TEST(ServerAdapter, MultiRegistrationChaining)
{
    // 다중 등록 시 체이닝 동작 확인
    ServerConfig config;
    config.num_cores = 1;
    config.affinity.enabled = false;
    Server server(config);
    auto& ref = server.add_adapter<TestAdapter>()
                    .add_adapter<AnotherTestAdapter>(std::string("a"), 1)
                    .add_adapter<AnotherTestAdapter>(std::string("b"), 2);

    EXPECT_EQ(&ref, &server);
    EXPECT_EQ(server.adapter<AnotherTestAdapter>("a").value(), 1);
    EXPECT_EQ(server.adapter<AnotherTestAdapter>("b").value(), 2);
}
