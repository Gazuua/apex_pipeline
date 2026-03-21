// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/arena_allocator.hpp>
#include <apex/core/bump_allocator.hpp>
#include <apex/core/configure_context.hpp>
#include <apex/core/error_sender.hpp>
#include <apex/core/kafka_message_meta.hpp>
#include <apex/core/message_dispatcher.hpp>
#include <apex/core/result.hpp>
#include <apex/core/session.hpp>
#include <apex/core/wire_context.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace apex::core
{

/// 비템플릿 서비스 인터페이스. Server가 서비스를 소유하기 위해 사용.
class ServiceBaseInterface
{
  public:
    virtual ~ServiceBaseInterface() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual bool started() const noexcept = 0;

    /// Server가 공유 디스패처를 바인딩할 때 호출.
    virtual void bind_dispatcher(MessageDispatcher& external) = 0;

    // ── 라이프사이클 훅 ─────────────────────────────────────────────────
    // Server::run()이 오케스트레이션 단계별로 internal_* 를 호출한다.
    // internal_* 는 프레임워크 전처리 후 on_* 를 호출한다.

    /// Server가 호출하는 진입점. 프레임워크 전처리 + on_configure 호출.
    /// ServiceBase<Derived>가 오버라이드하여 per_core_ 바인딩 등 수행.
    virtual void internal_configure(ConfigureContext& ctx)
    {
        on_configure(ctx);
    }

    /// [D7] Server가 internal_configure 전에 호출하여 io_context를 주입.
    /// ConfigureContext에 io_context를 노출하지 않고 spawn()을 사용 가능하게 함.
    virtual void bind_io_context(boost::asio::io_context&) {}

    /// Server가 호출하는 진입점. 프레임워크 전처리 + on_wire 호출.
    virtual void internal_wire(WireContext& ctx)
    {
        on_wire(ctx);
    }

    /// Phase 1: 어댑터/설정 접근 단계. 다른 서비스에 접근 불가.
    virtual void on_configure(ConfigureContext&) {}

    /// Phase 2: 서비스 간 와이어링 단계. 다른 서비스 lookup 가능.
    virtual void on_wire(WireContext&) {}

    /// 세션 종료 시 서비스에 통지. 리소스 정리용.
    virtual void on_session_closed(SessionId) {}

    // ── D2: Kafka auto-wiring support ──────────────────────────────────
    using KafkaHandler =
        std::function<boost::asio::awaitable<Result<void>>(KafkaMessageMeta, uint32_t, std::span<const uint8_t>)>;
    using KafkaHandlerMap = boost::unordered_flat_map<uint32_t, KafkaHandler>;

    /// Kafka 핸들러 등록 여부. KafkaAdapter auto-wiring에서 사용.
    [[nodiscard]] virtual bool has_kafka_handlers() const noexcept
    {
        return false;
    }

    /// Kafka 핸들러 맵 접근. has_kafka_handlers() == true일 때만 유효.
    [[nodiscard]] virtual const KafkaHandlerMap& kafka_handler_map() const noexcept
    {
        static const KafkaHandlerMap empty;
        return empty;
    }

    // ── D7: Outstanding coroutine tracking ─────────────────────────────

    /// [D7] Outstanding 코루틴 수. shutdown 대기용.
    [[nodiscard]] virtual uint32_t outstanding_coroutines() const noexcept
    {
        return 0;
    }
};

/// CRTP base class for defining services.
///
/// Usage (FlatBuffers typed handler via route<T>):
///   class EchoService : public ServiceBase<EchoService> {
///   public:
///       EchoService() : ServiceBase("echo") {}
///       void on_start() override {
///           route<EchoRequest>(0x0001, &EchoService::on_echo);
///       }
///       awaitable<Result<void>> on_echo(SessionPtr session, uint32_t msg_id,
///                               const EchoRequest* req) {
///           co_return apex::core::ok();
///       }
///   };
template <typename Derived> class ServiceBase : public ServiceBaseInterface
{
  public:
    explicit ServiceBase(std::string name)
        : name_(std::move(name))
    {}

    ServiceBase(const ServiceBase&) = delete;
    ServiceBase& operator=(const ServiceBase&) = delete;

    virtual void on_start() {}
    virtual void on_stop() {}

    void start() override
    {
        static_cast<Derived*>(this)->on_start();
        started_ = true;
    }

    void stop() override
    {
        started_ = false;
        // I-02: Guard against nullptr dispatcher (e.g., stop() called before bind_dispatcher())
        if (dispatcher_)
        {
            for (auto id : registered_msg_ids_)
            {
                dispatcher_->unregister_handler(id);
            }
        }
        registered_msg_ids_.clear();
        on_stop();
    }

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return name_;
    }
    [[nodiscard]] bool started() const noexcept override
    {
        return started_;
    }
    [[nodiscard]] MessageDispatcher& dispatcher() noexcept
    {
        return *dispatcher_;
    }
    [[nodiscard]] const MessageDispatcher& dispatcher() const noexcept
    {
        return *dispatcher_;
    }

    void bind_dispatcher(MessageDispatcher& external) override
    {
        assert(!started_ && "bind_dispatcher must be called before start()");
        dispatcher_ = &external;
        owned_dispatcher_.reset(); // standalone dispatcher 해제
    }

    // ── 프레임워크 내부 메서드 (Server가 라이프사이클 오케스트레이션 시 호출) ──

    /// Phase 1: per_core_ 바인딩 + on_configure 호출.
    /// @note Server::run() 내부에서만 호출. 서비스 코드가 직접 호출하지 않는다.
    void internal_configure(ConfigureContext& ctx) override
    {
        per_core_ = &ctx.per_core_state;
        // io_ctx_는 bind_io_context()에서 이미 바인딩됨
        static_cast<Derived*>(this)->on_configure(ctx);
    }

    /// [D7] io_context 바인딩 — Server가 internal_configure 전에 호출.
    void bind_io_context(boost::asio::io_context& io) override
    {
        io_ctx_ = &io;
    }

    /// Phase 2: on_wire 호출.
    /// @note Server::run() 내부에서만 호출. 서비스 코드가 직접 호출하지 않는다.
    void internal_wire(WireContext& ctx) override
    {
        static_cast<Derived*>(this)->on_wire(ctx);
    }

  protected:
    /// @warning 핸들러 람다가 this(Derived*) raw pointer를 캡처함.
    /// 서비스 수명이 디스패처 수명보다 길거나 같아야 한다.
    /// Server 사용 시 자동 보장됨 (서비스와 디스패처를 동시 소유).

    /// 로우 핸들러 등록 (코루틴, Result<void> 반환).
    void handle(uint32_t msg_id,
                boost::asio::awaitable<Result<void>> (Derived::*method)(SessionPtr, uint32_t, std::span<const uint8_t>))
    {
        auto* self = static_cast<Derived*>(this);
        dispatcher_->register_handler(
            msg_id,
            [self, method](SessionPtr session, uint32_t id, std::span<const uint8_t> payload)
                -> boost::asio::awaitable<Result<void>> { co_return co_await (self->*method)(session, id, payload); });
        registered_msg_ids_.insert(msg_id);
    }

    /// FlatBuffers 타입 핸들러 등록 (코루틴, Result<void> 반환).
    /// @note FlatBuffers 메시지 포인터(const T*)는 co_await 시점까지만 유효합니다.
    ///       핸들러 내에서 async_send 등 co_await 이후에 메시지 필드에 접근하면
    ///       댕글링 참조가 발생합니다. 필요한 데이터는 co_await 전에 로컬 변수에 복사하세요.
    /// @note FlatBuffers 검증 실패 시 클라이언트에 error frame을 자동 전송하고
    ///       ok()를 반환합니다 (세션이 유효한 경우).
    template <typename FbsType>
    void route(uint32_t msg_id,
               boost::asio::awaitable<Result<void>> (Derived::*method)(SessionPtr, uint32_t, const FbsType*))
    {
        auto* self = static_cast<Derived*>(this);
        dispatcher_->register_handler(
            msg_id,
            [self, method](SessionPtr session, uint32_t id,
                           std::span<const uint8_t> payload) -> boost::asio::awaitable<Result<void>> {
                flatbuffers::Verifier verifier(payload.data(), payload.size());
                if (!verifier.VerifyBuffer<FbsType>())
                {
                    spdlog::warn("[ServiceBase] FlatBuffers verify failed (msg_id={}, session={})", id,
                                 session ? session->id() : make_session_id(0));
                    // 자동 error frame 전송 후 ok() 반환
                    if (session)
                    {
                        auto frame = ErrorSender::build_error_frame(id, ErrorCode::FlatBuffersVerifyFailed);
                        (void)session->enqueue_write(std::move(frame));
                    }
                    co_return apex::core::ok();
                }
                auto* msg = flatbuffers::GetRoot<FbsType>(payload.data());
                co_return co_await (self->*method)(session, id, msg);
            });
        registered_msg_ids_.insert(msg_id);
    }

    /// 기본 핸들러 등록 (미등록 msg_id에 대한 폴백).
    /// Gateway처럼 모든 메시지를 범용 처리하는 프록시 서비스용.
    void set_default_handler(boost::asio::awaitable<Result<void>> (Derived::*method)(SessionPtr, uint32_t,
                                                                                     std::span<const uint8_t>))
    {
        auto* self = static_cast<Derived*>(this);
        dispatcher_->set_default_handler(
            [self, method](SessionPtr session, uint32_t id,
                           std::span<const uint8_t> payload) -> boost::asio::awaitable<Result<void>> {
                co_return co_await (self->*method)(session, id, payload);
            });
    }

    // ── Kafka 핸들러 등록 ──────────────────────────────────────────────────

    /// Kafka 메시지용 FlatBuffers 타입 핸들러 등록.
    /// KafkaDispatchBridge가 수신한 Kafka 메시지를 msg_id 기반으로 라우팅할 때 사용.
    /// @note FlatBuffers 메시지 포인터(const T*)는 co_await 시점까지만 유효합니다.
    template <typename FbsType>
    void kafka_route(uint32_t msg_id, boost::asio::awaitable<Result<void>> (Derived::*method)(const KafkaMessageMeta&,
                                                                                              uint32_t, const FbsType*))
    {
        auto* self = static_cast<Derived*>(this);
        kafka_handlers_[msg_id] = [self,
                                   method](KafkaMessageMeta meta, uint32_t id,
                                           std::span<const uint8_t> payload) -> boost::asio::awaitable<Result<void>> {
            flatbuffers::Verifier verifier(payload.data(), payload.size());
            if (!verifier.VerifyBuffer<FbsType>())
            {
                co_return apex::core::error(ErrorCode::FlatBuffersVerifyFailed);
            }
            auto* msg = flatbuffers::GetRoot<FbsType>(payload.data());
            co_return co_await (self->*method)(meta, id, msg);
        };
        has_kafka_handlers_ = true;
    }

    // ── Per-core 상태 접근자 ─────────────────────────────────────────────
    // internal_configure() 이후에만 유효. 그 전에 호출하면 UB.

    /// 요청/코루틴 수명 임시 데이터용 범프 할당자.
    /// @note 정의는 server.hpp에 위치 (PerCoreState complete type 필요).
    BumpAllocator& bump();

    /// 트랜잭션 수명 데이터용 아레나 할당자.
    ArenaAllocator& arena();

    /// 이 서비스가 실행 중인 코어 ID.
    uint32_t core_id() const;

    /// [D7] Tracked 코루틴 스폰. co_spawn(detached) 대신 사용.
    /// outstanding 카운터를 관리하여 shutdown 시 완료 대기 가능.
    template <typename F> void spawn(F&& coro_factory)
    {
        assert(io_ctx_ && "spawn() called before internal_configure");
        outstanding_coros_.fetch_add(1, std::memory_order_acq_rel);
        boost::asio::co_spawn(
            *io_ctx_,
            [this, f = std::forward<F>(coro_factory)]() -> boost::asio::awaitable<void> {
                try
                {
                    co_await f();
                }
                catch (const std::exception& e)
                {
                    spdlog::error("[{}] spawn() coroutine exception: {}", name_, e.what());
                }
                outstanding_coros_.fetch_sub(1, std::memory_order_acq_rel);
            },
            boost::asio::detached);
    }

    /// io_context에 작업 게시. io_context 직접 접근 대신 사용.
    template <typename F> void post(F&& fn)
    {
        assert(io_ctx_ && "post() called before internal_configure");
        boost::asio::post(*io_ctx_, std::forward<F>(fn));
    }

    /// io_context의 executor 반환. timer 등에 필요.
    [[nodiscard]] boost::asio::any_io_executor get_executor() noexcept
    {
        assert(io_ctx_ && "get_executor() called before internal_configure");
        return io_ctx_->get_executor();
    }

    // ── Logging helpers ──────────────────────────────────────────────────
    // core_id + service name이 자동 주입된다.
    // 3가지 오버로드: 기본 / +session / +session+msg_id

#define APEX_SVC_LOG_METHODS(level_name, spdlog_level)                                                                 \
    template <typename... Args> void log_##level_name(fmt::format_string<Args...> fmt, Args&&... args)                 \
    {                                                                                                                  \
        if (spdlog::should_log(spdlog::level::spdlog_level))                                                           \
            spdlog::log(spdlog::level::spdlog_level, "[core={}][{}] {}", core_id_for_log_(), name_,                    \
                        fmt::format(fmt, std::forward<Args>(args)...));                                                \
    }                                                                                                                  \
    template <typename... Args>                                                                                        \
    void log_##level_name(const SessionPtr& session, fmt::format_string<Args...> fmt, Args&&... args)                  \
    {                                                                                                                  \
        if (spdlog::should_log(spdlog::level::spdlog_level))                                                           \
            spdlog::log(spdlog::level::spdlog_level, "[core={}][{}][sess={}] {}", core_id_for_log_(), name_,           \
                        session ? session->id() : make_session_id(0), fmt::format(fmt, std::forward<Args>(args)...));  \
    }                                                                                                                  \
    template <typename... Args>                                                                                        \
    void log_##level_name(const SessionPtr& session, uint32_t msg_id, fmt::format_string<Args...> fmt, Args&&... args) \
    {                                                                                                                  \
        if (spdlog::should_log(spdlog::level::spdlog_level))                                                           \
            spdlog::log(spdlog::level::spdlog_level, "[core={}][{}][sess={}][msg=0x{:04X}] {}", core_id_for_log_(),    \
                        name_, session ? session->id() : make_session_id(0), msg_id,                                   \
                        fmt::format(fmt, std::forward<Args>(args)...));                                                \
    }

    APEX_SVC_LOG_METHODS(trace, trace)
    APEX_SVC_LOG_METHODS(debug, debug)
    APEX_SVC_LOG_METHODS(info, info)
    APEX_SVC_LOG_METHODS(warn, warn)
    APEX_SVC_LOG_METHODS(error, err)

#undef APEX_SVC_LOG_METHODS

  public:
    // ── Kafka 핸들러 접근자 (D2: ServiceBaseInterface override) ──────────

    /// KafkaDispatchBridge가 핸들러 맵에 접근하기 위한 getter.
    [[nodiscard]] bool has_kafka_handlers() const noexcept override
    {
        return has_kafka_handlers_;
    }
    [[nodiscard]] const KafkaHandlerMap& kafka_handler_map() const noexcept override
    {
        return kafka_handlers_;
    }

    // ── D7: Outstanding coroutine tracking ──────────────────────────────

    /// [D7] Outstanding 코루틴 수.
    [[nodiscard]] uint32_t outstanding_coroutines() const noexcept override
    {
        return outstanding_coros_.load(std::memory_order_acquire);
    }

  private:
    uint32_t core_id_for_log_() const noexcept
    {
        return per_core_ ? per_core_->core_id : UINT32_MAX;
    }

    std::string name_;
    // m-06: owned_dispatcher_ provides a default dispatcher for standalone use.
    // When used with Server, bind_dispatcher() replaces it with the shared per-core
    // dispatcher and immediately resets owned_dispatcher_ to free it.
    std::unique_ptr<MessageDispatcher> owned_dispatcher_{std::make_unique<MessageDispatcher>()};
    MessageDispatcher* dispatcher_{owned_dispatcher_.get()};
    bool started_{false};
    boost::unordered_flat_set<uint32_t> registered_msg_ids_;
    PerCoreState* per_core_{nullptr};
    boost::asio::io_context* io_ctx_{nullptr};   // D7: spawn()용
    std::atomic<uint32_t> outstanding_coros_{0}; // D7: outstanding 코루틴 카운터

    // ── Kafka 디스패치 ──────────────────────────────────────────────────
    KafkaHandlerMap kafka_handlers_;
    bool has_kafka_handlers_{false};
};

} // namespace apex::core
