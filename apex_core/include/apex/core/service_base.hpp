#pragma once

#include <apex/core/arena_allocator.hpp>
#include <apex/core/bump_allocator.hpp>
#include <apex/core/configure_context.hpp>
#include <apex/core/error_sender.hpp>
#include <apex/core/message_dispatcher.hpp>
#include <apex/core/result.hpp>
#include <apex/core/session.hpp>
#include <apex/core/wire_context.hpp>
#include <apex/shared/protocols/kafka/kafka_envelope.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <flatbuffers/flatbuffers.h>

#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace apex::core {

/// 비템플릿 서비스 인터페이스. Server가 서비스를 소유하기 위해 사용.
class ServiceBaseInterface {
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
    virtual void internal_configure(ConfigureContext& ctx) { on_configure(ctx); }

    /// Server가 호출하는 진입점. 프레임워크 전처리 + on_wire 호출.
    virtual void internal_wire(WireContext& ctx) { on_wire(ctx); }

    /// Phase 1: 어댑터/설정 접근 단계. 다른 서비스에 접근 불가.
    virtual void on_configure(ConfigureContext&) {}

    /// Phase 2: 서비스 간 와이어링 단계. 다른 서비스 lookup 가능.
    virtual void on_wire(WireContext&) {}

    /// 세션 종료 시 서비스에 통지. 리소스 정리용.
    virtual void on_session_closed(SessionId) {}
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
template <typename Derived>
class ServiceBase : public ServiceBaseInterface {
public:
    explicit ServiceBase(std::string name) : name_(std::move(name)) {}

    ServiceBase(const ServiceBase&) = delete;
    ServiceBase& operator=(const ServiceBase&) = delete;

    virtual void on_start() {}
    virtual void on_stop() {}

    void start() override {
        static_cast<Derived*>(this)->on_start();
        started_ = true;
    }

    void stop() override {
        started_ = false;
        // I-02: Guard against nullptr dispatcher (e.g., stop() called before bind_dispatcher())
        if (dispatcher_) {
            for (auto id : registered_msg_ids_) {
                dispatcher_->unregister_handler(id);
            }
        }
        registered_msg_ids_.clear();
        on_stop();
    }

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }
    [[nodiscard]] bool started() const noexcept override { return started_; }
    [[nodiscard]] MessageDispatcher& dispatcher() noexcept { return *dispatcher_; }
    [[nodiscard]] const MessageDispatcher& dispatcher() const noexcept { return *dispatcher_; }

    void bind_dispatcher(MessageDispatcher& external) override {
        assert(!started_ && "bind_dispatcher must be called before start()");
        dispatcher_ = &external;
        owned_dispatcher_.reset();  // standalone dispatcher 해제
    }

    // ── 프레임워크 내부 메서드 (Server가 라이프사이클 오케스트레이션 시 호출) ──

    /// Phase 1: per_core_ 바인딩 + on_configure 호출.
    /// @note Server::run() 내부에서만 호출. 서비스 코드가 직접 호출하지 않는다.
    void internal_configure(ConfigureContext& ctx) override {
        per_core_ = &ctx.per_core_state;
        static_cast<Derived*>(this)->on_configure(ctx);
    }

    /// Phase 2: on_wire 호출.
    /// @note Server::run() 내부에서만 호출. 서비스 코드가 직접 호출하지 않는다.
    void internal_wire(WireContext& ctx) override {
        static_cast<Derived*>(this)->on_wire(ctx);
    }

protected:
    /// @warning 핸들러 람다가 this(Derived*) raw pointer를 캡처함.
    /// 서비스 수명이 디스패처 수명보다 길거나 같아야 한다.
    /// Server 사용 시 자동 보장됨 (서비스와 디스패처를 동시 소유).

    /// 로우 핸들러 등록 (코루틴, Result<void> 반환).
    void handle(uint32_t msg_id,
                boost::asio::awaitable<Result<void>> (Derived::*method)(
                    SessionPtr, uint32_t, std::span<const uint8_t>))
    {
        auto* self = static_cast<Derived*>(this);
        dispatcher_->register_handler(msg_id,
            [self, method](SessionPtr session, uint32_t id,
                           std::span<const uint8_t> payload)
                -> boost::asio::awaitable<Result<void>> {
                co_return co_await (self->*method)(session, id, payload);
            });
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
               boost::asio::awaitable<Result<void>> (Derived::*method)(
                   SessionPtr, uint32_t, const FbsType*))
    {
        auto* self = static_cast<Derived*>(this);
        dispatcher_->register_handler(msg_id,
            [self, method](SessionPtr session, uint32_t id,
                           std::span<const uint8_t> payload)
                -> boost::asio::awaitable<Result<void>> {
                flatbuffers::Verifier verifier(payload.data(), payload.size());
                if (!verifier.VerifyBuffer<FbsType>()) {
                    // 자동 error frame 전송 후 ok() 반환
                    if (session) {
                        auto frame = ErrorSender::build_error_frame(
                            id, ErrorCode::FlatBuffersVerifyFailed);
                        session->enqueue_write(std::move(frame));
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
    void set_default_handler(
        boost::asio::awaitable<Result<void>> (Derived::*method)(
            SessionPtr, uint32_t, std::span<const uint8_t>))
    {
        auto* self = static_cast<Derived*>(this);
        dispatcher_->set_default_handler(
            [self, method](SessionPtr session, uint32_t id,
                           std::span<const uint8_t> payload)
                -> boost::asio::awaitable<Result<void>> {
                co_return co_await (self->*method)(session, id, payload);
            });
    }

    // ── Kafka 핸들러 등록 ──────────────────────────────────────────────────

    /// Kafka 메시지용 FlatBuffers 타입 핸들러 등록.
    /// KafkaDispatchBridge가 수신한 Kafka 메시지를 msg_id 기반으로 라우팅할 때 사용.
    /// @note FlatBuffers 메시지 포인터(const T*)는 co_await 시점까지만 유효합니다.
    template <typename FbsType>
    void kafka_route(uint32_t msg_id,
                     boost::asio::awaitable<Result<void>> (Derived::*method)(
                         const shared::protocols::kafka::MetadataPrefix&,
                         uint32_t, const FbsType*))
    {
        auto* self = static_cast<Derived*>(this);
        kafka_handlers_[msg_id] = [self, method](
            shared::protocols::kafka::MetadataPrefix meta, uint32_t id,
            std::span<const uint8_t> payload)
                -> boost::asio::awaitable<Result<void>> {
            flatbuffers::Verifier verifier(payload.data(), payload.size());
            if (!verifier.VerifyBuffer<FbsType>()) {
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
    BumpAllocator& bump() { return per_core_->bump_allocator; }

    /// 트랜잭션 수명 데이터용 아레나 할당자.
    ArenaAllocator& arena() { return per_core_->arena_allocator; }

    /// 이 서비스가 실행 중인 코어 ID.
    uint32_t core_id() const { return per_core_->core_id; }

public:
    // ── Kafka 핸들러 접근자 (KafkaDispatchBridge용) ──────────────────────

    /// Kafka 핸들러 타입.
    using KafkaHandler = std::function<
        boost::asio::awaitable<Result<void>>(
            shared::protocols::kafka::MetadataPrefix,
            uint32_t,
            std::span<const uint8_t>)>;

    /// KafkaDispatchBridge가 핸들러 맵에 접근하기 위한 getter.
    [[nodiscard]] const boost::unordered_flat_map<uint32_t, KafkaHandler>&
    kafka_handler_map() const noexcept { return kafka_handlers_; }

    /// Kafka 핸들러 등록 여부.
    [[nodiscard]] bool has_kafka_handlers() const noexcept { return has_kafka_handlers_; }

private:
    std::string name_;
    // m-06: owned_dispatcher_ provides a default dispatcher for standalone use.
    // When used with Server, bind_dispatcher() replaces it with the shared per-core
    // dispatcher and immediately resets owned_dispatcher_ to free it.
    std::unique_ptr<MessageDispatcher> owned_dispatcher_{std::make_unique<MessageDispatcher>()};
    MessageDispatcher* dispatcher_{owned_dispatcher_.get()};
    bool started_{false};
    boost::unordered_flat_set<uint32_t> registered_msg_ids_;
    PerCoreState* per_core_{nullptr};

    // ── Kafka 디스패치 ──────────────────────────────────────────────────
    boost::unordered_flat_map<uint32_t, KafkaHandler> kafka_handlers_;
    bool has_kafka_handlers_{false};
};

} // namespace apex::core
