#include <apex/shared/rate_limit/rate_limit_facade.hpp>

namespace apex::shared::rate_limit {

RateLimitFacade::RateLimitFacade(
    PerIpRateLimiter& ip_limiter,
    RedisRateLimiter& redis_limiter,
    EndpointRateConfig endpoint_config)
    : ip_limiter_(ip_limiter),
      redis_limiter_(redis_limiter),
      endpoint_config_(std::move(endpoint_config)) {}

bool RateLimitFacade::check_ip(
    std::string_view ip,
    SlidingWindowCounter::TimePoint now) noexcept {
    return ip_limiter_.allow(ip, now);
}

boost::asio::awaitable<apex::core::Result<RateLimitResult>>
RateLimitFacade::check_user(uint64_t user_id, uint64_t now_ms) {
    co_return co_await redis_limiter_.check_user(user_id, now_ms);
}

boost::asio::awaitable<apex::core::Result<RateLimitResult>>
RateLimitFacade::check_endpoint(uint64_t user_id, uint32_t msg_id,
                                uint64_t now_ms) {
    auto limit = endpoint_config_.limit_for(msg_id);
    co_return co_await redis_limiter_.check_endpoint(user_id, msg_id, now_ms, limit);
}

void RateLimitFacade::update_endpoint_config(EndpointRateConfig config) noexcept {
    endpoint_config_ = std::move(config);
}

} // namespace apex::shared::rate_limit
