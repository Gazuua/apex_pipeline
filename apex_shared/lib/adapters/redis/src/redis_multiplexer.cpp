// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/adapters/redis/redis_connection.hpp>
#include <apex/shared/adapters/redis/redis_multiplexer.hpp>

#include <spdlog/spdlog.h>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <string>

namespace apex::shared::adapters::redis
{

RedisMultiplexer::RedisMultiplexer(boost::asio::io_context& io_ctx, const RedisConfig& config)
    : io_ctx_(io_ctx)
    , config_(config)
    , slab_(64, {.auto_grow = true, .grow_chunk_size = {}, .max_total_count = config_.max_pending_commands})
{}

void RedisMultiplexer::connect()
{
    // Establish the initial connection.  redisAsyncConnect is non-blocking;
    // the TCP handshake completes once the io_context event loop runs.
    conn_ = RedisConnection::create(io_ctx_, config_);
    if (conn_ && conn_->is_connected())
    {
        if (!config_.password.empty())
        {
            // AUTH must run asynchronously after the event loop starts.
            boost::asio::co_spawn(
                io_ctx_,
                [this]() -> boost::asio::awaitable<void> {
                    auto result = co_await authenticate(*conn_);
                    if (!result.has_value())
                    {
                        spdlog::warn("RedisMultiplexer: initial AUTH failed ({}:{}), reconnecting", config_.host,
                                     config_.port);
                        conn_.reset();
                        on_disconnect();
                    }
                },
                boost::asio::detached);
        }
    }
    else
    {
        spdlog::warn("RedisMultiplexer: initial connect failed ({}:{}), starting reconnect", config_.host,
                     config_.port);
        conn_.reset(); // Ensure null on failure
        reconnecting_ = true;
        boost::asio::co_spawn(io_ctx_, reconnect_loop(), boost::asio::detached);
    }
}

RedisMultiplexer::~RedisMultiplexer()
{
    cancel_all_pending(apex::core::ErrorCode::AdapterError);
}

boost::asio::awaitable<apex::core::Result<RedisReply>> RedisMultiplexer::command(std::string_view cmd)
{
    // Log only the command verb (e.g. "SET", "GET") — never the full string,
    // which may contain passwords, tokens, or other sensitive values.
    auto verb_end = cmd.find(' ');
    spdlog::debug("[redis_multiplexer] command: {}", cmd.substr(0, verb_end));

    if (!conn_ || !conn_->is_connected())
    {
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    auto seq = next_sequence_++;
    auto* pending = slab_.construct(boost::asio::steady_timer(io_ctx_, std::chrono::steady_clock::time_point::max()),
                                    boost::asio::steady_timer(io_ctx_, config_.command_timeout),
                                    std::unexpected(apex::core::ErrorCode::Timeout), seq, false);
    if (!pending)
    {
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    pending_.push_back(pending);

    // Submit async command to hiredis
    std::string cmd_str(cmd);
    int ret = redisAsyncCommand(conn_->async_context(), &RedisMultiplexer::static_on_reply, this, cmd_str.c_str());
    if (ret != REDIS_OK)
    {
        pending_.pop_back();
        slab_.destroy(pending);
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    // Timeout handler — on expiry, mark as timed out and resume coroutine.
    // Capture raw pointer; the slab owns the object, so it stays valid
    // until explicitly destroyed.
    pending->timeout.async_wait([pending](boost::system::error_code ec) {
        if (!ec)
        {
            pending->timed_out = true;
            pending->result = std::unexpected(apex::core::ErrorCode::Timeout);
            pending->resolver.cancel();
        }
    });

    // Wait for reply (hiredis callback cancels resolver to resume)
    boost::system::error_code ec;
    co_await pending->resolver.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

    pending->timeout.cancel();

    auto result = std::move(pending->result);

    // Remove from pending_ (may already be removed by on_reply/on_disconnect)
    release_pending(pending);

    co_return std::move(result);
}

boost::asio::awaitable<apex::core::Result<RedisReply>> RedisMultiplexer::submit_formatted_command(const char* cmd,
                                                                                                  int len)
{
    if (!conn_ || !conn_->is_connected())
    {
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    auto seq = next_sequence_++;
    auto* pending = slab_.construct(boost::asio::steady_timer(io_ctx_, std::chrono::steady_clock::time_point::max()),
                                    boost::asio::steady_timer(io_ctx_, config_.command_timeout),
                                    std::unexpected(apex::core::ErrorCode::Timeout), seq, false);
    if (!pending)
    {
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    pending_.push_back(pending);

    int ret = redisAsyncFormattedCommand(conn_->async_context(), &RedisMultiplexer::static_on_reply, this, cmd,
                                         static_cast<size_t>(len));
    if (ret != REDIS_OK)
    {
        pending_.pop_back();
        slab_.destroy(pending);
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    pending->timeout.async_wait([pending](boost::system::error_code ec) {
        if (!ec)
        {
            pending->timed_out = true;
            pending->result = std::unexpected(apex::core::ErrorCode::Timeout);
            pending->resolver.cancel();
        }
    });

    boost::system::error_code ec;
    co_await pending->resolver.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

    pending->timeout.cancel();

    auto result = std::move(pending->result);
    release_pending(pending);

    co_return std::move(result);
}

boost::asio::awaitable<apex::core::Result<std::vector<RedisReply>>>
RedisMultiplexer::pipeline(std::span<const std::string> commands)
{
    std::vector<RedisReply> results;
    results.reserve(commands.size());

    for (const auto& cmd : commands)
    {
        // Format each command via hiredis for safe transmission.
        // pipeline() will be updated to accept parameterized commands in a
        // future revision; for now, treat each string as a pre-formatted
        // command (same as the deprecated command(string_view)).
        char* buf = nullptr;
        int len = redisFormatCommand(&buf, cmd.c_str());
        if (len < 0 || !buf)
        {
            co_return std::unexpected(apex::core::ErrorCode::AdapterError);
        }
        struct FreeGuard
        {
            char* p;
            ~FreeGuard()
            {
                if (p)
                    redisFreeCommand(p);
            }
        } guard{buf};
        auto result = co_await submit_formatted_command(buf, len);
        guard.p = nullptr; // ownership transferred, release guard
        redisFreeCommand(buf);
        if (!result.has_value())
        {
            co_return std::unexpected(result.error());
        }
        results.push_back(std::move(*result));
    }

    co_return results;
}

// hiredis callback — FIFO matching
void RedisMultiplexer::static_on_reply(redisAsyncContext* /*ac*/, void* reply, void* privdata)
{
    auto* self = static_cast<RedisMultiplexer*>(privdata);
    auto* r = static_cast<redisReply*>(reply);

    if (self->pending_.empty())
    {
        spdlog::trace("[redis_multiplexer] static_on_reply: no pending commands (orphan reply)");
        return;
    }

    auto* front = self->pending_.front();
    self->pending_.pop_front();

    // cancel_all_pending()가 이미 소유권을 가져간 경우 — 이중 해제 방지
    if (front->cancelled)
        return;

    // Step 2 of 2-step ownership transfer (see release_pending):
    // If this command already timed out, the coroutine has already resumed
    // and extracted the result. The PendingCommand is still alive in the slab
    // (release_pending was called but only removes from deque if present).
    // We must destroy it here since the coroutine won't do it.
    if (front->timed_out)
    {
        spdlog::trace("[redis_multiplexer] static_on_reply: seq={} already timed out, destroying", front->sequence);
        self->slab_.destroy(front);
        return;
    }

    if (r && r->type != REDIS_REPLY_ERROR)
    {
        spdlog::trace("[redis_multiplexer] static_on_reply: seq={} matched successfully", front->sequence);
        front->result = RedisReply{r};
    }
    else
    {
        spdlog::trace("[redis_multiplexer] static_on_reply: seq={} matched with error", front->sequence);
        front->result = std::unexpected(apex::core::ErrorCode::AdapterError);
    }
    front->resolver.cancel(); // Resume coroutine
}

void RedisMultiplexer::on_disconnect()
{
    // 재진입 방어: authenticate() co_await 중 disconnect 발생 시
    // on_disconnect()가 다시 호출될 수 있다. 이미 reconnect_loop가
    // 실행 중이면 이중 spawn을 방지한다.
    if (reconnecting_)
        return;

    reconnecting_ = true;
    cancel_all_pending(apex::core::ErrorCode::AdapterError);
    // Start reconnect loop
    boost::asio::co_spawn(io_ctx_, reconnect_loop(), boost::asio::detached);
}

boost::asio::awaitable<apex::core::Result<void>> RedisMultiplexer::authenticate(RedisConnection& conn)
{
    if (config_.password.empty())
        co_return apex::core::Result<void>{};

    // Use redisAsyncCommand directly (same pattern as command()) to bypass
    // command()'s reconnecting_ guard and avoid async_command's
    // std::function copy-constructibility requirement.
    auto seq = next_sequence_++;
    auto* pending = slab_.construct(boost::asio::steady_timer(io_ctx_, std::chrono::steady_clock::time_point::max()),
                                    boost::asio::steady_timer(io_ctx_, config_.command_timeout),
                                    std::unexpected(apex::core::ErrorCode::Timeout), seq, false);
    if (!pending)
    {
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    pending_.push_back(pending);

    int ret = redisAsyncCommand(conn.async_context(), &RedisMultiplexer::static_on_reply, this, "AUTH %s",
                                config_.password.c_str());
    if (ret != REDIS_OK)
    {
        pending_.pop_back();
        slab_.destroy(pending);
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    pending->timeout.async_wait([pending](boost::system::error_code ec) {
        if (!ec)
        {
            pending->timed_out = true;
            pending->result = std::unexpected(apex::core::ErrorCode::Timeout);
            pending->resolver.cancel();
        }
    });

    boost::system::error_code ec;
    co_await pending->resolver.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

    pending->timeout.cancel();

    auto result = std::move(pending->result);
    release_pending(pending);

    if (!result.has_value())
    {
        spdlog::warn("Redis AUTH failed");
        conn.disconnect();
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    // AUTH succeeded — REDIS_REPLY_ERROR is already handled by static_on_reply
    // which converts it to AdapterError (caught by !result.has_value() above).
    co_return apex::core::Result<void>{};
}

boost::asio::awaitable<void> RedisMultiplexer::reconnect_loop()
{
    spdlog::debug("[redis_multiplexer] reconnect_loop started ({}:{})", config_.host, config_.port);

    auto backoff = std::chrono::milliseconds{100};
    const auto max_backoff = config_.reconnect_max_backoff;

    while (reconnecting_)
    {
        ++reconnect_attempts_;
        spdlog::info("Redis reconnect attempt {} (backoff {}ms)", reconnect_attempts_, backoff.count());

        conn_ = RedisConnection::create(io_ctx_, config_);
        if (conn_ && conn_->is_connected())
        {
            // AUTH (password set => authenticate)
            auto auth = co_await authenticate(*conn_);
            if (!auth.has_value())
            {
                conn_.reset();
                // backoff then retry
                boost::asio::steady_timer timer(io_ctx_, backoff);
                co_await timer.async_wait(boost::asio::use_awaitable);
                backoff = std::min(backoff * 2, max_backoff);
                continue;
            }
            reconnecting_ = false;
            reconnect_attempts_ = 0;
            spdlog::debug("[redis_multiplexer] reconnect succeeded ({}:{})", config_.host, config_.port);
            spdlog::info("Redis reconnected successfully{}", config_.password.empty() ? "" : " (authenticated)");
            co_return;
        }

        // Exponential backoff wait
        boost::asio::steady_timer timer(io_ctx_, backoff);
        co_await timer.async_wait(boost::asio::use_awaitable);
        backoff = std::min(backoff * 2, max_backoff);
    }
}

// 2-step ownership transfer for timed-out commands:
//
//   Step 1 (here, release_pending): coroutine finishes but the hiredis
//     callback hasn't fired yet.  Do NOT destroy — leave the
//     PendingCommand in the deque + slab so static_on_reply can find it.
//
//   Step 2 (static_on_reply): hiredis eventually delivers the reply,
//     pops the front of the deque, sees timed_out == true, and calls
//     slab_.destroy().
//
// This keeps FIFO deque ordering intact and prevents use-after-free:
// neither side destroys the object while the other still references it.
void RedisMultiplexer::release_pending(PendingCommand* cmd)
{
    if (cmd->timed_out)
    {
        // Timed-out command: the hiredis callback has NOT yet fired.
        // The PendingCommand must stay in the deque for FIFO ordering
        // and alive in the slab. static_on_reply() will pop it from
        // the deque and call slab_.destroy() when the reply arrives.
        return;
    }

    // Normal path: static_on_reply already popped this from pending_.
    // Just return the memory to the slab.
    slab_.destroy(cmd);
}

void RedisMultiplexer::cancel_all_pending(apex::core::ErrorCode error)
{
    auto local = std::move(pending_);
    for (auto* cmd : local)
    {
        cmd->result = std::unexpected(error);
        cmd->cancelled = true; // 소유권 표시 — static_on_reply 이중 해제 방지

        if (cmd->timed_out)
        {
            // 이미 타임아웃됨 — 코루틴은 재개 완료. hiredis는 disconnect 후 콜백 안 함.
            // cancel_all_pending이 유일한 소유자이므로 안전하게 해제.
            slab_.destroy(cmd);
        }
        else
        {
            // 코루틴이 resolver.async_wait에 대기 중.
            // cancel()이 completion handler를 post → 코루틴 재개 → release_pending에서 해제.
            cmd->resolver.cancel();
        }
    }
}

bool RedisMultiplexer::connected() const noexcept
{
    return conn_ && conn_->is_connected() && !reconnecting_;
}

std::size_t RedisMultiplexer::pending_count() const noexcept
{
    return pending_.size();
}

uint32_t RedisMultiplexer::reconnect_attempts() const noexcept
{
    return reconnect_attempts_;
}

boost::asio::awaitable<void> RedisMultiplexer::close()
{
    reconnecting_ = false;
    cancel_all_pending(apex::core::ErrorCode::AdapterError);

    if (conn_)
    {
        conn_->disconnect();
        conn_.reset();
    }
    co_return;
}

} // namespace apex::shared::adapters::redis
