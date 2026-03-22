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
#include <utility>

namespace apex::shared::adapters::redis
{

RedisMultiplexer::RedisMultiplexer(boost::asio::io_context& io_ctx, const RedisConfig& config, uint32_t core_id,
                                   SpawnCallback spawn_cb)
    : io_ctx_(io_ctx)
    , config_(config)
    , slab_(64, {.auto_grow = true, .grow_chunk_size = {}, .max_total_count = config_.max_pending_commands})
    , backoff_timer_(io_ctx)
    , core_id_(core_id)
    , spawn_callback_(std::move(spawn_cb))
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
            // 멤버 함수 코루틴 사용 — IIFE 람다는 임시객체 파괴 후 dangling this.
            spawn_callback_(core_id_, initial_auth());
        }
    }
    else
    {
        spdlog::warn("RedisMultiplexer: initial connect failed ({}:{}), starting reconnect", config_.host,
                     config_.port);
        conn_.reset(); // Ensure null on failure
        reconnect_active_ = true;
        spawn_callback_(core_id_, reconnect_loop());
    }
}

RedisMultiplexer::~RedisMultiplexer()
{
    // 방어적 cleanup: close()가 호출되지 않은 경우에도 안전하게 정리.
    // 정상 경로에서는 AdapterBase::close() → do_close_per_core()가 close()를 호출하지만,
    // 테스트나 예외 경로에서 close() 없이 파괴될 수 있다.
    if (conn_)
    {
        spdlog::warn("RedisMultiplexer destroyed with active connection — calling close() in destructor");
        close();
    }
    // 소멸자 경로: cancel_all_pending 대신 직접 slab 해제.
    // cancel_all_pending의 resolver.cancel()은 코루틴 resume을 post하는데,
    // this 파괴 후 resume 시 slab_ 접근 → use-after-free.
    for (auto* cmd : pending_)
        slab_.destroy(cmd);
    pending_.clear();
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
    // 재진입 방어: reconnect_loop가 이미 활성이면 이중 spawn 방지
    if (reconnect_active_)
        return;

    cancel_all_pending(apex::core::ErrorCode::AdapterError);

    // spawn 시도. DRAINING 상태면 spawn_callback_이 거부하므로
    // reconnect_active_를 spawn 성공 시에만 설정.
    // ReconnectGuard가 코루틴 종료 시 false로 되돌림.
    reconnect_active_ = true;
    spawn_callback_(core_id_, reconnect_loop());
    // spawn_callback_이 거부하면 코루틴이 실행되지 않아 ReconnectGuard가 생성되지 않음.
    // 이 경우 reconnect_active_가 true로 남지만, DRAINING/CLOSED 이후 destroy 흐름이므로
    // 실질적 영향 없음 (multiplexer가 곧 파괴됨).
}

boost::asio::awaitable<void> RedisMultiplexer::initial_auth()
{
    auto result = co_await authenticate(*conn_);
    if (!result.has_value())
    {
        spdlog::warn("RedisMultiplexer: initial AUTH failed ({}:{}), reconnecting", config_.host, config_.port);
        conn_.reset();
        on_disconnect();
    }
}

boost::asio::awaitable<apex::core::Result<void>> RedisMultiplexer::authenticate(RedisConnection& conn)
{
    if (config_.password.empty())
        co_return apex::core::Result<void>{};

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

    co_return apex::core::Result<void>{};
}

boost::asio::awaitable<void> RedisMultiplexer::reconnect_loop()
{
    spdlog::debug("[redis_multiplexer] reconnect_loop started ({}:{})", config_.host, config_.port);

    auto backoff = std::chrono::milliseconds{100};
    const auto max_backoff = config_.reconnect_max_backoff;

    // RAII: 코루틴 종료 시(정상/취소/예외) reconnect_active_ 해제
    struct ReconnectGuard
    {
        bool& active;
        ~ReconnectGuard()
        {
            active = false;
        }
    } guard{reconnect_active_};

    for (;;)
    {
        ++reconnect_attempts_;
        spdlog::info("Redis reconnect attempt {} (backoff {}ms)", reconnect_attempts_, backoff.count());

        conn_ = RedisConnection::create(io_ctx_, config_);
        if (conn_ && conn_->is_connected())
        {
            auto auth = co_await authenticate(*conn_);
            if (!auth.has_value())
            {
                conn_.reset();
                backoff_timer_.expires_after(backoff);
                auto [ec] = co_await backoff_timer_.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
                if (ec == boost::asio::error::operation_aborted)
                    co_return;
                backoff = std::min(backoff * 2, max_backoff);
                continue;
            }
            reconnect_attempts_ = 0;
            spdlog::info("Redis reconnected successfully ({}:{}){}", config_.host, config_.port,
                         config_.password.empty() ? "" : " (authenticated)");
            co_return;
        }

        // Exponential backoff wait
        backoff_timer_.expires_after(backoff);
        auto [ec] = co_await backoff_timer_.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
        if (ec == boost::asio::error::operation_aborted)
            co_return;
        backoff = std::min(backoff * 2, max_backoff);
    }
}

void RedisMultiplexer::release_pending(PendingCommand* cmd)
{
    if (cmd->timed_out)
    {
        return;
    }
    slab_.destroy(cmd);
}

void RedisMultiplexer::cancel_all_pending(apex::core::ErrorCode error)
{
    auto local = std::move(pending_);
    for (auto* cmd : local)
    {
        cmd->result = std::unexpected(error);
        cmd->cancelled = true;

        if (cmd->timed_out)
        {
            slab_.destroy(cmd);
        }
        else
        {
            cmd->resolver.cancel();
        }
    }
}

bool RedisMultiplexer::connected() const noexcept
{
    return conn_ && conn_->is_connected();
}

std::size_t RedisMultiplexer::pending_count() const noexcept
{
    return pending_.size();
}

uint32_t RedisMultiplexer::reconnect_attempts() const noexcept
{
    return reconnect_attempts_;
}

void RedisMultiplexer::close()
{
    backoff_timer_.cancel(); // reconnect_loop가 대기 중일 수 있음
    cancel_all_pending(apex::core::ErrorCode::AdapterError);
    if (conn_)
    {
        conn_->disconnect();
        conn_.reset();
    }
}

} // namespace apex::shared::adapters::redis
