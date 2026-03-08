#pragma once

#include <apex/core/session.hpp>
#include <apex/core/timing_wheel.hpp>

#include <boost/asio/ip/tcp.hpp>

#include <cstdint>
#include <functional>
#include <unordered_map>

namespace apex::core {

/// 코어별 세션 매니저.
/// NOT thread-safe — 단일 코어(io_context-per-core) 전용.
class SessionManager {
public:
    explicit SessionManager(uint32_t core_id,
                            uint32_t heartbeat_timeout_ticks = 300,
                            size_t timer_wheel_slots = 1024,
                            size_t recv_buf_capacity = 8192);

    ~SessionManager();

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    [[nodiscard]] SessionPtr create_session(
        boost::asio::ip::tcp::socket socket);

    void remove_session(SessionId id);

    [[nodiscard]] SessionPtr find_session(SessionId id) const;

    void touch_session(SessionId id);

    void tick();

    using TimeoutCallback = std::function<void(SessionPtr)>;

    /// Set the callback invoked when a session times out (heartbeat expiry).
    ///
    /// @note When the callback is invoked, the session has already been removed
    /// from the sessions_ map but is NOT yet closed. The session will be closed
    /// immediately after the callback returns. Do NOT call find_session() for
    /// this session from within the callback — it will return nullptr.
    ///
    /// @warning The timeout callback must NOT call tick() or any method that
    /// modifies the timer/session maps. Doing so will invalidate internal
    /// iterators and cause undefined behavior.
    void set_timeout_callback(TimeoutCallback cb);

    /// Execute callback for all active sessions (e.g., broadcast).
    /// @warning Do NOT modify the SessionManager (add/remove sessions) from
    /// within the callback — this would invalidate the internal map iterator.
    void for_each(std::function<void(SessionPtr)> fn) const;

    [[nodiscard]] size_t session_count() const noexcept;
    [[nodiscard]] uint32_t core_id() const noexcept { return core_id_; }

private:
    /// WARNING: Invokes timeout_callback_ which must NOT re-enter tick()
    /// or mutate timer/session maps — iterator invalidation hazard.
    void on_timer_expire(TimingWheel::EntryId entry_id);

    uint32_t core_id_;
    uint32_t heartbeat_timeout_ticks_;
    size_t recv_buf_capacity_;
    // uint64_t wraps after ~584 billion years at 1M sessions/sec — effectively no overflow
    SessionId next_id_{1};

    std::unordered_map<SessionId, SessionPtr> sessions_;
    TimingWheel timer_wheel_;

    std::unordered_map<TimingWheel::EntryId, SessionId> timer_to_session_;
    std::unordered_map<SessionId, TimingWheel::EntryId> session_to_timer_;

    TimeoutCallback timeout_callback_;
};

} // namespace apex::core
