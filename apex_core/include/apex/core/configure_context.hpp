#pragma once

#include <cstdint>

namespace apex::core {

// 순환 의존 방지를 위한 전방 선언
class Server;
struct PerCoreState;

/// Phase 1 Context: 어댑터만 접근 가능.
/// ServiceRegistry 멤버 의도적 제외 → 다른 서비스 접근 시 컴파일 에러.
struct ConfigureContext {
    Server& server;
    uint32_t core_id;
    PerCoreState& per_core_state;
};

} // namespace apex::core
