// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/metrics_registry.hpp>

#include <gtest/gtest.h>

using namespace apex::core;

TEST(CounterTest, IncrementAndValue)
{
    Counter c;
    EXPECT_EQ(c.value(), 0u);
    c.increment();
    EXPECT_EQ(c.value(), 1u);
    c.increment(5);
    EXPECT_EQ(c.value(), 6u);
}

TEST(GaugeTest, IncrementDecrementSet)
{
    Gauge g;
    EXPECT_EQ(g.value(), 0);
    g.increment();
    EXPECT_EQ(g.value(), 1);
    g.decrement();
    EXPECT_EQ(g.value(), 0);
    g.set(42);
    EXPECT_EQ(g.value(), 42);
    g.increment(3);
    EXPECT_EQ(g.value(), 45);
    g.decrement(10);
    EXPECT_EQ(g.value(), 35);
}

TEST(MetricsRegistryTest, OwnedCounter)
{
    MetricsRegistry registry;
    auto& c = registry.counter("test_counter_total", "A test counter");
    c.increment(10);

    auto output = registry.serialize();
    EXPECT_NE(output.find("# HELP test_counter_total A test counter"), std::string::npos);
    EXPECT_NE(output.find("# TYPE test_counter_total counter"), std::string::npos);
    EXPECT_NE(output.find("test_counter_total 10"), std::string::npos);
}

TEST(MetricsRegistryTest, OwnedGauge)
{
    MetricsRegistry registry;
    auto& g = registry.gauge("test_gauge", "A test gauge");
    g.set(42);

    auto output = registry.serialize();
    EXPECT_NE(output.find("# HELP test_gauge A test gauge"), std::string::npos);
    EXPECT_NE(output.find("# TYPE test_gauge gauge"), std::string::npos);
    EXPECT_NE(output.find("test_gauge 42"), std::string::npos);
}

TEST(MetricsRegistryTest, CounterWithLabels)
{
    MetricsRegistry registry;
    auto& c0 = registry.counter("requests_total", "Total requests", {{"core", "0"}});
    auto& c1 = registry.counter("requests_total", "Total requests", {{"core", "1"}});
    c0.increment(100);
    c1.increment(200);

    auto output = registry.serialize();
    // HELP and TYPE should appear only once
    auto first_help = output.find("# HELP requests_total");
    auto second_help = output.find("# HELP requests_total", first_help + 1);
    EXPECT_NE(first_help, std::string::npos);
    EXPECT_EQ(second_help, std::string::npos); // no duplicate

    EXPECT_NE(output.find("requests_total{core=\"0\"} 100"), std::string::npos);
    EXPECT_NE(output.find("requests_total{core=\"1\"} 200"), std::string::npos);
}

TEST(MetricsRegistryTest, CounterFromExternalAtomic)
{
    MetricsRegistry registry;
    std::atomic<uint64_t> external{42};
    registry.counter_from("external_total", "External counter", {}, external);

    auto output = registry.serialize();
    EXPECT_NE(output.find("external_total 42"), std::string::npos);

    external.store(99, std::memory_order_relaxed);
    output = registry.serialize();
    EXPECT_NE(output.find("external_total 99"), std::string::npos);
}

TEST(MetricsRegistryTest, GaugeFn)
{
    MetricsRegistry registry;
    int64_t dynamic_value = 10;
    registry.gauge_fn("dynamic_gauge", "Dynamic gauge", {}, [&dynamic_value]() { return dynamic_value; });

    auto output = registry.serialize();
    EXPECT_NE(output.find("dynamic_gauge 10"), std::string::npos);

    dynamic_value = 25;
    output = registry.serialize();
    EXPECT_NE(output.find("dynamic_gauge 25"), std::string::npos);
}

TEST(MetricsRegistryTest, EmptyRegistrySerialize)
{
    MetricsRegistry registry;
    auto output = registry.serialize();
    EXPECT_TRUE(output.empty());
}

TEST(MetricsRegistryTest, MultipleLabels)
{
    MetricsRegistry registry;
    auto& c = registry.counter("http_requests_total", "HTTP requests", {{"method", "GET"}, {"path", "/metrics"}});
    c.increment(5);

    auto output = registry.serialize();
    EXPECT_NE(output.find("http_requests_total{method=\"GET\",path=\"/metrics\"} 5"), std::string::npos);
}
