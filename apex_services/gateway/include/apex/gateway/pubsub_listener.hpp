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

namespace apex::gateway {

/// Redis Pub/Sub dedicated listener.
/// Manages Redis SUBSCRIBE on a dedicated thread,
/// calls cross_core_post to fan-out messages to target cores.
///
/// Design principles (design doc section 5.2):
/// - Gateway does not know the meaning of channel names (opaque string)
/// - Subscribe/unsubscribe is directed by services via control messages
/// - Global broadcast channels are auto-subscribed on startup
class PubSubListener {
public:
    /// Message receive callback.
    /// @param channel Channel name
    /// @param message Message data
    using MessageCallback = std::function<void(
        std::string_view channel,
        std::span<const uint8_t> message)>;

    struct Config {
        std::string host = "localhost";
        uint16_t port = 6379;
        std::string password;
        std::vector<std::string> initial_channels;  // Auto-subscribe on start
    };

    explicit PubSubListener(const Config& config,
                            MessageCallback on_message);
    ~PubSubListener();

    PubSubListener(const PubSubListener&) = delete;
    PubSubListener& operator=(const PubSubListener&) = delete;

    /// Start Pub/Sub on dedicated thread.
    void start();

    /// Stop + join thread.
    void stop();

    /// Add channel subscription (thread-safe).
    void subscribe(const std::string& channel);

    /// Remove channel subscription (thread-safe).
    void unsubscribe(const std::string& channel);

private:
    void run_thread();

    Config config_;
    MessageCallback on_message_;

    std::thread thread_;
    std::atomic<bool> running_{false};

    std::mutex channels_mutex_;
    std::unordered_set<std::string> subscribed_channels_;
};

} // namespace apex::gateway
