// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/core_engine.hpp>
#include <apex/core/session_manager.hpp>

#include <hiredis/hiredis.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace apex::gateway
{

/// Redis Pub/Sub dedicated listener.
/// Manages Redis SUBSCRIBE on a dedicated thread,
/// calls cross_core_post to fan-out messages to target cores.
///
/// Design principles (design doc section 5.2):
/// - Gateway does not know the meaning of channel names (opaque string)
/// - Subscribe/unsubscribe is directed by services via control messages
/// - Global broadcast channels are auto-subscribed on startup
class PubSubListener
{
  public:
    /// Message receive callback.
    /// @param channel Channel name
    /// @param message Message data
    using MessageCallback = std::function<void(std::string_view channel, std::span<const uint8_t> message)>;

    struct Config
    {
        std::string host = "localhost";
        uint16_t port = 6379;
        std::string password;
        std::vector<std::string> initial_channels; // Auto-subscribe on start
        uint32_t reconnect_interval_ms = 1000;     ///< 재연결 대기 간격 (ms)
    };

    explicit PubSubListener(const Config& config, MessageCallback on_message);
    ~PubSubListener();

    PubSubListener(const PubSubListener&) = delete;
    PubSubListener& operator=(const PubSubListener&) = delete;

    /// Start Pub/Sub on dedicated thread.
    void start();

    /// Stop + join thread.
    void stop();

    /// Add channel subscription (thread-safe).
    /// Takes effect immediately (within ~1s select timeout) without reconnect.
    void subscribe(const std::string& channel);

    /// Remove channel subscription (thread-safe).
    void unsubscribe(const std::string& channel);

  private:
    void run_thread();

    /// Apply pending subscribe/unsubscribe to the live Redis connection.
    /// Called from run_thread() when has_pending_subs_ is set.
    void apply_pending_subscriptions(redisContext* ctx, std::unordered_set<std::string>& active_subs);

    Config config_;
    MessageCallback on_message_;

    std::thread thread_;
    std::atomic<bool> running_{false};

    std::mutex channels_mutex_;
    std::unordered_set<std::string> subscribed_channels_;

    /// Signals run_thread() to check for new subscribe/unsubscribe requests.
    std::atomic<bool> has_pending_subs_{false};
};

} // namespace apex::gateway
