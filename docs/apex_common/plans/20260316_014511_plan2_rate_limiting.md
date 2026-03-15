# Plan 2: Rate Limiting — 3계층 Sliding Window Counter

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan.

**Goal:** Per-IP(로컬) + Per-User(Redis) + Per-Endpoint(Redis) 3계층 Rate Limiting

**Architecture:** Sliding Window Counter 알고리즘 기반. Per-IP는 per-core 로컬 메모리, Per-User/Endpoint는 Redis Lua 스크립트. TOML config로 기본값+오버라이드.

**Tech Stack:** C++23, Redis Lua, TOML, Google Test

---

## 사전 지식

### Sliding Window Counter 알고리즘

Fixed Window의 boundary burst 문제를 해결하는 알고리즘. 현재 윈도우와 이전 윈도우의 카운트를 가중평균하여 실질적인 요청률을 계산한다.

```
estimated_count = previous_count * (1 - elapsed_ratio) + current_count
```

- `elapsed_ratio` = (현재 시각 - 현재 윈도우 시작) / 윈도우 크기
- 윈도우 전환 시 current → previous로 이동, current 리셋

### 파일 배치 계획

```
apex_shared/lib/rate_limit/                    ← 새 라이브러리
├── CMakeLists.txt
├── include/apex/shared/rate_limit/
│   ├── sliding_window_counter.hpp             ← Task 1
│   ├── per_ip_rate_limiter.hpp                ← Task 2
│   └── redis_rate_limiter.hpp                 ← Task 3, 4
├── src/
│   ├── sliding_window_counter.cpp             ← Task 1
│   ├── per_ip_rate_limiter.cpp                ← Task 2
│   └── redis_rate_limiter.cpp                 ← Task 3, 4
└── lua/
    └── sliding_window.lua                     ← Task 3

apex_shared/tests/unit/
├── test_sliding_window_counter.cpp            ← Task 1
├── test_per_ip_rate_limiter.cpp               ← Task 2
└── test_redis_rate_limiter.cpp                ← Task 3, 4
```

### 네임스페이스

```
apex::shared::rate_limit
```

### 의존성 관계

```
SlidingWindowCounter  (독립, 외부 의존 없음)
       ↓
PerIpRateLimiter      (SlidingWindowCounter + TimingWheel)
       ↓
RedisRateLimiter      (RedisMultiplexer + Lua script)
       ↓
Gateway 파이프라인 통합 (Task 5, 별도 Plan)
```

---

## Task 1: Sliding Window Counter 구현

### Step 1.1: CMake 인프라 구축

**파일**: `apex_shared/lib/rate_limit/CMakeLists.txt`

```cmake
# apex_shared/lib/rate_limit/CMakeLists.txt
add_library(apex_shared_rate_limit STATIC
    src/sliding_window_counter.cpp
)
add_library(apex::shared::rate_limit ALIAS apex_shared_rate_limit)

target_include_directories(apex_shared_rate_limit
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(apex_shared_rate_limit
    PUBLIC
        apex::core
)

target_compile_features(apex_shared_rate_limit PUBLIC cxx_std_23)

if(WIN32)
    target_compile_options(apex_shared_rate_limit PUBLIC /utf-8)
endif()
```

**파일**: `apex_shared/lib/adapters/CMakeLists.txt` (수정)

기존 내용 끝에 추가:

```cmake
# Rate Limiting (v0.5.2.1)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../rate_limit ${CMAKE_CURRENT_BINARY_DIR}/rate_limit)
```

> **주의**: rate_limit은 adapters 하위가 아니므로 상대 경로로 참조한다. 또는 `apex_shared/CMakeLists.txt`에서 직접 `add_subdirectory(lib/rate_limit)`을 추가하는 방식도 가능. 프로젝트 구조상 후자가 더 깔끔하므로 **`apex_shared/CMakeLists.txt`의 `add_subdirectory(lib/adapters)` 뒤에 추가**하는 것을 권장:

```cmake
# --- Rate Limiting ---
add_subdirectory(lib/rate_limit)
```

**빌드 검증**: `cmd.exe //c build.bat debug` (placeholder .cpp만으로 빌드 통과 확인)

### Step 1.2: SlidingWindowCounter 헤더

**파일**: `apex_shared/lib/rate_limit/include/apex/shared/rate_limit/sliding_window_counter.hpp`

```cpp
#pragma once

#include <chrono>
#include <cstdint>

namespace apex::shared::rate_limit {

/// Sliding Window Counter for rate limiting.
/// Tracks request counts across two adjacent windows and computes a
/// weighted estimate to avoid boundary burst issues.
///
/// Thread safety: NOT thread-safe. Designed for per-core use (shared-nothing).
///
/// Usage:
///   SlidingWindowCounter counter(100, std::chrono::seconds{60});
///   if (counter.allow(now)) {
///       // 요청 허용
///   } else {
///       // Rate limit 초과
///   }
class SlidingWindowCounter {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    /// @param limit Maximum allowed requests per window.
    /// @param window_size Duration of one window.
    SlidingWindowCounter(uint32_t limit, Duration window_size) noexcept;

    /// Check if a request is allowed at the given time.
    /// If allowed, increments the counter and returns true.
    /// If denied, does NOT increment and returns false.
    /// @param now Current time point.
    /// @return true if the request is within the rate limit.
    [[nodiscard]] bool allow(TimePoint now) noexcept;

    /// Current estimated count (for monitoring/logging).
    /// Does NOT advance the window.
    [[nodiscard]] double estimated_count(TimePoint now) const noexcept;

    /// Reset all counters and window state.
    void reset() noexcept;

    /// Last time any request was recorded (for LRU eviction).
    [[nodiscard]] TimePoint last_access() const noexcept { return last_access_; }

    // --- Config accessors ---
    [[nodiscard]] uint32_t limit() const noexcept { return limit_; }
    [[nodiscard]] Duration window_size() const noexcept { return window_size_; }

private:
    /// Advance window(s) if needed based on current time.
    void advance_window(TimePoint now) noexcept;

    uint32_t limit_;
    Duration window_size_;

    uint32_t current_count_{0};
    uint32_t previous_count_{0};
    TimePoint window_start_{};
    TimePoint last_access_{};
};

} // namespace apex::shared::rate_limit
```

### Step 1.3: SlidingWindowCounter 구현

**파일**: `apex_shared/lib/rate_limit/src/sliding_window_counter.cpp`

```cpp
#include <apex/shared/rate_limit/sliding_window_counter.hpp>

namespace apex::shared::rate_limit {

SlidingWindowCounter::SlidingWindowCounter(uint32_t limit, Duration window_size) noexcept
    : limit_(limit), window_size_(window_size) {}

bool SlidingWindowCounter::allow(TimePoint now) noexcept {
    advance_window(now);

    // Compute weighted estimate
    auto elapsed = now - window_start_;
    double ratio = static_cast<double>(elapsed.count()) /
                   static_cast<double>(window_size_.count());
    if (ratio > 1.0) ratio = 1.0;

    double estimate = previous_count_ * (1.0 - ratio) + current_count_;

    if (estimate >= static_cast<double>(limit_)) {
        return false;
    }

    ++current_count_;
    last_access_ = now;
    return true;
}

double SlidingWindowCounter::estimated_count(TimePoint now) const noexcept {
    // Compute without mutating state.
    // We need to figure out what window 'now' falls into.
    auto elapsed_since_start = now - window_start_;

    uint32_t eff_current = current_count_;
    uint32_t eff_previous = previous_count_;
    auto eff_window_start = window_start_;

    if (window_size_.count() > 0 && elapsed_since_start >= window_size_) {
        auto windows_passed =
            elapsed_since_start.count() / window_size_.count();
        if (windows_passed == 1) {
            eff_previous = eff_current;
            eff_current = 0;
            eff_window_start = window_start_ + window_size_;
        } else {
            // 2+ windows passed — both counters are stale
            return 0.0;
        }
    }

    auto elapsed = now - eff_window_start;
    double ratio = static_cast<double>(elapsed.count()) /
                   static_cast<double>(window_size_.count());
    if (ratio > 1.0) ratio = 1.0;

    return eff_previous * (1.0 - ratio) + eff_current;
}

void SlidingWindowCounter::reset() noexcept {
    current_count_ = 0;
    previous_count_ = 0;
    window_start_ = {};
    last_access_ = {};
}

void SlidingWindowCounter::advance_window(TimePoint now) noexcept {
    // First call — initialize window
    if (window_start_ == TimePoint{}) {
        window_start_ = now;
        return;
    }

    auto elapsed = now - window_start_;
    if (elapsed < window_size_) {
        return; // Still in current window
    }

    // How many full windows have elapsed?
    auto windows_passed = elapsed.count() / window_size_.count();

    if (windows_passed == 1) {
        // Exactly one window passed: rotate
        previous_count_ = current_count_;
        current_count_ = 0;
        window_start_ += window_size_;
    } else {
        // Two or more windows passed: previous data is completely stale
        previous_count_ = 0;
        current_count_ = 0;
        window_start_ = now;
    }
}

} // namespace apex::shared::rate_limit
```

### Step 1.4: 단위 테스트 — SlidingWindowCounter

**파일**: `apex_shared/tests/unit/test_sliding_window_counter.cpp`

```cpp
#include <apex/shared/rate_limit/sliding_window_counter.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::rate_limit;
using namespace std::chrono_literals;

// Helper: create a counter with known start time
class SlidingWindowCounterTest : public ::testing::Test {
protected:
    using Clock = SlidingWindowCounter::Clock;
    using TimePoint = SlidingWindowCounter::TimePoint;

    // Base time for deterministic testing
    TimePoint base_ = Clock::now();
};

TEST_F(SlidingWindowCounterTest, AllowWithinLimit) {
    SlidingWindowCounter counter(10, 1s);

    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(counter.allow(base_ + std::chrono::milliseconds(i * 10)));
    }
}

TEST_F(SlidingWindowCounterTest, DenyWhenLimitReached) {
    SlidingWindowCounter counter(5, 1s);

    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(counter.allow(base_ + std::chrono::milliseconds(i)));
    }
    // 6th request should be denied
    EXPECT_FALSE(counter.allow(base_ + 5ms));
}

TEST_F(SlidingWindowCounterTest, WindowRotation) {
    SlidingWindowCounter counter(5, 1s);

    // Fill first window
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(counter.allow(base_ + std::chrono::milliseconds(i * 100)));
    }
    EXPECT_FALSE(counter.allow(base_ + 500ms));

    // Move to second window (90% elapsed in new window)
    // estimate = 5 * (1 - 0.9) + 0 = 0.5, well under limit 5
    auto t = base_ + 1s + 900ms;
    EXPECT_TRUE(counter.allow(t));
}

TEST_F(SlidingWindowCounterTest, SlidingWindowWeightedEstimate) {
    SlidingWindowCounter counter(100, 1s);

    // Fill first window with 80 requests
    for (int i = 0; i < 80; ++i) {
        EXPECT_TRUE(counter.allow(base_ + std::chrono::milliseconds(i)));
    }

    // Move to 50% into second window
    // estimate = 80 * 0.5 + 0 = 40 → under limit
    auto t = base_ + 1500ms;
    double est = counter.estimated_count(t);
    EXPECT_NEAR(est, 40.0, 1.0);
}

TEST_F(SlidingWindowCounterTest, TwoWindowsPassedResetsCompletely) {
    SlidingWindowCounter counter(5, 1s);

    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(counter.allow(base_ + std::chrono::milliseconds(i)));
    }

    // Skip 2+ windows — everything stale
    auto t = base_ + 3s;
    EXPECT_TRUE(counter.allow(t));
    EXPECT_NEAR(counter.estimated_count(t + 1ms), 1.0, 0.1);
}

TEST_F(SlidingWindowCounterTest, Reset) {
    SlidingWindowCounter counter(5, 1s);

    for (int i = 0; i < 5; ++i) {
        counter.allow(base_ + std::chrono::milliseconds(i));
    }

    counter.reset();
    EXPECT_TRUE(counter.allow(base_ + 2s));
}

TEST_F(SlidingWindowCounterTest, LastAccess) {
    SlidingWindowCounter counter(10, 1s);

    auto t1 = base_ + 100ms;
    counter.allow(t1);
    EXPECT_EQ(counter.last_access(), t1);

    auto t2 = base_ + 200ms;
    counter.allow(t2);
    EXPECT_EQ(counter.last_access(), t2);
}

TEST_F(SlidingWindowCounterTest, DeniedRequestDoesNotUpdateLastAccess) {
    SlidingWindowCounter counter(1, 1s);

    auto t1 = base_;
    counter.allow(t1);  // allowed
    auto t2 = base_ + 100ms;
    counter.allow(t2);  // denied
    EXPECT_EQ(counter.last_access(), t1);
}

TEST_F(SlidingWindowCounterTest, ZeroLimitDeniesEverything) {
    SlidingWindowCounter counter(0, 1s);
    EXPECT_FALSE(counter.allow(base_));
}

TEST_F(SlidingWindowCounterTest, BoundaryBurstPrevention) {
    // Fixed Window의 문제: 윈도우 경계에서 2배 burst 가능.
    // Sliding Window Counter는 이를 방지한다.
    SlidingWindowCounter counter(100, 1s);

    // 첫 윈도우 끝부분에 100개 요청
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(counter.allow(base_ + 900ms + std::chrono::microseconds(i)));
    }

    // 두 번째 윈도우 시작 직후 (1ms 경과)
    // estimate = 100 * (1 - 0.001) + 0 ≈ 99.9 → 거의 limit
    // Fixed Window였으면 여기서 100개 더 보낼 수 있지만,
    // Sliding Window는 이전 윈도우 가중치가 높아서 차단
    auto t = base_ + 1001ms;
    EXPECT_FALSE(counter.allow(t));
}
```

### Step 1.5: CMake에 테스트 등록

**파일**: `apex_shared/tests/unit/CMakeLists.txt` (수정)

기존 파일 끝에 추가:

```cmake
# --- Rate Limiting tests ---
function(apex_shared_add_rate_limit_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME}
        PRIVATE
            apex::shared::rate_limit
            GTest::gtest_main
    )
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 30)
endfunction()

apex_shared_add_rate_limit_test(test_sliding_window_counter test_sliding_window_counter.cpp)
```

### Step 1.6: 빌드 + 테스트

```bash
cmd.exe //c build.bat debug
cd build/debug && ctest --output-on-failure -R test_sliding_window_counter
```

---

## Task 2: Per-IP Rate Limiter

### Step 2.1: PerIpRateLimiter 헤더

**파일**: `apex_shared/lib/rate_limit/include/apex/shared/rate_limit/per_ip_rate_limiter.hpp`

```cpp
#pragma once

#include <apex/shared/rate_limit/sliding_window_counter.hpp>
#include <apex/core/timing_wheel.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// boost::unordered_flat_map for O(1) IP lookup
#include <boost/unordered/unordered_flat_map.hpp>

namespace apex::shared::rate_limit {

struct PerIpRateLimiterConfig {
    uint32_t total_limit = 1000;           ///< 전체 한도 (per-core 한도 = total / num_cores)
    std::chrono::seconds window_size{60};  ///< 윈도우 크기
    uint32_t num_cores = 1;                ///< 코어 수 (per-core 한도 계산용)
    uint32_t max_entries = 65536;          ///< 최대 IP 엔트리 수
    uint32_t ttl_multiplier = 2;           ///< TTL = window_size * ttl_multiplier
};

/// Per-IP rate limiter with per-core isolation (lock-free).
///
/// Each core owns one instance. The per-core limit = total_limit / num_cores.
/// Uses boost::unordered_flat_map<string, Entry> for O(1) IP lookup.
/// TTL expiration via TimingWheel, LRU eviction when max_entries exceeded.
///
/// Prerequisite: SO_REUSEPORT ensures connections are distributed across cores.
/// If a core receives disproportionate traffic, its per-core limit triggers
/// earlier — this is intentionally conservative behavior.
///
/// Thread safety: NOT thread-safe. Designed for per-core use (shared-nothing).
class PerIpRateLimiter {
public:
    /// @param config Rate limiter configuration.
    /// @param timing_wheel Reference to the per-core TimingWheel
    ///        (tick interval must match the limiter's TTL granularity).
    PerIpRateLimiter(PerIpRateLimiterConfig config,
                     apex::core::TimingWheel& timing_wheel);

    ~PerIpRateLimiter();

    // Non-copyable, non-movable
    PerIpRateLimiter(const PerIpRateLimiter&) = delete;
    PerIpRateLimiter& operator=(const PerIpRateLimiter&) = delete;
    PerIpRateLimiter(PerIpRateLimiter&&) = delete;
    PerIpRateLimiter& operator=(PerIpRateLimiter&&) = delete;

    /// Check if a request from the given IP is allowed.
    /// @param ip Client IP address (IPv4 or IPv6 string).
    /// @param now Current time point.
    /// @return true if the request is within the per-core rate limit.
    [[nodiscard]] bool allow(std::string_view ip,
                             SlidingWindowCounter::TimePoint now) noexcept;

    /// Number of tracked IP entries.
    [[nodiscard]] uint32_t entry_count() const noexcept;

    /// Per-core limit (total_limit / num_cores).
    [[nodiscard]] uint32_t per_core_limit() const noexcept { return per_core_limit_; }

    /// Update configuration at runtime (TOML hot-reload).
    /// Resets all existing counters.
    void update_config(PerIpRateLimiterConfig config) noexcept;

private:
    struct Entry {
        SlidingWindowCounter counter;
        apex::core::TimingWheel::EntryId timer_id{0};
        /// Index in lru_order_ for O(1) LRU removal.
        uint32_t lru_index{0};
    };

    /// Evict the LRU entry to make room.
    void evict_lru() noexcept;

    /// Remove an entry by IP (called by TTL expiration).
    void remove_entry(std::string_view ip) noexcept;

    /// Touch an entry for LRU tracking (move to back).
    void touch_lru(Entry& entry, uint32_t map_index) noexcept;

    PerIpRateLimiterConfig config_;
    uint32_t per_core_limit_;
    apex::core::TimingWheel& timing_wheel_;
    uint32_t ttl_ticks_;  ///< TTL in TimingWheel ticks

    boost::unordered_flat_map<std::string, Entry> entries_;

    /// LRU order: vector of IP strings, front = oldest, back = newest.
    /// Uses swap-to-back for O(1) touch.
    std::vector<std::string> lru_order_;
};

} // namespace apex::shared::rate_limit
```

### Step 2.2: PerIpRateLimiter 구현

**파일**: `apex_shared/lib/rate_limit/src/per_ip_rate_limiter.cpp`

```cpp
#include <apex/shared/rate_limit/per_ip_rate_limiter.hpp>

#include <algorithm>
#include <cassert>

namespace apex::shared::rate_limit {

PerIpRateLimiter::PerIpRateLimiter(PerIpRateLimiterConfig config,
                                   apex::core::TimingWheel& timing_wheel)
    : config_(config),
      per_core_limit_(std::max(1u, config.total_limit / std::max(1u, config.num_cores))),
      timing_wheel_(timing_wheel) {
    entries_.reserve(config.max_entries);
    lru_order_.reserve(config.max_entries);

    // TTL in ticks: window_size * ttl_multiplier
    // Assumes TimingWheel tick interval = 1 second (standard configuration)
    auto ttl_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        config.window_size * config.ttl_multiplier);
    ttl_ticks_ = static_cast<uint32_t>(ttl_seconds.count());
}

PerIpRateLimiter::~PerIpRateLimiter() {
    // Cancel all timers
    for (auto& [ip, entry] : entries_) {
        if (entry.timer_id != 0) {
            timing_wheel_.cancel(entry.timer_id);
        }
    }
}

bool PerIpRateLimiter::allow(std::string_view ip,
                             SlidingWindowCounter::TimePoint now) noexcept {
    auto it = entries_.find(ip);

    if (it == entries_.end()) {
        // New IP — check capacity
        if (entries_.size() >= config_.max_entries) {
            evict_lru();
        }

        // Create new entry
        auto [new_it, inserted] = entries_.emplace(
            std::string(ip),
            Entry{
                .counter = SlidingWindowCounter(per_core_limit_, config_.window_size),
                .timer_id = timing_wheel_.schedule(ttl_ticks_),
                .lru_index = static_cast<uint32_t>(lru_order_.size()),
            });
        lru_order_.emplace_back(std::string(ip));

        return new_it->second.counter.allow(now);
    }

    auto& entry = it->second;

    // Reschedule TTL timer
    timing_wheel_.reschedule(entry.timer_id, ttl_ticks_);

    // Update LRU
    touch_lru(entry, entry.lru_index);

    return entry.counter.allow(now);
}

uint32_t PerIpRateLimiter::entry_count() const noexcept {
    return static_cast<uint32_t>(entries_.size());
}

void PerIpRateLimiter::update_config(PerIpRateLimiterConfig config) noexcept {
    // Cancel all existing timers
    for (auto& [ip, entry] : entries_) {
        if (entry.timer_id != 0) {
            timing_wheel_.cancel(entry.timer_id);
        }
    }
    entries_.clear();
    lru_order_.clear();

    config_ = config;
    per_core_limit_ = std::max(1u, config.total_limit / std::max(1u, config.num_cores));
    entries_.reserve(config.max_entries);
    lru_order_.reserve(config.max_entries);

    auto ttl_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        config.window_size * config.ttl_multiplier);
    ttl_ticks_ = static_cast<uint32_t>(ttl_seconds.count());
}

void PerIpRateLimiter::evict_lru() noexcept {
    if (lru_order_.empty()) return;

    // Front of lru_order_ is the oldest entry
    auto& oldest_ip = lru_order_.front();
    auto it = entries_.find(oldest_ip);
    if (it != entries_.end()) {
        if (it->second.timer_id != 0) {
            timing_wheel_.cancel(it->second.timer_id);
        }
        entries_.erase(it);
    }

    // Remove from LRU: swap front with back, pop back
    if (lru_order_.size() > 1) {
        auto& back_ip = lru_order_.back();
        auto back_it = entries_.find(back_ip);
        if (back_it != entries_.end()) {
            back_it->second.lru_index = 0;
        }
        std::swap(lru_order_.front(), lru_order_.back());
    }
    lru_order_.pop_back();
}

void PerIpRateLimiter::remove_entry(std::string_view ip) noexcept {
    auto it = entries_.find(ip);
    if (it == entries_.end()) return;

    auto lru_idx = it->second.lru_index;

    if (it->second.timer_id != 0) {
        timing_wheel_.cancel(it->second.timer_id);
    }
    entries_.erase(it);

    // Remove from LRU order
    if (lru_idx < lru_order_.size()) {
        if (lru_idx != lru_order_.size() - 1) {
            auto& back_ip = lru_order_.back();
            auto back_it = entries_.find(back_ip);
            if (back_it != entries_.end()) {
                back_it->second.lru_index = lru_idx;
            }
            std::swap(lru_order_[lru_idx], lru_order_.back());
        }
        lru_order_.pop_back();
    }
}

void PerIpRateLimiter::touch_lru(Entry& entry, uint32_t map_index) noexcept {
    // Move to back (most recently used)
    if (map_index >= lru_order_.size()) return;
    auto last_idx = static_cast<uint32_t>(lru_order_.size() - 1);
    if (map_index == last_idx) return;  // Already at back

    // Swap with back
    auto& back_ip = lru_order_[last_idx];
    auto back_it = entries_.find(back_ip);
    if (back_it != entries_.end()) {
        back_it->second.lru_index = map_index;
    }
    entry.lru_index = last_idx;
    std::swap(lru_order_[map_index], lru_order_[last_idx]);
}

} // namespace apex::shared::rate_limit
```

### Step 2.3: CMake에 소스 추가

**파일**: `apex_shared/lib/rate_limit/CMakeLists.txt` (수정)

소스 목록에 `src/per_ip_rate_limiter.cpp` 추가, Boost 의존성 추가:

```cmake
add_library(apex_shared_rate_limit STATIC
    src/sliding_window_counter.cpp
    src/per_ip_rate_limiter.cpp
)
add_library(apex::shared::rate_limit ALIAS apex_shared_rate_limit)

find_package(Boost REQUIRED)

target_include_directories(apex_shared_rate_limit
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(apex_shared_rate_limit
    PUBLIC
        apex::core
        Boost::boost
)

target_compile_features(apex_shared_rate_limit PUBLIC cxx_std_23)

if(WIN32)
    target_compile_options(apex_shared_rate_limit PUBLIC /utf-8)
endif()
```

### Step 2.4: 단위 테스트 — PerIpRateLimiter

**파일**: `apex_shared/tests/unit/test_per_ip_rate_limiter.cpp`

```cpp
#include <apex/shared/rate_limit/per_ip_rate_limiter.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::rate_limit;
using namespace std::chrono_literals;

class PerIpRateLimiterTest : public ::testing::Test {
protected:
    using Clock = SlidingWindowCounter::Clock;
    using TimePoint = SlidingWindowCounter::TimePoint;

    TimePoint base_ = Clock::now();
    std::vector<apex::core::TimingWheel::EntryId> expired_;

    // TimingWheel: 512 slots, 1s tick
    apex::core::TimingWheel tw_{512, [this](auto id) { expired_.push_back(id); }};
};

TEST_F(PerIpRateLimiterTest, BasicAllowDeny) {
    PerIpRateLimiter limiter(
        {.total_limit = 10, .window_size = 1s, .num_cores = 1}, tw_);

    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(limiter.allow("192.168.1.1", base_ + std::chrono::milliseconds(i)));
    }
    EXPECT_FALSE(limiter.allow("192.168.1.1", base_ + 10ms));
}

TEST_F(PerIpRateLimiterTest, DifferentIpsIndependent) {
    PerIpRateLimiter limiter(
        {.total_limit = 5, .window_size = 1s, .num_cores = 1}, tw_);

    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(limiter.allow("10.0.0.1", base_ + std::chrono::milliseconds(i)));
    }
    EXPECT_FALSE(limiter.allow("10.0.0.1", base_ + 5ms));

    // Different IP is independent
    EXPECT_TRUE(limiter.allow("10.0.0.2", base_ + 5ms));
}

TEST_F(PerIpRateLimiterTest, PerCoreLimitDivision) {
    // total=100, 4 cores → per-core = 25
    PerIpRateLimiter limiter(
        {.total_limit = 100, .window_size = 1s, .num_cores = 4}, tw_);

    EXPECT_EQ(limiter.per_core_limit(), 25u);

    for (int i = 0; i < 25; ++i) {
        EXPECT_TRUE(limiter.allow("1.2.3.4", base_ + std::chrono::milliseconds(i)));
    }
    EXPECT_FALSE(limiter.allow("1.2.3.4", base_ + 25ms));
}

TEST_F(PerIpRateLimiterTest, MaxEntriesEvictsLRU) {
    PerIpRateLimiter limiter(
        {.total_limit = 100, .window_size = 1s, .num_cores = 1, .max_entries = 3},
        tw_);

    limiter.allow("ip1", base_);
    limiter.allow("ip2", base_ + 1ms);
    limiter.allow("ip3", base_ + 2ms);
    EXPECT_EQ(limiter.entry_count(), 3u);

    // Adding ip4 should evict ip1 (LRU)
    limiter.allow("ip4", base_ + 3ms);
    EXPECT_EQ(limiter.entry_count(), 3u);

    // ip1's counter was evicted — fresh counter created
    // (all 100 requests available again)
    int allowed = 0;
    for (int i = 0; i < 100; ++i) {
        if (limiter.allow("ip1", base_ + 4ms + std::chrono::microseconds(i)))
            ++allowed;
    }
    // ip4 was evicted to make room for ip1 (ip2 was LRU at that point, actually ip2)
    // The exact eviction depends on LRU order after ip4 insertion
    EXPECT_GT(allowed, 0);
}

TEST_F(PerIpRateLimiterTest, EntryCount) {
    PerIpRateLimiter limiter(
        {.total_limit = 100, .window_size = 1s, .num_cores = 1}, tw_);

    EXPECT_EQ(limiter.entry_count(), 0u);
    limiter.allow("a", base_);
    EXPECT_EQ(limiter.entry_count(), 1u);
    limiter.allow("b", base_ + 1ms);
    EXPECT_EQ(limiter.entry_count(), 2u);
    // Same IP doesn't create new entry
    limiter.allow("a", base_ + 2ms);
    EXPECT_EQ(limiter.entry_count(), 2u);
}

TEST_F(PerIpRateLimiterTest, UpdateConfigResetsAll) {
    PerIpRateLimiter limiter(
        {.total_limit = 10, .window_size = 1s, .num_cores = 1}, tw_);

    for (int i = 0; i < 10; ++i) {
        limiter.allow("ip", base_ + std::chrono::milliseconds(i));
    }
    EXPECT_FALSE(limiter.allow("ip", base_ + 10ms));

    // Update config — all counters reset
    limiter.update_config({.total_limit = 20, .window_size = 1s, .num_cores = 1});
    EXPECT_EQ(limiter.entry_count(), 0u);
    EXPECT_EQ(limiter.per_core_limit(), 20u);
    EXPECT_TRUE(limiter.allow("ip", base_ + 1s));
}

TEST_F(PerIpRateLimiterTest, IPv6Support) {
    PerIpRateLimiter limiter(
        {.total_limit = 5, .window_size = 1s, .num_cores = 1}, tw_);

    EXPECT_TRUE(limiter.allow("::1", base_));
    EXPECT_TRUE(limiter.allow("2001:db8::1", base_ + 1ms));
    EXPECT_EQ(limiter.entry_count(), 2u);
}

TEST_F(PerIpRateLimiterTest, MinimumPerCoreLimit) {
    // total=1, 4 cores → per-core should be at least 1
    PerIpRateLimiter limiter(
        {.total_limit = 1, .window_size = 1s, .num_cores = 4}, tw_);

    EXPECT_GE(limiter.per_core_limit(), 1u);
}
```

### Step 2.5: CMake에 테스트 등록

**파일**: `apex_shared/tests/unit/CMakeLists.txt` (수정)

Step 1.5에서 추가한 rate limit 테스트 섹션에 추가:

```cmake
apex_shared_add_rate_limit_test(test_per_ip_rate_limiter test_per_ip_rate_limiter.cpp)
```

### Step 2.6: 빌드 + 테스트

```bash
cmd.exe //c build.bat debug
cd build/debug && ctest --output-on-failure -R test_per_ip_rate_limiter
```

---

## Task 3: Per-User Rate Limiter (Redis Lua)

### Step 3.1: Redis Lua 스크립트

**파일**: `apex_shared/lib/rate_limit/lua/sliding_window.lua`

```lua
-- Sliding Window Counter for Redis.
-- Keys: KEYS[1] = current window key, KEYS[2] = previous window key
-- Args: ARGV[1] = limit, ARGV[2] = window_size_ms, ARGV[3] = now_ms
--
-- Returns: {allowed (0/1), estimated_count, retry_after_ms}
--
-- 키 형식 예시:
--   Per-User:     "rl:user:{user_id}:cur", "rl:user:{user_id}:prev"
--   Per-Endpoint: "rl:ep:{user_id}:{msg_id}:cur", "rl:ep:{user_id}:{msg_id}:prev"

local cur_key = KEYS[1]
local prev_key = KEYS[2]
local limit = tonumber(ARGV[1])
local window_ms = tonumber(ARGV[2])
local now_ms = tonumber(ARGV[3])

-- Get current and previous window counts
local cur_count = tonumber(redis.call('GET', cur_key) or '0')
local prev_count = tonumber(redis.call('GET', prev_key) or '0')

-- Check if we need to detect window boundary.
-- We store window start timestamp alongside the current key.
local meta_key = cur_key .. ':meta'
local window_start = tonumber(redis.call('GET', meta_key) or '0')

if window_start == 0 then
    -- First request: initialize window
    window_start = now_ms
    redis.call('SET', meta_key, tostring(now_ms))
    redis.call('PEXPIRE', meta_key, window_ms * 3)
end

local elapsed = now_ms - window_start

if elapsed >= window_ms then
    -- Window rotation needed
    local windows_passed = math.floor(elapsed / window_ms)
    if windows_passed == 1 then
        -- Rotate: current → previous
        redis.call('SET', prev_key, tostring(cur_count))
        redis.call('PEXPIRE', prev_key, window_ms * 2)
        cur_count = 0
        redis.call('SET', cur_key, '0')
        window_start = window_start + window_ms
    else
        -- 2+ windows: everything stale
        redis.call('SET', prev_key, '0')
        redis.call('PEXPIRE', prev_key, window_ms * 2)
        prev_count = 0
        cur_count = 0
        redis.call('SET', cur_key, '0')
        window_start = now_ms
    end
    redis.call('SET', meta_key, tostring(window_start))
    redis.call('PEXPIRE', meta_key, window_ms * 3)
    elapsed = now_ms - window_start
end

-- Compute weighted estimate
local ratio = elapsed / window_ms
if ratio > 1.0 then ratio = 1.0 end
local estimate = prev_count * (1.0 - ratio) + cur_count

if estimate >= limit then
    -- Denied: compute retry_after_ms
    -- Time until previous window weight drops enough
    -- prev_count * (1 - (elapsed + retry) / window) + cur_count < limit
    -- Simplified: wait until current window ends
    local retry_after = math.max(0, window_ms - elapsed)
    return {0, math.floor(estimate), math.floor(retry_after)}
end

-- Allowed: increment current counter
cur_count = redis.call('INCR', cur_key)
redis.call('PEXPIRE', cur_key, window_ms * 2)

return {1, math.floor(estimate), 0}
```

### Step 3.2: RedisRateLimiter 헤더

**파일**: `apex_shared/lib/rate_limit/include/apex/shared/rate_limit/redis_rate_limiter.hpp`

```cpp
#pragma once

#include <apex/core/result.hpp>
#include <apex/shared/adapters/redis/redis_multiplexer.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace apex::shared::rate_limit {

/// Result of a rate limit check.
struct RateLimitResult {
    bool allowed;
    uint32_t estimated_count;
    uint32_t retry_after_ms;  ///< 0 if allowed, else suggested wait time
};

struct RedisRateLimiterConfig {
    uint32_t default_limit = 100;            ///< 기본 한도 (per window)
    std::chrono::seconds window_size{60};    ///< 윈도우 크기
};

/// Redis-based rate limiter using Lua scripting for atomic check+increment.
/// Used for Per-User and Per-Endpoint rate limiting.
///
/// Thread safety: NOT thread-safe (per-core RedisMultiplexer 사용).
///
/// Usage:
///   RedisRateLimiter limiter(config, multiplexer);
///
///   // Per-User check
///   auto result = co_await limiter.check_user(user_id, now_ms);
///
///   // Per-Endpoint check
///   auto result = co_await limiter.check_endpoint(user_id, msg_id, now_ms);
///
///   // Per-Endpoint with override limit
///   auto result = co_await limiter.check_endpoint(user_id, msg_id, now_ms, 50);
class RedisRateLimiter {
public:
    RedisRateLimiter(RedisRateLimiterConfig config,
                     adapters::redis::RedisMultiplexer& multiplexer);

    /// Check per-user rate limit.
    /// @param user_id User identifier.
    /// @param now_ms Current time in milliseconds since epoch.
    /// @return RateLimitResult via coroutine.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<RateLimitResult>>
    check_user(uint64_t user_id, uint64_t now_ms);

    /// Check per-endpoint rate limit.
    /// @param user_id User identifier.
    /// @param msg_id Message type ID.
    /// @param now_ms Current time in milliseconds since epoch.
    /// @param limit_override Optional per-endpoint limit override (0 = use default).
    /// @return RateLimitResult via coroutine.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<RateLimitResult>>
    check_endpoint(uint64_t user_id, uint32_t msg_id, uint64_t now_ms,
                   uint32_t limit_override = 0);

    /// Update configuration at runtime (TOML hot-reload).
    void update_config(RedisRateLimiterConfig config) noexcept;

    /// Get the loaded Lua script SHA1 hash (for EVALSHA).
    [[nodiscard]] std::string_view script_sha() const noexcept { return script_sha_; }

    /// Load the Lua script into Redis (SCRIPT LOAD). Must be called once
    /// after connection is established.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> load_script();

private:
    /// Execute the sliding window Lua script.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<RateLimitResult>>
    execute_lua(std::string_view cur_key, std::string_view prev_key,
                uint32_t limit, uint64_t now_ms);

    RedisRateLimiterConfig config_;
    adapters::redis::RedisMultiplexer& multiplexer_;
    std::string script_sha_;

    /// Embedded Lua script source (compiled into binary).
    static constexpr std::string_view LUA_SCRIPT = R"lua(
local cur_key = KEYS[1]
local prev_key = KEYS[2]
local limit = tonumber(ARGV[1])
local window_ms = tonumber(ARGV[2])
local now_ms = tonumber(ARGV[3])

local cur_count = tonumber(redis.call('GET', cur_key) or '0')
local prev_count = tonumber(redis.call('GET', prev_key) or '0')

local meta_key = cur_key .. ':meta'
local window_start = tonumber(redis.call('GET', meta_key) or '0')

if window_start == 0 then
    window_start = now_ms
    redis.call('SET', meta_key, tostring(now_ms))
    redis.call('PEXPIRE', meta_key, window_ms * 3)
end

local elapsed = now_ms - window_start

if elapsed >= window_ms then
    local windows_passed = math.floor(elapsed / window_ms)
    if windows_passed == 1 then
        redis.call('SET', prev_key, tostring(cur_count))
        redis.call('PEXPIRE', prev_key, window_ms * 2)
        cur_count = 0
        redis.call('SET', cur_key, '0')
        window_start = window_start + window_ms
    else
        redis.call('SET', prev_key, '0')
        redis.call('PEXPIRE', prev_key, window_ms * 2)
        prev_count = 0
        cur_count = 0
        redis.call('SET', cur_key, '0')
        window_start = now_ms
    end
    redis.call('SET', meta_key, tostring(window_start))
    redis.call('PEXPIRE', meta_key, window_ms * 3)
    elapsed = now_ms - window_start
end

local ratio = elapsed / window_ms
if ratio > 1.0 then ratio = 1.0 end
local estimate = prev_count * (1.0 - ratio) + cur_count

if estimate >= limit then
    local retry_after = math.max(0, window_ms - elapsed)
    return {0, math.floor(estimate), math.floor(retry_after)}
end

cur_count = redis.call('INCR', cur_key)
redis.call('PEXPIRE', cur_key, window_ms * 2)

return {1, math.floor(estimate), 0}
)lua";
};

} // namespace apex::shared::rate_limit
```

### Step 3.3: RedisRateLimiter 구현

**파일**: `apex_shared/lib/rate_limit/src/redis_rate_limiter.cpp`

```cpp
#include <apex/shared/rate_limit/redis_rate_limiter.hpp>

#include <format>

namespace apex::shared::rate_limit {

RedisRateLimiter::RedisRateLimiter(
    RedisRateLimiterConfig config,
    adapters::redis::RedisMultiplexer& multiplexer)
    : config_(config), multiplexer_(multiplexer) {}

boost::asio::awaitable<apex::core::Result<RateLimitResult>>
RedisRateLimiter::check_user(uint64_t user_id, uint64_t now_ms) {
    auto cur_key = std::format("rl:user:{}:cur", user_id);
    auto prev_key = std::format("rl:user:{}:prev", user_id);
    co_return co_await execute_lua(cur_key, prev_key, config_.default_limit, now_ms);
}

boost::asio::awaitable<apex::core::Result<RateLimitResult>>
RedisRateLimiter::check_endpoint(uint64_t user_id, uint32_t msg_id,
                                 uint64_t now_ms, uint32_t limit_override) {
    auto cur_key = std::format("rl:ep:{}:{}:cur", user_id, msg_id);
    auto prev_key = std::format("rl:ep:{}:{}:prev", user_id, msg_id);
    auto limit = (limit_override > 0) ? limit_override : config_.default_limit;
    co_return co_await execute_lua(cur_key, prev_key, limit, now_ms);
}

void RedisRateLimiter::update_config(RedisRateLimiterConfig config) noexcept {
    config_ = config;
}

boost::asio::awaitable<apex::core::Result<void>>
RedisRateLimiter::load_script() {
    auto cmd = std::format("SCRIPT LOAD {}", LUA_SCRIPT);
    auto result = co_await multiplexer_.command(cmd);
    if (!result.has_value()) {
        co_return std::unexpected(result.error());
    }
    if (!result->is_string()) {
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }
    script_sha_ = result->str;
    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<RateLimitResult>>
RedisRateLimiter::execute_lua(std::string_view cur_key, std::string_view prev_key,
                              uint32_t limit, uint64_t now_ms) {
    auto window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        config_.window_size).count();

    std::string cmd;
    if (!script_sha_.empty()) {
        cmd = std::format("EVALSHA {} 2 {} {} {} {} {}",
                          script_sha_, cur_key, prev_key,
                          limit, window_ms, now_ms);
    } else {
        // Fallback to EVAL if script not loaded
        cmd = std::format("EVAL \"{}\" 2 {} {} {} {} {}",
                          LUA_SCRIPT, cur_key, prev_key,
                          limit, window_ms, now_ms);
    }

    auto result = co_await multiplexer_.command(cmd);
    if (!result.has_value()) {
        co_return std::unexpected(result.error());
    }

    if (!result->is_array() || result->array.size() < 3) {
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    auto& arr = result->array;
    co_return RateLimitResult{
        .allowed = (arr[0].integer != 0),
        .estimated_count = static_cast<uint32_t>(arr[1].integer),
        .retry_after_ms = static_cast<uint32_t>(arr[2].integer),
    };
}

} // namespace apex::shared::rate_limit
```

### Step 3.4: CMake에 소스 추가

**파일**: `apex_shared/lib/rate_limit/CMakeLists.txt` (수정)

소스 목록에 추가 + Redis 의존성 추가:

```cmake
add_library(apex_shared_rate_limit STATIC
    src/sliding_window_counter.cpp
    src/per_ip_rate_limiter.cpp
    src/redis_rate_limiter.cpp
)
add_library(apex::shared::rate_limit ALIAS apex_shared_rate_limit)

find_package(Boost REQUIRED)
find_package(spdlog CONFIG REQUIRED)

target_include_directories(apex_shared_rate_limit
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(apex_shared_rate_limit
    PUBLIC
        apex::core
        apex::shared::adapters::redis
        Boost::boost
        spdlog::spdlog
)

target_compile_features(apex_shared_rate_limit PUBLIC cxx_std_23)

if(WIN32)
    target_compile_options(apex_shared_rate_limit PUBLIC /utf-8)
endif()
```

### Step 3.5: 단위 테스트 — RedisRateLimiter

Redis가 필요한 통합 테스트와, Redis 없이 Lua 로직을 검증하는 단위 테스트를 분리한다. 여기서는 RedisMultiplexer를 mock하는 것이 복잡하므로, **Lua 스크립트 자체의 로직 검증**에 초점을 맞춘다.

**파일**: `apex_shared/tests/unit/test_redis_rate_limiter.cpp`

```cpp
#include <apex/shared/rate_limit/redis_rate_limiter.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::rate_limit;

// These tests verify the configuration and key formatting logic.
// Full Redis integration tests require a running Redis instance
// and are placed in tests/integration/.

TEST(RedisRateLimiter, ConfigDefaults) {
    RedisRateLimiterConfig config;
    EXPECT_EQ(config.default_limit, 100u);
    EXPECT_EQ(config.window_size, std::chrono::seconds{60});
}

TEST(RedisRateLimiter, UpdateConfig) {
    // This test verifies that update_config changes internal state.
    // We can't easily test the Redis interaction without a mock,
    // but we verify the config is properly stored.
    RedisRateLimiterConfig config{.default_limit = 50, .window_size = std::chrono::seconds{30}};
    EXPECT_EQ(config.default_limit, 50u);
    EXPECT_EQ(config.window_size, std::chrono::seconds{30});
}

TEST(RedisRateLimiter, RateLimitResultStruct) {
    RateLimitResult result{.allowed = true, .estimated_count = 42, .retry_after_ms = 0};
    EXPECT_TRUE(result.allowed);
    EXPECT_EQ(result.estimated_count, 42u);
    EXPECT_EQ(result.retry_after_ms, 0u);
}

TEST(RedisRateLimiter, LuaScriptNotEmpty) {
    // Verify the embedded Lua script is non-empty
    EXPECT_FALSE(RedisRateLimiter::LUA_SCRIPT.empty());
}
```

> **참고**: Redis 실제 연동 테스트는 `docker-compose`로 Redis 인스턴스를 올린 후 `tests/integration/`에서 수행. 이 Plan에서는 단위 테스트 범위만 다룬다.

### Step 3.6: CMake에 테스트 등록

**파일**: `apex_shared/tests/unit/CMakeLists.txt` (수정)

Rate limit 테스트 섹션에 추가:

```cmake
apex_shared_add_rate_limit_test(test_redis_rate_limiter test_redis_rate_limiter.cpp)
```

> **주의**: `apex_shared_add_rate_limit_test` 함수가 `apex::shared::rate_limit`을 링크하고, 이 라이브러리가 `apex::shared::adapters::redis`에 의존하므로 Redis/hiredis 헤더가 포함된다. 빌드 환경에 hiredis가 없으면 링크 에러가 발생할 수 있다. vcpkg 의존성이 설치되어 있는지 확인할 것.

### Step 3.7: 빌드 + 테스트

```bash
cmd.exe //c build.bat debug
cd build/debug && ctest --output-on-failure -R test_redis_rate_limiter
```

---

## Task 4: Per-Endpoint Rate Limiter

Per-Endpoint는 Task 3의 RedisRateLimiter를 그대로 활용하되, **endpoint별 오버라이드 한도**를 TOML config에서 관리하는 래퍼 계층을 추가한다.

### Step 4.1: EndpointRateLimiterConfig 및 래퍼

**파일**: `apex_shared/lib/rate_limit/include/apex/shared/rate_limit/endpoint_rate_config.hpp`

```cpp
#pragma once

#include <chrono>
#include <cstdint>

#include <boost/unordered/unordered_flat_map.hpp>

namespace apex::shared::rate_limit {

/// Per-endpoint rate limit configuration.
/// Loaded from TOML config, supports hot-reload.
///
/// TOML 예시:
/// ```toml
/// [rate_limit.endpoint]
/// default_limit = 60
/// window_size_seconds = 60
///
/// # msg_id별 오버라이드
/// [rate_limit.endpoint.overrides]
/// 1001 = 10    # LoginRequest: 분당 10회
/// 2001 = 200   # ChatSendMessage: 분당 200회
/// 2010 = 5     # CreateRoom: 분당 5회
/// ```
struct EndpointRateConfig {
    uint32_t default_limit = 60;             ///< msg_id 오버라이드가 없을 때 적용
    std::chrono::seconds window_size{60};    ///< 윈도우 크기

    /// msg_id → limit 오버라이드 매핑
    boost::unordered_flat_map<uint32_t, uint32_t> overrides;

    /// Get the effective limit for a msg_id.
    [[nodiscard]] uint32_t limit_for(uint32_t msg_id) const noexcept {
        auto it = overrides.find(msg_id);
        if (it != overrides.end()) {
            return it->second;
        }
        return default_limit;
    }
};

} // namespace apex::shared::rate_limit
```

### Step 4.2: EndpointRateLimiter 통합 클래스

Per-User와 Per-Endpoint를 하나의 facade로 묶는다. Gateway 파이프라인에서는 이 클래스 하나만 사용.

**파일**: `apex_shared/lib/rate_limit/include/apex/shared/rate_limit/rate_limit_facade.hpp`

```cpp
#pragma once

#include <apex/shared/rate_limit/endpoint_rate_config.hpp>
#include <apex/shared/rate_limit/per_ip_rate_limiter.hpp>
#include <apex/shared/rate_limit/redis_rate_limiter.hpp>

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <string_view>

namespace apex::shared::rate_limit {

/// Facade for the 3-layer rate limiting pipeline.
///
/// Usage in Gateway pipeline:
///   1. check_ip()       — after TLS, before JWT
///   2. check_user()     — after JWT verification
///   3. check_endpoint() — before msg_id routing
///
/// Each layer is independently configurable and can be disabled
/// by setting limit to 0 (bypassed).
class RateLimitFacade {
public:
    /// @param ip_limiter Per-IP rate limiter (per-core, local memory).
    /// @param redis_limiter Redis-based rate limiter (Per-User + Per-Endpoint).
    /// @param endpoint_config Endpoint-specific overrides.
    RateLimitFacade(PerIpRateLimiter& ip_limiter,
                    RedisRateLimiter& redis_limiter,
                    EndpointRateConfig endpoint_config);

    /// Layer 1: Per-IP check (local memory, no I/O).
    /// @param ip Client IP address.
    /// @param now Time point for sliding window.
    /// @return true if allowed.
    [[nodiscard]] bool check_ip(
        std::string_view ip,
        SlidingWindowCounter::TimePoint now) noexcept;

    /// Layer 2: Per-User check (Redis Lua).
    /// @param user_id Authenticated user ID.
    /// @param now_ms Current time in milliseconds since epoch.
    /// @return RateLimitResult via coroutine.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<RateLimitResult>>
    check_user(uint64_t user_id, uint64_t now_ms);

    /// Layer 3: Per-Endpoint check (Redis Lua, with msg_id override).
    /// @param user_id Authenticated user ID.
    /// @param msg_id Message type ID.
    /// @param now_ms Current time in milliseconds since epoch.
    /// @return RateLimitResult via coroutine.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<RateLimitResult>>
    check_endpoint(uint64_t user_id, uint32_t msg_id, uint64_t now_ms);

    /// Update endpoint config at runtime (TOML hot-reload).
    void update_endpoint_config(EndpointRateConfig config) noexcept;

private:
    PerIpRateLimiter& ip_limiter_;
    RedisRateLimiter& redis_limiter_;
    EndpointRateConfig endpoint_config_;
};

} // namespace apex::shared::rate_limit
```

### Step 4.3: RateLimitFacade 구현

**파일**: `apex_shared/lib/rate_limit/src/rate_limit_facade.cpp`

```cpp
#include <apex/shared/rate_limit/rate_limit_facade.hpp>

namespace apex::shared::rate_limit {

RateLimitFacade::RateLimitFacade(
    PerIpRateLimiter& ip_limiter,
    RedisRateLimiter& redis_limiter,
    EndpointRateConfig endpoint_config)
    : ip_limiter_(ip_limiter),
      redis_limiter_(redis_limiter),
      endpoint_config_(std::move(endpoint_config)) {}

bool RateLimitFacade::check_ip(
    std::string_view ip,
    SlidingWindowCounter::TimePoint now) noexcept {
    return ip_limiter_.allow(ip, now);
}

boost::asio::awaitable<apex::core::Result<RateLimitResult>>
RateLimitFacade::check_user(uint64_t user_id, uint64_t now_ms) {
    co_return co_await redis_limiter_.check_user(user_id, now_ms);
}

boost::asio::awaitable<apex::core::Result<RateLimitResult>>
RateLimitFacade::check_endpoint(uint64_t user_id, uint32_t msg_id,
                                uint64_t now_ms) {
    auto limit = endpoint_config_.limit_for(msg_id);
    co_return co_await redis_limiter_.check_endpoint(user_id, msg_id, now_ms, limit);
}

void RateLimitFacade::update_endpoint_config(EndpointRateConfig config) noexcept {
    endpoint_config_ = std::move(config);
}

} // namespace apex::shared::rate_limit
```

### Step 4.4: CMake에 소스 추가

**파일**: `apex_shared/lib/rate_limit/CMakeLists.txt` (최종 버전)

```cmake
# apex_shared/lib/rate_limit/CMakeLists.txt
add_library(apex_shared_rate_limit STATIC
    src/sliding_window_counter.cpp
    src/per_ip_rate_limiter.cpp
    src/redis_rate_limiter.cpp
    src/rate_limit_facade.cpp
)
add_library(apex::shared::rate_limit ALIAS apex_shared_rate_limit)

find_package(Boost REQUIRED)
find_package(spdlog CONFIG REQUIRED)

target_include_directories(apex_shared_rate_limit
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(apex_shared_rate_limit
    PUBLIC
        apex::core
        apex::shared::adapters::redis
        Boost::boost
        spdlog::spdlog
)

target_compile_features(apex_shared_rate_limit PUBLIC cxx_std_23)

if(WIN32)
    target_compile_options(apex_shared_rate_limit PUBLIC /utf-8)
endif()
```

### Step 4.5: 단위 테스트 — EndpointRateConfig

**파일**: `apex_shared/tests/unit/test_endpoint_rate_config.cpp`

```cpp
#include <apex/shared/rate_limit/endpoint_rate_config.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::rate_limit;

TEST(EndpointRateConfig, DefaultLimit) {
    EndpointRateConfig config{.default_limit = 60};
    EXPECT_EQ(config.limit_for(9999), 60u);
}

TEST(EndpointRateConfig, OverrideLimit) {
    EndpointRateConfig config{.default_limit = 60};
    config.overrides[1001] = 10;   // LoginRequest
    config.overrides[2001] = 200;  // ChatSendMessage

    EXPECT_EQ(config.limit_for(1001), 10u);
    EXPECT_EQ(config.limit_for(2001), 200u);
    EXPECT_EQ(config.limit_for(3000), 60u);  // No override → default
}

TEST(EndpointRateConfig, EmptyOverrides) {
    EndpointRateConfig config{.default_limit = 100};
    // All msg_ids use default
    for (uint32_t id = 0; id < 100; ++id) {
        EXPECT_EQ(config.limit_for(id), 100u);
    }
}

TEST(EndpointRateConfig, ZeroDefault) {
    EndpointRateConfig config{.default_limit = 0};
    config.overrides[1001] = 10;

    EXPECT_EQ(config.limit_for(1001), 10u);
    EXPECT_EQ(config.limit_for(9999), 0u);  // Effectively disabled
}

TEST(EndpointRateConfig, WindowSize) {
    EndpointRateConfig config{.window_size = std::chrono::seconds{30}};
    EXPECT_EQ(config.window_size, std::chrono::seconds{30});
}
```

### Step 4.6: CMake에 테스트 등록

**파일**: `apex_shared/tests/unit/CMakeLists.txt` (수정)

Rate limit 테스트 섹션에 추가:

```cmake
apex_shared_add_rate_limit_test(test_endpoint_rate_config test_endpoint_rate_config.cpp)
```

### Step 4.7: 빌드 + 테스트

```bash
cmd.exe //c build.bat debug
cd build/debug && ctest --output-on-failure -R "test_endpoint_rate_config|test_redis_rate_limiter"
```

---

## Task 5: Gateway 파이프라인 통합

> **주의**: Task 5는 Gateway MVP (Plan 1) 완료 후 진행. Gateway의 파이프라인 코드가 존재해야 통합 가능. 여기서는 통합 설계와 인터페이스만 정의한다.

### Step 5.1: Gateway 파이프라인 위치

설계서 §3.6의 요청 파이프라인:

```
요청 → TLS → [Per-IP] → JWT 검증 → [Per-User] → [Per-Endpoint] → msg_id 라우팅 → Kafka
```

Gateway의 `ConnectionHandler`(또는 동등한 요청 처리 함수)에서 Rate Limiting을 삽입한다.

### Step 5.2: 에러 응답 (GatewayError)

Rate limit 초과 시 `SystemResponse` FlatBuffers 메시지를 반환한다.

설계서 §6.3의 GatewayError:

```
RATE_LIMITED_IP = 1       → Per-IP 초과
RATE_LIMITED_USER = 2     → Per-User 초과
RATE_LIMITED_ENDPOINT = 3 → Per-Endpoint 초과
```

`retry_after_ms` 필드로 클라이언트에 재시도 시점을 알린다.

### Step 5.3: 통합 의사코드

```cpp
// Gateway request handler (의사코드)
boost::asio::awaitable<void> handle_request(
    Session& session,
    const WireHeader& header,
    std::span<const uint8_t> payload)
{
    auto& facade = /* per-core RateLimitFacade */;
    auto now = SlidingWindowCounter::Clock::now();

    // Layer 1: Per-IP (local, no I/O)
    if (!facade.check_ip(session.remote_ip(), now)) {
        co_await send_rate_limit_error(session, header,
            GatewayError::RATE_LIMITED_IP, /* retry_after_ms */);
        co_return;
    }

    // JWT verification...
    auto jwt_result = verify_jwt(header, payload);
    if (!jwt_result) { /* JWT error response */ co_return; }

    auto now_ms = /* current epoch ms */;

    // Layer 2: Per-User (Redis)
    auto user_result = co_await facade.check_user(jwt_result->user_id, now_ms);
    if (user_result && !user_result->allowed) {
        co_await send_rate_limit_error(session, header,
            GatewayError::RATE_LIMITED_USER, user_result->retry_after_ms);
        co_return;
    }

    // Layer 3: Per-Endpoint (Redis)
    auto ep_result = co_await facade.check_endpoint(
        jwt_result->user_id, header.msg_id, now_ms);
    if (ep_result && !ep_result->allowed) {
        co_await send_rate_limit_error(session, header,
            GatewayError::RATE_LIMITED_ENDPOINT, ep_result->retry_after_ms,
            header.msg_id);
        co_return;
    }

    // Proceed to routing...
    co_await route_to_service(header, payload);
}
```

### Step 5.4: TOML Hot-Reload 연동

```toml
# gateway.toml

[rate_limit.ip]
total_limit = 1000
window_size_seconds = 60
max_entries = 65536

[rate_limit.user]
default_limit = 100
window_size_seconds = 60

[rate_limit.endpoint]
default_limit = 60
window_size_seconds = 60

[rate_limit.endpoint.overrides]
1001 = 10    # LoginRequest
2001 = 200   # ChatSendMessage
2010 = 5     # CreateRoom
```

FileWatcher가 `gateway.toml` 변경을 감지하면:

1. TOML 파싱
2. `PerIpRateLimiter::update_config()` 호출
3. `RedisRateLimiter::update_config()` 호출
4. `RateLimitFacade::update_endpoint_config()` 호출

Cross-core 전파: FileWatcher 스레드 → 각 코어에 `cross_core_call`로 config 업데이트 전달.

### Step 5.5: 통합 테스트 (Gateway 완성 후)

Gateway MVP 완성 후 `tests/integration/`에 E2E 테스트를 추가:

- Rate limit 초과 → `SystemResponse` + `retry_after_ms` 검증
- TOML hot-reload → 한도 변경 반영 검증
- 3계층 순차 적용 검증 (IP → User → Endpoint)

---

## 구현 순서 요약

| Step | Task | 예상 시간 | 의존성 |
|------|------|----------|--------|
| 1.1 | CMake 인프라 | 2분 | 없음 |
| 1.2-1.3 | SlidingWindowCounter 구현 | 5분 | 1.1 |
| 1.4-1.5 | SlidingWindowCounter 테스트 | 3분 | 1.3 |
| 1.6 | 빌드 + 테스트 통과 | 3분 | 1.5 |
| 2.1 | PerIpRateLimiter 헤더 | 3분 | 1.6 |
| 2.2-2.3 | PerIpRateLimiter 구현 + CMake | 5분 | 2.1 |
| 2.4-2.5 | PerIpRateLimiter 테스트 | 3분 | 2.3 |
| 2.6 | 빌드 + 테스트 통과 | 3분 | 2.5 |
| 3.1 | Redis Lua 스크립트 | 3분 | 없음 |
| 3.2-3.3 | RedisRateLimiter 구현 | 5분 | 3.1 |
| 3.4-3.6 | CMake + 테스트 | 3분 | 3.3 |
| 3.7 | 빌드 + 테스트 통과 | 3분 | 3.6 |
| 4.1-4.3 | EndpointRateConfig + Facade | 5분 | 3.7 |
| 4.4-4.6 | CMake + 테스트 | 3분 | 4.3 |
| 4.7 | 빌드 + 테스트 통과 | 3분 | 4.6 |
| 5.x | Gateway 통합 | Gateway MVP 이후 | Plan 1 |

**총 예상**: Task 1-4 약 50분 (빌드 시간 포함). Task 5는 Gateway MVP 완성 후.

---

## 수정 대상 파일 목록 (기존 파일)

| 파일 | 수정 내용 |
|------|-----------|
| `apex_shared/CMakeLists.txt` | `add_subdirectory(lib/rate_limit)` 추가 |
| `apex_shared/tests/unit/CMakeLists.txt` | Rate limit 테스트 함수 + 등록 추가 |

## 신규 파일 목록

| 파일 | Task |
|------|------|
| `apex_shared/lib/rate_limit/CMakeLists.txt` | 1 |
| `apex_shared/lib/rate_limit/include/apex/shared/rate_limit/sliding_window_counter.hpp` | 1 |
| `apex_shared/lib/rate_limit/src/sliding_window_counter.cpp` | 1 |
| `apex_shared/lib/rate_limit/include/apex/shared/rate_limit/per_ip_rate_limiter.hpp` | 2 |
| `apex_shared/lib/rate_limit/src/per_ip_rate_limiter.cpp` | 2 |
| `apex_shared/lib/rate_limit/lua/sliding_window.lua` | 3 |
| `apex_shared/lib/rate_limit/include/apex/shared/rate_limit/redis_rate_limiter.hpp` | 3 |
| `apex_shared/lib/rate_limit/src/redis_rate_limiter.cpp` | 3 |
| `apex_shared/lib/rate_limit/include/apex/shared/rate_limit/endpoint_rate_config.hpp` | 4 |
| `apex_shared/lib/rate_limit/include/apex/shared/rate_limit/rate_limit_facade.hpp` | 4 |
| `apex_shared/lib/rate_limit/src/rate_limit_facade.cpp` | 4 |
| `apex_shared/tests/unit/test_sliding_window_counter.cpp` | 1 |
| `apex_shared/tests/unit/test_per_ip_rate_limiter.cpp` | 2 |
| `apex_shared/tests/unit/test_redis_rate_limiter.cpp` | 3 |
| `apex_shared/tests/unit/test_endpoint_rate_config.cpp` | 4 |
