#include <apex/core/cross_core_dispatcher.hpp>
#include <gtest/gtest.h>

using namespace apex::core;

TEST(CrossCoreDispatcher, RegisterAndDispatch)
{
    CrossCoreDispatcher d;

    d.register_handler(CrossCoreOp::Noop, [](uint32_t, uint32_t, void* data) {
        auto* count = static_cast<int*>(data);
        ++(*count);
    });

    int count = 0;
    d.dispatch(0, 1, CrossCoreOp::Noop, &count);
    EXPECT_EQ(count, 1);
}

TEST(CrossCoreDispatcher, UnregisteredOpIsNoOp)
{
    CrossCoreDispatcher d;
    // dispatch for unregistered op should not crash
    d.dispatch(0, 1, static_cast<CrossCoreOp>(9999), nullptr);
}

TEST(CrossCoreDispatcher, HasHandler)
{
    CrossCoreDispatcher d;
    EXPECT_FALSE(d.has_handler(CrossCoreOp::Noop));

    d.register_handler(CrossCoreOp::Noop, [](uint32_t, uint32_t, void*) {});
    EXPECT_TRUE(d.has_handler(CrossCoreOp::Noop));
}
