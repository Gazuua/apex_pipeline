#include <apex/shared/adapters/redis/redis_connection.hpp>

#include <hiredis/async.h>
#include <hiredis/hiredis.h>

#include <boost/asio/async_result.hpp>

namespace apex::shared::adapters::redis {

// --- detail ---

namespace detail {

void command_callback(redisAsyncContext* /*ac*/, void* reply, void* privdata) {
    auto* ctx = static_cast<CommandContext*>(privdata);
    auto* r = static_cast<redisReply*>(reply);
    if (r == nullptr) {
        ctx->handler(boost::asio::error::connection_reset, nullptr);
    } else {
        ctx->handler({}, r);
    }
    delete ctx;
}

} // namespace detail

// --- RedisConnection ---

RedisConnection::RedisConnection(boost::asio::io_context& io_ctx,
                                 redisAsyncContext* ac)
    : io_ctx_(io_ctx), ac_(ac), connected_(true) {}

RedisConnection::~RedisConnection() {
    disconnect();
}

std::unique_ptr<RedisConnection>
RedisConnection::create(boost::asio::io_context& io_ctx,
                        const RedisConfig& config) {
    auto* ac = redisAsyncConnect(config.host.c_str(),
                                 static_cast<int>(config.port));
    if (!ac) {
        return nullptr;
    }

    if (ac->err) {
        redisAsyncFree(ac);
        return nullptr;
    }

    // 커넥션 생성 성공 — HiredisAsioAdapter로 Asio에 등록
    auto conn = std::unique_ptr<RedisConnection>(
        new RedisConnection(io_ctx, ac));
    conn->asio_adapter_ = std::make_unique<HiredisAsioAdapter>(io_ctx, ac);

    return conn;
}

bool RedisConnection::validate() const noexcept {
    return connected_ && ac_ && !ac_->err;
}

void RedisConnection::disconnect() {
    if (connected_ && ac_) {
        connected_ = false;
        // asio_adapter 먼저 정리 (cleanup 콜백에서 socket release)
        asio_adapter_.reset();
        redisAsyncFree(ac_);
        ac_ = nullptr;
    }
}

bool RedisConnection::is_connected() const noexcept {
    return connected_;
}

// --- Reply parsing utilities ---

std::optional<std::string> RedisConnection::parse_string_reply(
    const redisReply* reply)
{
    if (!reply) return std::nullopt;
    if (reply->type == REDIS_REPLY_NIL) return std::nullopt;
    if (reply->type == REDIS_REPLY_STRING || reply->type == REDIS_REPLY_STATUS) {
        return std::string(reply->str, reply->len);
    }
    return std::nullopt;
}

apex::core::Result<int64_t> RedisConnection::parse_integer_reply(
    const redisReply* reply)
{
    if (!reply) {
        return std::unexpected(apex::core::ErrorCode::AdapterError);
    }
    if (reply->type == REDIS_REPLY_INTEGER) {
        return reply->integer;
    }
    return std::unexpected(apex::core::ErrorCode::AdapterError);
}

bool RedisConnection::is_error_reply(const redisReply* reply) {
    return reply && reply->type == REDIS_REPLY_ERROR;
}

std::string RedisConnection::get_error_message(const redisReply* reply) {
    if (reply && reply->type == REDIS_REPLY_ERROR && reply->str) {
        return std::string(reply->str, reply->len);
    }
    return {};
}

} // namespace apex::shared::adapters::redis
