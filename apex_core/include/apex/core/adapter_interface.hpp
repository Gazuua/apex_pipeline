#pragma once

#include <memory>
#include <string_view>
#include <vector>

namespace apex::core {

class CoreEngine;  // forward declaration
class ServiceBaseInterface;  // forward declaration (D2: wire_services)

/// 어댑터 타입 소거 인터페이스 (Server 내부 저장용).
/// AdapterBase<Derived>의 AdapterWrapper가 이를 구현한다.
/// apex_core에 배치하여 server.hpp가 apex_shared에 의존하지 않게 한다.
class AdapterInterface {
public:
    virtual ~AdapterInterface() = default;
    virtual void init(CoreEngine& engine) = 0;
    virtual void drain() = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool is_ready() const noexcept = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// [D2] Adapter-service 자동 배선. Server가 Phase 3 이후 호출.
    /// 기본 구현은 no-op. KafkaAdapter가 override하여 KafkaDispatchBridge 자동 생성.
    virtual void wire_services(
        std::vector<std::unique_ptr<ServiceBaseInterface>>& services,
        CoreEngine& engine) {}
};

} // namespace apex::core
