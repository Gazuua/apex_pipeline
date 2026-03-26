// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/scoped_logger.hpp>
#include <apex/core/session.hpp>
#include <apex/core/slab_allocator.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

namespace apex::core
{

namespace
{
const ScopedLogger& s_logger()
{
    static const ScopedLogger instance{"Session", ScopedLogger::NO_CORE};
    return instance;
}
} // anonymous namespace

void intrusive_ptr_release(Session* s) noexcept
{
    auto prev = s->refcount_.fetch_sub(1, std::memory_order_acq_rel);
    assert(prev > 0 && "Session refcount underflow");
    if (prev == 1)
    {
        if (s->pool_owner_ != nullptr)
        {
            s->pool_owner_->destroy(s);
        }
        else
        {
            delete s; // NOLINT(cppcoreguidelines-owning-memory)
        }
    }
}

using boost::asio::awaitable;

Session::Session(SessionId id, std::unique_ptr<SocketBase> socket, uint32_t core_id, size_t recv_buf_capacity,
                 size_t max_queue_depth)
    : id_(id)
    , core_id_(core_id)
    , socket_(std::move(socket))
    , core_executor_(
          socket_->get_executor()) // 기본값: socket executor. accept_connection에서 core executor로 재설정됨.
    , recv_buf_(recv_buf_capacity)
    , max_queue_depth_(max_queue_depth)
{}

// M-2: Simplified — close() already checks Closed state and is idempotent
Session::~Session()
{
    s_logger().trace("~Session id={}", id_);
    close();
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
awaitable<Result<void>> Session::async_send(const WireHeader& header, std::span<const uint8_t> payload)
{
    if (!is_open())
        co_return error(ErrorCode::SessionClosed);

    // Serialize header + payload into a single buffer, then enqueue via write_pump.
    std::array<uint8_t, WireHeader::SIZE> hdr_buf{};
    header.serialize(hdr_buf);

    std::vector<uint8_t> frame;
    frame.reserve(WireHeader::SIZE + payload.size());
    frame.insert(frame.end(), hdr_buf.begin(), hdr_buf.end());
    frame.insert(frame.end(), payload.begin(), payload.end());

    co_return co_await enqueue_and_await(std::move(frame));
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
awaitable<Result<void>> Session::async_send_raw(std::span<const uint8_t> data)
{
    if (!is_open())
        co_return error(ErrorCode::SessionClosed);

    std::vector<uint8_t> copy(data.begin(), data.end());
    co_return co_await enqueue_and_await(std::move(copy));
}

void Session::close() noexcept
{
    auto prev = state_.load(std::memory_order_relaxed);
    if (prev == State::Closed)
        return;
    // Bypass set_state() which asserts on Closed->Closed transition
    // (already guarded by early return above)
    state_.store(State::Closed, std::memory_order_relaxed);
    s_logger().debug("close id={} prev_state={}", id_, static_cast<int>(prev));

    socket_->close();
}

// --- Write Queue (v0.5) ---

Result<void> Session::enqueue_write(std::vector<uint8_t> data)
{
    if (!is_open())
        return error(ErrorCode::SessionClosed);
    if (write_queue_.size() >= max_queue_depth_)
    {
        s_logger().warn("enqueue_write id={} queue full depth={}/{}", id_, write_queue_.size(), max_queue_depth_);
        return error(ErrorCode::BufferFull);
    }
    s_logger().trace("enqueue_write id={} size={} depth={}", id_, data.size(), write_queue_.size() + 1);
    write_queue_.push_back(WriteRequest{std::move(data), nullptr, nullptr});
    if (!pump_running_)
    {
        pump_running_ = true;
        // UAF 방어: co_spawn된 write_pump 코루틴이 실행되는 동안
        // Session이 소멸되지 않도록 intrusive_ptr로 refcount 증가.
        // 람다가 self를 캡처하여 코루틴 완료까지 생존 보장.
        // core_executor_ 사용: socket executor와 core executor가 다를 수 있음
        // (non-reuseport 경로에서 acceptor가 core 0에 있으면 socket executor는
        //  core 0이지만 session은 다른 core에서 실행). write_pump가 session과
        //  동일한 core에서 실행되어야 implicit strand invariant이 유지됨.
        SessionPtr self(this);
        boost::asio::co_spawn(core_executor_, [self]() { return self->write_pump(); }, boost::asio::detached);
    }
    return ok();
}

Result<void> Session::enqueue_write_raw(std::span<const uint8_t> data)
{
    std::vector<uint8_t> copy(data.begin(), data.end());
    return enqueue_write(std::move(copy));
}

// Thread-safety invariant (implicit strand):
// write_pump runs on the owning core's single-threaded io_context.
// Between co_await points, no other coroutine can interleave.
// The critical sequence "while-condition check -> pump_running_ = false"
// executes without any co_await, so enqueue_write() cannot observe
// pump_running_ == true with an empty queue simultaneously.
// If enqueue_write() is called during co_await async_write, it sees
// pump_running_ == true and simply appends to write_queue_, which
// the resumed write_pump will process in the next iteration.
// WARNING: Do NOT add co_await between the while-condition check and
// pump_running_ = false, as it would break this invariant.
awaitable<void> Session::write_pump()
{
    while (!write_queue_.empty() && is_open())
    {
        // Move front out so we can pop before signaling completion.
        // This ensures queue state is consistent before any completion callback.
        auto req = std::move(write_queue_.front());
        write_queue_.pop_front();

        auto [ec, bytes_written] = co_await socket_->async_write(boost::asio::buffer(req.data));
        (void)bytes_written;

        if (ec)
        {
            s_logger().warn("write_pump id={} write error: {}", id_, ec.message());
            // Signal error to the awaiting coroutine if present
            if (req.completion_timer)
            {
                *req.completion_result = error(ErrorCode::SendFailed);
                req.completion_timer->cancel();
            }

            // Signal error to all remaining queued requests with completion
            for (auto& pending : write_queue_)
            {
                if (pending.completion_timer)
                {
                    *pending.completion_result = error(ErrorCode::SendFailed);
                    pending.completion_timer->cancel();
                }
            }

            s_logger().debug("write_pump id={} draining {} pending requests", id_, write_queue_.size());
            write_queue_.clear(); // 에러 시 잔여 큐 정리
            close();
            break;
        }

        // Signal success to the awaiting coroutine if present
        if (req.completion_timer)
        {
            *req.completion_result = ok();
            req.completion_timer->cancel();
        }
    }
    pump_running_ = false;
}

awaitable<Result<void>> Session::enqueue_and_await(std::vector<uint8_t> data)
{
    // Create a timer as a completion signal. expires_at(max) = infinite wait.
    // write_pump will cancel() it after processing, which resumes this coroutine.
    // core_executor_ 사용: socket executor는 acceptor의 io_context를 가리킬 수 있어
    // timer가 다른 io_context의 timer service에 등록되면 shutdown 시 UAF 발생.
    auto timer =
        std::make_shared<boost::asio::steady_timer>(core_executor_, boost::asio::steady_timer::time_point::max());
    auto result = std::make_shared<Result<void>>(error(ErrorCode::SendFailed));

    // Check queue depth (same as enqueue_write)
    if (write_queue_.size() >= max_queue_depth_)
    {
        s_logger().warn("enqueue_and_await id={} queue full depth={}/{}", id_, write_queue_.size(), max_queue_depth_);
        co_return error(ErrorCode::BufferFull);
    }

    write_queue_.push_back(WriteRequest{std::move(data), timer, result});

    if (!pump_running_)
    {
        pump_running_ = true;
        SessionPtr self(this);
        boost::asio::co_spawn(core_executor_, [self]() { return self->write_pump(); }, boost::asio::detached);
    }

    // Wait for write_pump to signal completion via timer cancel.
    // cancel() causes co_await to return with operation_aborted, which is expected.
    auto [ec] = co_await timer->async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
    (void)ec; // operation_aborted is the expected "success" signal

    co_return *result;
}

} // namespace apex::core
