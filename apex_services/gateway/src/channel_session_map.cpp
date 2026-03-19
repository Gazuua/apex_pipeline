#include <apex/gateway/channel_session_map.hpp>

#include <apex/core/error_code.hpp>

#include <algorithm>

namespace apex::gateway {

ChannelSessionMap::ChannelSessionMap(uint32_t max_subscriptions_per_session)
    : max_subscriptions_per_session_(max_subscriptions_per_session) {}

apex::core::Result<void>
ChannelSessionMap::subscribe(const std::string& channel,
                               apex::core::SessionId session_id) {
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
    auto it = std::ranges::find(sessions, session_id);
    if (it == sessions.end()) {
        sessions.push_back(session_id);
    }

    channels.insert(channel);
    return apex::core::ok();
}

void ChannelSessionMap::unsubscribe(const std::string& channel,
                                     apex::core::SessionId session_id) {
    auto ch_it = channel_to_sessions_.find(channel);
    if (ch_it != channel_to_sessions_.end()) {
        auto& sessions = ch_it->second;
        std::erase(sessions, session_id);
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
    auto sess_it = session_to_channels_.find(session_id);
    if (sess_it == session_to_channels_.end()) return;

    for (const auto& channel : sess_it->second) {
        auto ch_it = channel_to_sessions_.find(channel);
        if (ch_it != channel_to_sessions_.end()) {
            auto& sessions = ch_it->second;
            std::erase(sessions, session_id);
            if (sessions.empty()) {
                channel_to_sessions_.erase(ch_it);
            }
        }
    }

    session_to_channels_.erase(sess_it);
}

const std::vector<apex::core::SessionId>*
ChannelSessionMap::get_subscribers(const std::string& channel) const {
    auto it = channel_to_sessions_.find(channel);
    if (it == channel_to_sessions_.end()) return nullptr;
    return &it->second;
}

std::vector<std::string>
ChannelSessionMap::subscribed_channels() const {
    std::vector<std::string> result;
    result.reserve(channel_to_sessions_.size());
    for (const auto& [ch, _] : channel_to_sessions_) {
        result.push_back(ch);
    }
    return result;
}

size_t ChannelSessionMap::total_subscriptions() const {
    size_t total = 0;
    for (const auto& [_, sessions] : channel_to_sessions_) {
        total += sessions.size();
    }
    return total;
}

} // namespace apex::gateway
