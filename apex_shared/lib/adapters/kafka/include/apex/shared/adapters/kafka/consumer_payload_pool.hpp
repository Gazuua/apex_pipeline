#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace apex::shared::adapters::kafka {

/// Kafka consumer 스레드 전용 payload 메모리 풀.
///
/// 고정 크기 버퍼를 사전 할당하고 재사용하여 consumer 콜백에서의
/// 매번 힙 할당을 피한다. Consumer 스레드에서 acquire(), 코어 스레드에서
/// shared_ptr custom deleter를 통해 자동 반환 — thread-safe.
///
/// 풀 고갈 시 통상 힙 할당으로 fallback (메시지 유실 방지).
///
/// Usage:
///   ConsumerPayloadPool pool(64, 4096, 8192);
///   auto buf = pool.acquire(payload_span);
///   // buf is shared_ptr<PayloadBuffer> — 코어 스레드로 이동 가능
///   // shared_ptr 소멸 시 자동으로 풀에 반환
class ConsumerPayloadPool {
public:
    /// 풀에서 관리하는 payload 버퍼.
    /// data 벡터에 실제 payload가 복사된다.
    struct PayloadBuffer {
        std::vector<uint8_t> data;

        /// span 접근 (편의)
        [[nodiscard]] std::span<const uint8_t> span() const noexcept {
            return {data.data(), data.size()};
        }
    };

    using PayloadPtr = std::shared_ptr<PayloadBuffer>;

    /// @param initial_count  사전 할당할 버퍼 수 (기본 64)
    /// @param buffer_size    각 버퍼의 초기 reserve 크기 (기본 4096 bytes)
    /// @param max_count      풀 최대 크기. 0이면 무제한 (기본 4096)
    explicit ConsumerPayloadPool(
        size_t initial_count = 64,
        size_t buffer_size = 4096,
        size_t max_count = 4096);

    ~ConsumerPayloadPool() = default;

    // Non-copyable, non-movable (mutex 포함)
    ConsumerPayloadPool(const ConsumerPayloadPool&) = delete;
    ConsumerPayloadPool& operator=(const ConsumerPayloadPool&) = delete;
    ConsumerPayloadPool(ConsumerPayloadPool&&) = delete;
    ConsumerPayloadPool& operator=(ConsumerPayloadPool&&) = delete;

    /// Consumer 스레드에서 호출. payload 데이터를 풀 버퍼에 복사.
    /// 풀 고갈 시 힙 할당 fallback. Thread-safe.
    [[nodiscard]] PayloadPtr acquire(std::span<const uint8_t> payload);

    // --- 메트릭 (Thread-safe) ---

    /// 현재 풀에 대기 중인 (미사용) 버퍼 수.
    [[nodiscard]] size_t free_count() const noexcept;

    /// 현재 사용 중인 (acquire 후 미반환) 버퍼 수.
    [[nodiscard]] size_t in_use_count() const noexcept;

    /// 총 생성된 버퍼 수 (풀 + fallback).
    [[nodiscard]] size_t total_created() const noexcept;

    /// 풀 고갈로 힙 할당 fallback한 횟수.
    [[nodiscard]] uint64_t fallback_alloc_count() const noexcept;

    /// acquire 호출 총 횟수.
    [[nodiscard]] uint64_t acquire_count() const noexcept;

    /// 동시 사용 중인 버퍼의 최대치 (high water mark).
    [[nodiscard]] size_t peak_in_use() const noexcept;

private:
    /// 버퍼를 풀로 반환. shared_ptr custom deleter에서 호출.
    void release(PayloadBuffer* buf);

    /// 새 버퍼 생성 (풀 성장 또는 fallback용).
    std::unique_ptr<PayloadBuffer> create_buffer() const;

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<PayloadBuffer>> free_list_;

    size_t buffer_size_;       ///< 각 버퍼의 초기 reserve 크기
    size_t max_count_;         ///< 풀 최대 크기 (0=무제한)
    size_t total_created_{0};  ///< 총 생성된 버퍼 수
    size_t in_use_{0};         ///< 현재 사용 중인 버퍼 수
    size_t peak_in_use_{0};    ///< 최대 동시 사용 수
    uint64_t acquire_count_{0};
    uint64_t fallback_alloc_count_{0};
};

} // namespace apex::shared::adapters::kafka
