// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/message_dispatcher.hpp>
#include <apex/core/service_registry.hpp>

#include <gtest/gtest.h>

namespace
{

/// ServiceBaseInterface를 구현하는 테스트용 목 서비스 A.
struct MockServiceA : apex::core::ServiceBaseInterface
{
    void start() override {}
    void stop() override {}
    std::string_view name() const noexcept override
    {
        return "a";
    }
    bool started() const noexcept override
    {
        return false;
    }
    void bind_dispatcher(apex::core::MessageDispatcher&) override {}

    int value = 42;
};

/// ServiceBaseInterface를 구현하는 테스트용 목 서비스 B (A와 다른 타입).
struct MockServiceB : apex::core::ServiceBaseInterface
{
    void start() override {}
    void stop() override {}
    std::string_view name() const noexcept override
    {
        return "b";
    }
    bool started() const noexcept override
    {
        return false;
    }
    void bind_dispatcher(apex::core::MessageDispatcher&) override {}
};

// ── 기본 기능 테스트 ──────────────────────────────────────────────────────────

TEST(ServiceRegistryTest, GetRegisteredService)
{
    apex::core::ServiceRegistry registry;
    auto svc = std::make_unique<MockServiceA>();
    svc->value = 99;
    registry.register_service(std::move(svc));

    // get<T>()가 등록된 서비스를 올바른 값으로 반환하는지 확인
    auto& found = registry.get<MockServiceA>();
    EXPECT_EQ(found.value, 99);
}

TEST(ServiceRegistryTest, FindReturnsNullptrForUnregistered)
{
    apex::core::ServiceRegistry registry;
    // 미등록 타입에 대해 find<T>()가 nullptr 반환하는지 확인
    EXPECT_EQ(registry.find<MockServiceB>(), nullptr);
}

TEST(ServiceRegistryTest, GetThrowsForUnregistered)
{
    apex::core::ServiceRegistry registry;
    // 미등록 타입에 대해 get<T>()가 std::logic_error를 throw하는지 확인
    EXPECT_THROW(registry.get<MockServiceB>(), std::logic_error);
}

TEST(ServiceRegistryTest, FindReturnsPointerForRegistered)
{
    apex::core::ServiceRegistry registry;
    registry.register_service(std::make_unique<MockServiceA>());
    // 등록된 타입에 대해 find<T>()가 non-null 반환하는지 확인
    EXPECT_NE(registry.find<MockServiceA>(), nullptr);
}

// ── 추가 기능 테스트 ──────────────────────────────────────────────────────────

TEST(ServiceRegistryTest, SizeReflectsRegisteredCount)
{
    apex::core::ServiceRegistry registry;
    EXPECT_EQ(registry.size(), 0u);

    registry.register_service(std::make_unique<MockServiceA>());
    EXPECT_EQ(registry.size(), 1u);

    registry.register_service(std::make_unique<MockServiceB>());
    EXPECT_EQ(registry.size(), 2u);
}

TEST(ServiceRegistryTest, ForEachVisitsAllServices)
{
    apex::core::ServiceRegistry registry;
    registry.register_service(std::make_unique<MockServiceA>());
    registry.register_service(std::make_unique<MockServiceB>());

    int count = 0;
    registry.for_each([&](apex::core::ServiceBaseInterface&) { ++count; });
    EXPECT_EQ(count, 2);
}

TEST(ServiceRegistryTest, FindReturnsSamePointerAsGet)
{
    apex::core::ServiceRegistry registry;
    registry.register_service(std::make_unique<MockServiceA>());

    // find<T>()와 get<T>()가 같은 인스턴스를 가리키는지 확인
    auto* ptr = registry.find<MockServiceA>();
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(ptr, &registry.get<MockServiceA>());
}

} // namespace
