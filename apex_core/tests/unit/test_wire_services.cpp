// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/adapter_interface.hpp>
#include <apex/core/core_engine.hpp>
#include <apex/core/service_base.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string_view>
#include <vector>

using namespace apex::core;

namespace
{

/// Concrete mock implementing all AdapterInterface pure virtuals.
class MockAdapter : public AdapterInterface
{
  public:
    void init(CoreEngine& /*engine*/) override {}
    void drain() override {}
    void close() override {}
    [[nodiscard]] bool is_ready() const noexcept override
    {
        return true;
    }
    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "mock";
    }
};

/// Adapter that overrides wire_services to modify the services vector.
class WiringAdapter : public AdapterInterface
{
  public:
    void init(CoreEngine& /*engine*/) override {}
    void drain() override {}
    void close() override {}
    [[nodiscard]] bool is_ready() const noexcept override
    {
        return true;
    }
    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "wiring";
    }

    bool wired = false;

    void wire_services(std::vector<std::unique_ptr<ServiceBaseInterface>>& /*services*/,
                       CoreEngine& /*engine*/) override
    {
        wired = true;
    }
};

} // namespace

TEST(WireServices, DefaultImplementation_NoOp)
{
    MockAdapter adapter;
    CoreEngine engine({.num_cores = 1});
    std::vector<std::unique_ptr<ServiceBaseInterface>> services;

    // Default wire_services is a no-op — should not throw or modify anything.
    EXPECT_NO_THROW(adapter.wire_services(services, engine));
    EXPECT_TRUE(services.empty());
}

TEST(WireServices, CustomAdapter_OverrideCalled)
{
    WiringAdapter adapter;
    CoreEngine engine({.num_cores = 1});
    std::vector<std::unique_ptr<ServiceBaseInterface>> services;

    EXPECT_FALSE(adapter.wired);
    adapter.wire_services(services, engine);
    EXPECT_TRUE(adapter.wired);
}
