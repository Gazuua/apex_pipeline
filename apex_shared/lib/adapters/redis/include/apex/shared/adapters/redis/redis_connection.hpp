#pragma once

#include <apex/shared/adapters/redis/hiredis_asio_adapter.hpp>
#include <apex/shared/adapters/redis/redis_config.hpp>
#include <apex/core/result.hpp>

#include <boost/asio/io_context.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

struct redisAsyncContext;
struct redisReply;

namespace apex::shared::adapters::redis {

/// 하나의 hiredis 비동기 커넥션을 래핑한다.
/// HiredisAsioAdapter를 소유하여 Asio에 통합.
/// 커맨드 발행 시 코루틴 awaitable로 결과를 반환한다.
class RedisConnection {
public:
    ~RedisConnection();

    // Non-copyable, non-movable
    RedisConnection(const RedisConnection&) = delete;
    RedisConnection& operator=(const RedisConnection&) = delete;
    RedisConnection(RedisConnection&&) = delete;
    RedisConnection& operator=(RedisConnection&&) = delete;

    /// 동기 커넥션 생성. io_context에서 실행.
    /// 실패 시 nullptr 반환.
    [[nodiscard]] static std::unique_ptr<RedisConnection>
    create(boost::asio::io_context& io_ctx, const RedisConfig& config);

    /// Redis 커맨드 실행 (variadic format string 기반).
    /// hiredis redisAsyncCommand의 코루틴 래퍼.
    /// 결과 redisReply*는 콜백 스코프 내에서만 유효 — 즉시 파싱해야 함.
    ///
    /// 내부: async_initiate + redisAsyncCommand callback -> completion handler
    template <typename CompletionToken>
    auto async_command(const char* cmd, CompletionToken&& token);

    /// 커넥션 유효성 검증 (상태 플래그 기반 — 서버 없이 동작)
    [[nodiscard]] bool validate() const noexcept;

    /// 커넥션 정리
    void disconnect();

    [[nodiscard]] bool is_connected() const noexcept;

    // --- Reply 파싱 유틸리티 (static) ---

    /// REDIS_REPLY_STRING -> std::optional<std::string>
    /// REDIS_REPLY_NIL -> std::nullopt
    [[nodiscard]] static std::optional<std::string> parse_string_reply(
        const redisReply* reply);

    /// REDIS_REPLY_INTEGER -> int64_t
    [[nodiscard]] static apex::core::Result<int64_t> parse_integer_reply(
        const redisReply* reply);

    /// 에러 체크: REDIS_REPLY_ERROR -> ErrorCode::AdapterError
    [[nodiscard]] static bool is_error_reply(const redisReply* reply);

    /// 에러 메시지 추출
    [[nodiscard]] static std::string get_error_message(const redisReply* reply);

private:
    RedisConnection(boost::asio::io_context& io_ctx,
                    redisAsyncContext* ac);

    boost::asio::io_context& io_ctx_;
    redisAsyncContext* ac_;
    std::unique_ptr<HiredisAsioAdapter> asio_adapter_;
    bool connected_ = false;
};

// --- Template implementations ---

namespace detail {

/// hiredis 콜백에서 Asio completion handler를 호출하는 컨텍스트.
struct CommandContext {
    std::function<void(boost::system::error_code, redisReply*)> handler;
};

/// hiredis redisAsyncCommand 콜백 — CommandContext를 통해 코루틴 resume
void command_callback(redisAsyncContext* ac, void* reply, void* privdata);

} // namespace detail

template <typename CompletionToken>
auto RedisConnection::async_command(const char* cmd, CompletionToken&& token) {
    return boost::asio::async_initiate<CompletionToken,
                                        void(boost::system::error_code, redisReply*)>(
        [this, cmd](auto handler) {
            if (!connected_ || !ac_) {
                handler(boost::asio::error::not_connected, nullptr);
                return;
            }
            auto* ctx = new detail::CommandContext{std::move(handler)};
            int ret = redisAsyncCommand(ac_, detail::command_callback, ctx, cmd);
            if (ret != 0) {
                // redisAsyncCommand 실패 — 즉시 에러 반환
                ctx->handler(boost::asio::error::connection_aborted, nullptr);
                delete ctx;
            }
        },
        token);
}

} // namespace apex::shared::adapters::redis
