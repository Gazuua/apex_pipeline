// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/session_manager.hpp>

namespace apex::core
{

SessionManager::SessionManager(uint32_t core_id, uint32_t heartbeat_timeout_ticks, size_t timer_wheel_slots,
                               size_t recv_buf_capacity, size_t max_queue_depth, size_t max_sessions_per_core)
    : core_id_(core_id)
    , heartbeat_timeout_ticks_(heartbeat_timeout_ticks)
    , recv_buf_capacity_(recv_buf_capacity)
    , max_queue_depth_(max_queue_depth)
    , session_pool_(max_sessions_per_core)
    , timer_wheel_(timer_wheel_slots, [this](TimingWheel::EntryId entry_id) { on_timer_expire(entry_id); })
{}

SessionManager::~SessionManager() = default;

SessionPtr SessionManager::create_session(boost::asio::ip::tcp::socket socket)
{
    SessionId id = make_session_id(next_id_++);
    Session* raw = session_pool_.construct(id, std::move(socket), core_id_, recv_buf_capacity_, max_queue_depth_);
    if (raw)
    {
        raw->pool_owner_ = &session_pool_;
    }
    else
    {
        // SlabAllocator exhausted → heap fallback
        raw = new Session(id, std::move(socket), core_id_, recv_buf_capacity_, max_queue_depth_);
        logger_.warn("session SlabAllocator exhausted, heap fallback");
    }
    SessionPtr session(raw);
    session->set_state(Session::State::Active);

    sessions_[id] = session;

    if (heartbeat_timeout_ticks_ > 0)
    {
        auto timer_id = timer_wheel_.schedule(heartbeat_timeout_ticks_);
        timer_to_session_[timer_id] = id;
        session->timer_entry_id_ = timer_id; // I-07: embedded in Session
    }

    logger_.info("session created id={} total={}", id, sessions_.size());
    return session;
}

void SessionManager::remove_session(SessionId id)
{
    auto it = sessions_.find(id);
    if (it == sessions_.end())
        return;

    logger_.debug("session removing id={}", id);

    auto& session = it->second;
    session->close();

    // I-07: Use embedded timer_entry_id_ instead of session_to_timer_ map
    if (session->timer_entry_id_ != 0)
    {
        timer_wheel_.cancel(session->timer_entry_id_);
        timer_to_session_.erase(session->timer_entry_id_);
        session->timer_entry_id_ = 0;
    }

    // 서비스에 세션 종료 통지 (sessions_ erase 전에 호출)
    if (remove_callback_)
    {
        remove_callback_(id);
    }

    sessions_.erase(it);
}

SessionPtr SessionManager::find_session(SessionId id) const
{
    auto it = sessions_.find(id);
    return (it != sessions_.end()) ? it->second : nullptr;
}

void SessionManager::touch_session(SessionId id)
{
    if (heartbeat_timeout_ticks_ == 0)
        return;

    // I-07: Lookup session to access embedded timer_entry_id_
    auto session_it = sessions_.find(id);
    if (session_it == sessions_.end())
        return;

    auto& session = session_it->second;
    if (session->timer_entry_id_ == 0)
        return;

    // cancel+schedule 대신 reschedule로 Entry 재사용 — new/delete 제거
    timer_wheel_.reschedule(session->timer_entry_id_, heartbeat_timeout_ticks_);
}

void SessionManager::tick()
{
    timer_wheel_.tick();

    // 주기적으로 모든 세션의 수신 버퍼 linearization 메모리를 정리
    if (++shrink_tick_counter_ >= kShrinkIntervalTicks)
    {
        shrink_tick_counter_ = 0;
        for (auto& [id, session] : sessions_)
        {
            session->recv_buffer().shrink_to_fit();
        }
    }
}

void SessionManager::set_remove_callback(RemoveCallback cb)
{
    remove_callback_ = std::move(cb);
}

void SessionManager::set_timeout_callback(TimeoutCallback cb)
{
    timeout_callback_ = std::move(cb);
}

void SessionManager::for_each(std::function<void(SessionPtr)> fn) const
{
    for (const auto& [id, session] : sessions_)
    {
        fn(session);
    }
}

size_t SessionManager::session_count() const noexcept
{
    return sessions_.size();
}

void SessionManager::on_timer_expire(TimingWheel::EntryId entry_id)
{
    auto it = timer_to_session_.find(entry_id);
    if (it == timer_to_session_.end())
        return;

    SessionId session_id = it->second;
    timer_to_session_.erase(it);

    auto session_it = sessions_.find(session_id);
    if (session_it == sessions_.end())
        return;

    logger_.info("session timeout id={}", session_id);

    auto session = session_it->second;
    session->timer_entry_id_ = 0; // I-07: clear embedded timer ID

    // 서비스에 세션 종료 통지 (erase 전에 호출 — 서비스가 세션 조회 가능)
    if (remove_callback_)
    {
        remove_callback_(session_id);
    }

    sessions_.erase(session_it); // timeout_callback 전에 erase (댕글링 이터레이터 방지)

    // 콜백에서 세션에 마지막 작업(로깅 등)을 수행할 수 있도록
    // close()는 콜백 이후에 호출한다.
    // timeout_callback 내에서 touch_session(session_id)이 호출되더라도,
    // timer_entry_id_가 이미 0으로 초기화되었으므로 안전하게 no-op 처리됨.
    // S-NET-4: close() is guaranteed even if timeout_callback_ throws,
    // since ~Session() calls close() and session (shared_ptr) is still alive.
    if (timeout_callback_)
    {
        try
        {
            timeout_callback_(session);
        }
        catch (const std::exception& e)
        {
            logger_.error("timeout callback exception for session {}: {}", session_id, e.what());
        }
        catch (...)
        {
            logger_.error("timeout callback unknown exception for session {}", session_id);
        }
    }

    session->close();
}

} // namespace apex::core
