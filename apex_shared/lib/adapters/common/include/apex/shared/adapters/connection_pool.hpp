#pragma once

#include <apex/shared/adapters/adapter_error.hpp>
#include <apex/core/result.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>

namespace apex::shared::adapters {

/// ConnectionPool 통계 카운터.
/// 풀 가동 후 누적 acquire/release/create/destroy/fail 횟수.
struct PoolStats {
    uint64_t total_acquired = 0;
    uint64_t total_released = 0;
    uint64_t total_created = 0;
    uint64_t total_destroyed = 0;
    uint64_t total_failed = 0;
};

/// ConnectionPool 설정
struct PoolConfig {
    size_t min_size = 1;                                         ///< 최소 유지 커넥션
    size_t max_size = 8;                                         ///< 최대 확장 한도
    std::chrono::seconds max_idle_time{60};                      ///< 유휴 커넥션 폐기 시간
    std::chrono::seconds health_check_interval{30};              ///< 헬스 체크 주기
};

/// Redis/PG 커넥션 풀 공통 추상화 (CRTP).
/// 코어별 독립 인스턴스로 사용 — 락 불필요.
///
/// Conn 템플릿 파라미터로 커넥션 타입을 전달 (MSVC 불완전 타입 우회).
///
/// Derived가 구현해야 할 메서드:
///   Conn do_create_connection()
///   void do_destroy_connection(Conn& conn)
///   bool do_validate(Conn& conn)
template <typename Derived, typename Conn>
class ConnectionPool {
public:
    using Connection = Conn;

    explicit ConnectionPool(PoolConfig config) : config_(std::move(config)) {}
    ~ConnectionPool() { close_all(); }

    // Non-copyable, non-movable
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    /// 풀에서 커넥션 획득
    [[nodiscard]] apex::core::Result<Connection> acquire() {
        // 유휴 커넥션에서 꺼내기
        while (!idle_.empty()) {
            auto conn = std::move(idle_.front().conn);
            idle_.pop_front();
            if (derived().do_validate(conn)) {
                ++active_count_;
                ++stats_.total_acquired;
                return conn;
            }
            derived().do_destroy_connection(conn);
            --total_count_;
            ++stats_.total_destroyed;
        }
        // 새 커넥션 생성 (한도 내)
        if (total_count_ < config_.max_size) {
            auto conn = derived().do_create_connection();
            ++total_count_;
            ++active_count_;
            ++stats_.total_created;
            ++stats_.total_acquired;
            return conn;
        }
        ++stats_.total_failed;
        return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    /// 커넥션 반환
    void release(Connection conn) {
        --active_count_;
        ++stats_.total_released;
        idle_.push_back({std::move(conn), std::chrono::steady_clock::now()});
    }

    /// 유휴 커넥션 폐기 (max_idle_time 초과)
    void shrink_idle() {
        auto now = std::chrono::steady_clock::now();
        while (!idle_.empty() && total_count_ > config_.min_size) {
            auto& entry = idle_.front();
            if (now - entry.last_used < config_.max_idle_time) break;
            derived().do_destroy_connection(entry.conn);
            idle_.pop_front();
            --total_count_;
            ++stats_.total_destroyed;
        }
    }

    /// 헬스 체크 — 유휴 커넥션 검증
    void health_check_tick() {
        std::deque<IdleEntry> valid;
        while (!idle_.empty()) {
            auto entry = std::move(idle_.front());
            idle_.pop_front();
            if (derived().do_validate(entry.conn)) {
                valid.push_back(std::move(entry));
            } else {
                derived().do_destroy_connection(entry.conn);
                --total_count_;
                ++stats_.total_destroyed;
            }
        }
        idle_ = std::move(valid);
    }

    /// 모든 커넥션 정리
    void close_all() {
        for (auto& entry : idle_) {
            derived().do_destroy_connection(entry.conn);
        }
        idle_.clear();
        total_count_ = 0;
        active_count_ = 0;
    }

    [[nodiscard]] size_t active_count() const noexcept { return active_count_; }
    [[nodiscard]] size_t idle_count() const noexcept { return idle_.size(); }
    [[nodiscard]] size_t total_count() const noexcept { return total_count_; }
    [[nodiscard]] const PoolConfig& config() const noexcept { return config_; }
    [[nodiscard]] const PoolStats& stats() const noexcept { return stats_; }

private:
    Derived& derived() noexcept { return static_cast<Derived&>(*this); }

    struct IdleEntry {
        Connection conn;
        std::chrono::steady_clock::time_point last_used;
    };

    PoolConfig config_;
    std::deque<IdleEntry> idle_;
    size_t total_count_ = 0;
    size_t active_count_ = 0;
    PoolStats stats_;
};

} // namespace apex::shared::adapters
