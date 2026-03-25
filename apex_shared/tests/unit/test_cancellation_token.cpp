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

// --- 엣지 케이스 테스트 ---

// TC-E1: cancel_all() 후 on_complete() 호출 순서 — cancel 후 complete가 호출되어야 함
TEST(CancellationToken, CancelAllThenOnComplete)
{
    boost::asio::io_context io;
    CancellationToken token;

    auto slot = token.new_slot();
    EXPECT_EQ(token.outstanding(), 1);

    bool cancel_observed = false;
    bool complete_called = false;

    boost::asio::steady_timer timer(io, std::chrono::hours{1});

    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            auto [ec] = co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
            if (ec == boost::asio::error::operation_aborted)
                cancel_observed = true;
            token.on_complete();
            complete_called = true;
        },
        boost::asio::bind_cancellation_slot(slot, boost::asio::detached));

    // 코루틴을 시작시킨다 (타이머 대기 진입)
    io.run_one();
    EXPECT_EQ(token.outstanding(), 1);

    // cancel_all 호출 — 코루틴에 취소 시그널 발행
    token.cancel_all();

    // 코루틴이 취소 처리 + on_complete 실행
    io.run();

    EXPECT_TRUE(cancel_observed) << "cancel should be observed before on_complete";
    EXPECT_TRUE(complete_called) << "on_complete should be called after cancel";
    EXPECT_EQ(token.outstanding(), 0);
}

// TC-E2: outstanding() 언더플로우 — 0일 때 on_complete 호출은 assert 실패
// Release 빌드에서는 unsigned underflow(wrap-around)가 발생한다.
// Debug 빌드에서는 assert에 걸린다. 여기서는 Release 모드에서 wrap 확인.
#ifdef NDEBUG
TEST(CancellationToken, OnCompleteUnderflowWrapsInRelease)
{
    CancellationToken token;
    EXPECT_EQ(token.outstanding(), 0);

    // Release 빌드: assert 없이 unsigned underflow → UINT32_MAX
    token.on_complete();
    EXPECT_EQ(token.outstanding(), UINT32_MAX);
}
#endif

// TC-E3: new_slot() → cancel_all() → new_slot() (재사용)
// cancel 후에도 새 슬롯 할당이 정상 동작해야 한다.
TEST(CancellationToken, ReuseAfterCancelAll)
{
    boost::asio::io_context io;
    CancellationToken token;

    // Phase 1: 슬롯 할당 후 cancel
    {
        auto slot1 = token.new_slot();
        EXPECT_EQ(token.outstanding(), 1);

        boost::asio::steady_timer timer1(io, std::chrono::hours{1});
        boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                auto [ec] = co_await timer1.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
                static_cast<void>(ec);
                token.on_complete();
            },
            boost::asio::bind_cancellation_slot(slot1, boost::asio::detached));

        io.poll();
        token.cancel_all();
        io.run();
        io.restart();
    }
    EXPECT_EQ(token.outstanding(), 0);

    // Phase 2: cancel 후 새 슬롯으로 재사용
    {
        auto slot2 = token.new_slot();
        EXPECT_EQ(token.outstanding(), 1);

        boost::asio::steady_timer timer2(io, std::chrono::hours{1});
        bool second_cancelled = false;

        boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                auto [ec] = co_await timer2.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
                if (ec == boost::asio::error::operation_aborted)
                    second_cancelled = true;
                token.on_complete();
            },
            boost::asio::bind_cancellation_slot(slot2, boost::asio::detached));

        io.poll();
        EXPECT_EQ(token.outstanding(), 1);

        token.cancel_all();
        io.run();

        EXPECT_TRUE(second_cancelled);
        EXPECT_EQ(token.outstanding(), 0);
    }
}

// TC-E4: 빈 상태에서 cancel_all() 호출 (no-op)
// 슬롯 없이 cancel_all을 호출해도 크래시하지 않아야 한다.
TEST(CancellationToken, CancelAllOnEmptyIsNoOp)
{
    CancellationToken token;
    EXPECT_EQ(token.outstanding(), 0);

    // 슬롯 없이 cancel_all — no-op이어야 함
    token.cancel_all();
    EXPECT_EQ(token.outstanding(), 0);

    // 이후 정상 동작 확인
    boost::asio::io_context io;
    auto slot = token.new_slot();
    EXPECT_EQ(token.outstanding(), 1);

    boost::asio::steady_timer timer(io, std::chrono::hours{1});
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            auto [ec] = co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
            static_cast<void>(ec);
            token.on_complete();
        },
        boost::asio::bind_cancellation_slot(slot, boost::asio::detached));

    io.poll();
    token.cancel_all();
    io.run();
    EXPECT_EQ(token.outstanding(), 0);
}
