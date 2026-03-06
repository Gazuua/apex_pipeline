#include <apex/core/message_dispatcher.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

using apex::core::DispatchError;
using apex::core::MessageDispatcher;
using apex::core::SessionPtr;

class MessageDispatcherTest : public ::testing::Test {
protected:
    // Heap-allocate to avoid stack overflow (65536 std::function = ~2MB)
    std::unique_ptr<MessageDispatcher> d = std::make_unique<MessageDispatcher>();
};

TEST_F(MessageDispatcherTest, InitiallyEmpty) {
    EXPECT_EQ(d->handler_count(), 0u);
    EXPECT_FALSE(d->has_handler(0));
    EXPECT_FALSE(d->has_handler(0x1234));
}

TEST_F(MessageDispatcherTest, RegisterAndDispatch) {
    bool called = false;
    d->register_handler(0x0001, [&](SessionPtr, uint16_t, std::span<const uint8_t>) {
        called = true;
    });
    EXPECT_TRUE(d->has_handler(0x0001));
    EXPECT_EQ(d->handler_count(), 1u);

    auto result = d->dispatch(nullptr,0x0001, {});
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(called);
}

TEST_F(MessageDispatcherTest, DispatchUnknownReturnsError) {
    auto result = d->dispatch(nullptr,0x9999, {});
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DispatchError::UnknownMessage);
}

TEST_F(MessageDispatcherTest, PayloadPassedThrough) {
    std::vector<uint8_t> received;
    d->register_handler(0x0010, [&](SessionPtr, uint16_t, std::span<const uint8_t> payload) {
        received.assign(payload.begin(), payload.end());
    });

    std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
    auto result = d->dispatch(nullptr,0x0010, data);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(received, data);
}

TEST_F(MessageDispatcherTest, OverwriteHandler) {
    int call_count_old = 0;
    int call_count_new = 0;

    d->register_handler(0x0001, [&](SessionPtr, uint16_t, std::span<const uint8_t>) {
        ++call_count_old;
    });
    EXPECT_EQ(d->handler_count(), 1u);

    d->register_handler(0x0001, [&](SessionPtr, uint16_t, std::span<const uint8_t>) {
        ++call_count_new;
    });
    EXPECT_EQ(d->handler_count(), 1u);

    (void)d->dispatch(nullptr,0x0001, {});
    EXPECT_EQ(call_count_old, 0);
    EXPECT_EQ(call_count_new, 1);
}

TEST_F(MessageDispatcherTest, UnregisterHandler) {
    d->register_handler(0x0042, [](SessionPtr, uint16_t, std::span<const uint8_t>) {});
    EXPECT_TRUE(d->has_handler(0x0042));
    EXPECT_EQ(d->handler_count(), 1u);

    d->unregister_handler(0x0042);
    EXPECT_FALSE(d->has_handler(0x0042));
    EXPECT_EQ(d->handler_count(), 0u);
}

TEST_F(MessageDispatcherTest, MultipleHandlers) {
    int counts[5] = {};

    for (uint16_t i = 0; i < 5; ++i) {
        d->register_handler(i, [&counts, i](SessionPtr, uint16_t, std::span<const uint8_t>) {
            ++counts[i];
        });
    }
    EXPECT_EQ(d->handler_count(), 5u);

    for (uint16_t i = 0; i < 5; ++i) {
        auto result = d->dispatch(nullptr,i, {});
        EXPECT_TRUE(result.has_value());
        EXPECT_EQ(counts[i], 1);
    }
}

TEST_F(MessageDispatcherTest, MaxMsgId) {
    bool called = false;
    d->register_handler(0xFFFF, [&](SessionPtr, uint16_t msg_id, std::span<const uint8_t>) {
        EXPECT_EQ(msg_id, 0xFFFF);
        called = true;
    });
    EXPECT_TRUE(d->has_handler(0xFFFF));

    auto result = d->dispatch(nullptr,0xFFFF, {});
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(called);
}
