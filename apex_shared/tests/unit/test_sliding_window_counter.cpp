#include <apex/shared/rate_limit/sliding_window_counter.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::rate_limit;
using namespace std::chrono_literals;

// Helper: create a counter with known start time
class SlidingWindowCounterTest : public ::testing::Test
{
  protected:
    using Clock = SlidingWindowCounter::Clock;
    using TimePoint = SlidingWindowCounter::TimePoint;

    // Base time for deterministic testing
    TimePoint base_ = Clock::now();
};

TEST_F(SlidingWindowCounterTest, AllowWithinLimit)
{
    SlidingWindowCounter counter(10, 1s);

    for (int i = 0; i < 10; ++i)
    {
        EXPECT_TRUE(counter.allow(base_ + std::chrono::milliseconds(i * 10)));
    }
}

TEST_F(SlidingWindowCounterTest, DenyWhenLimitReached)
{
    SlidingWindowCounter counter(5, 1s);

    for (int i = 0; i < 5; ++i)
    {
        EXPECT_TRUE(counter.allow(base_ + std::chrono::milliseconds(i)));
    }
    // 6th request should be denied
    EXPECT_FALSE(counter.allow(base_ + 5ms));
}

TEST_F(SlidingWindowCounterTest, WindowRotation)
{
    SlidingWindowCounter counter(5, 1s);

    // Fill first window
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_TRUE(counter.allow(base_ + std::chrono::milliseconds(i * 100)));
    }
    EXPECT_FALSE(counter.allow(base_ + 500ms));

    // Move to second window (90% elapsed in new window)
    // estimate = 5 * (1 - 0.9) + 0 = 0.5, well under limit 5
    auto t = base_ + 1s + 900ms;
    EXPECT_TRUE(counter.allow(t));
}

TEST_F(SlidingWindowCounterTest, SlidingWindowWeightedEstimate)
{
    SlidingWindowCounter counter(100, 1s);

    // Fill first window with 80 requests
    for (int i = 0; i < 80; ++i)
    {
        EXPECT_TRUE(counter.allow(base_ + std::chrono::milliseconds(i)));
    }

    // Move to 50% into second window
    // estimate = 80 * 0.5 + 0 = 40 -> under limit
    auto t = base_ + 1500ms;
    double est = counter.estimated_count(t);
    EXPECT_NEAR(est, 40.0, 1.0);
}

TEST_F(SlidingWindowCounterTest, TwoWindowsPassedResetsCompletely)
{
    SlidingWindowCounter counter(5, 1s);

    for (int i = 0; i < 5; ++i)
    {
        EXPECT_TRUE(counter.allow(base_ + std::chrono::milliseconds(i)));
    }

    // Skip 2+ windows -- everything stale
    auto t = base_ + 3s;
    EXPECT_TRUE(counter.allow(t));
    EXPECT_NEAR(counter.estimated_count(t + 1ms), 1.0, 0.1);
}

TEST_F(SlidingWindowCounterTest, Reset)
{
    SlidingWindowCounter counter(5, 1s);

    for (int i = 0; i < 5; ++i)
    {
        (void)counter.allow(base_ + std::chrono::milliseconds(i));
    }

    counter.reset();
    EXPECT_TRUE(counter.allow(base_ + 2s));
}

TEST_F(SlidingWindowCounterTest, LastAccess)
{
    SlidingWindowCounter counter(10, 1s);

    auto t1 = base_ + 100ms;
    (void)counter.allow(t1);
    EXPECT_EQ(counter.last_access(), t1);

    auto t2 = base_ + 200ms;
    (void)counter.allow(t2);
    EXPECT_EQ(counter.last_access(), t2);
}

TEST_F(SlidingWindowCounterTest, DeniedRequestDoesNotUpdateLastAccess)
{
    SlidingWindowCounter counter(1, 1s);

    auto t1 = base_;
    (void)counter.allow(t1); // allowed
    auto t2 = base_ + 100ms;
    (void)counter.allow(t2); // denied
    EXPECT_EQ(counter.last_access(), t1);
}

TEST_F(SlidingWindowCounterTest, ZeroLimitDeniesEverything)
{
    SlidingWindowCounter counter(0, 1s);
    EXPECT_FALSE(counter.allow(base_));
}

TEST_F(SlidingWindowCounterTest, BoundaryBurstPrevention)
{
    // Fixed Window의 문제: 윈도우 경계에서 2배 burst 가능.
    // Sliding Window Counter는 이를 방지한다.
    SlidingWindowCounter counter(100, 1s);

    // 첫 윈도우 끝부분에 100개 요청
    for (int i = 0; i < 100; ++i)
    {
        EXPECT_TRUE(counter.allow(base_ + 900ms + std::chrono::microseconds(i)));
    }

    // 두 번째 윈도우 시작 직후 (1ms 경과)
    // estimate = 100 * (1 - 0.001) + 0 ~= 99.9 -> 거의 limit
    // Fixed Window였으면 여기서 100개 더 보낼 수 있지만,
    // Sliding Window는 이전 윈도우 가중치가 높아서 차단
    auto t = base_ + 1001ms;
    EXPECT_FALSE(counter.allow(t));
}
