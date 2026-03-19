#pragma once

#include <apex/core/session.hpp>
#include <apex/core/slab_allocator.hpp>
#include <apex/core/timing_wheel.hpp>

#include <boost/asio/ip/tcp.hpp>

#include <boost/unordered/unordered_flat_map.hpp>

#include <cstdint>
#include <functional>

namespace apex::core
{

/// 코어별 세션 매니저.
/// NOT thread-safe — 단일 코어(io_context-per-core) 전용.
class SessionManager
{
  public:
    explicit SessionManager(uint32_t core_id, uint32_t heartbeat_timeout_ticks = 300, size_t timer_wheel_slots = 1024,
                            size_t recv_buf_capacity = 8192, size_t max_sessions_per_core = 1024);

    ~SessionManager();

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    [[nodiscard]] SessionPtr create_session(boost::asio::ip::tcp::socket socket);

    void remove_session(SessionId id);

    [[nodiscard]] SessionPtr find_session(SessionId id) const;

    void touch_session(SessionId id);

    void tick();

    /// 세션 제거 시 호출되는 콜백. 서비스에 on_session_closed 통지용.
    /// @note remove_session() 호출 시점에 세션은 아직 sessions_ 맵에 존재하지만,
    ///       콜백 반환 후 곧 제거된다.
    using RemoveCallback = std::function<void(SessionId)>;
    void set_remove_callback(RemoveCallback cb);

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
    [[nodiscard]] uint32_t core_id() const noexcept
    {
        return core_id_;
    }

  private:
    /// WARNING: Invokes timeout_callback_ which must NOT re-enter tick()
    /// or mutate timer/session maps — iterator invalidation hazard.
    void on_timer_expire(TimingWheel::EntryId entry_id);

    uint32_t core_id_;
    uint32_t heartbeat_timeout_ticks_;
    size_t recv_buf_capacity_;
    // uint64_t wraps after ~584 billion years at 1M sessions/sec — effectively no overflow
    SessionId next_id_{1};

    TypedSlabAllocator<Session> session_pool_;
    boost::unordered_flat_map<SessionId, SessionPtr> sessions_;
    TimingWheel timer_wheel_;

    boost::unordered_flat_map<TimingWheel::EntryId, SessionId> timer_to_session_;
    // I-07: session_to_timer_ map removed. Timer entry ID is now embedded in
    // Session::timer_entry_id_ (accessed via friend). This eliminates one of three
    // unordered_maps, reducing per-session memory overhead and lookup cost.

    TimeoutCallback timeout_callback_;
    RemoveCallback remove_callback_;

    // shrink_to_fit 주기 관리 (tick 단위)
    // 기본 tick_interval 100ms 기준: 600 ticks ≈ 60초
    static constexpr uint32_t kShrinkIntervalTicks = 600;
    uint32_t shrink_tick_counter_{0};
};

} // namespace apex::core
