#pragma once

#include <chrono>
#include <cstdint>

#include <boost/unordered/unordered_flat_map.hpp>

namespace apex::shared::rate_limit {

/// Per-endpoint rate limit configuration.
/// Loaded from TOML config, supports hot-reload.
///
/// TOML 예시:
/// ```toml
/// [rate_limit.endpoint]
/// default_limit = 60
/// window_size_seconds = 60
///
/// # msg_id별 오버라이드
/// [rate_limit.endpoint.overrides]
/// 1001 = 10    # LoginRequest: 분당 10회
/// 2001 = 200   # ChatSendMessage: 분당 200회
/// 2010 = 5     # CreateRoom: 분당 5회
/// ```
struct EndpointRateConfig {
    uint32_t default_limit = 60;             ///< msg_id 오버라이드가 없을 때 적용
    std::chrono::seconds window_size{60};    ///< 윈도우 크기

    /// msg_id -> limit 오버라이드 매핑
    boost::unordered_flat_map<uint32_t, uint32_t> overrides;

    /// Get the effective limit for a msg_id.
    [[nodiscard]] uint32_t limit_for(uint32_t msg_id) const noexcept {
        auto it = overrides.find(msg_id);
        if (it != overrides.end()) {
            return it->second;
        }
        return default_limit;
    }
};

} // namespace apex::shared::rate_limit
