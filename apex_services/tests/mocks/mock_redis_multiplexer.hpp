#pragma once

/// Mock RedisMultiplexer — command 기록 + 미리 설정된 응답 반환.
/// 실제 Redis 연결 없이 Redis 상호작용을 검증.

#include <apex/core/result.hpp>

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace apex::test
{

/// Recorded Redis command.
struct RedisCommand
{
    std::string command;
};

/// Simplified Redis reply for mock.
struct MockRedisReply
{
    enum class Type
    {
        kString,
        kInteger,
        kArray,
        kNil,
        kError
    };
    Type type{Type::kNil};
    std::string str_value;
    int64_t int_value{0};
    std::vector<MockRedisReply> array_value;
};

/// Mock RedisMultiplexer.
/// Records all commands issued and returns pre-configured responses.
/// Synchronous API (no coroutines) for unit test simplicity.
class MockRedisMultiplexer
{
  public:
    MockRedisMultiplexer() = default;

    // --- Command recording ---

    /// Execute command (simplified synchronous version).
    [[nodiscard]] apex::core::Result<MockRedisReply> command_sync(std::string_view cmd)
    {
        if (fail_commands_)
        {
            return apex::core::error(apex::core::ErrorCode::AdapterError);
        }
        std::lock_guard lock(mu_);
        commands_.push_back(RedisCommand{.command = std::string(cmd)});

        if (!response_queue_.empty())
        {
            auto reply = std::move(response_queue_.front());
            response_queue_.pop_front();
            return reply;
        }
        return MockRedisReply{.type = MockRedisReply::Type::kNil, .str_value = {}, .int_value = {}, .array_value = {}};
    }

    // --- Response pre-configuration ---

    /// Enqueue a response that will be returned on the next command.
    void enqueue_response(MockRedisReply reply)
    {
        std::lock_guard lock(mu_);
        response_queue_.push_back(std::move(reply));
    }

    /// Enqueue a simple string response.
    void enqueue_string(std::string_view value)
    {
        enqueue_response(MockRedisReply{
            .type = MockRedisReply::Type::kString,
            .str_value = std::string(value),
        });
    }

    /// Enqueue a simple integer response.
    void enqueue_integer(int64_t value)
    {
        enqueue_response(MockRedisReply{
            .type = MockRedisReply::Type::kInteger,
            .int_value = value,
        });
    }

    /// Enqueue a nil response.
    void enqueue_nil()
    {
        enqueue_response(
            MockRedisReply{.type = MockRedisReply::Type::kNil, .str_value = {}, .int_value = {}, .array_value = {}});
    }

    // --- Test inspection ---

    [[nodiscard]] const std::vector<RedisCommand>& commands() const
    {
        return commands_;
    }

    [[nodiscard]] size_t command_count() const
    {
        std::lock_guard lock(mu_);
        return commands_.size();
    }

    void clear()
    {
        std::lock_guard lock(mu_);
        commands_.clear();
        response_queue_.clear();
    }

    /// Set all commands to fail with AdapterError.
    void set_fail_commands(bool fail)
    {
        fail_commands_ = fail;
    }

  private:
    mutable std::mutex mu_;
    std::vector<RedisCommand> commands_;
    std::deque<MockRedisReply> response_queue_;
    bool fail_commands_{false};
};

} // namespace apex::test
