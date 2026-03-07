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
                            size_t timer_wheel_slots = 1024);

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
    void set_timeout_callback(TimeoutCallback cb);

    /// 모든 활성 세션에 대해 콜백 실행 (브로드캐스트 용도).
    void for_each(std::function<void(SessionPtr)> fn) const;

    [[nodiscard]] size_t session_count() const noexcept;
    [[nodiscard]] uint32_t core_id() const noexcept { return core_id_; }

private:
    void on_timer_expire(TimingWheel::EntryId entry_id);

    uint32_t core_id_;
    uint32_t heartbeat_timeout_ticks_;
    SessionId next_id_{1};

    std::unordered_map<SessionId, SessionPtr> sessions_;
    TimingWheel timer_wheel_;

    std::unordered_map<TimingWheel::EntryId, SessionId> timer_to_session_;
    std::unordered_map<SessionId, TimingWheel::EntryId> session_to_timer_;

    TimeoutCallback timeout_callback_;
};

} // namespace apex::core
