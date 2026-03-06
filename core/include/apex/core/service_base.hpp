#pragma once

#include <apex/core/message_dispatcher.hpp>

#include <boost/asio/awaitable.hpp>
#include <flatbuffers/flatbuffers.h>

#include <cassert>
#include <cstdint>
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
/// Usage:
///   class EchoService : public ServiceBase<EchoService> {
///   public:
///       EchoService() : ServiceBase("echo") {}
///       void on_start() override {
///           handle(0x0001, &EchoService::on_echo);
///       }
///       awaitable<void> on_echo(SessionPtr session, uint16_t msg_id,
///                               std::span<const uint8_t> payload) { co_return; }
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
        on_stop();
    }

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }
    [[nodiscard]] bool started() const noexcept override { return started_; }
    [[nodiscard]] MessageDispatcher& dispatcher() noexcept { return *dispatcher_; }
    [[nodiscard]] const MessageDispatcher& dispatcher() const noexcept { return *dispatcher_; }

    void bind_dispatcher(MessageDispatcher& external) override {
        assert(!started_ && "bind_dispatcher must be called before start()");
        dispatcher_ = &external;
    }

protected:
    /// 로우 핸들러 등록 (코루틴).
    void handle(uint16_t msg_id,
                boost::asio::awaitable<void> (Derived::*method)(
                    SessionPtr, uint16_t, std::span<const uint8_t>))
    {
        auto* self = static_cast<Derived*>(this);
        dispatcher_->register_handler(msg_id,
            [self, method](SessionPtr session, uint16_t id,
                           std::span<const uint8_t> payload)
                -> boost::asio::awaitable<void> {
                co_await (self->*method)(session, id, payload);
            });
    }

    /// FlatBuffers 타입 핸들러 등록 (코루틴).
    template <typename FbsType>
    void route(uint16_t msg_id,
               boost::asio::awaitable<void> (Derived::*method)(
                   SessionPtr, uint16_t, const FbsType*))
    {
        auto* self = static_cast<Derived*>(this);
        dispatcher_->register_handler(msg_id,
            [self, method](SessionPtr session, uint16_t id,
                           std::span<const uint8_t> payload)
                -> boost::asio::awaitable<void> {
                flatbuffers::Verifier verifier(payload.data(), payload.size());
                if (!verifier.VerifyBuffer<FbsType>()) co_return;
                auto* msg = flatbuffers::GetRoot<FbsType>(payload.data());
                co_await (self->*method)(session, id, msg);
            });
    }

private:
    std::string name_;
    MessageDispatcher owned_dispatcher_;
    MessageDispatcher* dispatcher_ = &owned_dispatcher_;
    bool started_{false};
};

} // namespace apex::core
