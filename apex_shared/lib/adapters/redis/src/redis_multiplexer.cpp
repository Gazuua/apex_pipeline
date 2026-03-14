#include <apex/shared/adapters/redis/redis_multiplexer.hpp>
#include <apex/shared/adapters/redis/redis_connection.hpp>

#include <spdlog/spdlog.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>


namespace apex::shared::adapters::redis {

RedisMultiplexer::RedisMultiplexer(boost::asio::io_context& io_ctx,
                                   const RedisConfig& config)
    : io_ctx_(io_ctx), config_(config) {}

RedisMultiplexer::~RedisMultiplexer() {
    // Move pending_ to local to avoid iterator invalidation if cancel()
    // triggers posted handlers synchronously.
    auto local = std::move(pending_);
    for (auto* cmd : local) {
        cmd->result = std::unexpected(apex::core::ErrorCode::AdapterError);
        cmd->resolver.cancel();
    }
}

boost::asio::awaitable<apex::core::Result<RedisReply>>
RedisMultiplexer::command(std::string_view cmd) {
    if (!conn_ || !conn_->is_connected()) {
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    auto seq = next_sequence_++;
    PendingCommand pending{
        .resolver = boost::asio::steady_timer(io_ctx_,
            std::chrono::steady_clock::time_point::max()),
        .timeout = boost::asio::steady_timer(io_ctx_, config_.command_timeout),
        .result = std::unexpected(apex::core::ErrorCode::Timeout),
        .sequence = seq,
        .timed_out = false
    };

    pending_.push_back(&pending);

    // Submit async command to hiredis
    std::string cmd_str(cmd);
    int ret = redisAsyncCommand(conn_->async_context(),
                                &RedisMultiplexer::static_on_reply,
                                this, cmd_str.c_str());
    if (ret != REDIS_OK) {
        pending_.pop_back();
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    // Timeout handler — on expiry, mark as timed out and resume coroutine
    pending.timeout.async_wait([&pending](boost::system::error_code ec) {
        if (!ec) {
            pending.timed_out = true;
            pending.result = std::unexpected(apex::core::ErrorCode::Timeout);
            pending.resolver.cancel();
        }
    });

    // Wait for reply (hiredis callback cancels resolver to resume)
    boost::system::error_code ec;
    co_await pending.resolver.async_wait(
        boost::asio::redirect_error(boost::asio::use_awaitable, ec));

    pending.timeout.cancel();

    // TODO: PendingCommand를 heap 할당(unique_ptr)으로 전환하여
    // 타임아웃 후 코루틴 프레임 소멸 시 dangling 방지 필요 (v0.5)

    co_return std::move(pending.result);
}

boost::asio::awaitable<apex::core::Result<std::vector<RedisReply>>>
RedisMultiplexer::pipeline(std::span<const std::string> commands) {
    std::vector<RedisReply> results;
    results.reserve(commands.size());

    for (const auto& cmd : commands) {
        auto result = co_await command(cmd);
        if (!result.has_value()) {
            co_return std::unexpected(result.error());
        }
        results.push_back(std::move(*result));
    }

    co_return results;
}

// hiredis callback — FIFO matching
void RedisMultiplexer::static_on_reply(redisAsyncContext* /*ac*/,
                                        void* reply, void* privdata) {
    auto* self = static_cast<RedisMultiplexer*>(privdata);
    auto* r = static_cast<redisReply*>(reply);

    if (self->pending_.empty()) return;

    auto* front = self->pending_.front();
    self->pending_.pop_front();

    // If this command already timed out, skip (result already set)
    if (front->timed_out) {
        return;
    }

    if (r && r->type != REDIS_REPLY_ERROR) {
        front->result = RedisReply{r};
    } else {
        front->result = std::unexpected(apex::core::ErrorCode::AdapterError);
    }
    front->resolver.cancel();  // Resume coroutine
}

void RedisMultiplexer::on_disconnect() {
    reconnecting_ = true;
    // Return error to all pending commands
    for (auto* cmd : pending_) {
        cmd->result = std::unexpected(apex::core::ErrorCode::AdapterError);
        cmd->resolver.cancel();
    }
    pending_.clear();
    // Start reconnect loop
    boost::asio::co_spawn(io_ctx_, reconnect_loop(), boost::asio::detached);
}

boost::asio::awaitable<void> RedisMultiplexer::reconnect_loop() {
    auto backoff = std::chrono::milliseconds{100};
    const auto max_backoff = config_.reconnect_max_backoff;

    while (reconnecting_) {
        ++reconnect_attempts_;
        spdlog::info("Redis reconnect attempt {} (backoff {}ms)",
                     reconnect_attempts_, backoff.count());

        conn_ = RedisConnection::create(io_ctx_, config_);
        if (conn_ && conn_->is_connected()) {
            reconnecting_ = false;
            reconnect_attempts_ = 0;
            spdlog::info("Redis reconnected successfully");
            co_return;
        }

        // Exponential backoff wait
        boost::asio::steady_timer timer(io_ctx_, backoff);
        co_await timer.async_wait(boost::asio::use_awaitable);
        backoff = std::min(backoff * 2, max_backoff);
    }
}

bool RedisMultiplexer::connected() const noexcept {
    return conn_ && conn_->is_connected() && !reconnecting_;
}

std::size_t RedisMultiplexer::pending_count() const noexcept {
    return pending_.size();
}

uint32_t RedisMultiplexer::reconnect_attempts() const noexcept {
    return reconnect_attempts_;
}

boost::asio::awaitable<void> RedisMultiplexer::close() {
    reconnecting_ = false;
    // Cancel all pending commands
    for (auto* cmd : pending_) {
        cmd->result = std::unexpected(apex::core::ErrorCode::AdapterError);
        cmd->resolver.cancel();
    }
    pending_.clear();

    if (conn_) {
        conn_->disconnect();
        conn_.reset();
    }
    co_return;
}

} // namespace apex::shared::adapters::redis
