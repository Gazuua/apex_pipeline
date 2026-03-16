#include <apex/gateway/channel_session_map.hpp>

#include <apex/core/error_code.hpp>

#include <algorithm>

namespace apex::gateway {

ChannelSessionMap::ChannelSessionMap(uint32_t max_subscriptions_per_session)
    : max_subscriptions_per_session_(max_subscriptions_per_session) {}

apex::core::Result<void>
ChannelSessionMap::subscribe(const std::string& channel,
                               apex::core::SessionId session_id,
                               uint32_t core_id) {
    std::unique_lock lock(mutex_);

    // Check per-session subscription limit
    auto& channels = session_to_channels_[session_id];
    if (max_subscriptions_per_session_ > 0 &&
        channels.size() >= max_subscriptions_per_session_ &&
        !channels.contains(channel)) {
        return apex::core::error(
            apex::core::ErrorCode::SubscriptionLimitExceeded);
    }

    auto& sessions = channel_to_sessions_[channel];
    // Check for duplicate
    auto it = std::ranges::find_if(sessions, [session_id](const SessionInfo& s) {
        return s.session_id == session_id;
    });
    if (it == sessions.end()) {
        sessions.push_back(SessionInfo{session_id, core_id});
    }

    channels.insert(channel);
    return apex::core::ok();
}

void ChannelSessionMap::unsubscribe(const std::string& channel,
                                     apex::core::SessionId session_id) {
    std::unique_lock lock(mutex_);

    auto ch_it = channel_to_sessions_.find(channel);
    if (ch_it != channel_to_sessions_.end()) {
        auto& sessions = ch_it->second;
        std::erase_if(sessions, [session_id](const SessionInfo& s) {
            return s.session_id == session_id;
        });
        if (sessions.empty()) {
            channel_to_sessions_.erase(ch_it);
        }
    }

    auto sess_it = session_to_channels_.find(session_id);
    if (sess_it != session_to_channels_.end()) {
        sess_it->second.erase(channel);
        if (sess_it->second.empty()) {
            session_to_channels_.erase(sess_it);
        }
    }
}

void ChannelSessionMap::unsubscribe_all(apex::core::SessionId session_id) {
    std::unique_lock lock(mutex_);

    auto sess_it = session_to_channels_.find(session_id);
    if (sess_it == session_to_channels_.end()) return;

    for (const auto& channel : sess_it->second) {
        auto ch_it = channel_to_sessions_.find(channel);
        if (ch_it != channel_to_sessions_.end()) {
            auto& sessions = ch_it->second;
            std::erase_if(sessions, [session_id](const SessionInfo& s) {
                return s.session_id == session_id;
            });
            if (sessions.empty()) {
                channel_to_sessions_.erase(ch_it);
            }
        }
    }

    session_to_channels_.erase(sess_it);
}

std::vector<ChannelSessionMap::SessionGroup>
ChannelSessionMap::get_subscribers(const std::string& channel) const {
    std::shared_lock lock(mutex_);

    auto it = channel_to_sessions_.find(channel);
    if (it == channel_to_sessions_.end()) return {};

    // Group by core_id
    std::unordered_map<uint32_t, std::vector<apex::core::SessionId>> grouped;
    for (const auto& s : it->second) {
        grouped[s.core_id].push_back(s.session_id);
    }

    std::vector<SessionGroup> result;
    result.reserve(grouped.size());
    for (auto& [core_id, session_ids] : grouped) {
        result.push_back(SessionGroup{core_id, std::move(session_ids)});
    }
    return result;
}

} // namespace apex::gateway
