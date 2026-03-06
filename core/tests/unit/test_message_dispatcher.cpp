#include <apex/core/message_dispatcher.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/awaitable.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

using apex::core::DispatchError;
using apex::core::MessageDispatcher;
using apex::core::SessionPtr;
using boost::asio::awaitable;

// awaitable을 동기적으로 실행하는 헬퍼
template <typename T>
T run_coro(boost::asio::io_context& ctx, boost::asio::awaitable<T> aw) {
    auto future = boost::asio::co_spawn(ctx, std::move(aw), boost::asio::use_future);
    ctx.run();
    ctx.restart();
    return future.get();
}

inline void run_coro(boost::asio::io_context& ctx, boost::asio::awaitable<void> aw) {
    auto future = boost::asio::co_spawn(ctx, std::move(aw), boost::asio::use_future);
    ctx.run();
    ctx.restart();
    future.get();
}

class MessageDispatcherTest : public ::testing::Test {
protected:
    boost::asio::io_context io_ctx_;
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
    d->register_handler(0x0001,
        [&](SessionPtr, uint16_t, std::span<const uint8_t>) -> awaitable<void> {
            called = true;
            co_return;
        });
    EXPECT_TRUE(d->has_handler(0x0001));
    EXPECT_EQ(d->handler_count(), 1u);

    auto result = run_coro(io_ctx_, d->dispatch(nullptr, 0x0001, {}));
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(called);
}

TEST_F(MessageDispatcherTest, DispatchUnknownReturnsError) {
    auto result = run_coro(io_ctx_, d->dispatch(nullptr, 0x9999, {}));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DispatchError::UnknownMessage);
}

TEST_F(MessageDispatcherTest, PayloadPassedThrough) {
    std::vector<uint8_t> received;
    d->register_handler(0x0010,
        [&](SessionPtr, uint16_t, std::span<const uint8_t> payload) -> awaitable<void> {
            received.assign(payload.begin(), payload.end());
            co_return;
        });

    std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
    auto result = run_coro(io_ctx_, d->dispatch(nullptr, 0x0010, data));
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(received, data);
}

TEST_F(MessageDispatcherTest, OverwriteHandler) {
    int call_count_old = 0;
    int call_count_new = 0;

    d->register_handler(0x0001,
        [&](SessionPtr, uint16_t, std::span<const uint8_t>) -> awaitable<void> {
            ++call_count_old;
            co_return;
        });
    EXPECT_EQ(d->handler_count(), 1u);

    d->register_handler(0x0001,
        [&](SessionPtr, uint16_t, std::span<const uint8_t>) -> awaitable<void> {
            ++call_count_new;
            co_return;
        });
    EXPECT_EQ(d->handler_count(), 1u);

    run_coro(io_ctx_, [&]() -> awaitable<void> {
        auto result = co_await d->dispatch(nullptr, 0x0001, {});
        (void)result;
    }());
    EXPECT_EQ(call_count_old, 0);
    EXPECT_EQ(call_count_new, 1);
}

TEST_F(MessageDispatcherTest, UnregisterHandler) {
    d->register_handler(0x0042,
        [](SessionPtr, uint16_t, std::span<const uint8_t>) -> awaitable<void> {
            co_return;
        });
    EXPECT_TRUE(d->has_handler(0x0042));
    EXPECT_EQ(d->handler_count(), 1u);

    d->unregister_handler(0x0042);
    EXPECT_FALSE(d->has_handler(0x0042));
    EXPECT_EQ(d->handler_count(), 0u);
}

TEST_F(MessageDispatcherTest, MultipleHandlers) {
    int counts[5] = {};

    for (uint16_t i = 0; i < 5; ++i) {
        d->register_handler(i,
            [&counts, i](SessionPtr, uint16_t, std::span<const uint8_t>) -> awaitable<void> {
                ++counts[i];
                co_return;
            });
    }
    EXPECT_EQ(d->handler_count(), 5u);

    for (uint16_t i = 0; i < 5; ++i) {
        auto result = run_coro(io_ctx_, d->dispatch(nullptr, i, {}));
        EXPECT_TRUE(result.has_value());
        EXPECT_EQ(counts[i], 1);
    }
}

TEST_F(MessageDispatcherTest, MaxMsgId) {
    bool called = false;
    d->register_handler(0xFFFF,
        [&](SessionPtr, uint16_t msg_id, std::span<const uint8_t>) -> awaitable<void> {
            EXPECT_EQ(msg_id, 0xFFFF);
            called = true;
            co_return;
        });
    EXPECT_TRUE(d->has_handler(0xFFFF));

    auto result = run_coro(io_ctx_, d->dispatch(nullptr, 0xFFFF, {}));
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(called);
}
