#include <apex/gateway/pubsub_listener.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>

namespace apex::gateway {

PubSubListener::PubSubListener(const Config& config,
                               MessageCallback on_message)
    : config_(config)
    , on_message_(std::move(on_message)) {}

PubSubListener::~PubSubListener() {
    stop();
}

void PubSubListener::start() {
    running_ = true;
    thread_ = std::thread([this] { run_thread(); });
    spdlog::info("PubSubListener started ({}:{})",
        config_.host, config_.port);
}

void PubSubListener::stop() {
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
    spdlog::info("PubSubListener stopped");
}

void PubSubListener::subscribe(const std::string& channel) {
    std::lock_guard lock(channels_mutex_);
    subscribed_channels_.insert(channel);
    // Note: if connected, the subscribe command will be sent on next reconnect
    // or we would need async command injection. For the sync implementation,
    // new subscriptions take effect on reconnect.
}

void PubSubListener::unsubscribe(const std::string& channel) {
    std::lock_guard lock(channels_mutex_);
    subscribed_channels_.erase(channel);
}

void PubSubListener::run_thread() {
    while (running_) {
        // Synchronous hiredis connection for blocking subscribe.
        redisContext* sync_ctx = redisConnect(
            config_.host.c_str(), config_.port);
        if (!sync_ctx || sync_ctx->err) {
            spdlog::error("PubSub connect failed: {}",
                sync_ctx ? sync_ctx->errstr : "null context");
            if (sync_ctx) redisFree(sync_ctx);
            std::this_thread::sleep_for(std::chrono::seconds{1});
            continue;
        }

        // Set read timeout so we can check running_ periodically
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        redisSetTimeout(sync_ctx, tv);

        // AUTH
        if (!config_.password.empty()) {
            auto* reply = static_cast<redisReply*>(
                redisCommand(sync_ctx, "AUTH %s", config_.password.c_str()));
            if (reply) freeReplyObject(reply);
        }

        // Initial subscriptions
        {
            std::lock_guard lock(channels_mutex_);
            for (const auto& ch : config_.initial_channels) {
                subscribed_channels_.insert(ch);
            }
            for (const auto& ch : subscribed_channels_) {
                auto* reply = static_cast<redisReply*>(
                    redisCommand(sync_ctx, "SUBSCRIBE %s", ch.c_str()));
                if (reply) freeReplyObject(reply);
            }
        }

        // Message receive loop
        while (running_) {
            redisReply* reply = nullptr;
            int status = redisGetReply(sync_ctx,
                reinterpret_cast<void**>(&reply));

            if (status != REDIS_OK || !reply) {
                // Timeout detection: hiredis sets REDIS_ERR_IO on read timeout.
                // errno == EAGAIN works on Linux but not Windows (WSAETIMEDOUT).
                // Since we configured a 1s read timeout via redisSetTimeout,
                // any REDIS_ERR_IO without a reply is treated as timeout.
                if (sync_ctx->err == REDIS_ERR_IO && !reply) {
                    // Read timeout -- check running_ and loop
                    continue;
                }
                spdlog::warn("PubSub read error (err={}), reconnecting...",
                    sync_ctx->err);
                break;
            }

            if (reply->type == REDIS_REPLY_ARRAY &&
                reply->elements == 3 &&
                reply->element[0]->str &&
                std::string_view(reply->element[0]->str,
                    reply->element[0]->len) == "message") {

                std::string_view channel(
                    reply->element[1]->str, reply->element[1]->len);
                auto* data = reinterpret_cast<const uint8_t*>(
                    reply->element[2]->str);
                size_t len = reply->element[2]->len;

                if (on_message_) {
                    on_message_(channel,
                        std::span<const uint8_t>(data, len));
                }
            }

            freeReplyObject(reply);
        }

        redisFree(sync_ctx);

        if (running_) {
            spdlog::info("PubSub reconnecting in 1 second...");
            std::this_thread::sleep_for(std::chrono::seconds{1});
        }
    }
}

} // namespace apex::gateway
