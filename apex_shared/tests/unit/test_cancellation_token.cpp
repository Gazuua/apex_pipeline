// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/adapters/cancellation_token.hpp>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::adapters;

TEST(CancellationToken, SpawnAndCancel)
{
    boost::asio::io_context io;

    CancellationToken token;

    auto slot = token.new_slot();
    EXPECT_EQ(token.outstanding(), 1);

    boost::asio::steady_timer timer(io, std::chrono::hours{1});
    bool cancelled = false;

    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            auto [ec] = co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
            if (ec == boost::asio::error::operation_aborted)
                cancelled = true;
            token.on_complete();
        },
        boost::asio::bind_cancellation_slot(slot, boost::asio::detached));

    io.run_one();
    EXPECT_EQ(token.outstanding(), 1);

    token.cancel_all();
    io.run();
    EXPECT_TRUE(cancelled);
    EXPECT_EQ(token.outstanding(), 0);
}

TEST(CancellationToken, MultipleCoros)
{
    boost::asio::io_context io;
    CancellationToken token;

    constexpr int N = 5;
    std::vector<boost::asio::steady_timer> timers;
    timers.reserve(N);
    for (int i = 0; i < N; ++i)
        timers.emplace_back(io, std::chrono::hours{1});

    int completed = 0;

    for (int i = 0; i < N; ++i)
    {
        auto slot = token.new_slot();
        boost::asio::co_spawn(
            io,
            [&, i]() -> boost::asio::awaitable<void> {
                auto [ec] = co_await timers[static_cast<size_t>(i)].async_wait(
                    boost::asio::as_tuple(boost::asio::use_awaitable));
                static_cast<void>(ec);
                ++completed;
                token.on_complete();
            },
            boost::asio::bind_cancellation_slot(slot, boost::asio::detached));
    }

    // 모든 co_spawn 핸들러를 처리해서 5개 코루틴 전부 시작 + 타이머 대기 진입
    io.poll();
    EXPECT_EQ(token.outstanding(), N);

    token.cancel_all();
    io.run();
    EXPECT_EQ(completed, N);
    EXPECT_EQ(token.outstanding(), 0);
}

TEST(CancellationToken, CancelIdempotent)
{
    CancellationToken token;
    token.cancel_all();
    token.cancel_all();
    EXPECT_EQ(token.outstanding(), 0);
}

TEST(CancellationToken, OutstandingIsAtomicRead)
{
    CancellationToken token;
    EXPECT_EQ(token.outstanding(), 0);

    // outstanding()는 어느 스레드에서든 안전하게 읽을 수 있어야 한다
    uint32_t val = 0;
    std::thread t([&] { val = token.outstanding(); });
    t.join();
    EXPECT_EQ(val, 0);
}
