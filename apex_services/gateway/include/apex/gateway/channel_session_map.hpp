#pragma once

#include <apex/core/result.hpp>
#include <apex/core/session.hpp>

#include <boost/unordered/unordered_flat_map.hpp>

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace apex::gateway
{

/// Per-core 채널 구독 맵. 뮤텍스 불필요 — 해당 코어의 io_context에서만 접근.
///
/// 세션은 per-core (한 세션은 정확히 하나의 코어에 소속).
/// 따라서 채널 구독 데이터도 자연스럽게 per-core.
///
/// Subscribe/Unsubscribe: 해당 코어의 서비스 핸들러에서 로컬 맵만 수정.
/// Broadcast: PubSubListener → 모든 코어에 post → 각 코어가 로컬 맵 조회.
class ChannelSessionMap
{
  public:
    /// @param max_subscriptions_per_session 0 = unlimited
    explicit ChannelSessionMap(uint32_t max_subscriptions_per_session = 0);

    /// Subscribe session to channel.
    /// @return SubscriptionLimitExceeded if per-session limit reached
    [[nodiscard]] apex::core::Result<void> subscribe(const std::string& channel, apex::core::SessionId session_id);

    /// Unsubscribe session from channel.
    void unsubscribe(const std::string& channel, apex::core::SessionId session_id);

    /// Unsubscribe session from all channels (on session close).
    void unsubscribe_all(apex::core::SessionId session_id);

    /// 해당 채널의 로컬(이 코어) 구독자 목록 반환. 없으면 nullptr.
    [[nodiscard]] const std::vector<apex::core::SessionId>* get_subscribers(const std::string& channel) const;

    /// 이 코어에서 구독 중인 채널 목록 (PubSub 구독 관리용)
    [[nodiscard]] std::vector<std::string> subscribed_channels() const;

    /// 전체 구독 수 (디버그/모니터링용)
    [[nodiscard]] size_t total_subscriptions() const;

  private:
    uint32_t max_subscriptions_per_session_;
    // 뮤텍스 제거 — per-core이므로 해당 코어 스레드에서만 접근
    boost::unordered_flat_map<std::string, std::vector<apex::core::SessionId>> channel_to_sessions_;
    // Reverse index: remove from all channels on session close
    boost::unordered_flat_map<apex::core::SessionId, std::unordered_set<std::string>> session_to_channels_;
};

} // namespace apex::gateway
