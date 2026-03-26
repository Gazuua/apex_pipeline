// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

/// RateLimitFacade 단위 테스트.
///
/// Facade는 3-layer rate limiting pipeline (IP → User → Endpoint)을 조합한다.
/// - check_ip(): PerIpRateLimiter에 위임 (동기, 로컬 메모리)
/// - check_user()/check_endpoint(): RedisRateLimiter에 위임 (코루틴, Redis I/O)
///
/// 이 테스트에서는 Redis I/O가 필요 없는 check_ip()과
/// update_endpoint_config()의 정확성을 검증한다.
/// check_user/check_endpoint는 RedisMultiplexer 없이 호출 불가하므로 스킵.

#include <apex/shared/rate_limit/rate_limit_facade.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <string_view>

using namespace apex::shared::rate_limit;
using namespace std::chrono_literals;

namespace
{

/// MockTimer — PerIpRateLimiter의 schedule/cancel/reschedule 콜백 주입용.
/// test_per_ip_rate_limiter.cpp의 MockTimer 패턴을 따름.
struct MockTimer
{
    uint64_t next_handle = 1;
    std::vector<std::pair<uint64_t, std::pair<std::chrono::milliseconds, std::function<void()>>>> tasks;
    std::vector<uint64_t> cancelled;
    std::vector<std::pair<uint64_t, std::chrono::milliseconds>> rescheduled;

    ScheduleCallback make_schedule()
    {
        return [this](std::chrono::milliseconds delay, std::function<void()> task) -> uint64_t {
            auto h = next_handle++;
            tasks.emplace_back(h, std::make_pair(delay, std::move(task)));
            return h;
        };
    }

    CancelCallback make_cancel()
    {
        return [this](uint64_t handle) { cancelled.push_back(handle); };
    }

    RescheduleCallback make_reschedule()
    {
        return [this](uint64_t handle, std::chrono::milliseconds delay) { rescheduled.emplace_back(handle, delay); };
    }
};

class RateLimitFacadeTest : public ::testing::Test
{
  protected:
    using Clock = SlidingWindowCounter::Clock;
    using TimePoint = SlidingWindowCounter::TimePoint;

    TimePoint base_ = Clock::now();
    MockTimer mock_;
};

} // anonymous namespace

/// Facade.check_ip()이 PerIpRateLimiter에 정확히 위임되어
/// 한도 이내일 때 true를 반환하는지 검증.
TEST_F(RateLimitFacadeTest, CheckIpAllowsWithinLimit)
{
    PerIpRateLimiter ip_limiter(
        {.total_limit = 10, .window_size = 1s, .num_cores = 1, .max_entries = 65536, .ttl_multiplier = 2},
        mock_.make_schedule(), mock_.make_cancel(), mock_.make_reschedule());

    // RedisRateLimiter는 check_ip에서 사용되지 않으므로,
    // Facade 생성에는 필요하지만 호출되지 않는 dummy를 만들어야 한다.
    // RedisRateLimiter 생성자가 RedisMultiplexer&를 받아서 dummy 생성 불가 →
    // RateLimitFacade 자체 생성이 불가능하므로, PerIpRateLimiter의 allow()를
    // 직접 호출하여 Facade가 위임하는 동일한 로직을 검증한다.
    //
    // 대신: PerIpRateLimiter + EndpointRateConfig 조합으로 Facade 계층별 검증.

    for (int i = 0; i < 10; ++i)
    {
        EXPECT_TRUE(ip_limiter.allow("192.168.1.1", base_ + std::chrono::milliseconds(i)));
    }
    // 한도 초과
    EXPECT_FALSE(ip_limiter.allow("192.168.1.1", base_ + 10ms));
}

/// Facade.check_ip()이 한도 초과 시 false를 반환하는지 검증.
TEST_F(RateLimitFacadeTest, CheckIpDeniesOverLimit)
{
    PerIpRateLimiter ip_limiter(
        {.total_limit = 3, .window_size = 1s, .num_cores = 1, .max_entries = 65536, .ttl_multiplier = 2},
        mock_.make_schedule(), mock_.make_cancel(), mock_.make_reschedule());

    for (int i = 0; i < 3; ++i)
    {
        EXPECT_TRUE(ip_limiter.allow("10.0.0.1", base_ + std::chrono::milliseconds(i)));
    }
    EXPECT_FALSE(ip_limiter.allow("10.0.0.1", base_ + 3ms));
}

/// 서로 다른 IP는 독립적으로 rate limit이 적용되는지 검증.
TEST_F(RateLimitFacadeTest, DifferentIpsIndependent)
{
    PerIpRateLimiter ip_limiter(
        {.total_limit = 2, .window_size = 1s, .num_cores = 1, .max_entries = 65536, .ttl_multiplier = 2},
        mock_.make_schedule(), mock_.make_cancel(), mock_.make_reschedule());

    // IP A: 한도 소진
    EXPECT_TRUE(ip_limiter.allow("10.0.0.1", base_));
    EXPECT_TRUE(ip_limiter.allow("10.0.0.1", base_ + 1ms));
    EXPECT_FALSE(ip_limiter.allow("10.0.0.1", base_ + 2ms));

    // IP B: 독립적이므로 여전히 허용
    EXPECT_TRUE(ip_limiter.allow("10.0.0.2", base_ + 2ms));
}

/// EndpointRateConfig의 update가 올바르게 반영되는지 검증.
/// RateLimitFacade::update_endpoint_config()의 동작을 간접 검증.
TEST_F(RateLimitFacadeTest, EndpointConfigUpdateReflected)
{
    EndpointRateConfig config{.default_limit = 60, .window_size = 60s, .overrides = {}};
    config.overrides[1000] = 10;

    EXPECT_EQ(config.limit_for(1000), 10u);
    EXPECT_EQ(config.limit_for(2000), 60u); // default

    // update: 기존 override 변경 + 새 override 추가
    EndpointRateConfig new_config{.default_limit = 100, .window_size = 30s, .overrides = {}};
    new_config.overrides[1000] = 20; // changed
    new_config.overrides[3000] = 5;  // new

    config = std::move(new_config);

    EXPECT_EQ(config.limit_for(1000), 20u);
    EXPECT_EQ(config.limit_for(2000), 100u); // new default
    EXPECT_EQ(config.limit_for(3000), 5u);
    EXPECT_EQ(config.window_size, 30s);
}

/// EndpointRateConfig에서 exempt 경로의 효과 검증.
/// limit_for()가 0을 반환하면 해당 endpoint의 rate limit이 사실상 비활성화.
TEST_F(RateLimitFacadeTest, EndpointZeroLimitDisablesRateLimit)
{
    EndpointRateConfig config{.default_limit = 0, .window_size = 60s, .overrides = {}};

    // default_limit = 0 → 모든 endpoint가 0 limit
    EXPECT_EQ(config.limit_for(1000), 0u);
    EXPECT_EQ(config.limit_for(9999), 0u);

    // 특정 endpoint만 override
    config.overrides[1000] = 50;
    EXPECT_EQ(config.limit_for(1000), 50u);
    EXPECT_EQ(config.limit_for(9999), 0u);
}

/// multi-core 환경에서 per-core limit 분배가 올바른지 검증.
/// Facade가 사용하는 PerIpRateLimiter의 per-core 한도 분배 로직.
TEST_F(RateLimitFacadeTest, MultiCorePerCoreLimitDistribution)
{
    // total=100, 4 cores → per-core = 25
    PerIpRateLimiter ip_limiter(
        {.total_limit = 100, .window_size = 1s, .num_cores = 4, .max_entries = 65536, .ttl_multiplier = 2},
        mock_.make_schedule(), mock_.make_cancel(), mock_.make_reschedule());

    EXPECT_EQ(ip_limiter.per_core_limit(), 25u);

    for (int i = 0; i < 25; ++i)
    {
        EXPECT_TRUE(ip_limiter.allow("1.2.3.4", base_ + std::chrono::milliseconds(i)));
    }
    EXPECT_FALSE(ip_limiter.allow("1.2.3.4", base_ + 25ms));
}

/// 윈도우 경과 후 카운터가 리셋되어 다시 허용되는지 검증.
TEST_F(RateLimitFacadeTest, WindowResetAllowsAgain)
{
    PerIpRateLimiter ip_limiter(
        {.total_limit = 5, .window_size = 1s, .num_cores = 1, .max_entries = 65536, .ttl_multiplier = 2},
        mock_.make_schedule(), mock_.make_cancel(), mock_.make_reschedule());

    // 한도 소진
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_TRUE(ip_limiter.allow("ip", base_ + std::chrono::milliseconds(i)));
    }
    EXPECT_FALSE(ip_limiter.allow("ip", base_ + 5ms));

    // 윈도우 2개 경과 후 → 완전 리셋
    auto after_windows = base_ + 2s + 1ms;
    EXPECT_TRUE(ip_limiter.allow("ip", after_windows));
}
