// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include "../test_helpers.hpp"
#include <apex/core/message_dispatcher.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

using apex::core::ErrorCode;
using apex::core::MessageDispatcher;
using apex::core::Result;
using apex::core::SessionPtr;
using apex::test::run_coro;
using boost::asio::awaitable;

class MessageDispatcherTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        io_ctx_.restart();
    }

    boost::asio::io_context io_ctx_;
    std::unique_ptr<MessageDispatcher> d = std::make_unique<MessageDispatcher>();
};

TEST_F(MessageDispatcherTest, InitiallyEmpty)
{
    EXPECT_EQ(d->handler_count(), 0u);
    EXPECT_FALSE(d->has_handler(0));
    EXPECT_FALSE(d->has_handler(0x1234));
}

TEST_F(MessageDispatcherTest, RegisterAndDispatch)
{
    bool called = false;
    d->register_handler(0x0001, [&](SessionPtr, uint32_t, std::span<const uint8_t>) -> awaitable<Result<void>> {
        called = true;
        co_return apex::core::ok();
    });
    EXPECT_TRUE(d->has_handler(0x0001));
    EXPECT_EQ(d->handler_count(), 1u);

    auto result = run_coro(io_ctx_, d->dispatch(nullptr, 0x0001, {}));
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(called);
}

TEST_F(MessageDispatcherTest, DispatchUnknownReturnsError)
{
    auto result = run_coro(io_ctx_, d->dispatch(nullptr, 0x9999, {}));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::HandlerNotFound);
}

TEST_F(MessageDispatcherTest, PayloadPassedThrough)
{
    std::vector<uint8_t> received;
    d->register_handler(0x0010, [&](SessionPtr, uint32_t, std::span<const uint8_t> payload) -> awaitable<Result<void>> {
        received.assign(payload.begin(), payload.end());
        co_return apex::core::ok();
    });

    std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
    auto result = run_coro(io_ctx_, d->dispatch(nullptr, 0x0010, data));
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(received, data);
}

TEST_F(MessageDispatcherTest, OverwriteHandler)
{
    int call_count_old = 0;
    int call_count_new = 0;

    d->register_handler(0x0001, [&](SessionPtr, uint32_t, std::span<const uint8_t>) -> awaitable<Result<void>> {
        ++call_count_old;
        co_return apex::core::ok();
    });
    EXPECT_EQ(d->handler_count(), 1u);

    d->register_handler(0x0001, [&](SessionPtr, uint32_t, std::span<const uint8_t>) -> awaitable<Result<void>> {
        ++call_count_new;
        co_return apex::core::ok();
    });
    EXPECT_EQ(d->handler_count(), 1u);

    run_coro(io_ctx_, [&]() -> awaitable<void> {
        auto result = co_await d->dispatch(nullptr, 0x0001, {});
        EXPECT_TRUE(result.has_value());
    }());
    EXPECT_EQ(call_count_old, 0);
    EXPECT_EQ(call_count_new, 1);
}

TEST_F(MessageDispatcherTest, UnregisterHandler)
{
    d->register_handler(0x0042, [](SessionPtr, uint32_t, std::span<const uint8_t>) -> awaitable<Result<void>> {
        co_return apex::core::ok();
    });
    EXPECT_TRUE(d->has_handler(0x0042));
    EXPECT_EQ(d->handler_count(), 1u);

    d->unregister_handler(0x0042);
    EXPECT_FALSE(d->has_handler(0x0042));
    EXPECT_EQ(d->handler_count(), 0u);
}

TEST_F(MessageDispatcherTest, MultipleHandlers)
{
    int counts[5] = {};

    for (uint32_t i = 0; i < 5; ++i)
    {
        d->register_handler(i, [&counts, i](SessionPtr, uint32_t, std::span<const uint8_t>) -> awaitable<Result<void>> {
            ++counts[i];
            co_return apex::core::ok();
        });
    }
    EXPECT_EQ(d->handler_count(), 5u);

    for (uint32_t i = 0; i < 5; ++i)
    {
        auto result = run_coro(io_ctx_, d->dispatch(nullptr, i, {}));
        EXPECT_TRUE(result.has_value());
        EXPECT_EQ(counts[i], 1);
    }
}

TEST_F(MessageDispatcherTest, MaxMsgId)
{
    bool called = false;
    d->register_handler(0xFFFFFFFF,
                        [&](SessionPtr, uint32_t msg_id, std::span<const uint8_t>) -> awaitable<Result<void>> {
                            EXPECT_EQ(msg_id, 0xFFFFFFFF);
                            called = true;
                            co_return apex::core::ok();
                        });
    EXPECT_TRUE(d->has_handler(0xFFFFFFFF));

    auto result = run_coro(io_ctx_, d->dispatch(nullptr, 0xFFFFFFFF, {}));
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(called);
}

TEST_F(MessageDispatcherTest, HandlerExceptionReturnsHandlerException)
{
    d->register_handler(0x0001, [](SessionPtr, uint32_t, std::span<const uint8_t>) -> awaitable<Result<void>> {
        throw std::runtime_error("test error");
        co_return apex::core::ok();
    });
    auto result = run_coro(io_ctx_, d->dispatch(nullptr, 0x0001, {}));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::HandlerException);
}

TEST_F(MessageDispatcherTest, HandlerReturnsErrorCode)
{
    d->register_handler(0x0001, [](SessionPtr, uint32_t, std::span<const uint8_t>) -> awaitable<Result<void>> {
        co_return apex::core::error(apex::core::ErrorCode::Timeout);
    });
    auto result = run_coro(io_ctx_, d->dispatch(nullptr, 0x0001, {}));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), apex::core::ErrorCode::Timeout);
}

TEST_F(MessageDispatcherTest, HandlersAccessorReturnsRegisteredHandlers)
{
    d->register_handler(0x0001, [](SessionPtr, uint32_t, std::span<const uint8_t>) -> awaitable<Result<void>> {
        co_return apex::core::ok();
    });
    d->register_handler(0x0002, [](SessionPtr, uint32_t, std::span<const uint8_t>) -> awaitable<Result<void>> {
        co_return apex::core::ok();
    });
    d->register_handler(0x0003, [](SessionPtr, uint32_t, std::span<const uint8_t>) -> awaitable<Result<void>> {
        co_return apex::core::ok();
    });

    const auto& handlers = d->handlers();
    EXPECT_EQ(handlers.size(), 3u);
    EXPECT_TRUE(handlers.contains(0x0001));
    EXPECT_TRUE(handlers.contains(0x0002));
    EXPECT_TRUE(handlers.contains(0x0003));
    EXPECT_FALSE(handlers.contains(0x9999));
}

TEST_F(MessageDispatcherTest, HandlersAccessorEmptyOnInit)
{
    const auto& handlers = d->handlers();
    EXPECT_TRUE(handlers.empty());
}

TEST_F(MessageDispatcherTest, SyncAllHandlersCopiesDefaultAndMsgIdHandlers)
{
    // Source dispatcher: register default + specific msg_id handlers
    MessageDispatcher source;
    int default_count = 0;
    int handler_1_count = 0;
    int handler_2_count = 0;

    source.set_default_handler(
        [&default_count](SessionPtr, uint32_t, std::span<const uint8_t>) -> awaitable<Result<void>> {
            ++default_count;
            co_return apex::core::ok();
        });
    source.register_handler(
        0x0010, [&handler_1_count](SessionPtr, uint32_t, std::span<const uint8_t>) -> awaitable<Result<void>> {
            ++handler_1_count;
            co_return apex::core::ok();
        });
    source.register_handler(
        0x0020, [&handler_2_count](SessionPtr, uint32_t, std::span<const uint8_t>) -> awaitable<Result<void>> {
            ++handler_2_count;
            co_return apex::core::ok();
        });

    // Target dispatcher: sync all handlers from source
    MessageDispatcher target;
    EXPECT_FALSE(target.has_default_handler());
    EXPECT_EQ(target.handler_count(), 0u);

    // Manually replicate what sync_all_handlers does (at MessageDispatcher level)
    if (source.has_default_handler())
    {
        target.set_default_handler(source.default_handler());
    }
    for (const auto& [msg_id, handler] : source.handlers())
    {
        target.register_handler(msg_id, handler);
    }

    // Verify all handlers were copied
    EXPECT_TRUE(target.has_default_handler());
    EXPECT_EQ(target.handler_count(), 2u);
    EXPECT_TRUE(target.has_handler(0x0010));
    EXPECT_TRUE(target.has_handler(0x0020));

    // Verify dispatched handlers work via target
    auto r1 = run_coro(io_ctx_, target.dispatch(nullptr, 0x0010, {}));
    EXPECT_TRUE(r1.has_value());
    EXPECT_EQ(handler_1_count, 1);

    auto r2 = run_coro(io_ctx_, target.dispatch(nullptr, 0x0020, {}));
    EXPECT_TRUE(r2.has_value());
    EXPECT_EQ(handler_2_count, 1);

    // Default handler should handle unknown msg_id
    auto r3 = run_coro(io_ctx_, target.dispatch(nullptr, 0x9999, {}));
    EXPECT_TRUE(r3.has_value());
    EXPECT_EQ(default_count, 1);
}

TEST_F(MessageDispatcherTest, SyncAllHandlersWithoutDefaultHandler)
{
    // Source with msg_id handlers only, no default
    MessageDispatcher source;
    source.register_handler(0x0001, [](SessionPtr, uint32_t, std::span<const uint8_t>) -> awaitable<Result<void>> {
        co_return apex::core::ok();
    });

    MessageDispatcher target;

    // Replicate sync_all_handlers logic
    if (source.has_default_handler())
    {
        target.set_default_handler(source.default_handler());
    }
    for (const auto& [msg_id, handler] : source.handlers())
    {
        target.register_handler(msg_id, handler);
    }

    EXPECT_FALSE(target.has_default_handler());
    EXPECT_EQ(target.handler_count(), 1u);
    EXPECT_TRUE(target.has_handler(0x0001));

    // Unregistered msg_id should return error (no default handler)
    auto result = run_coro(io_ctx_, target.dispatch(nullptr, 0x9999, {}));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::HandlerNotFound);
}
