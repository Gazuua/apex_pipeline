#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

namespace apex::core
{

/// 작업 핸들 — schedule()이 반환하는 불투명 식별자.
/// cancel() 호출 시 사용.
using TaskHandle = uint64_t;

/// 주기적 작업 스케줄러. per-core io_context에 steady_timer 기반으로 동작.
/// 각 작업은 지정된 인터벌마다 반복 실행된다.
/// Server shutdown 시 stop_all()로 전체 취소.
///
/// 주의: 단일 스레드(io_context 실행 스레드)에서만 사용해야 한다.
///       스레드 동기화 없음.
class PeriodicTaskScheduler
{
  public:
    explicit PeriodicTaskScheduler(boost::asio::io_context& io_ctx);
    ~PeriodicTaskScheduler();

    // 복사 금지 — 타이머 소유권 이전 불가
    PeriodicTaskScheduler(const PeriodicTaskScheduler&) = delete;
    PeriodicTaskScheduler& operator=(const PeriodicTaskScheduler&) = delete;

    /// 주기적 작업 등록. interval마다 task를 반복 실행한다.
    /// @return 취소에 사용할 TaskHandle. handle은 stop_all() 후 무효화된다.
    TaskHandle schedule(std::chrono::milliseconds interval, std::function<void()> task);

    /// 특정 작업 취소. 이미 취소됐거나 존재하지 않으면 no-op.
    void cancel(TaskHandle handle);

    /// 모든 작업을 취소하고 타이머를 해제한다.
    /// Server shutdown 시 io_context 정지 전에 호출해야 한다.
    void stop_all();

  private:
    struct TaskEntry
    {
        std::unique_ptr<boost::asio::steady_timer> timer;
        std::function<void()> task;
        std::chrono::milliseconds interval;
        bool cancelled = false;
    };

    /// handle의 타이머를 interval 후로 재설정한다.
    void schedule_next(TaskHandle handle);

    boost::asio::io_context& io_ctx_;
    std::unordered_map<TaskHandle, TaskEntry> tasks_;
    TaskHandle next_handle_{1};
};

} // namespace apex::core
