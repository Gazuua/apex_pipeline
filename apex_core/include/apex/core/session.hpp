// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/frame_codec.hpp>
#include <apex/core/result.hpp>
#include <apex/core/ring_buffer.hpp>
#include <apex/core/session_id.hpp>
#include <apex/core/timing_wheel.hpp>
#include <apex/core/wire_header.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/intrusive_ptr.hpp>
#include <fmt/format.h>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <vector>

namespace apex::core
{

/// Forward declarations for SlabAllocator integration (Tier 2 Task 2)
template <typename T> class TypedSlabAllocator;

/// 단일 클라이언트 연결을 나타내는 클래스.
/// SessionManager가 intrusive_ptr로 소유 (코루틴 안전성 보장).
///
/// 생명주기: Connected -> Active -> Closed
///
/// @warning Session is NOT thread-safe. All operations must be called from
/// the owning core's io_context thread (I-22). Use boost::asio::post() to
/// dispatch operations from other threads. state_ is intentionally non-atomic
/// because each Session is confined to a single core's strand.
///
/// @note intrusive_ptr with non-atomic refcount: per-core architecture guarantees
/// Session objects are accessed from a single core thread only. Cross-core
/// communication uses SessionId (enum class), never SessionPtr.
class Session
{
  public:
    enum class State : uint8_t
    {
        Connected,
        Active,
        // Closing 제거 (I-2): 미사용 상태였음. graceful shutdown은 Closed로 직접 전이.
        Closed,
    };

    /// @param recv_buf_capacity Receive buffer size. When used with Server,
    ///        must be >= Server::TMP_BUF_SIZE (validated in Server constructor).
    /// @param max_queue_depth  Per-session write queue depth limit (0 = unlimited).
    Session(SessionId id, boost::asio::ip::tcp::socket socket, uint32_t core_id, size_t recv_buf_capacity = 8192,
            size_t max_queue_depth = 256);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // --- intrusive_ptr refcount (atomic for safe cross-thread shutdown) ---
    // Per-core 설계지만 shutdown 시 io_context 소멸 경로에서 cross-thread release 발생 가능.
    // relaxed ordering으로 단일 코어 hot path 성능 영향 최소화.
    [[nodiscard]] uint32_t refcount() const noexcept
    {
        return refcount_.load(std::memory_order_relaxed);
    }

    friend void intrusive_ptr_add_ref(Session* s) noexcept
    {
        [[maybe_unused]] auto prev = s->refcount_.fetch_add(1, std::memory_order_relaxed);
        assert(prev < UINT32_MAX && "Session refcount overflow");
    }

    friend void intrusive_ptr_release(Session* s) noexcept;

    /// 프레임 응답을 이 세션에 비동기 전송.
    /// 내부적으로 write_pump를 경유하여 순서 보장 + 동시 쓰기 안전.
    /// @pre 호출자는 이 세션이 속한 io_context의 implicit strand에서 호출해야 한다.
    /// @note header + payload를 단일 버퍼로 직렬화 후 write queue에 적재.
    ///       write_pump가 실제 async_write를 수행하고 completion을 시그널한다.
    [[nodiscard]] boost::asio::awaitable<Result<void>> async_send(const WireHeader& header,
                                                                  std::span<const uint8_t> payload);

    /// 미리 빌드된 로우 프레임 비동기 전송.
    /// 내부적으로 write_pump를 경유하여 순서 보장 + 동시 쓰기 안전.
    [[nodiscard]] boost::asio::awaitable<Result<void>> async_send_raw(std::span<const uint8_t> data);

    // --- Write Queue API (v0.5) ---

    struct WriteRequest
    {
        std::vector<uint8_t> data;

        /// Completion signal for awaitable senders (async_send / async_send_raw).
        /// nullptr for fire-and-forget enqueue_write.
        /// Pattern: timer starts at steady_timer::time_point::max() (= infinite wait).
        /// write_pump cancels the timer after write completes to wake the waiter.
        /// The waiter stores write result in completion_result before co_await.
        std::shared_ptr<boost::asio::steady_timer> completion_timer;
        std::shared_ptr<Result<void>> completion_result;
    };

    /// 비동기 전송 큐에 적재 (동기, 즉시 반환).
    /// write pump가 미실행이면 기동한다.
    /// @return BufferFull if queue exceeds max_queue_depth_
    [[nodiscard]] Result<void> enqueue_write(std::vector<uint8_t> data);

    /// raw 데이터를 write queue에 적재.
    [[nodiscard]] Result<void> enqueue_write_raw(std::span<const uint8_t> data);

    /// 세션 그레이스풀 종료.
    void close() noexcept;

    [[nodiscard]] SessionId id() const noexcept
    {
        return id_;
    }
    [[nodiscard]] uint32_t core_id() const noexcept
    {
        return core_id_;
    }
    [[nodiscard]] State state() const noexcept
    {
        return state_;
    }
    [[nodiscard]] bool is_open() const noexcept
    {
        return state_ == State::Connected || state_ == State::Active;
    }
    [[nodiscard]] boost::asio::ip::tcp::socket& socket() noexcept
    {
        return socket_;
    }
    [[nodiscard]] RingBuffer& recv_buffer() noexcept
    {
        return recv_buf_;
    }

    /// 이 세션이 실제로 실행되는 core의 io_context executor를 설정한다.
    /// accept 시점에 socket executor(= acceptor의 io_context)와 실제 core의
    /// io_context가 다를 수 있으므로, timer/write_pump에서는 이 executor를 사용해야 한다.
    void set_core_executor(boost::asio::any_io_executor exe) noexcept
    {
        core_executor_ = std::move(exe);
    }

  private:
    // M-1: Assert valid state transition — Closed is a terminal state
    void set_state(State s) noexcept
    {
        assert(state_ != State::Closed && "Cannot transition from Closed state");
        state_ = s;
    }
    friend class SessionManager;

    std::atomic<uint32_t> refcount_{0};                // atomic for safe cross-thread shutdown
    TypedSlabAllocator<Session>* pool_owner_{nullptr}; // set by SessionManager when pool-allocated

    SessionId id_;
    uint32_t core_id_;
    State state_{State::Connected};
    boost::asio::ip::tcp::socket socket_;
    boost::asio::any_io_executor core_executor_; // 실제 core io_context의 executor (socket executor와 다를 수 있음)
    RingBuffer recv_buf_;

    // I-07: Timer entry ID embedded in Session to eliminate session_to_timer_ map
    // in SessionManager. 0 = no timer (sentinel value, never issued by TimingWheel).
    TimingWheel::EntryId timer_entry_id_{0};

    // v0.5: Per-session write queue
    std::deque<WriteRequest> write_queue_;
    bool pump_running_{false};
    size_t max_queue_depth_;

    boost::asio::awaitable<void> write_pump();

    /// Enqueue data with a completion timer and wait for write_pump to process it.
    /// Used internally by async_send / async_send_raw to route through write_pump.
    [[nodiscard]] boost::asio::awaitable<Result<void>> enqueue_and_await(std::vector<uint8_t> data);
};

using SessionPtr = boost::intrusive_ptr<Session>;

} // namespace apex::core
