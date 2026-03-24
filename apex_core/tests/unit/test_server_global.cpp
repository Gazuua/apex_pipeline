// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/server.hpp>

#include <gtest/gtest.h>

#include <string>

using namespace apex::core;

namespace
{

ServerConfig minimal_config()
{
    return {.num_cores = 1, .handle_signals = false, .metrics = {}};
}

struct GlobalA
{
    int value;
    explicit GlobalA(int v)
        : value(v)
    {}
};

struct GlobalB
{
    std::string name;
    explicit GlobalB(std::string n)
        : name(std::move(n))
    {}
};

} // namespace

TEST(ServerGlobal, FactoryCalledOnce)
{
    Server server(minimal_config());

    int call_count = 0;
    server.global<GlobalA>([&] {
        ++call_count;
        return GlobalA{42};
    });
    server.global<GlobalA>([&] {
        ++call_count;
        return GlobalA{99};
    });

    EXPECT_EQ(call_count, 1);
}

TEST(ServerGlobal, ReturnsSameInstance)
{
    Server server(minimal_config());

    auto& first = server.global<GlobalA>([] { return GlobalA{10}; });
    auto& second = server.global<GlobalA>([] { return GlobalA{20}; });

    EXPECT_EQ(&first, &second);
    EXPECT_EQ(first.value, 10);
}

TEST(ServerGlobal, MultipleTypesIndependent)
{
    Server server(minimal_config());

    auto& a = server.global<GlobalA>([] { return GlobalA{1}; });
    auto& b = server.global<GlobalB>([] { return GlobalB{"hello"}; });

    EXPECT_EQ(a.value, 1);
    EXPECT_EQ(b.name, "hello");

    // Ensure they are truly independent (second calls still return originals)
    auto& a2 = server.global<GlobalA>([] { return GlobalA{999}; });
    auto& b2 = server.global<GlobalB>([] { return GlobalB{"world"}; });

    EXPECT_EQ(a2.value, 1);
    EXPECT_EQ(b2.name, "hello");
}

TEST(ServerGlobal, FactoryWithCapture)
{
    Server server(minimal_config());

    int captured_value = 77;
    std::string captured_name = "captured";

    auto& result = server.global<GlobalA>([&] { return GlobalA{captured_value}; });
    EXPECT_EQ(result.value, 77);

    auto& result_b = server.global<GlobalB>([&] { return GlobalB{captured_name + "_suffix"}; });
    EXPECT_EQ(result_b.name, "captured_suffix");
}
