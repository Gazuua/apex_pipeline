#pragma once

#include <apex/core/session.hpp>

#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace apex::gateway {

/// Channel -> session mapping.
/// PubSubListener looks up target sessions through this map on message receive.
///
/// Synchronization:
/// - PubSubListener thread reads (shared_lock)
/// - Service logic threads write (unique_lock)
/// - shared_mutex for reader-favoring (broadcasts far more frequent than sub/unsub)
class ChannelSessionMap {
public:
    /// Subscribe session to channel.
    void subscribe(const std::string& channel,
                   apex::core::SessionId session_id,
                   uint32_t core_id);

    /// Unsubscribe session from channel.
    void unsubscribe(const std::string& channel,
                     apex::core::SessionId session_id);

    /// Unsubscribe session from all channels (on session close).
    void unsubscribe_all(apex::core::SessionId session_id);

    /// Get subscribers for channel (grouped by core_id).
    /// @return core_id -> session_ids map
    struct SessionGroup {
        uint32_t core_id;
        std::vector<apex::core::SessionId> session_ids;
    };
    [[nodiscard]] std::vector<SessionGroup>
    get_subscribers(const std::string& channel) const;

private:
    struct SessionInfo {
        apex::core::SessionId session_id;
        uint32_t core_id;
    };

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string,
                       std::vector<SessionInfo>> channel_to_sessions_;
    // Reverse index: remove from all channels on session close
    std::unordered_map<apex::core::SessionId,
                       std::unordered_set<std::string>> session_to_channels_;
};

} // namespace apex::gateway
