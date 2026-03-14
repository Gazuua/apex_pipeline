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
    : slab_(64, {.auto_grow = true, .max_total_count = 4096}),
      io_ctx_(io_ctx), config_(config) {}

RedisMultiplexer::~RedisMultiplexer() {
    cancel_all_pending(apex::core::ErrorCode::AdapterError);
}

boost::asio::awaitable<apex::core::Result<RedisReply>>
RedisMultiplexer::command(std::string_view cmd) {
    if (!conn_ || !conn_->is_connected()) {
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    auto seq = next_sequence_++;
    auto* pending = slab_.construct(
        boost::asio::steady_timer(io_ctx_,
            std::chrono::steady_clock::time_point::max()),
        boost::asio::steady_timer(io_ctx_, config_.command_timeout),
        std::unexpected(apex::core::ErrorCode::Timeout),
        seq,
        false
    );
    if (!pending) {
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    pending_.push_back(pending);

    // Submit async command to hiredis
    std::string cmd_str(cmd);
    int ret = redisAsyncCommand(conn_->async_context(),
                                &RedisMultiplexer::static_on_reply,
                                this, cmd_str.c_str());
    if (ret != REDIS_OK) {
        pending_.pop_back();
        slab_.destroy(pending);
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    // Timeout handler — on expiry, mark as timed out and resume coroutine.
    // Capture raw pointer; the slab owns the object, so it stays valid
    // until explicitly destroyed.
    pending->timeout.async_wait([pending](boost::system::error_code ec) {
        if (!ec) {
            pending->timed_out = true;
            pending->result = std::unexpected(apex::core::ErrorCode::Timeout);
            pending->resolver.cancel();
        }
    });

    // Wait for reply (hiredis callback cancels resolver to resume)
    boost::system::error_code ec;
    co_await pending->resolver.async_wait(
        boost::asio::redirect_error(boost::asio::use_awaitable, ec));

    pending->timeout.cancel();

    auto result = std::move(pending->result);

    // Remove from pending_ (may already be removed by on_reply/on_disconnect)
    release_pending(pending);

    co_return std::move(result);
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

    // Step 2 of 2-step ownership transfer (see release_pending):
    // If this command already timed out, the coroutine has already resumed
    // and extracted the result. The PendingCommand is still alive in the slab
    // (release_pending was called but only removes from deque if present).
    // We must destroy it here since the coroutine won't do it.
    if (front->timed_out) {
        self->slab_.destroy(front);
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
    cancel_all_pending(apex::core::ErrorCode::AdapterError);
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
void RedisMultiplexer::release_pending(PendingCommand* cmd) {
    if (cmd->timed_out) {
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

void RedisMultiplexer::cancel_all_pending(apex::core::ErrorCode error) {
    // Move to local to avoid iterator invalidation if cancel()
    // triggers posted handlers synchronously.
    auto local = std::move(pending_);
    for (auto* cmd : local) {
        cmd->result = std::unexpected(error);
        cmd->resolver.cancel();
    }
    // Destroy all PendingCommands immediately. This is safe because:
    // - On disconnect: hiredis won't fire further callbacks
    // - On close/destructor: we're tearing down the connection
    // The coroutines will be cancelled but their resume handlers (posted
    // to io_context) must not access this multiplexer after destruction.
    for (auto* cmd : local) {
        slab_.destroy(cmd);
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
    cancel_all_pending(apex::core::ErrorCode::AdapterError);

    if (conn_) {
        conn_->disconnect();
        conn_.reset();
    }
    co_return;
}

} // namespace apex::shared::adapters::redis
