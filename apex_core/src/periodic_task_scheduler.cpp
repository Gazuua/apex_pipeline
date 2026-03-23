// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/periodic_task_scheduler.hpp>
#include <apex/core/scoped_logger.hpp>

namespace apex::core
{

namespace
{
ScopedLogger s_logger{"PeriodicTaskScheduler", ScopedLogger::NO_CORE};
} // anonymous namespace

PeriodicTaskScheduler::PeriodicTaskScheduler(boost::asio::io_context& io_ctx)
    : io_ctx_(io_ctx)
{}

PeriodicTaskScheduler::~PeriodicTaskScheduler()
{
    stop_all();
}

TaskHandle PeriodicTaskScheduler::schedule(std::chrono::milliseconds interval, std::function<void()> task)
{
    auto handle = next_handle_++;
    auto& entry = tasks_[handle];
    entry.timer = std::make_unique<boost::asio::steady_timer>(io_ctx_);
    entry.task = std::move(task);
    entry.interval = interval;
    entry.cancelled = false;

    s_logger.info("schedule handle={} interval={}ms", handle, interval.count());
    schedule_next(handle);
    return handle;
}

void PeriodicTaskScheduler::cancel(TaskHandle handle)
{
    auto it = tasks_.find(handle);
    if (it != tasks_.end())
    {
        s_logger.info("cancel handle={}", handle);
        it->second.cancelled = true;
        it->second.timer->cancel();
        tasks_.erase(it);
    }
}

void PeriodicTaskScheduler::stop_all()
{
    s_logger.info("stop_all count={}", tasks_.size());
    for (auto& [handle, entry] : tasks_)
    {
        entry.cancelled = true;
        entry.timer->cancel();
    }
    tasks_.clear();
}

void PeriodicTaskScheduler::schedule_next(TaskHandle handle)
{
    auto it = tasks_.find(handle);
    if (it == tasks_.end())
        return;

    auto& entry = it->second;
    entry.timer->expires_after(entry.interval);
    // handle을 값으로 캡처 — this는 생명주기 내에서만 유효
    entry.timer->async_wait([this, handle](boost::system::error_code ec) {
        // ec가 설정된 경우: 취소됐거나 오류 발생 — 종료
        if (ec)
            return;

        auto it = tasks_.find(handle);
        if (it == tasks_.end() || it->second.cancelled)
            return;

        // 작업 실행 후 다음 주기 예약
        s_logger.trace("execute handle={}", handle);
        it->second.task();
        schedule_next(handle);
    });
}

} // namespace apex::core
