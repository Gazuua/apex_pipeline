#pragma once

#include <apex/shared/adapters/adapter_base.hpp>
#include <apex/shared/adapters/redis/redis_config.hpp>
#include <apex/shared/adapters/redis/redis_pool.hpp>

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

namespace apex::shared::adapters::redis {

/// Redis 어댑터. Server에 전역 1개 인스턴스로 등록.
/// 내부적으로 코어별 RedisPool 인스턴스를 관리한다.
///
/// 사용법:
///   Server({.port = 9000, .num_cores = 4})
///       .add_adapter<RedisAdapter>(redis_config)
///       .add_service<MyService>()
///       .run();
///
///   // 서비스 내부:
///   auto& redis = server.adapter<RedisAdapter>();
///   auto val = co_await redis.get("key");
class RedisAdapter : public AdapterBase<RedisAdapter> {
public:
    explicit RedisAdapter(RedisConfig config);
    ~RedisAdapter();

    // --- AdapterBase CRTP 구현 ---

    /// 코어별 RedisPool 인스턴스 생성.
    void do_init(apex::core::CoreEngine& engine);

    /// 새 요청 거부 시그널 -> 각 풀에 전파.
    void do_drain();

    /// 모든 풀의 커넥션 정리.
    void do_close();

    /// 어댑터 이름.
    [[nodiscard]] std::string_view do_name() const noexcept { return "redis"; }

    // --- Monitoring API ---

    /// 현재 코어의 풀 상태 (모니터링용)
    [[nodiscard]] size_t active_connections() const noexcept;
    [[nodiscard]] size_t idle_connections() const noexcept;

    /// 설정 접근
    [[nodiscard]] const RedisConfig& config() const noexcept { return config_; }

    /// 코어별 풀 접근 (테스트/고급 사용)
    [[nodiscard]] RedisPool& pool(uint32_t core_id);

private:
    RedisConfig config_;
    std::vector<std::unique_ptr<RedisPool>> pools_;  ///< 코어별 인스턴스
    bool draining_ = false;
};

} // namespace apex::shared::adapters::redis
