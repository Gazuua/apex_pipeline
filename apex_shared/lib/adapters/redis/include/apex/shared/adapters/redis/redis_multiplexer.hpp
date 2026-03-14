#pragma once

#include <apex/shared/adapters/redis/redis_config.hpp>
#include <apex/shared/adapters/redis/redis_connection.hpp>
#include <apex/shared/adapters/redis/redis_reply.hpp>
#include <apex/core/result.hpp>

#include <hiredis/async.h>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apex::shared::adapters::redis {

/// Single-connection multiplexer for Redis commands.
/// Uses hiredis async API with FIFO reply matching (Redis protocol guarantees
/// response ordering). Supports coroutine-based command/pipeline with
/// automatic reconnect on disconnect.
///
/// Design:
///   - One hiredis async connection per multiplexer instance
///   - Commands are sent via redisAsyncCommand; replies matched FIFO via pending_ deque
///   - Each pending command has a resolver timer (for coroutine resume) and a timeout timer
///   - On disconnect, all pending commands get AdapterError and reconnect loop starts
class RedisMultiplexer {
public:
    RedisMultiplexer(boost::asio::io_context& io_ctx, const RedisConfig& config);
    ~RedisMultiplexer();

    // Non-copyable, non-movable
    RedisMultiplexer(const RedisMultiplexer&) = delete;
    RedisMultiplexer& operator=(const RedisMultiplexer&) = delete;
    RedisMultiplexer(RedisMultiplexer&&) = delete;
    RedisMultiplexer& operator=(RedisMultiplexer&&) = delete;

    /// Execute a single Redis command. Suspends the coroutine until reply arrives
    /// or timeout expires.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<RedisReply>>
    command(std::string_view cmd);

    /// Execute multiple commands as a pipeline. Returns results in order.
    /// hiredis internally queues writes, so consecutive redisAsyncCommand calls
    /// are automatically pipelined.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<std::vector<RedisReply>>>
    pipeline(std::span<const std::string> commands);

    /// Whether the underlying connection is established and not reconnecting.
    [[nodiscard]] bool connected() const noexcept;

    /// Number of commands awaiting reply.
    [[nodiscard]] std::size_t pending_count() const noexcept;

    /// Number of reconnection attempts since last successful connect.
    [[nodiscard]] uint32_t reconnect_attempts() const noexcept;

    /// Graceful close — cancel all pending commands with AdapterError.
    boost::asio::awaitable<void> close();

private:
    struct PendingCommand {
        boost::asio::steady_timer resolver;
        boost::asio::steady_timer timeout;
        apex::core::Result<RedisReply> result;
        uint64_t sequence;
        bool timed_out{false};
    };

    /// hiredis callback — static trampoline to on_reply
    static void static_on_reply(redisAsyncContext* ac, void* reply, void* privdata);

    /// Handle disconnect from hiredis
    void on_disconnect();

    /// Exponential backoff reconnect loop
    boost::asio::awaitable<void> reconnect_loop();

    std::deque<PendingCommand*> pending_;
    std::unique_ptr<RedisConnection> conn_;
    boost::asio::io_context& io_ctx_;
    RedisConfig config_;
    uint64_t next_sequence_{0};
    bool reconnecting_{false};
    uint32_t reconnect_attempts_{0};
};

} // namespace apex::shared::adapters::redis
