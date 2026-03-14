#pragma once

#include <apex/core/message_dispatcher.hpp>
#include <apex/core/result.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <flatbuffers/flatbuffers.h>

#include <cassert>
#include <cstdint>
#include <memory>
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
///       awaitable<Result<void>> on_echo(SessionPtr session, uint16_t msg_id,
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
        on_start();
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

protected:
    /// @warning 핸들러 람다가 this(Derived*) raw pointer를 캡처함.
    /// 서비스 수명이 디스패처 수명보다 길거나 같아야 한다.
    /// Server 사용 시 자동 보장됨 (서비스와 디스패처를 동시 소유).

    /// 로우 핸들러 등록 (코루틴, Result<void> 반환).
    void handle(uint16_t msg_id,
                boost::asio::awaitable<Result<void>> (Derived::*method)(
                    SessionPtr, uint16_t, std::span<const uint8_t>))
    {
        auto* self = static_cast<Derived*>(this);
        dispatcher_->register_handler(msg_id,
            [self, method](SessionPtr session, uint16_t id,
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
    template <typename FbsType>
    void route(uint16_t msg_id,
               boost::asio::awaitable<Result<void>> (Derived::*method)(
                   SessionPtr, uint16_t, const FbsType*))
    {
        auto* self = static_cast<Derived*>(this);
        dispatcher_->register_handler(msg_id,
            [self, method](SessionPtr session, uint16_t id,
                           std::span<const uint8_t> payload)
                -> boost::asio::awaitable<Result<void>> {
                flatbuffers::Verifier verifier(payload.data(), payload.size());
                if (!verifier.VerifyBuffer<FbsType>()) {
                    co_return apex::core::error(ErrorCode::FlatBuffersVerifyFailed);
                }
                auto* msg = flatbuffers::GetRoot<FbsType>(payload.data());
                co_return co_await (self->*method)(session, id, msg);
            });
        registered_msg_ids_.insert(msg_id);
    }

private:
    std::string name_;
    // m-06: owned_dispatcher_ provides a default dispatcher for standalone use.
    // When used with Server, bind_dispatcher() replaces it with the shared per-core
    // dispatcher and immediately resets owned_dispatcher_ to free it.
    std::unique_ptr<MessageDispatcher> owned_dispatcher_{std::make_unique<MessageDispatcher>()};
    MessageDispatcher* dispatcher_{owned_dispatcher_.get()};
    bool started_{false};
    boost::unordered_flat_set<uint16_t> registered_msg_ids_;
};

} // namespace apex::core
