#pragma once

#include <apex/shared/adapters/adapter_error.hpp>
#include <apex/core/result.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>

namespace apex::shared::adapters {

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
/// Derived가 구현해야 할 메서드:
///   using ConnectionType = ...;
///   ConnectionType do_create_connection()
///   void do_destroy_connection(ConnectionType& conn)
///   bool do_validate(ConnectionType& conn)
template <typename Derived>
class ConnectionPool {
public:
    using Connection = typename Derived::ConnectionType;

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
                return conn;
            }
            derived().do_destroy_connection(conn);
            --total_count_;
        }
        // 새 커넥션 생성 (한도 내)
        if (total_count_ < config_.max_size) {
            auto conn = derived().do_create_connection();
            ++total_count_;
            ++active_count_;
            return conn;
        }
        return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    /// 커넥션 반환
    void release(Connection conn) {
        --active_count_;
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
};

} // namespace apex::shared::adapters
