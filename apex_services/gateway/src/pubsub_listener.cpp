// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/gateway/pubsub_listener.hpp>

#include <chrono>
#include <thread>

#ifdef _WIN32
#include <WinSock2.h>
#else
#include <sys/select.h>
#endif

namespace apex::gateway
{

PubSubListener::PubSubListener(const Config& config, MessageCallback on_message)
    : config_(config)
    , on_message_(std::move(on_message))
{}

PubSubListener::~PubSubListener()
{
    stop();
}

void PubSubListener::start()
{
    if (running_.exchange(true))
    {
        logger_.warn("PubSubListener::start() called while already running -- ignoring");
        return;
    }
    thread_ = std::thread([this] { run_thread(); });
    logger_.info("PubSubListener started ({}:{})", config_.host, config_.port);
}

void PubSubListener::stop()
{
    if (!running_.exchange(false))
    {
        return; // Already stopped or never started
    }
    if (thread_.joinable())
    {
        thread_.join();
    }
    logger_.info("PubSubListener stopped");
}

void PubSubListener::subscribe(const std::string& channel)
{
    std::lock_guard lock(channels_mutex_);
    subscribed_channels_.insert(channel);
    has_pending_subs_.store(true, std::memory_order_release);
}

void PubSubListener::unsubscribe(const std::string& channel)
{
    std::lock_guard lock(channels_mutex_);
    subscribed_channels_.erase(channel);
    has_pending_subs_.store(true, std::memory_order_release);
}

void PubSubListener::apply_pending_subscriptions(redisContext* ctx, std::unordered_set<std::string>& active_subs)
{
    std::lock_guard lock(channels_mutex_);

    // Collect pending commands for reply consumption
    size_t num_commands = 0;

    // Track newly subscribed channels for rollback on error
    std::vector<std::string> newly_subscribed;

    // Subscribe new channels (in subscribed_channels_ but not active)
    for (const auto& ch : subscribed_channels_)
    {
        if (!active_subs.contains(ch))
        {
            redisAppendCommand(ctx, "SUBSCRIBE %s", ch.c_str());
            newly_subscribed.push_back(ch);
            active_subs.insert(ch);
            ++num_commands;
            logger_.info("PubSub subscribing to '{}'", ch);
        }
    }

    // Unsubscribe removed channels (in active but not subscribed_channels_)
    std::vector<std::string> to_remove;
    for (const auto& ch : active_subs)
    {
        if (!subscribed_channels_.contains(ch))
        {
            redisAppendCommand(ctx, "UNSUBSCRIBE %s", ch.c_str());
            to_remove.push_back(ch);
            ++num_commands;
            logger_.info("PubSub unsubscribing from '{}'", ch);
        }
    }
    for (const auto& ch : to_remove)
    {
        active_subs.erase(ch);
    }

    has_pending_subs_.store(false, std::memory_order_release);

    if (num_commands == 0)
    {
        return;
    }

    // Flush all appended commands to the server
    int wdone = 0;
    do
    {
        if (redisBufferWrite(ctx, &wdone) == REDIS_ERR)
        {
            logger_.warn("PubSub write error during subscription update");
            // Rollback: remove newly subscribed from active_subs
            for (const auto& ch : newly_subscribed)
            {
                active_subs.erase(ch);
            }
            // Rollback: re-add unsubscribed channels to active_subs
            for (const auto& ch : to_remove)
            {
                active_subs.insert(ch);
            }
            return;
        }
    }
    while (!wdone);

    // Consume replies for each appended command
    for (size_t i = 0; i < num_commands; ++i)
    {
        redisReply* reply = nullptr;
        if (redisGetReply(ctx, reinterpret_cast<void**>(&reply)) != REDIS_OK)
        {
            logger_.warn("PubSub reply error during subscription update (cmd {}/{})", i + 1, num_commands);
            // Rollback: remove newly subscribed from active_subs
            for (const auto& ch : newly_subscribed)
            {
                active_subs.erase(ch);
            }
            // Rollback: re-add unsubscribed channels to active_subs
            for (const auto& ch : to_remove)
            {
                active_subs.insert(ch);
            }
            return;
        }
        if (reply)
        {
            if (reply->type == REDIS_REPLY_ERROR)
            {
                logger_.warn("PubSub command error: {}", reply->str ? reply->str : "unknown");
            }
            freeReplyObject(reply);
        }
    }
}

void PubSubListener::run_thread()
{
    while (running_)
    {
        // Synchronous hiredis connection for blocking subscribe.
        redisContext* sync_ctx = redisConnect(config_.host.c_str(), config_.port);
        if (!sync_ctx || sync_ctx->err)
        {
            logger_.error("PubSub connect failed: {}", sync_ctx ? sync_ctx->errstr : "null context");
            if (sync_ctx)
                redisFree(sync_ctx);
            std::this_thread::sleep_for(std::chrono::milliseconds{config_.reconnect_interval_ms});
            continue;
        }

        // NOTE: Do NOT call redisSetTimeout() here.
        // On Windows, SO_RCVTIMEO timeout causes hiredis to set c->err
        // permanently (WSAETIMEDOUT → REDIS_ERR_IO), breaking all subsequent
        // reads. Instead, we use select() to wait for data with timeout.

        // AUTH
        if (!config_.password.empty())
        {
            auto* reply = static_cast<redisReply*>(redisCommand(sync_ctx, "AUTH %s", config_.password.c_str()));
            if (reply)
                freeReplyObject(reply);
        }

        // Initial subscriptions — merge initial_channels into subscribed set
        // and subscribe all known channels on the fresh connection.
        std::unordered_set<std::string> active_subs;
        {
            std::lock_guard lock(channels_mutex_);
            for (const auto& ch : config_.initial_channels)
            {
                subscribed_channels_.insert(ch);
            }
            for (const auto& ch : subscribed_channels_)
            {
                auto* reply = static_cast<redisReply*>(redisCommand(sync_ctx, "SUBSCRIBE %s", ch.c_str()));
                if (reply)
                    freeReplyObject(reply);
                active_subs.insert(ch);
            }
            has_pending_subs_.store(false, std::memory_order_release);
        }

        logger_.info("PubSub connected, subscribed to {} channels", active_subs.size());

        // Message receive loop — select() based (no socket timeout).
        while (running_)
        {
            // 1. Apply pending subscribe/unsubscribe requests
            if (has_pending_subs_.load(std::memory_order_acquire))
            {
                apply_pending_subscriptions(sync_ctx, active_subs);
                if (sync_ctx->err)
                {
                    logger_.warn("PubSub error after subscription update, "
                                 "reconnecting...");
                    break;
                }
            }

            // 2. Wait for data with select() (1 second timeout)
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sync_ctx->fd, &rfds);

            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;

#ifdef _WIN32
            int nfds = 0; // Ignored on Windows
#else
            int nfds = sync_ctx->fd + 1;
#endif

            int ready = ::select(nfds, &rfds, nullptr, nullptr, &tv);

            if (ready < 0)
            {
                logger_.warn("PubSub select() error, reconnecting...");
                break;
            }
            if (ready == 0)
            {
                // Timeout — no data, loop back to check running_ and pending
                continue;
            }

            // 3. Data available — read from socket into hiredis buffer
            if (redisBufferRead(sync_ctx) != REDIS_OK)
            {
                logger_.warn("PubSub read error (err={}), reconnecting...", sync_ctx->err);
                break;
            }

            // 4. Parse all complete replies from the buffer
            void* reply_ptr = nullptr;
            while (redisReaderGetReply(sync_ctx->reader, &reply_ptr) == REDIS_OK)
            {
                if (!reply_ptr)
                    break; // No more complete replies

                auto* reply = static_cast<redisReply*>(reply_ptr);

                if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 3 && reply->element[0]->str)
                {

                    std::string_view type(reply->element[0]->str, reply->element[0]->len);

                    if (type == "message")
                    {
                        std::string_view channel(reply->element[1]->str, reply->element[1]->len);
                        auto* data = reinterpret_cast<const uint8_t*>(reply->element[2]->str);
                        size_t len = reply->element[2]->len;

                        if (on_message_)
                        {
                            on_message_(channel, std::span<const uint8_t>(data, len));
                        }
                    }
                    // subscribe/unsubscribe acks are silently consumed
                }

                freeReplyObject(reply);
            }
        }

        redisFree(sync_ctx);

        if (running_)
        {
            logger_.info("PubSub reconnecting in {}ms...", config_.reconnect_interval_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds{config_.reconnect_interval_ms});
        }
    }
}

} // namespace apex::gateway
