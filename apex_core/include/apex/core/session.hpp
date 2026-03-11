#pragma once

#include <apex/core/frame_codec.hpp>
#include <apex/core/result.hpp>
#include <apex/core/ring_buffer.hpp>
#include <apex/core/timing_wheel.hpp>
#include <apex/core/wire_header.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/awaitable.hpp>

#include <cassert>
#include <cstdint>
#include <memory>

namespace apex::core {

/// 고유 세션 식별자 (코어별 단조 증가)
using SessionId = uint64_t;

/// 단일 클라이언트 연결을 나타내는 클래스.
/// SessionManager가 shared_ptr로 소유 (코루틴 안전성 보장).
///
/// 생명주기: Connected -> Active -> Closed
///
/// @warning Session is NOT thread-safe. All operations must be called from
/// the owning core's io_context thread (I-22). Use boost::asio::post() to
/// dispatch operations from other threads. state_ is intentionally non-atomic
/// because each Session is confined to a single core's strand.
///
/// @note enable_shared_from_this is reserved for future use (m-10):
/// coroutine self-capture pattern where the Session must prevent its own
/// destruction while an async operation is in flight.
class Session : public std::enable_shared_from_this<Session> {
public:
    enum class State : uint8_t {
        Connected,
        Active,
        // Closing 제거 (I-2): 미사용 상태였음. graceful shutdown은 Closed로 직접 전이.
        Closed,
    };

    /// @param recv_buf_capacity Receive buffer size. When used with Server,
    ///        must be >= Server::TMP_BUF_SIZE (validated in Server constructor).
    Session(SessionId id, boost::asio::ip::tcp::socket socket,
            uint32_t core_id, size_t recv_buf_capacity = 8192);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// 프레임 응답을 이 세션에 비동기 전송.
    /// @pre 호출자는 이 세션이 속한 io_context의 implicit strand에서 호출해야 한다.
    /// @pre payload 데이터는 co_await 완료 시점까지 유효해야 한다.
    ///      (코루틴 프레임이 복사를 보장하지 않음)
    /// @warning Only one async_send (or async_send_raw) operation may be
    /// in-flight per Session at any time. Concurrent writes to the same
    /// socket produce undefined behavior. The Server pipeline guarantees
    /// this by processing one frame at a time per session.
    [[nodiscard]] boost::asio::awaitable<Result<void>>
    async_send(const WireHeader& header, std::span<const uint8_t> payload);

    /// 미리 빌드된 로우 프레임 비동기 전송.
    [[nodiscard]] boost::asio::awaitable<Result<void>>
    async_send_raw(std::span<const uint8_t> data);

    /// 세션 그레이스풀 종료.
    void close();

    [[nodiscard]] SessionId id() const noexcept { return id_; }
    [[nodiscard]] uint32_t core_id() const noexcept { return core_id_; }
    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool is_open() const noexcept {
        return state_ == State::Connected || state_ == State::Active;
    }
    [[nodiscard]] boost::asio::ip::tcp::socket& socket() noexcept {
        return socket_;
    }
    [[nodiscard]] RingBuffer& recv_buffer() noexcept { return recv_buf_; }

private:
    // M-1: Assert valid state transition — Closed is a terminal state
    void set_state(State s) noexcept {
        assert(state_ != State::Closed && "Cannot transition from Closed state");
        state_ = s;
    }
    friend class SessionManager;

    SessionId id_;
    uint32_t core_id_;
    State state_{State::Connected};
    boost::asio::ip::tcp::socket socket_;
    RingBuffer recv_buf_;

    // I-07: Timer entry ID embedded in Session to eliminate session_to_timer_ map
    // in SessionManager. 0 = no timer (sentinel value, never issued by TimingWheel).
    TimingWheel::EntryId timer_entry_id_{0};
};

using SessionPtr = std::shared_ptr<Session>;

} // namespace apex::core
