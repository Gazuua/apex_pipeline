// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/periodic_task_scheduler.hpp>

#include "../test_helpers.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace apex::core;
using namespace std::chrono_literals;

// ScheduleExecutesMultipleTimes:
// 50ms 인터벌 작업을 등록하고 250ms 동안 실행 — 최소 3회 이상 실행되어야 한다.
TEST(PeriodicTaskScheduler, ScheduleExecutesMultipleTimes)
{
    boost::asio::io_context io_ctx;
    PeriodicTaskScheduler scheduler(io_ctx);

    std::atomic<int> count{0};

    scheduler.schedule(50ms, [&count] { count.fetch_add(1, std::memory_order_relaxed); });

    // run_for: 250ms 동안 io_context를 실행 (Boost >= 1.77)
    io_ctx.run_for(250ms);

    // 50ms 인터벌 × 250ms = 이론상 5회. 타이밍 여유를 고려해 3회 이상을 기대.
    EXPECT_GE(count.load(), 3);
}

// CancelStopsExecution:
// 작업을 등록하고 80ms 후 취소 — 1회 이상 실행 후 취소 이후로는 실행이 없어야 한다.
//
// 주의: PeriodicTaskScheduler는 단일 스레드(io_context 실행 스레드) 전용이다.
// cancel()은 반드시 io_context 스레드에서 호출해야 한다.
// boost::asio::post로 cancel을 io_context 스레드에 디스패치하여 데이터 레이스를 방지.
TEST(PeriodicTaskScheduler, CancelStopsExecution)
{
    boost::asio::io_context io_ctx;
    PeriodicTaskScheduler scheduler(io_ctx);

    std::atomic<int> count{0};
    std::atomic<int> count_at_cancel{0};
    std::atomic<bool> cancelled{false};

    auto handle = scheduler.schedule(50ms, [&count] { count.fetch_add(1, std::memory_order_relaxed); });

    // 별도 스레드에서 io_context 실행
    auto work = boost::asio::make_work_guard(io_ctx);
    std::thread io_thread([&io_ctx] { io_ctx.run(); });

    // 50ms 인터벌이므로 최소 1회 실행될 때까지 이벤트 대기 (BACKLOG-119).
    // 고정 sleep_for 대신 wait_for 폴링으로 TSAN/ASAN 환경에서도 안정.
    auto count_reached = apex::test::wait_for([&] { return count.load(std::memory_order_relaxed) >= 1; });

    // cancel을 io_context 스레드에 post — 스케줄러는 단일 스레드 전용
    boost::asio::post(io_ctx, [&] {
        scheduler.cancel(handle);
        // 취소 시점의 카운트를 기록 (io_context 스레드에서 읽으므로 안전)
        count_at_cancel.store(count.load(std::memory_order_relaxed), std::memory_order_release);
        cancelled.store(true, std::memory_order_release);
    });

    // cancel 완료 대기
    while (!cancelled.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(1ms);
    }

    // wait_for가 타임아웃 없이 1회 이상 실행을 확인했으므로 검증
    EXPECT_TRUE(count_reached);
    EXPECT_GE(count_at_cancel.load(), 1);

    // 부재 증명: 취소 이후 추가 실행 없어야 함 (BACKLOG-119).
    // TSAN/ASAN 환경에서는 timeout_multiplier로 스케일링.
    std::this_thread::sleep_for(100ms * apex::test::timeout_multiplier());
    int count_after_wait = count.load(std::memory_order_acquire);

    work.reset();
    io_ctx.stop();
    io_thread.join();

    // 취소 후 카운트가 증가하지 않았음을 확인
    EXPECT_EQ(count_at_cancel.load(), count_after_wait);
}

// StopAllCancelsEverything:
// 2개 작업 등록 후 즉시 stop_all() 호출 — io_context 실행 시 카운트는 0이어야 한다.
TEST(PeriodicTaskScheduler, StopAllCancelsEverything)
{
    boost::asio::io_context io_ctx;
    PeriodicTaskScheduler scheduler(io_ctx);

    std::atomic<int> count_a{0};
    std::atomic<int> count_b{0};

    scheduler.schedule(50ms, [&count_a] { count_a.fetch_add(1, std::memory_order_relaxed); });
    scheduler.schedule(50ms, [&count_b] { count_b.fetch_add(1, std::memory_order_relaxed); });

    // 즉시 전체 취소 — 타이머 만료 전에 취소됨
    scheduler.stop_all();

    // 취소 후 io_context 실행 — 이미 취소된 타이머이므로 콜백이 호출되면 안 됨
    io_ctx.run_for(200ms);

    EXPECT_EQ(count_a.load(), 0);
    EXPECT_EQ(count_b.load(), 0);
}

// CancelNonexistentHandle:
// 존재하지 않는 핸들을 cancel하면 안전하게 무시되어야 한다.
TEST(PeriodicTaskScheduler, CancelNonexistentHandle)
{
    boost::asio::io_context io_ctx;
    PeriodicTaskScheduler scheduler(io_ctx);

    // 한 번도 schedule하지 않은 핸들
    TaskHandle bogus_handle = static_cast<TaskHandle>(9999);
    scheduler.cancel(bogus_handle); // should not crash or throw
}

// DoubleCancelSafe:
// 동일 핸들을 두 번 cancel해도 UB가 발생하지 않아야 한다.
TEST(PeriodicTaskScheduler, DoubleCancelSafe)
{
    boost::asio::io_context io_ctx;
    PeriodicTaskScheduler scheduler(io_ctx);

    std::atomic<int> count{0};
    auto h = scheduler.schedule(50ms, [&count] { count.fetch_add(1, std::memory_order_relaxed); });

    scheduler.cancel(h);
    scheduler.cancel(h); // double cancel — should not crash

    io_ctx.run_for(200ms);
    EXPECT_EQ(count.load(), 0);
}

// ScheduleAfterStopAll:
// stop_all 후에도 새 작업을 schedule할 수 있어야 한다.
TEST(PeriodicTaskScheduler, ScheduleAfterStopAll)
{
    boost::asio::io_context io_ctx;
    PeriodicTaskScheduler scheduler(io_ctx);

    std::atomic<int> count{0};
    scheduler.schedule(50ms, [&count] { count.fetch_add(1, std::memory_order_relaxed); });
    scheduler.stop_all();

    // stop_all 후 새 작업 등록
    std::atomic<int> count2{0};
    scheduler.schedule(50ms, [&count2] { count2.fetch_add(1, std::memory_order_relaxed); });

    io_ctx.run_for(250ms);
    EXPECT_EQ(count.load(), 0);
    EXPECT_GE(count2.load(), 3);
}
