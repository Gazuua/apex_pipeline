// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/result.hpp>
#include <apex/core/slab_allocator.hpp>
#include <apex/shared/adapters/redis/redis_config.hpp>
#include <apex/shared/adapters/redis/redis_connection.hpp>
#include <apex/shared/adapters/redis/redis_reply.hpp>

#include <hiredis/async.h>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apex::shared::adapters::redis
{

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
class RedisMultiplexer
{
  public:
    RedisMultiplexer(boost::asio::io_context& io_ctx, const RedisConfig& config);
    ~RedisMultiplexer();

    // Non-copyable, non-movable
    RedisMultiplexer(const RedisMultiplexer&) = delete;
    RedisMultiplexer& operator=(const RedisMultiplexer&) = delete;
    RedisMultiplexer(RedisMultiplexer&&) = delete;
    RedisMultiplexer& operator=(RedisMultiplexer&&) = delete;

    /// Execute a single Redis command (raw string, no parameter escaping).
    /// @deprecated Use the parameterized overload to prevent Redis command injection.
    [[deprecated("Use command(const char* fmt, Args&&... args) for safe parameter binding")]] [[nodiscard]] boost::
        asio::awaitable<apex::core::Result<RedisReply>>
        command(std::string_view cmd);

    /// Execute a Redis command with printf-style parameter binding.
    /// Parameters are escaped by hiredis (via redisFormatCommand), preventing
    /// command injection. Use %s for strings, %d for integers, %b for binary
    /// (requires pointer + length pair).
    ///
    /// Example:
    ///   co_await mux.command("SETEX %s %d %s", key, ttl, value);
    template <typename... Args>
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<RedisReply>> command(const char* fmt, Args&&... args)
    {
        char* buf = nullptr;
        int len = redisFormatCommand(&buf, fmt, std::forward<Args>(args)...);
        if (len < 0 || !buf)
        {
            co_return std::unexpected(apex::core::ErrorCode::AdapterError);
        }
        // RAII guard for hiredis-allocated buffer
        struct FreeGuard
        {
            char* p;
            ~FreeGuard()
            {
                if (p)
                    redisFreeCommand(p);
            }
        } guard{buf};
        co_return co_await submit_formatted_command(buf, len);
    }

    /// Execute multiple commands as a pipeline. Returns results in order.
    /// hiredis internally queues writes, so consecutive redisAsyncCommand calls
    /// are automatically pipelined.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<std::vector<RedisReply>>>
    pipeline(std::span<const std::string> commands);

    /// Establish the initial Redis connection.
    /// Call once after construction (typically from RedisAdapter::do_init).
    /// If the connection fails, starts the automatic reconnect loop.
    /// If password is configured, schedules AUTH after the event loop starts.
    void connect();

    /// Whether the underlying connection is established and not reconnecting.
    [[nodiscard]] bool connected() const noexcept;

    /// Number of commands awaiting reply.
    [[nodiscard]] std::size_t pending_count() const noexcept;

    /// Number of reconnection attempts since last successful connect.
    [[nodiscard]] uint32_t reconnect_attempts() const noexcept;

    /// Graceful close — cancel all pending commands with AdapterError.
    boost::asio::awaitable<void> close();

  private:
    /// Authenticate connection via AUTH command.
    /// Uses raw_async_command to bypass reconnecting_ guard.
    boost::asio::awaitable<apex::core::Result<void>> authenticate(RedisConnection& conn);

    struct PendingCommand
    {
        boost::asio::steady_timer resolver;
        boost::asio::steady_timer timeout;
        apex::core::Result<RedisReply> result;
        uint64_t sequence;
        bool timed_out{false};
        bool cancelled{false}; // cancel_all_pending()에서 소유권 이전 완료 플래그
    };

    /// hiredis callback — static trampoline to on_reply
    static void static_on_reply(redisAsyncContext* ac, void* reply, void* privdata);

    /// Handle disconnect from hiredis
    void on_disconnect();

    /// Remove a PendingCommand from pending_ and return it to the slab.
    void release_pending(PendingCommand* cmd);

    /// Cancel all pending commands with the given error and release them.
    void cancel_all_pending(apex::core::ErrorCode error);

    /// Submit an already-formatted Redis protocol command and await reply.
    /// Used by both the deprecated command(string_view) and the new
    /// parameterized command(fmt, args...) overload.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<RedisReply>> submit_formatted_command(const char* cmd,
                                                                                                  int len);

    /// Exponential backoff reconnect loop
    boost::asio::awaitable<void> reconnect_loop();

    std::deque<PendingCommand*> pending_;
    boost::asio::io_context& io_ctx_;
    RedisConfig config_;
    apex::core::TypedSlabAllocator<PendingCommand> slab_;
    std::unique_ptr<RedisConnection> conn_;
    uint64_t next_sequence_{0};
    bool reconnecting_{false};
    boost::asio::steady_timer backoff_timer_;
    uint32_t reconnect_attempts_{0};
};

} // namespace apex::shared::adapters::redis
