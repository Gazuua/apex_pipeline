#pragma once

#include <string_view>

namespace apex::core {

class CoreEngine;  // forward declaration

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
};

} // namespace apex::core
