#pragma once

#include <apex/core/message_dispatcher.hpp>

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace apex::core {

/// CRTP base class for defining services.
/// Services register message handlers and the framework dispatches to them.
///
/// Usage:
///   class EchoService : public ServiceBase<EchoService> {
///   public:
///       EchoService() : ServiceBase("echo") {}
///       void on_start() override { register_handlers(); }
///   private:
///       void register_handlers() {
///           handle(0x0001, &EchoService::on_echo);
///       }
///       void on_echo(uint16_t msg_id, std::span<const uint8_t> payload) { ... }
///   };
template <typename Derived>
class ServiceBase {
public:
    explicit ServiceBase(std::string name) : name_(std::move(name)) {}
    virtual ~ServiceBase() = default;

    ServiceBase(const ServiceBase&) = delete;
    ServiceBase& operator=(const ServiceBase&) = delete;

    virtual void on_start() {}
    virtual void on_stop() {}

    void start() {
        on_start();
        started_ = true;
    }

    void stop() {
        started_ = false;
        on_stop();
    }

    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] MessageDispatcher& dispatcher() noexcept { return dispatcher_; }
    [[nodiscard]] const MessageDispatcher& dispatcher() const noexcept { return dispatcher_; }

protected:
    /// Register a handler for msg_id. Type-safe via member function pointer.
    void handle(uint16_t msg_id,
                void (Derived::*method)(uint16_t, std::span<const uint8_t>))
    {
        auto* self = static_cast<Derived*>(this);
        dispatcher_.register_handler(msg_id,
            [self, method](uint16_t id, std::span<const uint8_t> payload) {
                (self->*method)(id, payload);
            });
    }

    /// FlatBuffers 타입 메시지 핸들러 등록.
    /// 검증 실패 시 핸들러 호출하지 않고 메시지를 무시한다.
    template <typename FbsType>
    void route(uint16_t msg_id,
               void (Derived::*method)(uint16_t, const FbsType*))
    {
        auto* self = static_cast<Derived*>(this);
        dispatcher_.register_handler(msg_id,
            [self, method](uint16_t id, std::span<const uint8_t> payload) {
                flatbuffers::Verifier verifier(payload.data(), payload.size());
                if (!verifier.VerifyBuffer<FbsType>()) {
                    return;
                }
                auto* msg = flatbuffers::GetRoot<FbsType>(payload.data());
                (self->*method)(id, msg);
            });
    }

private:
    std::string name_;
    MessageDispatcher dispatcher_;
    bool started_{false};
};

} // namespace apex::core
