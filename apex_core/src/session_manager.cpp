#include <apex/core/session_manager.hpp>

namespace apex::core {

SessionManager::SessionManager(uint32_t core_id,
                               uint32_t heartbeat_timeout_ticks,
                               size_t timer_wheel_slots,
                               size_t recv_buf_capacity)
    : core_id_(core_id)
    , heartbeat_timeout_ticks_(heartbeat_timeout_ticks)
    , recv_buf_capacity_(recv_buf_capacity)
    , timer_wheel_(timer_wheel_slots, [this](TimingWheel::EntryId entry_id) {
          on_timer_expire(entry_id);
      })
{
}

SessionManager::~SessionManager() = default;

SessionPtr SessionManager::create_session(
    boost::asio::ip::tcp::socket socket)
{
    SessionId id = next_id_++;
    auto session = std::make_shared<Session>(
        id, std::move(socket), core_id_, recv_buf_capacity_);
    session->set_state(Session::State::Active);

    sessions_[id] = session;

    if (heartbeat_timeout_ticks_ > 0) {
        auto timer_id = timer_wheel_.schedule(heartbeat_timeout_ticks_);
        timer_to_session_[timer_id] = id;
        session_to_timer_[id] = timer_id;
    }

    return session;
}

void SessionManager::remove_session(SessionId id) {
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;

    auto& session = it->second;
    session->close();

    auto timer_it = session_to_timer_.find(id);
    if (timer_it != session_to_timer_.end()) {
        timer_wheel_.cancel(timer_it->second);
        timer_to_session_.erase(timer_it->second);
        session_to_timer_.erase(timer_it);
    }

    sessions_.erase(it);
}

SessionPtr SessionManager::find_session(SessionId id) const {
    auto it = sessions_.find(id);
    return (it != sessions_.end()) ? it->second : nullptr;
}

void SessionManager::touch_session(SessionId id) {
    if (heartbeat_timeout_ticks_ == 0) return;

    auto timer_it = session_to_timer_.find(id);
    if (timer_it == session_to_timer_.end()) return;

    // cancel+schedule 대신 reschedule로 Entry 재사용 — new/delete 제거
    timer_wheel_.reschedule(timer_it->second, heartbeat_timeout_ticks_);
}

void SessionManager::tick() {
    timer_wheel_.tick();
}

void SessionManager::set_timeout_callback(TimeoutCallback cb) {
    timeout_callback_ = std::move(cb);
}

void SessionManager::for_each(std::function<void(SessionPtr)> fn) const {
    for (const auto& [id, session] : sessions_) {
        fn(session);
    }
}

size_t SessionManager::session_count() const noexcept {
    return sessions_.size();
}

void SessionManager::on_timer_expire(TimingWheel::EntryId entry_id) {
    auto it = timer_to_session_.find(entry_id);
    if (it == timer_to_session_.end()) return;

    SessionId session_id = it->second;
    timer_to_session_.erase(it);
    session_to_timer_.erase(session_id);

    auto session_it = sessions_.find(session_id);
    if (session_it == sessions_.end()) return;

    auto session = session_it->second;
    sessions_.erase(session_it);  // 콜백 전에 erase (댕글링 이터레이터 방지)

    // 콜백에서 세션에 마지막 작업(로깅 등)을 수행할 수 있도록
    // close()는 콜백 이후에 호출한다.
    // timeout_callback 내에서 touch_session(session_id)이 호출되더라도,
    // session_to_timer_에서 이미 erase되었으므로 안전하게 no-op 처리됨.
    // S-NET-4: close() is guaranteed even if timeout_callback_ throws,
    // since ~Session() calls close() and session (shared_ptr) is still alive.
    if (timeout_callback_) {
        try {
            timeout_callback_(session);
        } catch (...) {
            // Ensure session is closed even if callback throws
            session->close();
            throw;
        }
    }

    session->close();
}

} // namespace apex::core
